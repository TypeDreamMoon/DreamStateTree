// Copyright DreamMoon. All Rights Reserved.
//
// The semantic stage's entry points: LowerDst, the places table, and the bind names the decompiler needs. The work is
// in the DstLower*.cpp files next to this one (DstLowerPrivate.h lists them).

#include "DstLower.h"

#include "DstLowerPrivate.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeSchema.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler
{
	// ------------------------------------------------------------------------------------------
	// FDstPlaces
	// ------------------------------------------------------------------------------------------

	namespace
	{
		/** The key of what encloses a key: a function of its node, a node of its state, a state of its parent, then the header. */
		FString GetEnclosingKey(const FString& Key)
		{
			int32 Index = Key.Find(TEXT("/Fn:"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (Index != INDEX_NONE)
			{
				return Key.Left(Index);
			}
			if (Key.StartsWith(TEXT("Param:")) || Key.StartsWith(TEXT("Global:")) || Key.StartsWith(TEXT("Global[")))
			{
				return FString();
			}

			// `<state>/Transition[n]/Condition[m]` belongs to the transition `<state>#n`.
			static const TCHAR* const TransitionMark = TEXT("/Transition[");
			Index = Key.Find(TransitionMark, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (Index != INDEX_NONE)
			{
				const int32 NumberStart = Index + FCString::Strlen(TransitionMark);
				const int32 Close = Key.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, NumberStart);
				if (Close != INDEX_NONE)
				{
					return Key.Left(Index) + TEXT("#") + Key.Mid(NumberStart, Close - NumberStart);
				}
			}

			// `<state>#n` and `<state>#on` belong to the state.
			Index = Key.Find(TEXT("#"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (Index != INDEX_NONE)
			{
				return Key.Left(Index);
			}

			// `<state>/Task[0]`, `<state>/Param:X`, `<state>/Child`.
			Index = Key.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (Index != INDEX_NONE)
			{
				return Key.Left(Index);
			}
			return FString();
		}
	}

	void FDstPlaces::Add(const FString& Key, const Lang::FDstSpan& Span)
	{
		Spans.Add(Key, Span);
	}

	Lang::FDstSpan FDstPlaces::Find(const FString& Key) const
	{
		FString Current = Key;
		while (true)
		{
			if (const Lang::FDstSpan* Found = Spans.Find(Current))
			{
				return *Found;
			}
			if (Current.IsEmpty())
			{
				return Lang::FDstSpan();
			}
			Current = GetEnclosingKey(Current);
		}
	}

	// ------------------------------------------------------------------------------------------
	// LowerDst
	// ------------------------------------------------------------------------------------------

	bool LowerDst(const Lang::FDstCompilationUnit& Unit, FDstLowerResult& Out, Lang::FDstDiagnosticSink& Sink, const FDstLowerOptions& Options)
	{
		Lowering::FDstLowerer Lowerer(Unit, Out, Sink, Options);
		return Lowerer.Run();
	}

	namespace Lowering
	{
		bool FDstLowerer::Run()
		{
			using namespace Lang;
			using namespace Model;

			const int32 ErrorsBefore = Sink.Real.NumErrors();
			if (!Unit.Main || !Unit.Main->Document)
			{
				// The parser has said why.
				return true;
			}
			Document = Unit.Main->Document.Get();
			Out.Places.FilePath = Unit.Main->Path;
			bIsLibraryMain = Document->DocumentKind == EDstDocumentKind::StateTreeLibrary;
			if (Document->DocumentKind == EDstDocumentKind::Unknown)
			{
				return Sink.Real.NumErrors() == ErrorsBefore;
			}

			CollectTagPrefixes();

			// A library on its own is checked as far as it can be: what it may hold.
			if (bIsLibraryMain)
			{
				CheckLibraries();
				return Sink.Real.NumErrors() == ErrorsBefore;
			}

			const bool bHeaderOk = ReadHeader(*Document);
			CheckLibraries();
			if (!bHeaderOk || !SchemaClass)
			{
				return false;
			}

			CollectSubtreesAndTaskNames();
			LowerSections(*Document);
			LowerGlobalsSection();
			LowerStates(*Document);
			ResolvePendingTargets();
			CheckStructure();
			Out.Tree.UpdatePaths();

			return Sink.Real.NumErrors() == ErrorsBefore;
		}
	}

	// ------------------------------------------------------------------------------------------
	// EnsureDstBindNames
	// ------------------------------------------------------------------------------------------

	namespace LowerPrivate
	{
		using namespace Lang;
		using namespace Model;
		using Model::FDstNode;

		void VisitNode(FDstNode& Node,const TFunctionRef<void(FDstNode&)>& Visit)
		{
			Visit(Node);
			for (FDstArg& Arg : Node.Args)
			{
				if (Arg.Function.IsValid())
				{
					VisitNode(*Arg.Function, Visit);
				}
			}
		}

		void VisitNodes(TArray<FDstNode>& Nodes, const TFunctionRef<void(FDstNode&)>& Visit)
		{
			for (FDstNode& Node : Nodes)
			{
				VisitNode(Node, Visit);
			}
		}

		void VisitStateNodes(FDstState& State, const TFunctionRef<void(FDstNode&)>& Visit, const TFunctionRef<void(const FDstSourceRef&)>& VisitSource)
		{
			VisitNodes(State.EnterConditions, Visit);
			VisitNodes(State.Considerations, Visit);
			VisitNodes(State.Tasks, Visit);
			for (FDstTransition& Transition : State.Transitions)
			{
				VisitNodes(Transition.Conditions, Visit);
				if (Transition.Delegate.IsSet())
				{
					VisitSource(Transition.Delegate);
				}
			}
			for (FDstState& Child : State.Children)
			{
				VisitStateNodes(Child, Visit, VisitSource);
			}
		}

		bool ContainsName(const TArray<FString>& Names, const FString& Name)
		{
			for (const FString& Existing : Names)
			{
				if (Existing.Equals(Name, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		}

		FString ShortNameOf(const FDstNode& Node)
		{
			FDstNameRegistry& Registry = FDstNameRegistry::Get();
			const FDstNodeType* Type = nullptr;
			if (!Node.BlueprintClass.IsEmpty())
			{
				if (const UClass* Class = FindObject<UClass>(nullptr, *Node.BlueprintClass))
				{
					Type = Registry.FindByBlueprintClass(Class);
				}
			}
			else if (const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *Node.StructType))
			{
				Type = Registry.FindByStruct(Struct);
			}
			if (Type)
			{
				return Lowering::GetNodeTypeShortName(*Type);
			}
			// A type the registry does not list: the last part of its path.
			FString Path = Node.BlueprintClass.IsEmpty() ? Node.StructType : Node.BlueprintClass;
			int32 Dot = INDEX_NONE;
			if (Path.FindLastChar(TEXT('.'), Dot))
			{
				Path = Path.Mid(Dot + 1);
			}
			Path.RemoveFromEnd(TEXT("_C"));
			return Path.IsEmpty() ? FString(TEXT("Node")) : FDstNameRegistry::MakeShortName(TEXT("F") + Path);
		}
	}

	void EnsureDstBindNames(Model::FDstTree& Tree)
	{
		using namespace Lang;
		using namespace Model;
		using namespace LowerPrivate;
		using Model::FDstNode;

		// What is already a name: nodes' own, the words of paths, and the schema's contexts.
		TArray<FString> Used = { TEXT("Params"), TEXT("Event") };
		if (const UClass* SchemaClass = FindObject<UClass>(nullptr, *Tree.SchemaClass))
		{
			if (const UStateTreeSchema* Schema = Cast<UStateTreeSchema>(SchemaClass->GetDefaultObject()))
			{
				for (const FStateTreeExternalDataDesc& Desc : Schema->GetContextDataDescs())
				{
					Used.Add(Desc.Name.ToString());
				}
			}
		}

		// The nodes something binds to or listens to.
		TArray<FString> Referenced;
		auto VisitSource = [&Referenced](const FDstSourceRef& Source)
		{
			if ((Source.Kind == EDstSourceKind::Node || Source.Kind == EDstSourceKind::Global) && !Source.Owner.IsEmpty())
			{
				Referenced.AddUnique(Source.Owner);
			}
		};
		auto Collect = [&Used, &VisitSource](FDstNode& Node)
		{
			if (!Node.BindName.IsEmpty())
			{
				Used.Add(Node.BindName);
			}
			for (const FDstArg& Arg : Node.Args)
			{
				if (Arg.Kind != EDstValueKind::Constant && Arg.Kind != EDstValueKind::Function)
				{
					VisitSource(Arg.Source);
				}
			}
		};
		VisitNodes(Tree.Globals, Collect);
		for (FDstState& State : Tree.States)
		{
			VisitStateNodes(State, Collect, VisitSource);
		}

		// Every global node has a name in the text; so does every node something refers to.
		auto Name = [&Used, &Referenced](FDstNode& Node)
		{
			const bool bNeedsName = Node.Role == EDstNodeRole::Evaluator || Node.Role == EDstNodeRole::GlobalTask
				|| Referenced.ContainsByPredicate([&Node](const FString& Key) { return Key.Equals(Node.Key, ESearchCase::CaseSensitive); });
			if (!bNeedsName || !Node.BindName.IsEmpty())
			{
				return;
			}
			const FString Base = ShortNameOf(Node);
			FString Candidate = Base;
			for (int32 Number = 2; ContainsName(Used, Candidate); ++Number)
			{
				Candidate = FString::Printf(TEXT("%s%d"), *Base, Number);
			}
			Node.BindName = Candidate;
			Used.Add(Candidate);
		};
		VisitNodes(Tree.Globals, Name);
		for (FDstState& State : Tree.States)
		{
			VisitStateNodes(State, Name, VisitSource);
		}
	}
}
