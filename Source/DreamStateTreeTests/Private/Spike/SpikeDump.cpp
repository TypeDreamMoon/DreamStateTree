// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeDump.h"

#include "GameplayTagContainer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PropertyBindingBindableStructDescriptor.h"
#include "PropertyBindingBindingCollection.h"
#include "PropertyBindingPath.h"
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorDataExtension.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeEditorSchema.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeExtension.h"
#include "StateTreePropertyBindings.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StateTreeTypes.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Spike
{
namespace
{
	// ---- Text helpers ----------------------------------------------------------------------------------------

	template <typename EnumType>
	FString EnumToString(const EnumType Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			const FString Name = Enum->GetNameStringByValue(static_cast<int64>(Value));
			if (!Name.IsEmpty())
			{
				return Name;
			}
		}
		return FString::Printf(TEXT("%lld"), static_cast<int64>(Value));
	}

	FString CompletionConditionText(const UE::StateTree::ETaskCompletionCondition Condition)
	{
		switch (Condition)
		{
		case UE::StateTree::ETaskCompletionCondition::Succeeds:
			return TEXT("Succeeds");
		case UE::StateTree::ETaskCompletionCondition::Fails:
			return TEXT("Fails");
		case UE::StateTree::ETaskCompletionCondition::Completes:
			return TEXT("Completes");
		}
		return TEXT("?");
	}

	FString BoolText(const bool bValue)
	{
		return bValue ? TEXT("true") : TEXT("false");
	}

	FString FloatText(const float Value)
	{
		return FString::SanitizeFloat(Value);
	}

	FString Quote(const FString& Text)
	{
		return FString::Printf(TEXT("\"%s\""), *Text.ReplaceCharWithEscapedChar());
	}

	FString ObjectPathText(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString(TEXT("None"));
	}

	bool IsHexDigitChar(const TCHAR Char)
	{
		return (Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f')) || (Char >= TEXT('A') && Char <= TEXT('F'));
	}

	bool IsWordChar(const TCHAR Char)
	{
		return FChar::IsAlnum(Char) || Char == TEXT('_');
	}

	/**
	 * An FGuid exports as 32 hex digits. Blanking them out lets two dumps of trees whose IDs differ compare equal
	 * even where the ID sits inside a value we do not format ourselves (a struct property, an instanced struct).
	 */
	void ScrubGuids(FString& Text)
	{
		const int32 Length = Text.Len();
		FString Result;
		Result.Reserve(Length);

		int32 Index = 0;
		while (Index < Length)
		{
			if (!IsWordChar(Text[Index]))
			{
				Result.AppendChar(Text[Index]);
				++Index;
				continue;
			}

			int32 End = Index;
			bool bAllHex = true;
			while (End < Length && IsWordChar(Text[End]))
			{
				bAllHex = bAllHex && IsHexDigitChar(Text[End]);
				++End;
			}

			if (End - Index == 32 && bAllHex)
			{
				Result.Append(TEXT("<guid>"));
			}
			else
			{
				Result.AppendChars(&Text[Index], End - Index);
			}
			Index = End;
		}

		Text = MoveTemp(Result);
	}

	bool CaseSensitiveLess(const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	}

	class FLineWriter
	{
	public:
		void Add(const int32 Indent, const FString& Line)
		{
			Text.Append(FString::ChrN(Indent * 2, TEXT(' ')));
			Text.Append(Line);
			Text.AppendChar(TEXT('\n'));
		}

		FString Text;
	};

	// ---- Generic property dumping ----------------------------------------------------------------------------

	struct FPropertyDumpSettings
	{
		/** Print every property, not only the ones that differ from the default. */
		bool bAllValues = false;
		/** Blank out GUID-looking tokens in the values. */
		bool bScrubGuids = false;
		/** Leave out what the node base structs declare (they are printed as part of the node line, or are compile results). */
		bool bSkipNodeBaseProperties = false;
		/** Formats a value that export text would print poorly. Returns true when it handled the value. */
		TFunction<bool(const FProperty& /*Property*/, const void* /*Value*/, FString& /*OutText*/)> CustomFormat;
	};

	bool IsNodeBaseOwner(const UStruct* Owner)
	{
		return Owner == FStateTreeNodeBase::StaticStruct()
			|| Owner == FStateTreeTaskBase::StaticStruct()
			|| Owner == FStateTreeConditionBase::StaticStruct()
			|| Owner == FStateTreeConsiderationBase::StaticStruct()
			|| Owner == FStateTreeEvaluatorBase::StaticStruct()
			|| Owner == FStateTreePropertyFunctionBase::StaticStruct();
	}

	FString FormatValue(const FProperty& Property, const void* Container, const int32 ArrayIndex, const FPropertyDumpSettings& Settings)
	{
		FString Text;
		const void* ValuePtr = Property.ContainerPtrToValuePtr<void>(Container, ArrayIndex);
		if (!(Settings.CustomFormat && Settings.CustomFormat(Property, ValuePtr, Text)))
		{
			Property.ExportText_InContainer(ArrayIndex, Text, Container, nullptr, nullptr, PPF_Delimited);
		}
		if (Settings.bScrubGuids)
		{
			ScrubGuids(Text);
		}
		return Text;
	}

	/**
	 * Writes "<Prefix><Name>=<value>" for every property of the struct (or class) value at Data that differs from
	 * Defaults. With no Defaults, every property is written.
	 */
	void DumpStructProperties(FLineWriter& Writer, const int32 Indent, const FString& Prefix, const UStruct* Struct, const void* Data, const void* Defaults, const FPropertyDumpSettings& Settings)
	{
		if (!Struct || !Data)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			if (Settings.bSkipNodeBaseProperties && IsNodeBaseOwner(Property->GetOwnerStruct()))
			{
				continue;
			}

			for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
			{
				if (Defaults && !Settings.bAllValues && Property->Identical_InContainer(Data, Defaults, ArrayIndex, PPF_None))
				{
					continue;
				}

				FString Name = Prefix + Property->GetName();
				if (Property->ArrayDim > 1)
				{
					Name += FString::Printf(TEXT("[%d]"), ArrayIndex);
				}
				Writer.Add(Indent, Name + TEXT("=") + FormatValue(*Property, Data, ArrayIndex, Settings));
			}
		}
	}

	void DumpObjectProperties(FLineWriter& Writer, const int32 Indent, const FString& Prefix, const UObject* Object, const FPropertyDumpSettings& Settings)
	{
		if (!Object)
		{
			return;
		}
		const UObject* Defaults = Object->GetClass()->GetDefaultObject();
		DumpStructProperties(Writer, Indent, Prefix, Object->GetClass(), Object, Defaults, Settings);
	}

	/** Dumps a struct value against a default-constructed value of the same struct. */
	void DumpStructAgainstDefault(FLineWriter& Writer, const int32 Indent, const FString& Prefix, const UScriptStruct* Struct, const void* Data, const FPropertyDumpSettings& Settings)
	{
		if (!Struct || !Data)
		{
			return;
		}
		FStructOnScope Default(Struct);
		DumpStructProperties(Writer, Indent, Prefix, Struct, Data, Default.IsValid() ? Default.GetStructMemory() : nullptr, Settings);
	}

	// ---- Property bags ---------------------------------------------------------------------------------------

	FString BagTypeText(const FPropertyBagPropertyDesc& Desc)
	{
		FString Type = EnumToString(Desc.ValueType);
		if (Desc.ValueTypeObject)
		{
			Type += FString::Printf(TEXT("(%s)"), *Desc.ValueTypeObject->GetPathName());
		}
		for (const EPropertyBagContainerType Container : Desc.ContainerTypes)
		{
			Type = FString::Printf(TEXT("%s<%s>"), *EnumToString(Container), *Type);
		}
		if (Desc.ContainerTypes.GetFirstContainerType() == EPropertyBagContainerType::Map)
		{
			FString KeyType = EnumToString(Desc.KeyType);
			if (Desc.KeyTypeObject)
			{
				KeyType += FString::Printf(TEXT("(%s)"), *Desc.KeyTypeObject->GetPathName());
			}
			Type += TEXT(" key=") + KeyType;
		}
		return Type;
	}

	void DumpPropertyBag(FLineWriter& Writer, const int32 Indent, const FString& Title, const FInstancedPropertyBag& Bag, const TArray<FGuid>* Overrides, const bool bIncludeIds, TArray<FString>* Notes)
	{
		const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
		const TConstArrayView<FPropertyBagPropertyDesc> Descs = BagStruct ? BagStruct->GetPropertyDescs() : TConstArrayView<FPropertyBagPropertyDesc>();
		Writer.Add(Indent, FString::Printf(TEXT("%s count=%d"), *Title, Descs.Num()));

		const uint8* Memory = Bag.GetValue().GetMemory();
		for (const FPropertyBagPropertyDesc& Desc : Descs)
		{
			FString Line = FString::Printf(TEXT("param %s type=%s"), *Desc.Name.ToString(), *BagTypeText(Desc));
			if (Desc.CachedProperty && Memory)
			{
				FString Value;
				Desc.CachedProperty->ExportText_InContainer(0, Value, Memory, nullptr, nullptr, PPF_Delimited);
				if (!bIncludeIds)
				{
					ScrubGuids(Value);
				}
				Line += TEXT(" value=") + Value;
			}
			else
			{
				Line += TEXT(" value=<unavailable>");
			}

			if (Overrides && Overrides->Contains(Desc.ID))
			{
				Line += TEXT(" override");
			}

#if WITH_EDITORONLY_DATA
			if (!Desc.MetaData.IsEmpty())
			{
				TArray<FString> MetaItems;
				for (const FPropertyBagPropertyDescMetaData& Meta : Desc.MetaData)
				{
					MetaItems.Add(FString::Printf(TEXT("%s=%s"), *Meta.Key.ToString(), *Meta.Value));
				}
				Line += FString::Printf(TEXT(" meta=[%s]"), *FString::Join(MetaItems, TEXT(",")));
			}
#endif

			if (bIncludeIds)
			{
				Line += TEXT(" id=") + Desc.ID.ToString();
			}
			Writer.Add(Indent + 1, Line);
		}

		if (Overrides && Notes)
		{
			for (const FGuid& OverrideId : *Overrides)
			{
				const bool bKnown = Descs.ContainsByPredicate([&OverrideId](const FPropertyBagPropertyDesc& Desc)
				{
					return Desc.ID == OverrideId;
				});
				if (!bKnown)
				{
					Notes->AddUnique(FString::Printf(TEXT("%s: override refers to a parameter that is not in the bag"), *Title));
				}
			}
		}
	}

	// ---- Editor data: ID labels ------------------------------------------------------------------------------

	FString NodeShortName(const FStateTreeEditorNode& Node)
	{
		if (Node.InstanceObject)
		{
			return FString::Printf(TEXT("BP:%s"), *Node.InstanceObject->GetClass()->GetName());
		}
		if (Node.Node.IsValid())
		{
			return Node.Node.GetScriptStruct()->GetName();
		}
		return TEXT("<none>");
	}

	/** Path segments as authored, without the display names FPropertyBindingPath::ToString prefers. */
	FString BindingPathText(const FPropertyBindingPath& Path)
	{
		FString Result;
		for (const FPropertyBindingPathSegment& Segment : Path.GetSegments())
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT(".");
			}
			Result += Segment.GetName().ToString();
			if (Segment.GetArrayIndex() >= 0)
			{
				Result += FString::Printf(TEXT("[%d]"), Segment.GetArrayIndex());
			}
			if (const UStruct* InstanceStruct = Segment.GetInstanceStruct())
			{
				Result += FString::Printf(TEXT("<%s>"), *InstanceStruct->GetName());
			}
		}
		return Result;
	}

	/** The property function node a binding carries, if it is a function binding. */
	const FStateTreeEditorNode* GetFunctionNode(const FStateTreePropertyPathBinding& Binding)
	{
		return Binding.GetPropertyFunctionNode().GetPtr<const FStateTreeEditorNode>();
	}

	FString SiblingSegment(const TArray<TObjectPtr<UStateTreeState>>& Siblings, const int32 Index)
	{
		const UStateTreeState* State = Siblings[Index];
		if (!State)
		{
			return TEXT("<null>");
		}

		int32 NumSameName = 0;
		int32 NumSameNameBefore = 0;
		for (int32 OtherIndex = 0; OtherIndex < Siblings.Num(); ++OtherIndex)
		{
			const UStateTreeState* Other = Siblings[OtherIndex];
			if (Other && Other->Name == State->Name)
			{
				++NumSameName;
				if (OtherIndex < Index)
				{
					++NumSameNameBefore;
				}
			}
		}

		const FString Name = State->Name.ToString();
		return NumSameName > 1 ? FString::Printf(TEXT("%s#%d"), *Name, NumSameNameBefore) : Name;
	}

	struct FIdEntry
	{
		FString Label;
		FString Kind;
	};

	/**
	 * Gives every bindable struct of the tree a readable name that does not depend on its GUID: tree parameters,
	 * context data, nodes (instance data and node template), state parameters, events, transitions and property
	 * functions. The names are built from the position in the tree, so two isomorphic trees get the same names.
	 */
	class FIdLabels
	{
	public:
		void Build(const UStateTreeEditorData& Data)
		{
			AddEntry(Data.GetRootParametersGuid(), TEXT("Tree.Parameters"), TEXT("TreeParameter"));
			if (Data.Schema)
			{
				for (const FStateTreeExternalDataDesc& Desc : Data.Schema->GetContextDataDescs())
				{
					AddEntry(Desc.ID, FString::Printf(TEXT("Context.%s"), *Desc.Name.ToString()), TEXT("Context"));
				}
			}

			for (int32 Index = 0; Index < Data.Evaluators.Num(); ++Index)
			{
				AddNode(Data.Evaluators[Index], FString::Printf(TEXT("Global.Evaluator[%d]:%s"), Index, *NodeShortName(Data.Evaluators[Index])), TEXT("Evaluator"));
			}
			for (int32 Index = 0; Index < Data.GlobalTasks.Num(); ++Index)
			{
				AddNode(Data.GlobalTasks[Index], FString::Printf(TEXT("Global.Task[%d]:%s"), Index, *NodeShortName(Data.GlobalTasks[Index])), TEXT("GlobalTask"));
			}

			BuildStates(Data.SubTrees, FString());
			BuildFunctions(Data);
		}

		const FIdEntry* Find(const FGuid& Id) const
		{
			return Entries.Find(Id);
		}

		/** Readable path of a state ("Root/Idle"); empty when unknown. */
		FString StatePath(const UStateTreeState* State) const
		{
			const FString* Path = StatePaths.Find(State);
			return Path ? *Path : FString();
		}

		TMap<const UStateTreeState*, FString> StatePaths;
		/** Property function nodes, with the label they were given. */
		TArray<TPair<FString, const FStateTreeEditorNode*>> Functions;
		TArray<FString> Notes;

	private:
		void AddEntry(const FGuid& Id, const FString& Label, const TCHAR* Kind)
		{
			if (!Id.IsValid())
			{
				return;
			}
			if (const FIdEntry* Existing = Entries.Find(Id))
			{
				Notes.AddUnique(FString::Printf(TEXT("two bindable structs share an id: %s and %s"), *Existing->Label, *Label));
				return;
			}
			Entries.Add(Id, FIdEntry{Label, Kind});
		}

		void AddNode(const FStateTreeEditorNode& Node, const FString& Label, const TCHAR* Kind)
		{
			AddEntry(Node.ID, Label, Kind);
			AddEntry(Node.GetNodeID(), Label + TEXT("(node)"), Kind);
		}

		void BuildStates(const TArray<TObjectPtr<UStateTreeState>>& Siblings, const FString& ParentPath)
		{
			for (int32 Index = 0; Index < Siblings.Num(); ++Index)
			{
				const UStateTreeState* State = Siblings[Index];
				if (!State)
				{
					continue;
				}

				const FString Segment = SiblingSegment(Siblings, Index);
				const FString Path = ParentPath.IsEmpty() ? Segment : ParentPath + TEXT("/") + Segment;
				StatePaths.Add(State, Path);

				AddEntry(State->ID, Path, TEXT("State"));
				AddEntry(State->Parameters.ID, Path + TEXT(".Parameters"), TEXT("StateParameter"));
				AddEntry(State->GetEventID(), Path + TEXT(".EnterEvent"), TEXT("StateEvent"));

				for (int32 NodeIndex = 0; NodeIndex < State->EnterConditions.Num(); ++NodeIndex)
				{
					const FStateTreeEditorNode& Node = State->EnterConditions[NodeIndex];
					AddNode(Node, FString::Printf(TEXT("%s.EnterCondition[%d]:%s"), *Path, NodeIndex, *NodeShortName(Node)), TEXT("Condition"));
				}
				for (int32 NodeIndex = 0; NodeIndex < State->Considerations.Num(); ++NodeIndex)
				{
					const FStateTreeEditorNode& Node = State->Considerations[NodeIndex];
					AddNode(Node, FString::Printf(TEXT("%s.Consideration[%d]:%s"), *Path, NodeIndex, *NodeShortName(Node)), TEXT("Consideration"));
				}
				for (int32 NodeIndex = 0; NodeIndex < State->Tasks.Num(); ++NodeIndex)
				{
					const FStateTreeEditorNode& Node = State->Tasks[NodeIndex];
					AddNode(Node, FString::Printf(TEXT("%s.Task[%d]:%s"), *Path, NodeIndex, *NodeShortName(Node)), TEXT("Task"));
				}
				if (State->SingleTask.Node.IsValid())
				{
					AddNode(State->SingleTask, FString::Printf(TEXT("%s.SingleTask:%s"), *Path, *NodeShortName(State->SingleTask)), TEXT("Task"));
				}

				for (int32 TransitionIndex = 0; TransitionIndex < State->Transitions.Num(); ++TransitionIndex)
				{
					const FStateTreeTransition& Transition = State->Transitions[TransitionIndex];
					const FString TransitionLabel = FString::Printf(TEXT("%s.Transition[%d]"), *Path, TransitionIndex);
					AddEntry(Transition.ID, TransitionLabel, TEXT("Transition"));
					AddEntry(Transition.GetEventID(), TransitionLabel + TEXT(".Event"), TEXT("TransitionEvent"));
					for (int32 NodeIndex = 0; NodeIndex < Transition.Conditions.Num(); ++NodeIndex)
					{
						const FStateTreeEditorNode& Node = Transition.Conditions[NodeIndex];
						AddNode(Node, FString::Printf(TEXT("%s.Condition[%d]:%s"), *TransitionLabel, NodeIndex, *NodeShortName(Node)), TEXT("Condition"));
					}
				}

				BuildStates(State->Children, Path);
			}
		}

		/**
		 * Property functions only exist inside the binding that uses them. A function is named after what it feeds
		 * ("fn:Type@<target>.<path>"); functions that feed other functions need their target named first, so this
		 * repeats until nothing more can be named.
		 */
		void BuildFunctions(const UStateTreeEditorData& Data)
		{
			TArray<const FStateTreePropertyPathBinding*> Pending;
			for (const FStateTreePropertyPathBinding& Binding : Data.EditorBindings.GetBindings())
			{
				if (GetFunctionNode(Binding))
				{
					Pending.Add(&Binding);
				}
			}

			bool bProgress = true;
			while (!Pending.IsEmpty() && bProgress)
			{
				bProgress = false;
				for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
				{
					const FStateTreePropertyPathBinding& Binding = *Pending[Index];
					const FIdEntry* Target = Entries.Find(Binding.GetTargetPath().GetStructID());
					if (!Target)
					{
						continue;
					}

					RegisterFunction(Binding, Target->Label);
					Pending.RemoveAtSwap(Index);
					bProgress = true;
				}
			}

			for (const FStateTreePropertyPathBinding* Binding : Pending)
			{
				Notes.AddUnique(FString::Printf(TEXT("property function feeds a target that is not in the tree: %s"), *BindingPathText(Binding->GetTargetPath())));
				RegisterFunction(*Binding, TEXT("?unresolved"));
			}
		}

		void RegisterFunction(const FStateTreePropertyPathBinding& Binding, const FString& TargetLabel)
		{
			const FStateTreeEditorNode* Function = GetFunctionNode(Binding);
			const FString TargetPath = BindingPathText(Binding.GetTargetPath());
			const FString Label = FString::Printf(TEXT("fn:%s@%s%s%s"), *NodeShortName(*Function), *TargetLabel, TargetPath.IsEmpty() ? TEXT("") : TEXT("."), *TargetPath);
			AddNode(*Function, Label, TEXT("PropertyFunction"));
			Functions.Emplace(Label, Function);
		}

		TMap<FGuid, FIdEntry> Entries;
	};

	// ---- Editor data dumper ----------------------------------------------------------------------------------

	class FEditorDataDumper
	{
	public:
		FEditorDataDumper(const UStateTreeEditorData& InData, const FDumpOptions& InOptions)
			: Data(InData)
			, Options(InOptions)
		{
			DataSettings.bAllValues = Options.bAllValues;
			DataSettings.bScrubGuids = !Options.bIncludeIds;
			DataSettings.CustomFormat = [this](const FProperty& Property, const void* Value, FString& OutText)
			{
				return FormatSpecialValue(Property, Value, OutText);
			};

			NodeSettings = DataSettings;
			NodeSettings.bSkipNodeBaseProperties = true;
		}

		FString Run()
		{
			Labels.Build(Data);

			Writer.Add(0, FString::Printf(TEXT("EditorData class=%s"), *Data.GetClass()->GetPathName()));
			DumpSchemas();
			DumpExtensions();

			DumpPropertyBag(Writer, 0, TEXT("RootParameters"), Data.GetRootParametersPropertyBag(), nullptr, Options.bIncludeIds, &Notes);
			if (Options.bIncludeIds)
			{
				Writer.Add(1, TEXT("rootParametersId=") + Data.GetRootParametersGuid().ToString());
			}

			DumpNodes(0, TEXT("Evaluators"), Data.Evaluators, false, TEXT("Evaluator"));
			Writer.Add(0, FString::Printf(TEXT("GlobalTasksCompletion=%s"), *EnumToString(Data.GlobalTasksCompletion)));
			DumpNodes(0, TEXT("GlobalTasks"), Data.GlobalTasks, false, TEXT("GlobalTask"));

			DumpColors();

			Writer.Add(0, FString::Printf(TEXT("SubTrees count=%d"), Data.SubTrees.Num()));
			for (const UStateTreeState* SubTree : Data.SubTrees)
			{
				if (SubTree)
				{
					DumpState(1, *SubTree);
				}
			}

			DumpBindings();
			DumpFunctions();
			DumpNotes();
			return Writer.Text;
		}

	private:
		// -- Special values --

		FString DescribeLink(const FStateTreeStateLink& Link)
		{
			FString Text = EnumToString(Link.LinkType);
			if (Link.LinkType == EStateTreeTransitionType::GotoState)
			{
				const FIdEntry* Entry = Labels.Find(Link.ID);
				if (Entry && Entry->Kind == TEXT("State"))
				{
					Text += TEXT(" ") + Entry->Label;
				}
				else
				{
					Text += FString::Printf(TEXT(" <missing:%s>"), *Link.Name.ToString());
					Notes.AddUnique(FString::Printf(TEXT("a state link points to a state that is not in the tree (name at link time: %s)"), *Link.Name.ToString()));
				}
			}
			if (Link.Fallback != EStateTreeSelectionFallback::None)
			{
				Text += TEXT(" fallback=") + EnumToString(Link.Fallback);
			}
			if (Options.bIncludeIds && Link.ID.IsValid())
			{
				Text += TEXT(" id=") + Link.ID.ToString();
			}
			return Text;
		}

		bool FormatSpecialValue(const FProperty& Property, const void* Value, FString& OutText)
		{
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
			{
				if (StructProperty->Struct == FStateTreeStateLink::StaticStruct())
				{
					OutText = DescribeLink(*static_cast<const FStateTreeStateLink*>(Value));
					return true;
				}
			}
			return false;
		}

		FString DescribeEvent(const FStateTreeEventDesc& Event) const
		{
			return FString::Printf(TEXT("tag=%s payload=%s consume=%s"), *Event.Tag.ToString(), *ObjectPathText(Event.PayloadStruct), *BoolText(Event.bConsumeEventOnSelect));
		}

		FString DescribeColor(const FStateTreeEditorColorRef& Ref) const
		{
			if (!Ref.ID.IsValid())
			{
				return TEXT("none");
			}
			if (const FStateTreeEditorColor* Color = Data.FindColor(Ref))
			{
				FString Text = Quote(Color->DisplayName);
				if (Options.bIncludeIds)
				{
					Text += TEXT(" ref=") + Ref.ID.ToString();
				}
				return Text;
			}
			return TEXT("<missing>");
		}

		/** A binding endpoint: the readable name of the struct, then the path inside it. */
		FString DescribeEndpoint(const FPropertyBindingPath& Path)
		{
			FString Label;
			if (const FIdEntry* Entry = Labels.Find(Path.GetStructID()))
			{
				Label = Entry->Label;
			}
			else
			{
				Label = FallbackLabel(Path.GetStructID());
			}

			const FString PathText = BindingPathText(Path);
			return PathText.IsEmpty() ? Label : Label + TEXT(".") + PathText;
		}

		/** For a struct the label pass did not know: ask the engine, so a gap in the labels shows up as a note. */
		FString FallbackLabel(const FGuid& Id)
		{
			TInstancedStruct<FPropertyBindingBindableStructDescriptor> Desc;
			if (Data.GetBindableStructByID(Id, Desc))
			{
				if (const FStateTreeBindableStructDesc* StateTreeDesc = Desc.GetPtr<FStateTreeBindableStructDesc>())
				{
					const FString Label = FString::Printf(TEXT("?engine(%s:%s/%s)"), *EnumToString(StateTreeDesc->DataSource), *StateTreeDesc->StatePath, *StateTreeDesc->Name.ToString());
					Notes.AddUnique(TEXT("a binding endpoint is known to the engine but was not labelled: ") + Label);
					return Label;
				}
			}

			Notes.AddUnique(TEXT("a binding endpoint is not a known bindable struct"));
			return Options.bIncludeIds ? FString::Printf(TEXT("?unresolved(%s)"), *Id.ToString()) : FString(TEXT("?unresolved"));
		}

		// -- Schema, extensions, colors --

		void DumpSchemas()
		{
			if (Data.Schema)
			{
				Writer.Add(0, FString::Printf(TEXT("Schema class=%s"), *Data.Schema->GetClass()->GetPathName()));
				DumpObjectProperties(Writer, 1, TEXT("prop."), Data.Schema, DataSettings);

				const TConstArrayView<FStateTreeExternalDataDesc> Contexts = Data.Schema->GetContextDataDescs();
				Writer.Add(1, FString::Printf(TEXT("contextData count=%d"), Contexts.Num()));
				for (int32 Index = 0; Index < Contexts.Num(); ++Index)
				{
					const FStateTreeExternalDataDesc& Desc = Contexts[Index];
					FString Line = FString::Printf(TEXT("[%d] name=%s struct=%s requirement=%s"), Index, *Desc.Name.ToString(), *ObjectPathText(Desc.Struct), *EnumToString(Desc.Requirement));
					if (Options.bIncludeIds)
					{
						Line += TEXT(" id=") + Desc.ID.ToString();
					}
					Writer.Add(2, Line);
				}
			}
			else
			{
				Writer.Add(0, TEXT("Schema class=None"));
			}

			if (Data.EditorSchema)
			{
				Writer.Add(0, FString::Printf(TEXT("EditorSchema class=%s"), *Data.EditorSchema->GetClass()->GetPathName()));
				DumpObjectProperties(Writer, 1, TEXT("prop."), Data.EditorSchema, DataSettings);
			}
			else
			{
				Writer.Add(0, TEXT("EditorSchema class=None"));
			}
		}

		void DumpExtensions()
		{
			Writer.Add(0, FString::Printf(TEXT("Extensions count=%d"), Data.Extensions.Num()));
			for (int32 Index = 0; Index < Data.Extensions.Num(); ++Index)
			{
				const UStateTreeEditorDataExtension* Extension = Data.Extensions[Index];
				Writer.Add(1, FString::Printf(TEXT("[%d] class=%s"), Index, *ObjectPathText(Extension ? Extension->GetClass() : nullptr)));
				DumpObjectProperties(Writer, 2, TEXT("prop."), Extension, DataSettings);
			}
		}

		void DumpColors()
		{
			TArray<FString> Lines;
			for (const FStateTreeEditorColor& Color : Data.Colors)
			{
				FString Line = FString::Printf(TEXT("color %s value=%s"), *Quote(Color.DisplayName), *Color.Color.ToString());
				if (Options.bIncludeIds)
				{
					Line += TEXT(" ref=") + Color.ColorRef.ID.ToString();
				}
				Lines.Add(Line);
			}
			// A TSet has no stable order.
			Lines.Sort(CaseSensitiveLess);

			Writer.Add(0, FString::Printf(TEXT("Colors count=%d"), Lines.Num()));
			for (const FString& Line : Lines)
			{
				Writer.Add(1, Line);
			}
		}

		// -- Nodes --

		void DumpNodes(const int32 Indent, const TCHAR* Title, const TArray<FStateTreeEditorNode>& Nodes, const bool bExpression, const TCHAR* ItemName)
		{
			Writer.Add(Indent, FString::Printf(TEXT("%s count=%d"), Title, Nodes.Num()));
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				DumpNode(Indent + 1, FString::Printf(TEXT("%s[%d]"), ItemName, Index), Nodes[Index], bExpression);
			}
		}

		/** Properties of the node template that the dump prints on the node line, found by name. */
		void AppendNodeAttributes(FString& Line, const FStateTreeEditorNode& Node) const
		{
			const UScriptStruct* NodeStruct = Node.Node.GetScriptStruct();
			const uint8* NodeMemory = Node.Node.GetMemory();

			if (const FNameProperty* NameProperty = CastField<FNameProperty>(NodeStruct->FindPropertyByName(TEXT("Name"))))
			{
				const FName NodeName = NameProperty->GetPropertyValue_InContainer(NodeMemory);
				if (!NodeName.IsNone())
				{
					Line += TEXT(" name=") + Quote(NodeName.ToString());
				}
			}

			static const TCHAR* AttributeNames[] = { TEXT("bTaskEnabled"), TEXT("bConsideredForCompletion"), TEXT("TransitionHandlingPriority"), TEXT("EvaluationMode") };
			for (const TCHAR* AttributeName : AttributeNames)
			{
				if (const FProperty* Property = NodeStruct->FindPropertyByName(AttributeName))
				{
					FString Value;
					Property->ExportText_InContainer(0, Value, NodeMemory, nullptr, nullptr, PPF_None);
					Line += FString::Printf(TEXT(" %s=%s"), AttributeName, *Value);
				}
			}
		}

		void DumpNode(const int32 Indent, const FString& Title, const FStateTreeEditorNode& Node, const bool bExpression)
		{
			const bool bHasNode = Node.Node.IsValid();
			FString Line = FString::Printf(TEXT("%s node=%s"), *Title, bHasNode ? *Node.Node.GetScriptStruct()->GetPathName() : TEXT("<none>"));
			if (Node.InstanceObject)
			{
				Line += FString::Printf(TEXT(" instanceObject=%s"), *Node.InstanceObject->GetClass()->GetPathName());
			}
			if (bExpression)
			{
				Line += FString::Printf(TEXT(" operand=%s indent=%d"), *EnumToString(Node.ExpressionOperand), static_cast<int32>(Node.ExpressionIndent));
			}
			if (bHasNode)
			{
				AppendNodeAttributes(Line, Node);
			}
			if (Options.bIncludeIds)
			{
				Line += FString::Printf(TEXT(" id=%s nodeId=%s"), *Node.ID.ToString(), *Node.GetNodeID().ToString());
			}
			Writer.Add(Indent, Line);

			const int32 Inner = Indent + 1;
			if (bHasNode)
			{
				const UScriptStruct* NodeStruct = Node.Node.GetScriptStruct();
				DumpStructAgainstDefault(Writer, Inner, TEXT("node."), NodeStruct, Node.Node.GetMemory(), NodeSettings);
			}

			if (Node.InstanceObject)
			{
				DumpObjectProperties(Writer, Inner, TEXT("instance."), Node.InstanceObject, DataSettings);
			}
			else if (Node.Instance.IsValid())
			{
				Writer.Add(Inner, FString::Printf(TEXT("instance=%s"), *Node.Instance.GetScriptStruct()->GetPathName()));
				DumpStructAgainstDefault(Writer, Inner, TEXT("instance."), Node.Instance.GetScriptStruct(), Node.Instance.GetMemory(), DataSettings);
			}

			if (Node.ExecutionRuntimeDataObject)
			{
				Writer.Add(Inner, FString::Printf(TEXT("runtimeObject=%s"), *Node.ExecutionRuntimeDataObject->GetClass()->GetPathName()));
				DumpObjectProperties(Writer, Inner, TEXT("runtime."), Node.ExecutionRuntimeDataObject, DataSettings);
			}
			else if (Node.ExecutionRuntimeData.IsValid())
			{
				Writer.Add(Inner, FString::Printf(TEXT("runtime=%s"), *Node.ExecutionRuntimeData.GetScriptStruct()->GetPathName()));
				DumpStructAgainstDefault(Writer, Inner, TEXT("runtime."), Node.ExecutionRuntimeData.GetScriptStruct(), Node.ExecutionRuntimeData.GetMemory(), DataSettings);
			}
		}

		// -- States --

		void DumpState(const int32 Indent, const UStateTreeState& State)
		{
			FString Head = FString::Printf(TEXT("State %s path=%s"), *Quote(State.Name.ToString()), *Labels.StatePath(&State));
			if (Options.bIncludeIds)
			{
				Head += TEXT(" id=") + State.ID.ToString();
			}
			Writer.Add(Indent, Head);

			const int32 Inner = Indent + 1;
			Writer.Add(Inner, FString::Printf(TEXT("enabled=%s"), *BoolText(State.bEnabled)));
			Writer.Add(Inner, FString::Printf(TEXT("type=%s"), *EnumToString(State.Type)));
			Writer.Add(Inner, FString::Printf(TEXT("selection=%s"), *EnumToString(State.SelectionBehavior)));
			Writer.Add(Inner, FString::Printf(TEXT("tasksCompletion=%s"), *EnumToString(State.TasksCompletion)));
			Writer.Add(Inner, FString::Printf(TEXT("tag=%s"), *State.Tag.ToString()));
			Writer.Add(Inner, FString::Printf(TEXT("color=%s"), *DescribeColor(State.ColorRef)));
			Writer.Add(Inner, FString::Printf(TEXT("description=%s"), *Quote(State.Description)));
			Writer.Add(Inner, FString::Printf(TEXT("hasCustomTickRate=%s customTickRate=%s"), *BoolText(State.bHasCustomTickRate), *FloatText(State.CustomTickRate)));
			Writer.Add(Inner, FString::Printf(TEXT("copyParameterBindingsOnTick=%s"), *BoolText(State.bCopyParameterBindingsOnTick)));
			Writer.Add(Inner, FString::Printf(TEXT("checkPrerequisitesWhenActivatingChildDirectly=%s"), *BoolText(State.bCheckPrerequisitesWhenActivatingChildDirectly)));
			Writer.Add(Inner, FString::Printf(TEXT("requiredEventToEnter=%s %s"), *BoolText(State.bHasRequiredEventToEnter), *DescribeEvent(State.RequiredEventToEnter)));
			Writer.Add(Inner, FString::Printf(TEXT("weight=%s"), *FloatText(State.Weight)));
			Writer.Add(Inner, FString::Printf(TEXT("linkedSubtree=%s"), *DescribeLink(State.LinkedSubtree)));
			Writer.Add(Inner, FString::Printf(TEXT("linkedAsset=%s"), *ObjectPathText(State.LinkedAsset)));

			Writer.Add(Inner, FString::Printf(TEXT("parametersFixedLayout=%s"), *BoolText(State.Parameters.bFixedLayout)));
			if (Options.bIncludeIds)
			{
				Writer.Add(Inner, TEXT("parametersId=") + State.Parameters.ID.ToString());
			}
			DumpPropertyBag(Writer, Inner, TEXT("parameters"), State.Parameters.Parameters, &State.Parameters.PropertyOverrides, Options.bIncludeIds, &Notes);

			DumpNodes(Inner, TEXT("enterConditions"), State.EnterConditions, true, TEXT("EnterCondition"));
			DumpNodes(Inner, TEXT("considerations"), State.Considerations, true, TEXT("Consideration"));
			DumpNodes(Inner, TEXT("tasks"), State.Tasks, false, TEXT("Task"));
			if (State.SingleTask.Node.IsValid())
			{
				DumpNode(Inner, TEXT("singleTask"), State.SingleTask, false);
			}

			Writer.Add(Inner, FString::Printf(TEXT("transitions count=%d"), State.Transitions.Num()));
			for (int32 Index = 0; Index < State.Transitions.Num(); ++Index)
			{
				DumpTransition(Inner + 1, Index, State.Transitions[Index]);
			}

			int32 NumChildren = 0;
			for (const UStateTreeState* Child : State.Children)
			{
				NumChildren += Child ? 1 : 0;
			}
			Writer.Add(Inner, FString::Printf(TEXT("children count=%d"), NumChildren));
			for (const UStateTreeState* Child : State.Children)
			{
				if (Child)
				{
					DumpState(Inner + 1, *Child);
				}
			}
		}

		void DumpTransition(const int32 Indent, const int32 Index, const FStateTreeTransition& Transition)
		{
			FString Head = FString::Printf(TEXT("Transition[%d] trigger=%s priority=%s enabled=%s target=%s"),
				Index, *EnumToString(Transition.Trigger), *EnumToString(Transition.Priority), *BoolText(Transition.bTransitionEnabled), *DescribeLink(Transition.State));
			if (Options.bIncludeIds)
			{
				Head += TEXT(" id=") + Transition.ID.ToString();
			}
			Writer.Add(Indent, Head);

			const int32 Inner = Indent + 1;
			Writer.Add(Inner, FString::Printf(TEXT("requiredEvent=%s"), *DescribeEvent(Transition.RequiredEvent)));
			Writer.Add(Inner, FString::Printf(TEXT("delay=%s duration=%s variance=%s"), *BoolText(Transition.bDelayTransition), *FloatText(Transition.DelayDuration), *FloatText(Transition.DelayRandomVariance)));
			Writer.Add(Inner, FString::Printf(TEXT("reactivateTargetState=%s"), *EnumToString(Transition.ReactivateTargetState)));
			DumpNodes(Inner, TEXT("conditions"), Transition.Conditions, true, TEXT("Condition"));
		}

		// -- Bindings --

		void DumpBindings()
		{
			TArray<FString> Lines;
			for (const FStateTreePropertyPathBinding& Binding : Data.EditorBindings.GetBindings())
			{
				FString Line = DescribeEndpoint(Binding.GetTargetPath()) + TEXT(" <- ");

				const TOptional<UE::StateTree::ETaskCompletionCondition> Completion = Binding.GetCompletionBinding();
				if (Completion.IsSet())
				{
					Line += FString::Printf(TEXT("completion[%s] of "), *CompletionConditionText(Completion.GetValue()));
				}
				Line += DescribeEndpoint(Binding.GetSourcePath());

				if (Binding.IsOutputBinding())
				{
					Line += TEXT(" (output)");
				}
				if (Options.bIncludeIds)
				{
					Line += FString::Printf(TEXT(" target=%s source=%s"), *Binding.GetTargetPath().GetStructID().ToString(), *Binding.GetSourcePath().GetStructID().ToString());
				}
				Lines.Add(Line);
			}
			// The binding array order depends on how the bindings were added and removed.
			Lines.Sort(CaseSensitiveLess);

			Writer.Add(0, FString::Printf(TEXT("Bindings count=%d"), Lines.Num()));
			for (const FString& Line : Lines)
			{
				Writer.Add(1, Line);
			}
		}

		void DumpFunctions()
		{
			TArray<TPair<FString, const FStateTreeEditorNode*>> Functions = Labels.Functions;
			Functions.Sort([](const TPair<FString, const FStateTreeEditorNode*>& A, const TPair<FString, const FStateTreeEditorNode*>& B)
			{
				return CaseSensitiveLess(A.Key, B.Key);
			});

			Writer.Add(0, FString::Printf(TEXT("PropertyFunctions count=%d"), Functions.Num()));
			for (const TPair<FString, const FStateTreeEditorNode*>& Function : Functions)
			{
				DumpNode(1, TEXT("Function ") + Function.Key, *Function.Value, false);
			}
		}

		void DumpNotes()
		{
			TArray<FString> AllNotes = Labels.Notes;
			for (const FString& Note : Notes)
			{
				AllNotes.AddUnique(Note);
			}
			AllNotes.Sort(CaseSensitiveLess);

			Writer.Add(0, FString::Printf(TEXT("Notes count=%d"), AllNotes.Num()));
			for (const FString& Note : AllNotes)
			{
				Writer.Add(1, Note);
			}
		}

		const UStateTreeEditorData& Data;
		FDumpOptions Options;
		FLineWriter Writer;
		FIdLabels Labels;
		FPropertyDumpSettings DataSettings;
		FPropertyDumpSettings NodeSettings;
		TArray<FString> Notes;
	};

	// ---- Compiled data ---------------------------------------------------------------------------------------

	const TCHAR* CompiledNodeKind(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return TEXT("Invalid");
		}
		if (Struct->IsChildOf(FStateTreeEvaluatorBase::StaticStruct()))
		{
			return TEXT("Evaluator");
		}
		if (Struct->IsChildOf(FStateTreeTaskBase::StaticStruct()))
		{
			return TEXT("Task");
		}
		if (Struct->IsChildOf(FStateTreeConditionBase::StaticStruct()))
		{
			return TEXT("Condition");
		}
		if (Struct->IsChildOf(FStateTreeConsiderationBase::StaticStruct()))
		{
			return TEXT("Consideration");
		}
		if (Struct->IsChildOf(FStateTreePropertyFunctionBase::StaticStruct()))
		{
			return TEXT("PropertyFunction");
		}
		return TEXT("Other");
	}

	FString IndexText(const FStateTreeIndex16 Index)
	{
		return Index.IsValid() ? FString::FromInt(Index.Get()) : FString(TEXT("-"));
	}

	FString CompiledEventText(const FCompactEventDesc& Event)
	{
		return FString::Printf(TEXT("tag=%s payload=%s"), *Event.Tag.ToString(), *ObjectPathText(Event.PayloadStruct));
	}

	void DumpCompiledNode(FLineWriter& Writer, const int32 Indent, const int32 Index, const FConstStructView& View)
	{
		const UScriptStruct* Struct = View.GetScriptStruct();
		FString Line = FString::Printf(TEXT("[%d] %s struct=%s"), Index, CompiledNodeKind(Struct), *ObjectPathText(Struct));

		if (const FStateTreeNodeBase* Base = View.GetPtr<FStateTreeNodeBase>())
		{
			Line += FString::Printf(TEXT(" name=%s instanceTemplate=%s executionRuntimeTemplate=%s instanceHandle=%s bindings=%s outputBindings=%s"),
				*Base->Name.ToString(), *IndexText(Base->InstanceTemplateIndex), *IndexText(Base->ExecutionRuntimeTemplateIndex),
				*Base->InstanceDataHandle.Describe(), *IndexText(Base->BindingsBatch), *IndexText(Base->OutputBindingsBatch));
		}

		if (const FStateTreeTaskBase* Task = View.GetPtr<FStateTreeTaskBase>())
		{
			Line += FString::Printf(TEXT(" enabled=%s priority=%s tick=%d tickOnlyOnEvents=%d reselect=%d copyOnTick=%d copyOnExit=%d affectTransitions=%d scheduling=%d completionDelegate=%d"),
				*BoolText(Task->bTaskEnabled), *EnumToString(Task->TransitionHandlingPriority),
				static_cast<int32>(Task->bShouldCallTick), static_cast<int32>(Task->bShouldCallTickOnlyOnEvents), static_cast<int32>(Task->bShouldStateChangeOnReselect),
				static_cast<int32>(Task->bShouldCopyBoundPropertiesOnTick), static_cast<int32>(Task->bShouldCopyBoundPropertiesOnExitState),
				static_cast<int32>(Task->bShouldAffectTransitions), static_cast<int32>(Task->bConsideredForScheduling), static_cast<int32>(Task->bHasTaskCompletionDelegateDispatcher));
#if WITH_EDITORONLY_DATA
			Line += FString::Printf(TEXT(" consideredForCompletion=%s"), *BoolText(Task->bConsideredForCompletion));
#endif
		}
		else if (const FStateTreeConditionBase* Condition = View.GetPtr<FStateTreeConditionBase>())
		{
			Line += FString::Printf(TEXT(" operand=%s deltaIndent=%d evaluation=%s stateChangeEvents=%d reselect=%d"),
				*EnumToString(Condition->Operand), static_cast<int32>(Condition->DeltaIndent), *EnumToString(Condition->EvaluationMode),
				static_cast<int32>(Condition->bHasShouldCallStateChangeEvents), static_cast<int32>(Condition->bShouldStateChangeOnReselect));
		}
		else if (const FStateTreeConsiderationBase* Consideration = View.GetPtr<FStateTreeConsiderationBase>())
		{
			Line += FString::Printf(TEXT(" operand=%s deltaIndent=%d"), *EnumToString(Consideration->Operand), static_cast<int32>(Consideration->DeltaIndent));
		}
		Writer.Add(Indent, Line);

		FPropertyDumpSettings Settings;
		Settings.bScrubGuids = true;
		Settings.bSkipNodeBaseProperties = true;
		DumpStructAgainstDefault(Writer, Indent + 1, TEXT("node."), Struct, View.GetMemory(), Settings);
	}

	void DumpCompiledState(FLineWriter& Writer, const int32 Indent, const int32 Index, const UStateTree& StateTree, const FCompactStateTreeState& State)
	{
		TArray<FString> Flags;
		auto AddFlag = [&Flags](const TCHAR* Name, const bool bValue)
		{
			if (bValue)
			{
				Flags.Add(Name);
			}
		};
		AddFlag(TEXT("enabled"), State.bEnabled);
		AddFlag(TEXT("hasTransitionTasks"), State.bHasTransitionTasks);
		AddFlag(TEXT("hasStateChangeConditions"), State.bHasStateChangeConditions);
		AddFlag(TEXT("hasTickTasks"), State.bHasTickTasks);
		AddFlag(TEXT("hasTickTasksOnlyOnEvents"), State.bHasTickTasksOnlyOnEvents);
		AddFlag(TEXT("requestTick"), State.bCachedRequestTick);
		AddFlag(TEXT("requestTickOnlyOnEvents"), State.bCachedRequestTickOnlyOnEvents);
		AddFlag(TEXT("hasTickTriggerTransitions"), State.bHasTickTriggerTransitions);
		AddFlag(TEXT("hasEventTriggerTransitions"), State.bHasEventTriggerTransitions);
		AddFlag(TEXT("hasDelegateTriggerTransitions"), State.bHasDelegateTriggerTransitions);
		AddFlag(TEXT("hasCompletedTriggerTransitions"), State.bHasCompletedTriggerTransitions);
		AddFlag(TEXT("hasSucceededTriggerTransitions"), State.bHasSucceededTriggerTransitions);
		AddFlag(TEXT("hasFailedTriggerTransitions"), State.bHasFailedTriggerTransitions);
		AddFlag(TEXT("checkPrerequisitesWhenActivatingChildDirectly"), State.bCheckPrerequisitesWhenActivatingChildDirectly);
		AddFlag(TEXT("consumeEventOnSelect"), State.bConsumeEventOnSelect);
		AddFlag(TEXT("hasCustomTickRate"), State.bHasCustomTickRate);
		AddFlag(TEXT("copyParameterBindingsOnTick"), State.bCopyParameterBindingsOnTick);
		AddFlag(TEXT("canOverrideLinkedAssetAtRuntime"), State.bCanOverrideLinkedAssetAtRuntime);

		Writer.Add(Indent, FString::Printf(TEXT("[%d] name=%s tag=%s type=%s selection=%s parent=%s depth=%d children=[%d,%d)"),
			Index, *State.Name.ToString(), *State.Tag.ToString(), *EnumToString(State.Type), *EnumToString(State.SelectionBehavior),
			*State.Parent.Describe(), static_cast<int32>(State.Depth), static_cast<int32>(State.ChildrenBegin), static_cast<int32>(State.ChildrenEnd)));

		const int32 Inner = Indent + 1;
		Writer.Add(Inner, FString::Printf(TEXT("linkedState=%s linkedAsset=%s weight=%s customTickRate=%s"),
			*State.LinkedState.Describe(), *ObjectPathText(State.LinkedAsset), *FloatText(State.Weight), *FloatText(State.CustomTickRate)));
		Writer.Add(Inner, FString::Printf(TEXT("enterConditions=[%d,+%d) considerations=[%d,+%d) tasks=[%d,+%d) enabledTasks=%d transitions=[%d,+%d) instanceData=%d"),
			static_cast<int32>(State.EnterConditionsBegin), static_cast<int32>(State.EnterConditionsNum),
			static_cast<int32>(State.UtilityConsiderationsBegin), static_cast<int32>(State.UtilityConsiderationsNum),
			static_cast<int32>(State.TasksBegin), static_cast<int32>(State.TasksNum), static_cast<int32>(State.EnabledTasksNum),
			static_cast<int32>(State.TransitionsBegin), static_cast<int32>(State.TransitionsNum), static_cast<int32>(State.InstanceDataNum)));
		Writer.Add(Inner, FString::Printf(TEXT("completion=%s mask=%08x maskBuffer=%d maskOffset=%d"),
			*EnumToString(State.CompletionTasksControl), State.CompletionTasksMask,
			static_cast<int32>(State.CompletionTasksMaskBufferIndex), static_cast<int32>(State.CompletionTasksMaskBitsOffset)));
		Writer.Add(Inner, FString::Printf(TEXT("requiredEventToEnter=%s"), *CompiledEventText(State.RequiredEventToEnter)));
		Writer.Add(Inner, FString::Printf(TEXT("parameters template=%s handle=%s bindingsBatch=%s eventData=%s"),
			*IndexText(State.ParameterTemplateIndex), *State.ParameterDataHandle.Describe(), *IndexText(State.ParameterBindingsBatch), *IndexText(State.EventDataIndex)));
		Writer.Add(Inner, FString::Printf(TEXT("flags=[%s]"), *FString::Join(Flags, TEXT(","))));

		if (const FCompactStateTreeFrame* Frame = StateTree.GetFrameFromHandle(FStateTreeStateHandle(Index)))
		{
			Writer.Add(Inner, FString::Printf(TEXT("frameRoot numberOfTasksStatusMasks=%d"), static_cast<int32>(Frame->NumberOfTasksStatusMasks)));
		}
	}

	void DumpCompiledTransition(FLineWriter& Writer, const int32 Indent, const int32 Index, const FCompactStateTransition& Transition)
	{
		Writer.Add(Indent, FString::Printf(TEXT("[%d] trigger=%s priority=%s target=%s fallback=%s enabled=%s consumeEvent=%s changeType=%s"),
			Index, *EnumToString(Transition.Trigger), *EnumToString(Transition.Priority), *Transition.State.Describe(), *EnumToString(Transition.Fallback),
			*BoolText(Transition.bTransitionEnabled), *BoolText(Transition.bConsumeEventOnSelect), *EnumToString(Transition.ChangeTypeTargetStateRule)));
		Writer.Add(Indent + 1, FString::Printf(TEXT("conditions=[%d,+%d) delay=%s duration=%s variance=%s requiredEvent=%s delegateDispatcher=%s"),
			static_cast<int32>(Transition.ConditionsBegin), static_cast<int32>(Transition.ConditionsNum), *BoolText(Transition.HasDelay()),
			*FloatText(Transition.Delay.GetDuration()), *FloatText(Transition.Delay.GetRandomVariance()),
			*CompiledEventText(Transition.RequiredEvent), *BoolText(Transition.RequiredDelegateDispatcher.IsValid())));
	}

	/** FStateTreeInstanceData and FInstanceContainer share the accessors used here. */
	template <typename ContainerType>
	void DumpInstanceContainer(FLineWriter& Writer, const int32 Indent, const TCHAR* Title, const ContainerType& Container)
	{
		Writer.Add(Indent, FString::Printf(TEXT("%s count=%d"), Title, Container.Num()));

		FPropertyDumpSettings Settings;
		Settings.bScrubGuids = true;
		for (int32 Index = 0; Index < Container.Num(); ++Index)
		{
			if (Container.IsObject(Index))
			{
				const UObject* Object = Container.GetObject(Index);
				Writer.Add(Indent + 1, FString::Printf(TEXT("[%d] object=%s"), Index, *ObjectPathText(Object ? Object->GetClass() : nullptr)));
				DumpObjectProperties(Writer, Indent + 2, FString(), Object, Settings);
			}
			else
			{
				const FConstStructView View = Container.GetStruct(Index);
				Writer.Add(Indent + 1, FString::Printf(TEXT("[%d] struct=%s"), Index, *ObjectPathText(View.GetScriptStruct())));
				DumpStructAgainstDefault(Writer, Indent + 2, FString(), View.GetScriptStruct(), View.GetMemory(), Settings);
			}
		}
	}

	FString DataHandleText(const FInstancedStruct& Handle)
	{
		if (const FStateTreeDataHandle* DataHandle = Handle.GetPtr<FStateTreeDataHandle>())
		{
			return DataHandle->Describe();
		}
		return Handle.IsValid() ? Handle.GetScriptStruct()->GetName() : FString(TEXT("none"));
	}

	void DumpCompiledBindings(FLineWriter& Writer, const int32 Indent, const FStateTreePropertyBindings& Bindings)
	{
		// The accessors below are public on the base class (the derived class re-declares some as protected).
		const FPropertyBindingBindingCollection& Collection = Bindings;
		const bool bResolved = Collection.IsValid();
		Writer.Add(Indent, FString::Printf(TEXT("propertyBindings resolved=%s bindings=%d batches=%d"), *BoolText(bResolved), Collection.GetNumBindings(), Collection.GetNumCopyBatches()));

		const TArray<FPropertyBindingCopyInfoBatch>& Batches = Collection.GetCopyBatches();
		for (int32 BatchIndex = 0; BatchIndex < Batches.Num(); ++BatchIndex)
		{
			const FPropertyBindingCopyInfoBatch& Batch = Batches[BatchIndex];

			FString Target = TEXT("none");
			const UStruct* TargetStruct = nullptr;
			if (const FStateTreeBindableStructDesc* Desc = Batch.TargetStruct.GetPtr<FStateTreeBindableStructDesc>())
			{
				Target = FString::Printf(TEXT("%s dataHandle=%s struct=%s"), *EnumToString(Desc->DataSource), *Desc->DataHandle.Describe(), *ObjectPathText(Desc->Struct));
				TargetStruct = Desc->Struct;
			}
			else if (const FPropertyBindingBindableStructDescriptor* BaseDesc = Batch.TargetStruct.GetPtr())
			{
				Target = FString::Printf(TEXT("struct=%s"), *ObjectPathText(BaseDesc->Struct));
				TargetStruct = BaseDesc->Struct;
			}

			Writer.Add(Indent + 1, FString::Printf(TEXT("batch[%d] target={%s} bindings=[%d,%d) functions=[%d,%d)"), BatchIndex, *Target,
				Batch.BindingsBegin.AsInt32(), Batch.BindingsEnd.AsInt32(), Batch.PropertyFunctionsBegin.AsInt32(), Batch.PropertyFunctionsEnd.AsInt32()));

			// The copies are resolved at link time; reading them before that would run off the end of the array.
			if (!bResolved || !Batch.BindingsBegin.IsValid() || !Batch.BindingsEnd.IsValid())
			{
				continue;
			}

			int32 CopyIndex = 0;
			for (const FPropertyBindingCopyInfo& Copy : Collection.GetBatchCopies(Batch))
			{
				// The property the copy sits in at the top of the target struct: the leaf alone does not say it when the binding
				// feeds a member (`Interval.Min`) or an element (`Items[2]`), and the values the test ignores belong to that property.
				FString TargetRoot = TEXT("null");
				if (TargetStruct)
				{
					for (TFieldIterator<FProperty> It(TargetStruct); It; ++It)
					{
						if (It->GetOffset_ForInternal() == Copy.TargetIndirection.Offset)
						{
							TargetRoot = It->GetName();
							break;
						}
					}
				}
				Writer.Add(Indent + 2, FString::Printf(TEXT("copy[%d] type=%s size=%d source={handle=%s struct=%s property=%s} target={property=%s root=%s} fromTarget=%s promoteArray=%s"),
					CopyIndex, *EnumToString(Copy.Type), Copy.CopySize, *DataHandleText(Copy.SourceDataHandle), *ObjectPathText(Copy.SourceStructType),
					Copy.SourceLeafProperty ? *Copy.SourceLeafProperty->GetName() : TEXT("null"),
					Copy.TargetLeafProperty ? *Copy.TargetLeafProperty->GetName() : TEXT("null"), *TargetRoot,
					*BoolText(Copy.bCopyFromTargetToSource), *BoolText(Copy.bPromoteArrayCopy)));
				++CopyIndex;
			}
		}
	}

	void DumpDataDescs(FLineWriter& Writer, const int32 Indent, const TCHAR* Title, const TConstArrayView<FStateTreeExternalDataDesc> Descs)
	{
		Writer.Add(Indent, FString::Printf(TEXT("%s count=%d"), Title, Descs.Num()));
		for (int32 Index = 0; Index < Descs.Num(); ++Index)
		{
			const FStateTreeExternalDataDesc& Desc = Descs[Index];
			Writer.Add(Indent + 1, FString::Printf(TEXT("[%d] name=%s struct=%s requirement=%s handle=%s"),
				Index, *Desc.Name.ToString(), *ObjectPathText(Desc.Struct), *EnumToString(Desc.Requirement), *Desc.Handle.DataHandle.Describe()));
		}
	}
} // namespace

FString DumpEditorData(const UStateTreeEditorData& EditorData, const FDumpOptions& Options)
{
	FEditorDataDumper Dumper(EditorData, Options);
	return Dumper.Run();
}

FString DumpCompiled(const UStateTree& StateTree)
{
	FLineWriter Writer;
	// The asset name is left out on purpose, so the dumps of two assets built from the same data are equal.
	Writer.Add(0, TEXT("StateTree compiled"));
	Writer.Add(1, FString::Printf(TEXT("readyToRun=%s"), *BoolText(StateTree.IsReadyToRun())));

	FPropertyDumpSettings ObjectSettings;
	ObjectSettings.bScrubGuids = true;

	const UStateTreeSchema* Schema = StateTree.GetSchema();
	Writer.Add(1, FString::Printf(TEXT("schema=%s"), *ObjectPathText(Schema ? Schema->GetClass() : nullptr)));
	DumpObjectProperties(Writer, 2, TEXT("prop."), Schema, ObjectSettings);

	DumpDataDescs(Writer, 1, TEXT("contextDataDescs"), StateTree.GetContextDataDescs());
	DumpDataDescs(Writer, 1, TEXT("externalDataDescs"), StateTree.GetExternalDataDescs());
	Writer.Add(1, FString::Printf(TEXT("numContextDataViews=%d"), StateTree.GetNumContextDataViews()));
	DumpPropertyBag(Writer, 1, TEXT("defaultParameters"), StateTree.GetDefaultParameters(), nullptr, false, nullptr);

	Writer.Add(1, FString::Printf(TEXT("globalEvaluators begin=%d num=%d"), static_cast<int32>(StateTree.GetGlobalEvaluatorsBegin()), static_cast<int32>(StateTree.GetGlobalEvaluatorsNum())));
	Writer.Add(1, FString::Printf(TEXT("globalTasks begin=%d num=%d"), static_cast<int32>(StateTree.GetGlobalTasksBegin()), static_cast<int32>(StateTree.GetGlobalTasksNum())));
	Writer.Add(1, TEXT("globalTasksCompletion=<not readable: private>"));

	const EStateTreeStateSelectionRules Rules = StateTree.GetStateSelectionRules();
	Writer.Add(1, FString::Printf(TEXT("stateSelectionRules=%u completedTransitionStatesCreateNewStates=%s completedStateBeforeTransitionSourceFailsTransition=%s reselectedStateCreatesNewStates=%s"),
		static_cast<uint32>(Rules),
		*BoolText(EnumHasAnyFlags(Rules, EStateTreeStateSelectionRules::CompletedTransitionStatesCreateNewStates)),
		*BoolText(EnumHasAnyFlags(Rules, EStateTreeStateSelectionRules::CompletedStateBeforeTransitionSourceFailsTransition)),
		*BoolText(EnumHasAnyFlags(Rules, EStateTreeStateSelectionRules::ReselectedStateCreatesNewStates))));
	Writer.Add(1, FString::Printf(TEXT("scheduledTickAllowed=%s"), *BoolText(StateTree.IsScheduledTickAllowed())));
	Writer.Add(1, FString::Printf(TEXT("globalTasksTick=%s globalTasksRequestTick=%s"), *BoolText(StateTree.ShouldTickGlobalTasks(false)), *BoolText(StateTree.DoesRequestTickGlobalTasks(false))));

	const FInstancedStructContainer& Nodes = StateTree.GetNodes();
	Writer.Add(1, FString::Printf(TEXT("nodes count=%d"), Nodes.Num()));
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		DumpCompiledNode(Writer, 2, Index, Nodes[Index]);
	}

	const TConstArrayView<FCompactStateTreeState> States = StateTree.GetStates();
	Writer.Add(1, FString::Printf(TEXT("states count=%d"), States.Num()));
	for (int32 Index = 0; Index < States.Num(); ++Index)
	{
		DumpCompiledState(Writer, 2, Index, StateTree, States[Index]);
	}

	// There is no accessor for the number of transitions: read until the index is out of range.
	int32 NumTransitions = 0;
	while (StateTree.GetTransitionFromIndex(FStateTreeIndex16(NumTransitions)))
	{
		++NumTransitions;
	}
	Writer.Add(1, FString::Printf(TEXT("transitions count=%d"), NumTransitions));
	for (int32 Index = 0; Index < NumTransitions; ++Index)
	{
		DumpCompiledTransition(Writer, 2, Index, *StateTree.GetTransitionFromIndex(FStateTreeIndex16(Index)));
	}

	DumpInstanceContainer(Writer, 1, TEXT("defaultInstanceData"), StateTree.GetDefaultInstanceData());
	if (const TSharedPtr<FStateTreeInstanceData> Shared = StateTree.GetSharedInstanceData())
	{
		DumpInstanceContainer(Writer, 1, TEXT("sharedInstanceData"), *Shared);
	}
	else
	{
		// The tree has no shared instance data (no conditions or considerations), or it is not compiled.
		Writer.Add(1, TEXT("sharedInstanceData count=0"));
	}
	DumpInstanceContainer(Writer, 1, TEXT("evaluationScopeInstanceData"), StateTree.GetDefaultEvaluationScopeInstanceData());
	DumpInstanceContainer(Writer, 1, TEXT("executionRuntimeData"), StateTree.GetDefaultExecutionRuntimeData());

	DumpCompiledBindings(Writer, 1, StateTree.GetPropertyBindings());

	const TConstArrayView<UE::StateTree::FTaskCompletionDispatcher> Dispatchers = StateTree.GetTaskCompletionDispatchers();
	Writer.Add(1, FString::Printf(TEXT("taskCompletionDispatchers count=%d"), Dispatchers.Num()));
	for (int32 Index = 0; Index < Dispatchers.Num(); ++Index)
	{
		Writer.Add(2, FString::Printf(TEXT("[%d] taskNode=%s condition=%s"), Index, *IndexText(Dispatchers[Index].TaskNodeIndex), *CompletionConditionText(Dispatchers[Index].Condition)));
	}

	const TConstArrayView<UStateTreeExtension*> Extensions = StateTree.GetExtensions();
	Writer.Add(1, FString::Printf(TEXT("extensions count=%d"), Extensions.Num()));
	for (int32 Index = 0; Index < Extensions.Num(); ++Index)
	{
		Writer.Add(2, FString::Printf(TEXT("[%d] class=%s"), Index, *ObjectPathText(Extensions[Index] ? Extensions[Index]->GetClass() : nullptr)));
		DumpObjectProperties(Writer, 3, TEXT("prop."), Extensions[Index], ObjectSettings);
	}

	return Writer.Text;
}

FString DiffLines(const FString& A, const FString& B, const int32 MaxLines)
{
	auto SplitLines = [](const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty*/ false);
		for (FString& Line : Lines)
		{
			Line.TrimEndInline();
		}
		// A trailing newline leaves one empty element that is not a line.
		if (!Lines.IsEmpty() && Lines.Last().IsEmpty() && Text.EndsWith(TEXT("\n")))
		{
			Lines.RemoveAt(Lines.Num() - 1);
		}
		return Lines;
	};

	const TArray<FString> LinesA = SplitLines(A);
	const TArray<FString> LinesB = SplitLines(B);

	auto CountLines = [](const TArray<FString>& Lines)
	{
		TMap<FString, int32> Counts;
		for (const FString& Line : Lines)
		{
			Counts.FindOrAdd(Line) += 1;
		}
		return Counts;
	};

	// Multiset difference: a line counts as matched as often as the other side has it.
	auto Unmatched = [](const TArray<FString>& Lines, TMap<FString, int32> OtherCounts)
	{
		TArray<FString> Result;
		for (const FString& Line : Lines)
		{
			int32* Remaining = OtherCounts.Find(Line);
			if (Remaining && *Remaining > 0)
			{
				--(*Remaining);
			}
			else
			{
				Result.Add(Line);
			}
		}
		return Result;
	};

	const TArray<FString> OnlyA = Unmatched(LinesA, CountLines(LinesB));
	const TArray<FString> OnlyB = Unmatched(LinesB, CountLines(LinesA));
	if (OnlyA.IsEmpty() && OnlyB.IsEmpty())
	{
		return FString();
	}

	FString Result;
	auto AppendSide = [&Result, MaxLines](const TCHAR* Title, const TCHAR* Marker, const TArray<FString>& Lines)
	{
		Result += FString::Printf(TEXT("--- only in %s (%d lines)\n"), Title, Lines.Num());
		const int32 NumShown = FMath::Min(Lines.Num(), FMath::Max(MaxLines, 0));
		for (int32 Index = 0; Index < NumShown; ++Index)
		{
			Result += FString::Printf(TEXT("%s %s\n"), Marker, *Lines[Index]);
		}
		if (NumShown < Lines.Num())
		{
			Result += FString::Printf(TEXT("... %d more\n"), Lines.Num() - NumShown);
		}
	};
	AppendSide(TEXT("A"), TEXT("<"), OnlyA);
	AppendSide(TEXT("B"), TEXT(">"), OnlyB);
	return Result;
}

FString SaveReport(const FString& Folder, const FString& Name, const FString& Text)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamStateTreeTests"), TEXT("Spike"), Folder);
	const FString FileName = FPaths::MakeValidFileName(Name) + TEXT(".txt");
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Directory, FileName));

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Warning, TEXT("DreamStateTree spike: could not write %s"), *Path);
		return FString();
	}
	return Path;
}
} // namespace UE::DreamStateTree::Spike
