// Copyright DreamMoon. All Rights Reserved.
//
// The extractor: StateTree asset -> model. This file is the tree and its states; DstExtractNode.cpp reads the nodes,
// DstExtractBinding.cpp the bindings (see DstExtractCommon.h for the map).
//
// The extraction is literal. It records what the editor data holds in the order the editor holds it, applies none of the
// text's sugar, and sorts the few things the engine keeps unordered (the color set) so that the same asset always gives
// the same model, character for character in DumpDstModel.
//
// What it reports instead of writing (DST8xxx):
//   8001  the editor data is a subclass of UStateTreeEditorData          8006  an event binding repaired (warning)
//   8002  an editor data extension the model does not know               8007  a binding the model has no place for
//   8003  a binding source that cannot be resolved                       8008  something dropped because the model has no field for it (warning)
//   8004  a value or type the model cannot write                         8009  a state link that points at no state of the tree
//   8005  two sibling states with the same name (their paths collide)    8010  the asset has no editor data or no schema

#include "DstExtract.h"

#include "DstExtractCommon.h"
#include "DstValues.h"
#include "GameplayTagContainer.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorDataExtension.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorTypes.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTasksStatus.h"
#include "StateTreeTypes.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Extracting
{
	using namespace Model;

	namespace
	{
		FString TagToText(const FGameplayTag& Tag)
		{
			return Tag.IsValid() ? Tag.ToString() : FString();
		}

		FDstEvent MakeEvent(const FStateTreeEventDesc& Desc)
		{
			FDstEvent Event;
			Event.Tag = TagToText(Desc.Tag);
			Event.PayloadType = Desc.PayloadStruct ? Desc.PayloadStruct->GetPathName() : FString();
			Event.bConsumeOnSelect = Desc.bConsumeEventOnSelect;
			return Event;
		}

		EDstStateType ToDstStateType(const EStateTreeStateType Type)
		{
			switch (Type)
			{
			case EStateTreeStateType::State: return EDstStateType::State;
			case EStateTreeStateType::Group: return EDstStateType::Group;
			case EStateTreeStateType::Linked: return EDstStateType::Linked;
			case EStateTreeStateType::LinkedAsset: return EDstStateType::LinkedAsset;
			case EStateTreeStateType::Subtree: return EDstStateType::Subtree;
			}
			return EDstStateType::State;
		}

		EDstOperand ToDstOperand(const EStateTreeExpressionOperand Operand)
		{
			switch (Operand)
			{
			case EStateTreeExpressionOperand::Copy: return EDstOperand::Copy;
			case EStateTreeExpressionOperand::And: return EDstOperand::And;
			case EStateTreeExpressionOperand::Or: return EDstOperand::Or;
			case EStateTreeExpressionOperand::Multiply: return EDstOperand::Multiply;
			}
			return EDstOperand::And;
		}

		/** Whether Property differs between two containers, as a value and as export text; OutText gets the actual value's text. */
		bool DiffersFromDefault(const FProperty& Property, const void* Actual, const void* Default, FString& OutText)
		{
			if (Property.Identical_InContainer(Actual, Default, 0, PPF_None))
			{
				return false;
			}
			OutText = DstExportDefault(Property, Actual);
			return !OutText.Equals(DstExportDefault(Property, Default), ESearchCase::CaseSensitive);
		}

		/** The canonical type string of FDstParameter::Type for a property bag property. False with OutProblem when it has none. */
		bool BagDescToType(const FPropertyBagPropertyDesc& Desc, FString& OutType, FString& OutProblem)
		{
			const FString ObjectPath = Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString();

			FString Scalar;
			const TCHAR* ObjectPrefix = nullptr;
			switch (Desc.ValueType)
			{
			case EPropertyBagPropertyType::Bool: Scalar = TEXT("bool"); break;
			case EPropertyBagPropertyType::Int32: Scalar = TEXT("int32"); break;
			case EPropertyBagPropertyType::Int64: Scalar = TEXT("int64"); break;
			case EPropertyBagPropertyType::Float: Scalar = TEXT("float"); break;
			case EPropertyBagPropertyType::Double: Scalar = TEXT("double"); break;
			case EPropertyBagPropertyType::Name: Scalar = TEXT("Name"); break;
			case EPropertyBagPropertyType::String: Scalar = TEXT("String"); break;
			case EPropertyBagPropertyType::Text: Scalar = TEXT("Text"); break;
			case EPropertyBagPropertyType::Enum: ObjectPrefix = TEXT("Enum:"); break;
			case EPropertyBagPropertyType::Struct: ObjectPrefix = TEXT("Struct:"); break;
			case EPropertyBagPropertyType::Object: ObjectPrefix = TEXT("Object:"); break;
			case EPropertyBagPropertyType::SoftObject: ObjectPrefix = TEXT("SoftObject:"); break;
			case EPropertyBagPropertyType::Class: ObjectPrefix = TEXT("Class:"); break;
			case EPropertyBagPropertyType::SoftClass: ObjectPrefix = TEXT("SoftClass:"); break;
			default:
				// Byte, the small and unsigned integers: the text has no name for them.
				OutProblem = FString::Printf(TEXT("the type %s has no name in the text"), *EnumToName(Desc.ValueType));
				return false;
			}
			if (ObjectPrefix)
			{
				if (ObjectPath.IsEmpty())
				{
					OutProblem = TEXT("the struct, enum or class it holds is missing");
					return false;
				}
				Scalar = FString(ObjectPrefix) + ObjectPath;
			}

			// ContainerTypes[0] is the outermost container.
			for (int32 Index = static_cast<int32>(Desc.ContainerTypes.Num()) - 1; Index >= 0; --Index)
			{
				switch (Desc.ContainerTypes[Index])
				{
				case EPropertyBagContainerType::Array: Scalar = FString::Printf(TEXT("Array<%s>"), *Scalar); break;
				case EPropertyBagContainerType::Set: Scalar = FString::Printf(TEXT("Set<%s>"), *Scalar); break;
				default:
					OutProblem = TEXT("the model holds arrays and sets, not maps");
					return false;
				}
			}

			OutType = MoveTemp(Scalar);
			return true;
		}
	}

	// ------------------------------------------------------------------------------------------
	// The run
	// ------------------------------------------------------------------------------------------

	FExtractor::FExtractor(const UStateTree& InAsset, Lang::FDstDiagnosticSink& InSink)
		: Asset(InAsset)
		, Sink(InSink)
	{
	}

	bool FExtractor::Run(FDstTree& OutTree, TMap<FString, FGuid>& OutIds)
	{
		Tree = &OutTree;
		const int32 ErrorsBefore = Sink.NumErrors();

		if (CheckEditorData())
		{
			Tree->AssetPath = Asset.GetPathName();
			Tree->SchemaClass = Data->Schema->GetClass()->GetPathName();

			// Everything that names a struct by ID needs the whole tree known first: a binding in one state reads from
			// another.
			RegisterStructs();
			GroupBindings();

			ExtractSchema();
			ExtractExtensions();
			ExtractSettings();
			ExtractTreeParameters();
			ExtractColors();
			ExtractGlobals();
			ExtractStates();
			ReportUnconsumedBindings();

			Tree->UpdatePaths();
		}

		OutIds = MoveTemp(Ids);
		return Sink.NumErrors() == ErrorsBefore;
	}

	bool FExtractor::CheckEditorData()
	{
		const UObject* EditorDataObject = Asset.EditorData.Get();
		if (!EditorDataObject)
		{
			return Sink.Error(TEXT("DST8010"), Lang::FDstSpan(),
				FString::Printf(TEXT("`%s` has no editor data, so there is nothing to extract."), *Asset.GetPathName()),
				TEXT("An asset that was cooked without its editor data cannot be decompiled."));
		}

		Data = Cast<const UStateTreeEditorData>(EditorDataObject);
		if (!Data || Data->GetClass() != UStateTreeEditorData::StaticClass())
		{
			const FString ClassPath = EditorDataObject->GetClass()->GetPathName();
			Data = nullptr;
			return Sink.Error(TEXT("DST8001"), Lang::FDstSpan(),
				FString::Printf(TEXT("The editor data of `%s` is a `%s`, not a plain UStateTreeEditorData, and the model cannot hold what a subclass adds."),
					*Asset.GetPathName(), *ClassPath),
				TEXT("Trees whose editor data is a subclass (the ones other plugins add their own data to) cannot be decompiled."));
		}

		if (!Data->Schema)
		{
			return Sink.Error(TEXT("DST8010"), Lang::FDstSpan(),
				FString::Printf(TEXT("The editor data of `%s` has no schema."), *Asset.GetPathName()),
				TEXT("Set a schema on the tree in the editor first."));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Tree level
	// ------------------------------------------------------------------------------------------

	void FExtractor::ExtractSchema()
	{
		const UStateTreeSchema* Schema = Data->Schema;
		const UObject* Defaults = Schema->GetClass()->GetDefaultObject();

		// What the author set on the schema: editable properties that differ from the class default. The schema's
		// context list is derived from them and is not one of them (it has no Edit flag).
		for (TFieldIterator<FProperty> It(Schema->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || Property->ArrayDim != 1)
			{
				continue;
			}
			FString Text;
			if (DiffersFromDefault(*Property, Schema, Defaults, Text))
			{
				Tree->SchemaProperties.Add(Property->GetName(), Text);
			}
		}
	}

	void FExtractor::ExtractExtensions()
	{
		// The editor's own view settings; they are display state, not part of the tree.
		static const FString ViewExtensionClass(TEXT("/Script/StateTreeEditorModule.StateCentricViewEditorDataExtension"));

		for (const UStateTreeEditorDataExtension* Extension : Data->Extensions)
		{
			if (!Extension)
			{
				continue;
			}
			const FString ClassPath = Extension->GetClass()->GetPathName();
			if (ClassPath.Equals(ViewExtensionClass, ESearchCase::CaseSensitive))
			{
				continue;
			}
			Sink.Error(TEXT("DST8002"), Lang::FDstSpan(),
				FString::Printf(TEXT("The editor data of `%s` carries the extension `%s`, which the model does not know."), *Asset.GetPathName(), *ClassPath),
				TEXT("Only the editor's view settings (StateCentricViewEditorDataExtension) are ignored; any other extension would be lost."));
		}
	}

	void FExtractor::ExtractSettings()
	{
		const UStateTreeEditorData* Defaults = GetDefault<UStateTreeEditorData>();
		if (const FProperty* Property = UStateTreeEditorData::StaticClass()->FindPropertyByName(TEXT("GlobalTasksCompletion")))
		{
			FString Text;
			if (DiffersFromDefault(*Property, Data, Defaults, Text))
			{
				Tree->Settings.Add(Property->GetName(), Text);
			}
		}
	}

	void FExtractor::ExtractParameters(const FInstancedPropertyBag& Bag, const UStateTreeState* OverridesOwner, const FString& Label,
		const FString& IdPrefix, TArray<FDstParameter>& Out)
	{
		const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
		if (!BagStruct)
		{
			return;
		}

		const uint8* Memory = Bag.GetValue().GetMemory();
		for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
		{
			FDstParameter Parameter;
			Parameter.Name = Desc.Name.ToString();

			FString Problem;
			if (!BagDescToType(Desc, Parameter.Type, Problem))
			{
				Sink.Error(TEXT("DST8004"), Lang::FDstSpan(),
					FString::Printf(TEXT("The parameter `%s` of %s cannot be written: %s."), *Parameter.Name, *Label, *Problem),
					TEXT("Change the parameter to a type the text has a name for."));
				continue;
			}

			// The model spells "the type's zero value" as no text, the way the semantic stage does for a parameter written
			// without a value (a fresh property bag value is that zero).
			if (Desc.CachedProperty && Memory)
			{
				const FString Text = DstExportDefault(*Desc.CachedProperty, Memory);
				if (Text != DstExportDefault(*Desc.CachedProperty, nullptr))
				{
					Parameter.ValueText = Text;
				}
			}
			if (OverridesOwner)
			{
				Parameter.bOverridden = OverridesOwner->IsParametersPropertyOverridden(Desc.ID);
			}
			for (const FPropertyBagPropertyDescMetaData& Meta : Desc.MetaData)
			{
				Parameter.Meta.Add(Meta.Key.ToString(), Meta.Value);
			}

			if (Desc.ID.IsValid())
			{
				Ids.Add(IdPrefix + Parameter.Name, Desc.ID);
			}
			Out.Add(MoveTemp(Parameter));
		}
	}

	void FExtractor::ExtractTreeParameters()
	{
		ExtractParameters(Data->GetRootParametersPropertyBag(), nullptr, TEXT("the tree"), TEXT("Param:"), Tree->Parameters);
		if (Data->GetRootParametersGuid().IsValid())
		{
			Ids.Add(TEXT("Parameters"), Data->GetRootParametersGuid());
		}
	}

	void FExtractor::ExtractColors()
	{
		const FProperty* ColorProperty = FStateTreeEditorColor::StaticStruct()->FindPropertyByName(TEXT("Color"));

		// The editor data constructor adds one entry with no ID; a tree that still has it untouched has no color of its own.
		const FLinearColor UntouchedColor(FColor(31, 151, 167));

		TArray<FDstColor> Colors;
		for (const FStateTreeEditorColor& Color : Data->Colors)
		{
			if (!Color.ColorRef.ID.IsValid() && Color.DisplayName.Equals(TEXT("Default Color"), ESearchCase::CaseSensitive) && Color.Color == UntouchedColor)
			{
				continue;
			}

			FDstColor& Out = Colors.AddDefaulted_GetRef();
			Out.Name = Color.DisplayName;
			if (ColorProperty)
			{
				Out.ColorText = DstExportDefault(*ColorProperty, &Color);
			}
			if (Color.ColorRef.ID.IsValid())
			{
				Ids.Add(TEXT("Color:") + Color.DisplayName, Color.ColorRef.ID);
			}
		}

		// The colors are a TSet, which has no order of its own.
		Colors.StableSort([](const FDstColor& A, const FDstColor& B)
		{
			const int32 ByName = A.Name.Compare(B.Name, ESearchCase::CaseSensitive);
			return ByName != 0 ? ByName < 0 : A.ColorText.Compare(B.ColorText, ESearchCase::CaseSensitive) < 0;
		});
		Tree->Colors = MoveTemp(Colors);
	}

	void FExtractor::ExtractGlobals()
	{
		// Evaluators first, then global tasks, numbered as one list.
		FNodeSite GlobalSite;
		int32 Index = 0;
		for (const FStateTreeEditorNode& Node : Data->Evaluators)
		{
			if (Node.Node.IsValid())
			{
				Tree->Globals.Add(ExtractNode(Node, MakeGlobalKey(Index++), EDstNodeRole::Evaluator, GlobalSite));
			}
		}
		for (const FStateTreeEditorNode& Node : Data->GlobalTasks)
		{
			if (Node.Node.IsValid())
			{
				Tree->Globals.Add(ExtractNode(Node, MakeGlobalKey(Index++), EDstNodeRole::GlobalTask, GlobalSite));
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// States
	// ------------------------------------------------------------------------------------------

	void FExtractor::ExtractStates()
	{
		for (const TObjectPtr<UStateTreeState>& State : Data->SubTrees)
		{
			if (State)
			{
				FDstState& Out = Tree->States.AddDefaulted_GetRef();
				ExtractState(*State, Out);
			}
		}
	}

	FString FExtractor::FindStatePath(const FGuid& StateId, const FString& What)
	{
		if (const FString* Path = StatePathById.Find(StateId))
		{
			return *Path;
		}
		Sink.Error(TEXT("DST8009"), Lang::FDstSpan(),
			FString::Printf(TEXT("%s points at a state (ID %s) that is not in this tree."), *What, *StateId.ToString(EGuidFormats::Digits)),
			TEXT("The state was deleted after the link was made. Set the link again in the editor."));
		return FString();
	}

	void FExtractor::ExtractNodeList(const TArray<const FStateTreeEditorNode*>& Nodes, const EDstNodeRole Role, const FNodeSite& Site,
		const bool bExpression, TFunctionRef<FString(int32)> MakeKey, TArray<FDstNode>& Out)
	{
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FStateTreeEditorNode& Editor = *Nodes[Index];
			FDstNode Node = ExtractNode(Editor, MakeKey(Index), Role, Site);
			// The first of a list is always Copy at indent 0, whatever the editor node stored (the compiler ignores it).
			if (bExpression && Index > 0)
			{
				Node.Operand = ToDstOperand(Editor.ExpressionOperand);
				Node.Indent = FMath::Clamp<int32>(Editor.ExpressionIndent, 0, UE::StateTree::MaxExpressionIndent);
			}
			Out.Add(MoveTemp(Node));
		}
	}

	void FExtractor::ExtractState(const UStateTreeState& State, FDstState& Out)
	{
		const FString& Path = StatePaths.FindChecked(&State);
		const UStateTreeState* DefaultState = GetDefault<UStateTreeState>();

		Out.Name = State.Name.ToString();
		Out.Type = ToDstStateType(State.Type);
		Out.Selection = EnumToName(State.SelectionBehavior);
		Out.TasksCompletion = EnumToName(State.TasksCompletion);
		Out.Tag = TagToText(State.Tag);
		Out.Description = State.Description;
		Out.bEnabled = State.bEnabled;
		Out.bCopyParameterBindingsOnTick = State.bCopyParameterBindingsOnTick;
		Out.bCheckPrerequisitesWhenActivatingChildDirectly = State.bCheckPrerequisitesWhenActivatingChildDirectly;

		if (State.ID.IsValid())
		{
			Ids.Add(Path, State.ID);
		}

		// The color, by name. An empty ref is the tree's default color, which the model does not name.
		if (State.ColorRef.ID.IsValid())
		{
			if (const FStateTreeEditorColor* Color = Data->FindColor(State.ColorRef))
			{
				Out.ColorName = Color->DisplayName;
			}
			else
			{
				Sink.Warning(TEXT("DST8008"), Lang::FDstSpan(),
					FString::Printf(TEXT("State `%s` has a color that is not in the tree's color list; the color was left out."), *Path));
			}
		}

		// Weight: its default is 1, so an exported zero (an empty text) must not be mistaken for "unchanged".
		if (const FProperty* Property = UStateTreeState::StaticClass()->FindPropertyByName(TEXT("Weight")))
		{
			const void* StateData = &State;
			const void* DefaultData = DefaultState;
			if (!Property->Identical_InContainer(StateData, DefaultData, 0, PPF_None))
			{
				const FString Text = DstExportDefault(*Property, StateData);
				Out.WeightText = Text.IsEmpty() ? FString(TEXT("0")) : Text;
			}
		}

		// Like a transition's delay, the rate only counts while it is switched on.
		Out.bCustomTickRate = State.bHasCustomTickRate;
		if (State.bHasCustomTickRate)
		{
			if (const FProperty* Property = UStateTreeState::StaticClass()->FindPropertyByName(TEXT("CustomTickRate")))
			{
				FString Text;
				if (DiffersFromDefault(*Property, &State, DefaultState, Text))
				{
					Out.TickRateText = Text;
				}
			}
		}

		if (State.bHasRequiredEventToEnter)
		{
			Out.Event = MakeEvent(State.RequiredEventToEnter);
		}

		switch (State.Type)
		{
		case EStateTreeStateType::Linked:
			if (State.LinkedSubtree.ID.IsValid())
			{
				Out.LinkTarget = FindStatePath(State.LinkedSubtree.ID, FString::Printf(TEXT("The link of state `%s`"), *Path));
			}
			break;
		case EStateTreeStateType::LinkedAsset:
			if (State.LinkedAsset)
			{
				Out.LinkTarget = State.LinkedAsset->GetPathName();
			}
			break;
		default:
			break;
		}

		if (State.Parameters.ID.IsValid())
		{
			Ids.Add(Path + TEXT("|Parameters"), State.Parameters.ID);
		}
		ExtractParameters(State.Parameters.Parameters, &State, FString::Printf(TEXT("state `%s`"), *Path), Path + TEXT("/Param:"), Out.Parameters);
		Out.bFixedLayout = State.Parameters.bFixedLayout;
		if (State.Type == EStateTreeStateType::Linked || State.Type == EStateTreeStateType::LinkedAsset)
		{
			// Only a link takes its parameters from elsewhere; a binding into any other state's parameters is left for
			// ReportUnconsumedBindings.
			ExtractParameterBindings(State, Path, Out);
		}

		FNodeSite StateSite;
		StateSite.State = &State;
		ExtractNodeList(CollectNodes(State.EnterConditions), EDstNodeRole::EnterCondition, StateSite, /*bExpression*/ true,
			[&Path](const int32 Index) { return MakeStateNodeKey(Path, TEXT("EnterCondition"), Index); }, Out.EnterConditions);
		ExtractNodeList(CollectNodes(State.Considerations), EDstNodeRole::Consideration, StateSite, /*bExpression*/ true,
			[&Path](const int32 Index) { return MakeStateNodeKey(Path, TEXT("Consideration"), Index); }, Out.Considerations);
		ExtractNodeList(CollectTasks(State), EDstNodeRole::Task, StateSite, /*bExpression*/ false,
			[&Path](const int32 Index) { return MakeStateNodeKey(Path, TEXT("Task"), Index); }, Out.Tasks);

		for (int32 TransitionIndex = 0; TransitionIndex < State.Transitions.Num(); ++TransitionIndex)
		{
			FDstTransition& Transition = Out.Transitions.AddDefaulted_GetRef();
			ExtractTransition(State, Path, TransitionIndex, State.Transitions[TransitionIndex], Transition);
		}

		for (const TObjectPtr<UStateTreeState>& Child : State.Children)
		{
			if (Child)
			{
				FDstState& ChildOut = Out.Children.AddDefaulted_GetRef();
				ExtractState(*Child, ChildOut);
			}
		}
	}

	void FExtractor::ExtractTransition(const UStateTreeState& State, const FString& StatePath, const int32 TransitionIndex,
		const FStateTreeTransition& Transition, FDstTransition& Out)
	{
		const FString Key = MakeTransitionKey(StatePath, TransitionIndex);
		if (Transition.ID.IsValid())
		{
			Ids.Add(Key, Transition.ID);
		}

		switch (Transition.Trigger)
		{
		case EStateTreeTransitionTrigger::OnStateCompleted: Out.Trigger = EDstTransitionTrigger::OnStateCompleted; break;
		case EStateTreeTransitionTrigger::OnStateSucceeded: Out.Trigger = EDstTransitionTrigger::OnStateSucceeded; break;
		case EStateTreeTransitionTrigger::OnStateFailed: Out.Trigger = EDstTransitionTrigger::OnStateFailed; break;
		case EStateTreeTransitionTrigger::OnTick: Out.Trigger = EDstTransitionTrigger::OnTick; break;
		case EStateTreeTransitionTrigger::OnEvent: Out.Trigger = EDstTransitionTrigger::OnEvent; break;
		case EStateTreeTransitionTrigger::OnDelegate: Out.Trigger = EDstTransitionTrigger::OnDelegate; break;
		default:
			Sink.Error(TEXT("DST8004"), Lang::FDstSpan(),
				FString::Printf(TEXT("Transition `%s` has the trigger %d, which is not one of the six the model has."), *Key, static_cast<int32>(Transition.Trigger)),
				TEXT("Pick one of the triggers in the editor."));
			break;
		}

		const FStateTreeStateLink& Link = Transition.State;
		switch (Link.LinkType)
		{
		case EStateTreeTransitionType::Succeeded: Out.TargetKind = EDstTransitionTarget::Succeeded; break;
		case EStateTreeTransitionType::Failed: Out.TargetKind = EDstTransitionTarget::Failed; break;
		case EStateTreeTransitionType::Parent: Out.TargetKind = EDstTransitionTarget::Parent; break;
		case EStateTreeTransitionType::NextState: Out.TargetKind = EDstTransitionTarget::NextState; break;
		case EStateTreeTransitionType::NextSelectableState: Out.TargetKind = EDstTransitionTarget::NextSelectableState; break;
		case EStateTreeTransitionType::NextParent: Out.TargetKind = EDstTransitionTarget::NextParent; break;
		case EStateTreeTransitionType::NextSelectableParent: Out.TargetKind = EDstTransitionTarget::NextSelectableParent; break;
		case EStateTreeTransitionType::GotoState:
			Out.TargetKind = EDstTransitionTarget::GotoState;
			if (Link.ID.IsValid())
			{
				Out.TargetState = FindStatePath(Link.ID, FString::Printf(TEXT("Transition `%s`"), *Key));
			}
			else
			{
				Sink.Error(TEXT("DST8009"), Lang::FDstSpan(),
					FString::Printf(TEXT("Transition `%s` goes to a state but has none set."), *Key),
					TEXT("Pick the target state in the editor."));
			}
			break;
		default:
			// None, and the deprecated NotSet.
			Out.TargetKind = EDstTransitionTarget::None;
			break;
		}
		if (Link.Fallback != EStateTreeSelectionFallback::None)
		{
			Sink.Warning(TEXT("DST8008"), Lang::FDstSpan(),
				FString::Printf(TEXT("Transition `%s` has a selection fallback (%s), which the model has no field for; it was left out."),
					*Key, *EnumToName(Link.Fallback)));
		}

		if (Transition.Trigger == EStateTreeTransitionTrigger::OnEvent)
		{
			Out.Event = MakeEvent(Transition.RequiredEvent);
		}

		FNodeSite TransitionSite;
		TransitionSite.State = &State;
		TransitionSite.TransitionIndex = TransitionIndex;
		ExtractDelegate(Transition, Key, TransitionSite, Out);

		Out.Priority = EnumToName(Transition.Priority);

		// The delay only counts while it is switched on; the values left in the fields after switching it off are inert.
		Out.bDelay = Transition.bDelayTransition;
		if (Out.bDelay)
		{
			const FStructOnScope DefaultTransition(FStateTreeTransition::StaticStruct());
			if (const FProperty* Property = FStateTreeTransition::StaticStruct()->FindPropertyByName(TEXT("DelayDuration")))
			{
				FString Text;
				if (DiffersFromDefault(*Property, &Transition, DefaultTransition.GetStructMemory(), Text))
				{
					Out.DelayText = Text;
				}
			}
			if (const FProperty* Property = FStateTreeTransition::StaticStruct()->FindPropertyByName(TEXT("DelayRandomVariance")))
			{
				FString Text;
				if (DiffersFromDefault(*Property, &Transition, DefaultTransition.GetStructMemory(), Text))
				{
					Out.VarianceText = Text;
				}
			}
		}

		switch (Transition.ReactivateTargetState)
		{
		case EStateTreeTransitionChangeTypeRules::ForceChanged:
			Out.bReactivate = true;
			break;
		case EStateTreeTransitionChangeTypeRules::ForceSustained:
			// The model's bReactivate is ForceChanged; the nearest value for this one is the default.
			Sink.Warning(TEXT("DST8004"), Lang::FDstSpan(),
				FString::Printf(TEXT("Transition `%s` forces the target state to stay as it is (ForceSustained), which the model cannot hold; it was read as the default."), *Key),
				TEXT("Only the default and `Reactivate` (ForceChanged) have a text form."));
			break;
		case EStateTreeTransitionChangeTypeRules::Default:
			break;
		}
		Out.bEnabled = Transition.bTransitionEnabled;

		ExtractNodeList(CollectNodes(Transition.Conditions), EDstNodeRole::TransitionCondition, TransitionSite, /*bExpression*/ true,
			[&StatePath, TransitionIndex](const int32 Index) { return MakeConditionKey(StatePath, TransitionIndex, Index); }, Out.Conditions);
	}
}

namespace UE::DreamStateTree::Compiler
{
	bool ExtractDst(const UStateTree& Asset, Model::FDstTree& OutTree, Lang::FDstDiagnosticSink& Sink, FDstExtractResult* OutResult)
	{
		OutTree = Model::FDstTree();

		TMap<FString, FGuid> Ids;
		Extracting::FExtractor Extractor(Asset, Sink);
		const bool bSucceeded = Extractor.Run(OutTree, Ids);

		if (OutResult)
		{
			OutResult->Ids = MoveTemp(Ids);
		}
		return bSucceeded;
	}
}
