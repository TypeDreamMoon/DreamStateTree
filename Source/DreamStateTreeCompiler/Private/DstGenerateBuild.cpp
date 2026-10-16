// Copyright DreamMoon. All Rights Reserved.
//
// Fills the editor data of a StateTree from the model. The order matters, and it is the engine's, not the text's
// (spike-report.md, "决定一"):
//
//   1. reset: the previous content leaves the asset;
//   2. settings, colors, tree parameters, global nodes;
//   3. every state, with its final ID set the moment it exists (links and transitions copy the ID of their target);
//   4. links (SetLinkedState / SetLinkedStateAsset), once every subtree and its parameters exist;
//   5. nodes and transitions of every state, with their constants;
//   6. bindings, last: a binding needs both its ends to exist, and every node bound into gets OnBindingChanged.

#include "DstGenerateInternal.h"

#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "GameplayTagContainer.h"
#include "StateTree.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::GeneratePrivate
{
	namespace
	{
		//--------------------------------------------------------------------------------------------------------------//
		// Model to engine enums
		//--------------------------------------------------------------------------------------------------------------//

		EStateTreeStateType ToEngineStateType(const Model::EDstStateType Type)
		{
			switch (Type)
			{
			case Model::EDstStateType::Group: return EStateTreeStateType::Group;
			case Model::EDstStateType::Linked: return EStateTreeStateType::Linked;
			case Model::EDstStateType::LinkedAsset: return EStateTreeStateType::LinkedAsset;
			case Model::EDstStateType::Subtree: return EStateTreeStateType::Subtree;
			case Model::EDstStateType::State:
			default: return EStateTreeStateType::State;
			}
		}

		EStateTreeTransitionTrigger ToEngineTrigger(const Model::EDstTransitionTrigger Trigger)
		{
			switch (Trigger)
			{
			case Model::EDstTransitionTrigger::OnStateSucceeded: return EStateTreeTransitionTrigger::OnStateSucceeded;
			case Model::EDstTransitionTrigger::OnStateFailed: return EStateTreeTransitionTrigger::OnStateFailed;
			case Model::EDstTransitionTrigger::OnTick: return EStateTreeTransitionTrigger::OnTick;
			case Model::EDstTransitionTrigger::OnEvent: return EStateTreeTransitionTrigger::OnEvent;
			case Model::EDstTransitionTrigger::OnDelegate: return EStateTreeTransitionTrigger::OnDelegate;
			case Model::EDstTransitionTrigger::OnStateCompleted:
			default: return EStateTreeTransitionTrigger::OnStateCompleted;
			}
		}

		EStateTreeTransitionType ToEngineTargetType(const Model::EDstTransitionTarget Target)
		{
			switch (Target)
			{
			case Model::EDstTransitionTarget::Succeeded: return EStateTreeTransitionType::Succeeded;
			case Model::EDstTransitionTarget::Failed: return EStateTreeTransitionType::Failed;
			case Model::EDstTransitionTarget::Parent: return EStateTreeTransitionType::Parent;
			case Model::EDstTransitionTarget::NextState: return EStateTreeTransitionType::NextState;
			case Model::EDstTransitionTarget::NextSelectableState: return EStateTreeTransitionType::NextSelectableState;
			case Model::EDstTransitionTarget::NextParent: return EStateTreeTransitionType::NextParent;
			case Model::EDstTransitionTarget::NextSelectableParent: return EStateTreeTransitionType::NextSelectableParent;
			case Model::EDstTransitionTarget::None: return EStateTreeTransitionType::None;
			case Model::EDstTransitionTarget::GotoState:
			default: return EStateTreeTransitionType::GotoState;
			}
		}

		EStateTreeExpressionOperand ToEngineOperand(const Model::EDstOperand Operand)
		{
			switch (Operand)
			{
			case Model::EDstOperand::And: return EStateTreeExpressionOperand::And;
			case Model::EDstOperand::Or: return EStateTreeExpressionOperand::Or;
			case Model::EDstOperand::Multiply: return EStateTreeExpressionOperand::Multiply;
			case Model::EDstOperand::Copy:
			default: return EStateTreeExpressionOperand::Copy;
			}
		}

		bool ToEngineCompletion(const Model::EDstCompletion Completion, UE::StateTree::ETaskCompletionCondition& Out)
		{
			switch (Completion)
			{
			case Model::EDstCompletion::Succeeds: Out = UE::StateTree::ETaskCompletionCondition::Succeeds; return true;
			case Model::EDstCompletion::Fails: Out = UE::StateTree::ETaskCompletionCondition::Fails; return true;
			case Model::EDstCompletion::Completes: Out = UE::StateTree::ETaskCompletionCondition::Completes; return true;
			case Model::EDstCompletion::None:
			default: return false;
			}
		}

		/** An enum value by its name; the model keeps the engine's names. */
		template <typename EnumType>
		bool ParseEnum(const FString& Name, EnumType& Out)
		{
			const UEnum* Enum = StaticEnum<EnumType>();
			const int64 Value = Enum ? Enum->GetValueByNameString(Name) : INDEX_NONE;
			if (Value == INDEX_NONE)
			{
				return false;
			}
			Out = static_cast<EnumType>(Value);
			return true;
		}

		/** Export text of a float. */
		float ParseFloat(const FString& Text)
		{
			return static_cast<float>(FCString::Atof(*Text));
		}

		/** Whether the project's tag table has the tag; tags are never added by the generator. */
		bool IsKnownTag(const FString& Name)
		{
			return FGameplayTag::RequestGameplayTag(FName(*Name), /*ErrorIfNotFound*/ false).IsValid();
		}

		//--------------------------------------------------------------------------------------------------------------//
		// Trash
		//--------------------------------------------------------------------------------------------------------------//

		/**
		 * Moves an object the asset no longer wants out of its package: left behind it would be saved with the asset, and an
		 * old state keeps listening to compiles. States go with their children and their Blueprint node instances.
		 */
		void TrashState(UStateTreeState& State)
		{
			for (UStateTreeState* Child : State.Children)
			{
				if (Child)
				{
					TrashState(*Child);
				}
			}
			State.Children.Reset();
			State.LinkedAsset = nullptr;
			UE::StateTree::Compiler::RenameObjectToTransientPackage(&State);
		}

		void TrashNodeObjects(TArray<FStateTreeEditorNode>& Nodes)
		{
			for (FStateTreeEditorNode& Node : Nodes)
			{
				if (UObject* InstanceObject = Node.InstanceObject)
				{
					UE::StateTree::Compiler::RenameObjectToTransientPackage(InstanceObject);
				}
				if (UObject* RuntimeObject = Node.ExecutionRuntimeDataObject)
				{
					UE::StateTree::Compiler::RenameObjectToTransientPackage(RuntimeObject);
				}
			}
		}

		//--------------------------------------------------------------------------------------------------------------//
		// The builder
		//--------------------------------------------------------------------------------------------------------------//

		/** Where a model node lives in the editor data. */
		struct FNodeSlot
		{
			UStateTreeState* State = nullptr;
			Model::EDstNodeRole Role = Model::EDstNodeRole::Task;
			/** Transition conditions only: the transition. */
			int32 TransitionIndex = INDEX_NONE;
			/** The index in the array of its role. */
			int32 Index = INDEX_NONE;
			/** A task kept in the state's single-task slot (schemas that allow one task). */
			bool bSingleTask = false;
		};

		struct FStatePair
		{
			const Model::FDstState* Source = nullptr;
			UStateTreeState* State = nullptr;
			FString Path;
		};

		class FBuilder
		{
		public:
			FBuilder(FGenContext& InCtx, UStateTree& InStateTree, UStateTreeEditorData& InData)
				: Ctx(InCtx)
				, StateTree(InStateTree)
				, Data(InData)
			{
			}

			bool Run();

		private:
			// Steps, in order.
			void Reset();
			void BuildSettings();
			void BuildColors();
			void BuildTreeParameters();
			void BuildGlobals();
			void CreateState(const Model::FDstState& Model, const FString& Path, UStateTreeState* Parent);
			void LinkStates();
			void BuildStateContent(const FStatePair& Pair);
			void BuildTransitions(UStateTreeState& State, const Model::FDstState& Model, const FString& Path);
			void BindAll();

			// Parameters
			bool AddParameters(FInstancedPropertyBag& Bag, const TArray<Model::FDstParameter>& Parameters, const FString& StatePath);
			void ApplyLinkParameters(UStateTreeState& State, const Model::FDstState& Model, const FString& Path);
			FInstancedPropertyBag& GetRootBag() const;

			// Nodes
			FStateTreeEditorNode* AddNode(TArray<FStateTreeEditorNode>& Array, UObject* Outer, const Model::FDstNode& Model, FNodeSlot Slot);
			bool InitNode(FStateTreeEditorNode& Editor, UObject* Outer, const Model::FDstNode& Model);
			bool InitBlueprintNode(FStateTreeEditorNode& Editor, UObject* Outer, UClass& Class);
			void ApplyConstants(FStateTreeEditorNode& Editor, UObject* Owner, const Model::FDstNode& Model);
			FStateTreeEditorNode* FindNode(const FString& Key);

			// Bindings
			void BindNode(const Model::FDstNode& Node);
			void BindTransition(UStateTreeState& State, const Model::FDstTransition& Transition, int32 Index, const FString& Key);
			void BindArg(const FString& OwnerKey, const Model::FDstArg& Arg, const FGuid& InstanceId, const FGuid& NodeStructId, bool bNotify);
			void AddFunctionArg(const FString& OwnerKey, const FPropertyBindingPath& Target, const Model::FDstNode& Function, bool bNotify);
			void BindStateParameters(const FStatePair& Pair);
			bool ResolveSource(const Model::FDstSourceRef& Ref, const FString& PlaceKey, FPropertyBindingPath& OutPath);
			void NotifyBindingChanged(const FString& NodeKey, const FPropertyBindingPath& Source, const FPropertyBindingPath& Target);

			// Small things
			bool ResolveTag(const FString& Name, const FString& PlaceKey, FGameplayTag& OutTag);
			const UScriptStruct* ResolvePayload(const FString& Path, const FString& PlaceKey);
			void SetRootParametersGuid(const FGuid& Id) const;

			FGenContext& Ctx;
			UStateTree& StateTree;
			UStateTreeEditorData& Data;

			/** Whether the schema lets a state hold more than one task; if not, the one task goes to SingleTask. */
			bool bAllowMultipleTasks = true;

			TMap<FString, UStateTreeState*> States;
			TArray<FStatePair> StatePairs;
			/** Node key -> the ID of its instance data. */
			TMap<FString, FGuid> NodeIds;
			TMap<FString, FNodeSlot> Slots;
			/** Color name -> the ID of its reference. */
			TMap<FString, FGuid> ColorRefs;
		};

		//--------------------------------------------------------------------------------------------------------------//
		// Run and reset
		//--------------------------------------------------------------------------------------------------------------//

		bool FBuilder::Run()
		{
			const int32 ErrorsBefore = Ctx.NumErrors;
			bAllowMultipleTasks = Data.Schema ? Data.Schema->AllowMultipleTasks() : true;

			Reset();
			BuildSettings();
			BuildColors();
			BuildTreeParameters();
			BuildGlobals();

			for (const Model::FDstState& State : Ctx.Tree.States)
			{
				CreateState(State, State.Name, nullptr);
			}
			LinkStates();
			for (const FStatePair& Pair : StatePairs)
			{
				BuildStateContent(Pair);
			}
			BindAll();

			// The parameter layouts and the bindings are final: let the editor data catch up with them.
			Data.UpdateBindings();
			UStateTreeEditingSubsystem::MarkAsPubliclyModified(&StateTree);
			return Ctx.NumErrors == ErrorsBefore;
		}

		FInstancedPropertyBag& FBuilder::GetRootBag() const
		{
			// The editor data only offers its root parameters read-only; the engine's own tests add to them the same way.
			return const_cast<FInstancedPropertyBag&>(Data.GetRootParametersPropertyBag());
		}

		void FBuilder::SetRootParametersGuid(const FGuid& Id) const
		{
			// A private property that PostInitProperties fills with a random GUID; nothing binds to it yet.
			if (const FStructProperty* GuidProperty = CastField<FStructProperty>(Data.GetClass()->FindPropertyByName(TEXT("RootParametersGuid"))))
			{
				if (GuidProperty->Struct == TBaseStructure<FGuid>::Get())
				{
					*GuidProperty->ContainerPtrToValuePtr<FGuid>(&Data) = Id;
				}
			}
		}

		void FBuilder::Reset()
		{
			for (UStateTreeState* Old : Data.SubTrees)
			{
				if (Old)
				{
					TrashState(*Old);
				}
			}
			Data.SubTrees.Reset();

			TrashNodeObjects(Data.Evaluators);
			Data.Evaluators.Reset();
			TrashNodeObjects(Data.GlobalTasks);
			Data.GlobalTasks.Reset();

			Data.EditorBindings.RemoveBindings([](FPropertyBindingBinding&) { return true; });
			Data.CompiledDispatchers.Reset();
			Data.Colors.Reset();
			GetRootBag().Reset();
		}

		//--------------------------------------------------------------------------------------------------------------//
		// Settings, colors, tree parameters, globals
		//--------------------------------------------------------------------------------------------------------------//

		void FBuilder::BuildSettings()
		{
			// A setting the text leaves out goes back to its default, or a rebuild would keep the previous build's.
			if (const UStateTreeEditorData* Defaults = Data.GetClass()->GetDefaultObject<UStateTreeEditorData>())
			{
				Data.GlobalTasksCompletion = Defaults->GlobalTasksCompletion;
			}

			TArray<FString> Keys;
			Ctx.Tree.Settings.GetKeys(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				const FProperty* Property = Data.GetClass()->FindPropertyByName(FName(*Key));
				if (!Property)
				{
					Ctx.Dst7003(FString(), FString::Printf(TEXT("The editor data has no setting `%s`."), *Key));
					continue;
				}
				FString Error;
				if (!ImportPropertyText(*Property, &Data, &Data, Ctx.Tree.Settings[Key], Error))
				{
					Ctx.Dst7003(FString(), Error);
				}
			}
		}

		void FBuilder::BuildColors()
		{
			const FProperty* ColorProperty = FStateTreeEditorColor::StaticStruct()->FindPropertyByName(TEXT("Color"));
			bool bDefaultGiven = false;

			for (const Model::FDstColor& Color : Ctx.Tree.Colors)
			{
				FStateTreeEditorColor Entry;
				Entry.DisplayName = Color.Name;

				// The color a state without one refers to has no ID; a color the model calls by that name takes its place.
				if (Color.Name.Equals(DefaultColorName, ESearchCase::CaseSensitive))
				{
					Entry.ColorRef = FStateTreeEditorColorRef();
					bDefaultGiven = true;
				}
				else
				{
					const FString IdKey = TEXT("Color:") + Color.Name;
					Entry.ColorRef = FStateTreeEditorColorRef(Ctx.Assign(IdKey, IdKey, nullptr, FString()));
				}
				ColorRefs.Add(Color.Name, Entry.ColorRef.ID);

				FString Error;
				if (!ColorProperty || !ImportPropertyText(*ColorProperty, &Entry, &Data, Color.ColorText, Error))
				{
					Ctx.Dst7003(FString(), Error.IsEmpty() ? FString::Printf(TEXT("The color `%s` cannot be set."), *Color.Name) : Error);
				}
				Data.Colors.Add(Entry);
			}

			if (!bDefaultGiven)
			{
				// What the editor data starts with, and what a state without a color shows.
				FStateTreeEditorColor DefaultColor;
				DefaultColor.ColorRef = FStateTreeEditorColorRef();
				DefaultColor.DisplayName = DefaultColorName;
				DefaultColor.Color = FLinearColor(FColor(31, 151, 167));
				Data.Colors.Add(DefaultColor);
			}
		}

		void FBuilder::BuildTreeParameters()
		{
			SetRootParametersGuid(Ctx.Assign(TEXT("Parameters"), TEXT("Parameters"), nullptr, FString()));
			AddParameters(GetRootBag(), Ctx.Tree.Parameters, FString());
		}

		bool FBuilder::AddParameters(FInstancedPropertyBag& Bag, const TArray<Model::FDstParameter>& Parameters, const FString& StatePath)
		{
			if (Parameters.IsEmpty())
			{
				return true;
			}

			bool bOk = true;
			TArray<FPropertyBagPropertyDesc> Descs;
			TArray<const Model::FDstParameter*> Added;
			for (const Model::FDstParameter& Parameter : Parameters)
			{
				// A state's parameters are placed at the state, the tree's at their own line.
				const FString IdKey = StatePath.IsEmpty() ? TEXT("Param:") + Parameter.Name : StatePath + TEXT("/Param:") + Parameter.Name;
				const FString PlaceKey = StatePath.IsEmpty() ? IdKey : StatePath;

				FParsedParameterType Parsed;
				FString Error;
				if (!ParseParameterType(Parameter.Type, Parsed, Error))
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("Parameter `%s`: %s"), *Parameter.Name, *Error));
					bOk = false;
					continue;
				}

				FPropertyBagPropertyDesc& Desc = Descs.Emplace_GetRef(FName(*Parameter.Name), Parsed.Containers, Parsed.ValueType, Parsed.TypeObject);
				Desc.ID = Ctx.Assign(IdKey, IdKey, nullptr, PlaceKey);
				TArray<FString> MetaKeys;
				Parameter.Meta.GetKeys(MetaKeys);
				MetaKeys.Sort();
				for (const FString& MetaKey : MetaKeys)
				{
					Desc.MetaData.Emplace(FName(*MetaKey), Parameter.Meta[MetaKey]);
				}
				Added.Add(&Parameter);
			}
			if (Descs.IsEmpty())
			{
				return false;
			}

			// One call for all of them: every call builds a new property bag struct.
			const EPropertyBagAlterationResult Altered = Bag.AddProperties(Descs);
			if (Altered != EPropertyBagAlterationResult::Success)
			{
				Ctx.Dst7003(StatePath.IsEmpty() ? FString(TEXT("Param:") + Parameters[0].Name) : StatePath,
					FString::Printf(TEXT("The engine's property bag refused the parameters (%s); check the names."), *UEnum::GetValueAsString(Altered)));
				return false;
			}

			uint8* Memory = Bag.GetMutableValue().GetMemory();
			for (const Model::FDstParameter* Parameter : Added)
			{
				if (Parameter->ValueText.IsEmpty())
				{
					continue;
				}
				const FString PlaceKey = StatePath.IsEmpty() ? TEXT("Param:") + Parameter->Name : StatePath;
				const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(FName(*Parameter->Name));
				FString Error;
				if (!Desc || !Desc->CachedProperty || !Memory)
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("Parameter `%s` has no storage in the property bag."), *Parameter->Name));
					bOk = false;
				}
				else if (!ImportPropertyText(*Desc->CachedProperty, Memory, nullptr, Parameter->ValueText, Error))
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("Parameter `%s`: %s"), *Parameter->Name, *Error));
					bOk = false;
				}
			}
			return bOk;
		}

		void FBuilder::BuildGlobals()
		{
			for (const Model::FDstNode& Node : Ctx.Tree.Globals)
			{
				const bool bEvaluator = Node.Role == Model::EDstNodeRole::Evaluator;
				if (!bEvaluator && Node.Role != Model::EDstNodeRole::GlobalTask)
				{
					Ctx.Dst7003(Node.Key, FString::Printf(TEXT("`%s` is a %s; only evaluators and global tasks are global."), *Node.Key, Model::LexToString(Node.Role)));
					continue;
				}
				FNodeSlot Slot;
				Slot.Role = Node.Role;
				AddNode(bEvaluator ? Data.Evaluators : Data.GlobalTasks, &Data, Node, Slot);
				if (!Node.BindName.IsEmpty())
				{
					Ctx.AltPlaceKeys.Add(Node.Key, TEXT("Global:") + Node.BindName);
				}
			}
		}

		//--------------------------------------------------------------------------------------------------------------//
		// States
		//--------------------------------------------------------------------------------------------------------------//

		bool FBuilder::ResolveTag(const FString& Name, const FString& PlaceKey, FGameplayTag& OutTag)
		{
			OutTag = FGameplayTag::RequestGameplayTag(FName(*Name), /*ErrorIfNotFound*/ false);
			if (!OutTag.IsValid())
			{
				Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("The gameplay tag `%s` is not in the project's tag table."), *Name),
					TEXT("Tags are never added automatically; add it to the project's tag settings."));
				return false;
			}
			return true;
		}

		const UScriptStruct* FBuilder::ResolvePayload(const FString& Path, const FString& PlaceKey)
		{
			if (Path.IsEmpty())
			{
				return nullptr;
			}
			const UScriptStruct* Struct = FindStructByPath(Path);
			if (!Struct)
			{
				Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("The event payload type `%s` does not exist."), *Path));
			}
			return Struct;
		}

		void FBuilder::CreateState(const Model::FDstState& Model, const FString& Path, UStateTreeState* Parent)
		{
			const FName Name(*Model.Name);
			const EStateTreeStateType Type = ToEngineStateType(Model.Type);
			UStateTreeState& State = Parent ? Parent->AddChildState(Name, Type) : Data.AddSubTree(Name, Type);

			// Links and transitions made from here on copy these IDs, so they are final before anything else happens.
			State.ID = Ctx.Assign(Path, Path, nullptr, Path);
			State.Parameters.ID = Ctx.Assign(Path + TEXT("|Parameters"), Path, TEXT("Parameters"), Path);
			Ctx.MapId(State.GetEventID(), Path);

			States.Add(Path, &State);
			FStatePair& Pair = StatePairs.AddDefaulted_GetRef();
			Pair.Source = &Model;
			Pair.State = &State;
			Pair.Path = Path;

			State.Description = Model.Description;
			if (!Model.Tag.IsEmpty())
			{
				ResolveTag(Model.Tag, Path, State.Tag);
			}
			if (!Model.ColorName.IsEmpty())
			{
				if (const FGuid* ColorId = ColorRefs.Find(Model.ColorName))
				{
					State.ColorRef = FStateTreeEditorColorRef(*ColorId);
				}
				else
				{
					Ctx.Dst7003(Path, FString::Printf(TEXT("The color `%s` is not in the Colors section."), *Model.ColorName));
				}
			}
			if (!ParseEnum(Model.Selection, State.SelectionBehavior))
			{
				Ctx.Dst7003(Path, FString::Printf(TEXT("`%s` is not a state selection behavior."), *Model.Selection));
			}
			if (!ParseEnum(Model.TasksCompletion, State.TasksCompletion))
			{
				Ctx.Dst7003(Path, FString::Printf(TEXT("`%s` is not a tasks completion type."), *Model.TasksCompletion));
			}
			State.bEnabled = Model.bEnabled;
			if (!Model.WeightText.IsEmpty())
			{
				State.Weight = ParseFloat(Model.WeightText);
			}
			State.bHasCustomTickRate = Model.bCustomTickRate;
			if (!Model.TickRateText.IsEmpty())
			{
				State.CustomTickRate = ParseFloat(Model.TickRateText);
			}
			State.bCopyParameterBindingsOnTick = Model.bCopyParameterBindingsOnTick;
			State.bCheckPrerequisitesWhenActivatingChildDirectly = Model.bCheckPrerequisitesWhenActivatingChildDirectly;

			if (Model.Event.IsSet())
			{
				const Model::FDstEvent& Event = Model.Event.GetValue();
				State.bHasRequiredEventToEnter = true;
				ResolveTag(Event.Tag, Path, State.RequiredEventToEnter.Tag);
				State.RequiredEventToEnter.PayloadStruct = ResolvePayload(Event.PayloadType, Path);
				State.RequiredEventToEnter.bConsumeEventOnSelect = Event.bConsumeOnSelect;
			}

			State.Parameters.bFixedLayout = Model.bFixedLayout;
			// A linked state's parameters come from its target (LinkStates); everyone else declares them.
			if (Model.Type != Model::EDstStateType::Linked && Model.Type != Model::EDstStateType::LinkedAsset)
			{
				AddParameters(State.Parameters.Parameters, Model.Parameters, Path);
			}

			for (const Model::FDstState& Child : Model.Children)
			{
				CreateState(Child, Path + TEXT("/") + Child.Name, &State);
			}
		}

		void FBuilder::LinkStates()
		{
			for (const FStatePair& Pair : StatePairs)
			{
				const Model::FDstState& Model = *Pair.Source;
				UStateTreeState& State = *Pair.State;

				if (Model.Type == Model::EDstStateType::Linked)
				{
					UStateTreeState* const* Target = States.Find(Model.LinkTarget);
					if (!Target || (*Target)->Type != EStateTreeStateType::Subtree)
					{
						Ctx.Dst7003(Pair.Path, FString::Printf(TEXT("`%s` links to `%s`, which is not a subtree of this tree."), *Pair.Path, *Model.LinkTarget));
						continue;
					}
					// What the editor does when a link is chosen: fixes the layout, copies the target's parameters, and selects Enter.
					State.SetLinkedState((*Target)->GetLinkToState());
					ApplyLinkParameters(State, Model, Pair.Path);
				}
				else if (Model.Type == Model::EDstStateType::LinkedAsset)
				{
					UStateTree* Asset = Cast<UStateTree>(FindObject<UObject>(nullptr, *Model.LinkTarget));
					if (!Asset)
					{
						Asset = LoadObject<UStateTree>(nullptr, *Model.LinkTarget, nullptr, LOAD_NoWarn | LOAD_Quiet);
					}
					if (!Asset || Asset == &StateTree)
					{
						Ctx.Dst7003(Pair.Path, FString::Printf(TEXT("`%s` links to the StateTree asset `%s`, which %s."), *Pair.Path, *Model.LinkTarget,
							Asset ? TEXT("is this asset itself") : TEXT("does not exist")));
						continue;
					}
					State.SetLinkedStateAsset(Asset);
					ApplyLinkParameters(State, Model, Pair.Path);
				}
			}
		}

		void FBuilder::ApplyLinkParameters(UStateTreeState& State, const Model::FDstState& Model, const FString& Path)
		{
			FInstancedPropertyBag& Bag = State.Parameters.Parameters;

			// The IDs of the parameters are the target's, not derived here; the result records what the asset holds.
			if (const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct())
			{
				for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
				{
					Ctx.Result.Ids.Add(Path + TEXT("/Param:") + Desc.Name.ToString(), Desc.ID);
					Ctx.MapId(Desc.ID, Path);
				}
			}

			for (const Model::FDstParameter& Parameter : Model.Parameters)
			{
				if (!Parameter.bOverridden)
				{
					continue;
				}
				const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(FName(*Parameter.Name));
				if (!Desc)
				{
					Ctx.Dst7003(Path, FString::Printf(TEXT("The link target has no parameter `%s` to override."), *Parameter.Name));
					continue;
				}
				State.SetParametersPropertyOverridden(Desc->ID, true);
				uint8* Memory = Bag.GetMutableValue().GetMemory();
				FString Error;
				if (!Parameter.ValueText.IsEmpty() && Desc->CachedProperty && Memory
					&& !ImportPropertyText(*Desc->CachedProperty, Memory, nullptr, Parameter.ValueText, Error))
				{
					Ctx.Dst7003(Path, FString::Printf(TEXT("Parameter `%s`: %s"), *Parameter.Name, *Error));
				}
			}
		}

		//--------------------------------------------------------------------------------------------------------------//
		// Nodes
		//--------------------------------------------------------------------------------------------------------------//

		bool FBuilder::InitBlueprintNode(FStateTreeEditorNode& Editor, UObject* Outer, UClass& Class)
		{
			// The wrapper struct names the Blueprint class; the instance data is an object of that class, outered to the state.
			if (Class.IsChildOf(UStateTreeTaskBlueprintBase::StaticClass()))
			{
				FStateTreeBlueprintTaskWrapper Wrapper;
				Wrapper.TaskClass = &Class;
				Editor.InitializeAs<FStateTreeBlueprintTaskWrapper>(Outer, MoveTemp(Wrapper));
			}
			else if (Class.IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass()))
			{
				FStateTreeBlueprintEvaluatorWrapper Wrapper;
				Wrapper.EvaluatorClass = &Class;
				Editor.InitializeAs<FStateTreeBlueprintEvaluatorWrapper>(Outer, MoveTemp(Wrapper));
			}
			else if (Class.IsChildOf(UStateTreeConditionBlueprintBase::StaticClass()))
			{
				FStateTreeBlueprintConditionWrapper Wrapper;
				Wrapper.ConditionClass = &Class;
				Editor.InitializeAs<FStateTreeBlueprintConditionWrapper>(Outer, MoveTemp(Wrapper));
			}
			else if (Class.IsChildOf(UStateTreeConsiderationBlueprintBase::StaticClass()))
			{
				FStateTreeBlueprintConsiderationWrapper Wrapper;
				Wrapper.ConsiderationClass = &Class;
				Editor.InitializeAs<FStateTreeBlueprintConsiderationWrapper>(Outer, MoveTemp(Wrapper));
			}
			else
			{
				return false;
			}
			return true;
		}

		bool FBuilder::InitNode(FStateTreeEditorNode& Editor, UObject* Outer, const Model::FDstNode& Model)
		{
			if (Model.Key.IsEmpty())
			{
				Ctx.Dst7003(FString(), TEXT("A node of the model has no key."));
				return false;
			}
			if (NodeIds.Contains(Model.Key))
			{
				Ctx.Dst7003(Model.Key, FString::Printf(TEXT("Two nodes have the key `%s`."), *Model.Key));
				return false;
			}

			FResolvedNodeType Type;
			FString Error;
			if (!ResolveNodeType(Model, Type, Error))
			{
				Ctx.Dst7003(Model.Key, Error);
				return false;
			}

			if (Type.BlueprintClass)
			{
				if (!InitBlueprintNode(Editor, Outer, *Type.BlueprintClass))
				{
					Ctx.Dst7003(Model.Key, FString::Printf(TEXT("`%s` is not a class a StateTree node can be made from."), *Model.BlueprintClass));
					return false;
				}
			}
			else
			{
				Editor.InitializeAs(Outer, Type.Struct);
				// What loading does to every node (UStateTreeEditorData::PostLoad): bring its data up to the current
				// version. A node's constructor can leave a version field at zero, which the next load would read as old
				// data to upgrade. Before the constants, so an upgrade cannot overwrite them.
				if (FStateTreeNodeBase* NodeBase = Editor.Node.GetMutablePtr<FStateTreeNodeBase>())
				{
					NodeBase->PostLoad(Editor.GetInstance());
				}
			}

			// The ID, right away: nothing else may see the random one.
			Editor.ID = Ctx.Assign(Model.Key, Model.Key, nullptr, Model.Key);
			Ctx.MapId(Editor.GetNodeID(), Model.Key);
			NodeIds.Add(Model.Key, Editor.ID);

			switch (Model.Role)
			{
			case Model::EDstNodeRole::EnterCondition:
			case Model::EDstNodeRole::Consideration:
			case Model::EDstNodeRole::TransitionCondition:
				Editor.ExpressionOperand = ToEngineOperand(Model.Operand);
				// Written as the model has it; the compiler clamps to its own limit when it reads the editor data.
				Editor.ExpressionIndent = static_cast<uint8>(FMath::Clamp(Model.Indent, 0, 255));
				break;
			default:
				break;
			}

			ApplyConstants(Editor, Outer, Model);
			if (!Model.BindName.IsEmpty())
			{
				Editor.SetNodeName(FName(*Model.BindName));
			}
			return true;
		}

		FStateTreeEditorNode* FBuilder::AddNode(TArray<FStateTreeEditorNode>& Array, UObject* Outer, const Model::FDstNode& Model, FNodeSlot Slot)
		{
			FStateTreeEditorNode& Editor = Array.AddDefaulted_GetRef();
			Slot.Index = Array.Num() - 1;
			if (!InitNode(Editor, Outer, Model))
			{
				Array.Pop();
				return nullptr;
			}
			Slots.Add(Model.Key, Slot);
			return &Editor;
		}

		void FBuilder::ApplyConstants(FStateTreeEditorNode& Editor, UObject* Owner, const Model::FDstNode& Model)
		{
			for (const Model::FDstArg& Arg : Model.Args)
			{
				if (Arg.Kind != Model::EDstValueKind::Constant)
				{
					continue;
				}

				const UStruct* Struct = nullptr;
				void* Memory = nullptr;
				UObject* ImportOwner = Owner;
				if (Arg.Side == Model::EDstPropertySide::Node)
				{
					Struct = Editor.Node.GetScriptStruct();
					Memory = Editor.Node.GetMutableMemory();
				}
				else
				{
					// Instance data is a struct, or an object of the Blueprint node's class.
					const FStateTreeDataView Instance = Editor.GetInstance();
					if (Instance.IsValid())
					{
						Struct = Instance.GetStruct();
						Memory = Instance.GetMutableMemory();
						if (Editor.InstanceObject)
						{
							ImportOwner = Editor.InstanceObject;
						}
					}
				}
				if (!Struct || !Memory)
				{
					Ctx.Dst7003(Model.Key, FString::Printf(TEXT("`%s` has no %s data to set `%s` on."), *Model.Key,
						Arg.Side == Model::EDstPropertySide::Node ? TEXT("node") : TEXT("instance"), *Arg.Property.ToString()));
					continue;
				}

				const FProperty* Property = nullptr;
				void* Container = nullptr;
				FString Error;
				if (!ResolveConstantTarget(Struct, Memory, Arg.Property, Property, Container, Error)
					|| !ImportPropertyText(*Property, Container, ImportOwner, Arg.ConstantText, Error))
				{
					Ctx.Dst7003(Model.Key, Error);
				}
			}
		}

		FStateTreeEditorNode* FBuilder::FindNode(const FString& Key)
		{
			const FNodeSlot* Slot = Slots.Find(Key);
			if (!Slot)
			{
				return nullptr;
			}

			switch (Slot->Role)
			{
			case Model::EDstNodeRole::Evaluator:
				return Data.Evaluators.IsValidIndex(Slot->Index) ? &Data.Evaluators[Slot->Index] : nullptr;
			case Model::EDstNodeRole::GlobalTask:
				return Data.GlobalTasks.IsValidIndex(Slot->Index) ? &Data.GlobalTasks[Slot->Index] : nullptr;
			case Model::EDstNodeRole::EnterCondition:
				return Slot->State && Slot->State->EnterConditions.IsValidIndex(Slot->Index) ? &Slot->State->EnterConditions[Slot->Index] : nullptr;
			case Model::EDstNodeRole::Consideration:
				return Slot->State && Slot->State->Considerations.IsValidIndex(Slot->Index) ? &Slot->State->Considerations[Slot->Index] : nullptr;
			case Model::EDstNodeRole::Task:
				if (!Slot->State)
				{
					return nullptr;
				}
				if (Slot->bSingleTask)
				{
					return &Slot->State->SingleTask;
				}
				return Slot->State->Tasks.IsValidIndex(Slot->Index) ? &Slot->State->Tasks[Slot->Index] : nullptr;
			case Model::EDstNodeRole::TransitionCondition:
				if (Slot->State && Slot->State->Transitions.IsValidIndex(Slot->TransitionIndex))
				{
					TArray<FStateTreeEditorNode>& Conditions = Slot->State->Transitions[Slot->TransitionIndex].Conditions;
					return Conditions.IsValidIndex(Slot->Index) ? &Conditions[Slot->Index] : nullptr;
				}
				return nullptr;
			default:
				return nullptr;
			}
		}

		//--------------------------------------------------------------------------------------------------------------//
		// Contents of a state
		//--------------------------------------------------------------------------------------------------------------//

		void FBuilder::BuildStateContent(const FStatePair& Pair)
		{
			const Model::FDstState& Model = *Pair.Source;
			UStateTreeState& State = *Pair.State;

			for (const Model::FDstNode& Node : Model.EnterConditions)
			{
				FNodeSlot Slot;
				Slot.State = &State;
				Slot.Role = Model::EDstNodeRole::EnterCondition;
				AddNode(State.EnterConditions, &State, Node, Slot);
			}
			for (const Model::FDstNode& Node : Model.Considerations)
			{
				FNodeSlot Slot;
				Slot.State = &State;
				Slot.Role = Model::EDstNodeRole::Consideration;
				AddNode(State.Considerations, &State, Node, Slot);
			}

			if (bAllowMultipleTasks)
			{
				for (const Model::FDstNode& Node : Model.Tasks)
				{
					FNodeSlot Slot;
					Slot.State = &State;
					Slot.Role = Model::EDstNodeRole::Task;
					AddNode(State.Tasks, &State, Node, Slot);
				}
			}
			else if (!Model.Tasks.IsEmpty())
			{
				// The schema allows one task per state; the engine keeps it in SingleTask, and names it after the state.
				if (Model.Tasks.Num() > 1)
				{
					Ctx.Dst7003(Model.Tasks[1].Key, FString::Printf(TEXT("The schema allows one task per state; `%s` has %d."), *Pair.Path, Model.Tasks.Num()));
				}
				const Model::FDstNode& Node = Model.Tasks[0];
				if (InitNode(State.SingleTask, &State, Node))
				{
					State.SingleTask.SetNodeName(State.Name);
					FNodeSlot Slot;
					Slot.State = &State;
					Slot.Role = Model::EDstNodeRole::Task;
					Slot.bSingleTask = true;
					Slots.Add(Node.Key, Slot);
				}
			}

			BuildTransitions(State, Model, Pair.Path);
		}

		void FBuilder::BuildTransitions(UStateTreeState& State, const Model::FDstState& Model, const FString& Path)
		{
			for (int32 Index = 0; Index < Model.Transitions.Num(); ++Index)
			{
				const Model::FDstTransition& Source = Model.Transitions[Index];
				const FString Key = FString::Printf(TEXT("%s#%d"), *Path, Index);

				const EStateTreeTransitionTrigger Trigger = ToEngineTrigger(Source.Trigger);
				EStateTreeTransitionType Type = ToEngineTargetType(Source.TargetKind);
				const UStateTreeState* Target = nullptr;
				if (Source.TargetKind == Model::EDstTransitionTarget::GotoState)
				{
					UStateTreeState* const* Found = States.Find(Source.TargetState);
					if (Found)
					{
						Target = *Found;
					}
					else
					{
						Ctx.Dst7003(Key, FString::Printf(TEXT("The transition goes to `%s`, which is not a state of this tree."), *Source.TargetState));
						Type = EStateTreeTransitionType::None;
					}
				}

				FGameplayTag EventTag;
				if (Source.Trigger == Model::EDstTransitionTrigger::OnEvent)
				{
					ResolveTag(Source.Event.Tag, Key, EventTag);
				}

				// AddTransition gives the transition its default trigger and target the way the editor's array add does.
				FStateTreeTransition& Transition = Source.Trigger == Model::EDstTransitionTrigger::OnEvent
					? State.AddTransition(Trigger, EventTag, Type, Target)
					: State.AddTransition(Trigger, Type, Target);
				Transition.ID = Ctx.Assign(Key, Key, nullptr, Key);
				Ctx.MapId(Transition.GetEventID(), Key);

				if (Source.Trigger == Model::EDstTransitionTrigger::OnEvent)
				{
					Transition.RequiredEvent.PayloadStruct = ResolvePayload(Source.Event.PayloadType, Key);
					Transition.RequiredEvent.bConsumeEventOnSelect = Source.Event.bConsumeOnSelect;
				}
				if (!ParseEnum(Source.Priority, Transition.Priority))
				{
					Ctx.Dst7003(Key, FString::Printf(TEXT("`%s` is not a transition priority."), *Source.Priority));
				}
				Transition.bDelayTransition = Source.bDelay;
				if (!Source.DelayText.IsEmpty())
				{
					Transition.DelayDuration = ParseFloat(Source.DelayText);
				}
				if (!Source.VarianceText.IsEmpty())
				{
					Transition.DelayRandomVariance = ParseFloat(Source.VarianceText);
				}
				Transition.ReactivateTargetState = Source.bReactivate ? EStateTreeTransitionChangeTypeRules::ForceChanged : EStateTreeTransitionChangeTypeRules::Default;
				Transition.bTransitionEnabled = Source.bEnabled;

				for (const Model::FDstNode& Condition : Source.Conditions)
				{
					FNodeSlot Slot;
					Slot.State = &State;
					Slot.Role = Model::EDstNodeRole::TransitionCondition;
					Slot.TransitionIndex = Index;
					AddNode(Transition.Conditions, &State, Condition, Slot);
				}
			}
		}

		//--------------------------------------------------------------------------------------------------------------//
		// Bindings
		//--------------------------------------------------------------------------------------------------------------//

		void FBuilder::BindAll()
		{
			for (const Model::FDstNode& Node : Ctx.Tree.Globals)
			{
				BindNode(Node);
			}
			for (const FStatePair& Pair : StatePairs)
			{
				const Model::FDstState& Model = *Pair.Source;
				BindStateParameters(Pair);
				for (const Model::FDstNode& Node : Model.EnterConditions)
				{
					BindNode(Node);
				}
				for (const Model::FDstNode& Node : Model.Considerations)
				{
					BindNode(Node);
				}
				for (const Model::FDstNode& Node : Model.Tasks)
				{
					BindNode(Node);
				}
				for (int32 Index = 0; Index < Model.Transitions.Num(); ++Index)
				{
					const FString Key = FString::Printf(TEXT("%s#%d"), *Pair.Path, Index);
					BindTransition(*Pair.State, Model.Transitions[Index], Index, Key);
					for (const Model::FDstNode& Condition : Model.Transitions[Index].Conditions)
					{
						BindNode(Condition);
					}
				}
			}
		}

		void FBuilder::BindStateParameters(const FStatePair& Pair)
		{
			const Model::FDstState& Model = *Pair.Source;
			if (Model.ParameterBindings.IsEmpty())
			{
				return;
			}
			if (Model.Type != Model::EDstStateType::Linked && Model.Type != Model::EDstStateType::LinkedAsset)
			{
				Ctx.Dst7003(Pair.Path, FString::Printf(TEXT("`%s` binds parameters, but only a linked state takes its parameters from elsewhere."), *Pair.Path));
				return;
			}

			UStateTreeState& State = *Pair.State;
			for (const Model::FDstArg& Arg : Model.ParameterBindings)
			{
				if (Arg.Kind == Model::EDstValueKind::Constant)
				{
					continue;
				}
				if (Arg.Property.Segments.IsEmpty())
				{
					Ctx.Dst7003(Pair.Path, TEXT("A parameter binding names no parameter."));
					continue;
				}

				// A parameter that is fed is an overridden one: switching the override off removes the binding again.
				const FPropertyBagPropertyDesc* Desc = State.Parameters.Parameters.FindPropertyDescByName(FName(*Arg.Property.Segments[0].Name));
				if (!Desc)
				{
					Ctx.Dst7003(Pair.Path, FString::Printf(TEXT("The link target has no parameter `%s` to feed."), *Arg.Property.Segments[0].Name));
					continue;
				}
				State.SetParametersPropertyOverridden(Desc->ID, true);

				// The target is the state's parameters struct; there is no node to tell about the binding.
				BindArg(Pair.Path, Arg, State.Parameters.ID, State.Parameters.ID, /*bNotify*/ false);
			}
		}

		void FBuilder::BindNode(const Model::FDstNode& Node)
		{
			const FStateTreeEditorNode* Editor = FindNode(Node.Key);
			if (!Editor)
			{
				return;
			}
			// Only the IDs are kept: every binding added below may move things around.
			const FGuid InstanceId = Editor->GetInstanceDataID();
			const FGuid NodeStructId = Editor->GetNodeID();
			for (const Model::FDstArg& Arg : Node.Args)
			{
				BindArg(Node.Key, Arg, InstanceId, NodeStructId, /*bNotify*/ true);
			}
		}

		void FBuilder::BindTransition(UStateTreeState& State, const Model::FDstTransition& Source, const int32 Index, const FString& Key)
		{
			if (Source.Trigger != Model::EDstTransitionTrigger::OnDelegate || !State.Transitions.IsValidIndex(Index))
			{
				return;
			}

			const FStateTreeTransition& Transition = State.Transitions[Index];
			const FPropertyBindingPath Target(Transition.ID, FName(TEXT("DelegateListener")));

			UE::StateTree::ETaskCompletionCondition Completion = UE::StateTree::ETaskCompletionCondition::Completes;
			if (ToEngineCompletion(Source.Completion, Completion))
			{
				// A task's completion: the compiler makes the dispatcher. The source is the task's node struct.
				const FStateTreeEditorNode* Task = FindNode(Source.Delegate.Owner);
				if (!Task)
				{
					Ctx.Dst7003(Key, FString::Printf(TEXT("The transition listens to the completion of `%s`, which is not a task of this tree."), *Source.Delegate.Owner));
					return;
				}
				Data.EditorBindings.AddTaskCompletionBinding(Task->GetNodeID(), Target, Completion);
			}
			else if (Source.Delegate.IsSet())
			{
				FPropertyBindingPath SourcePath;
				if (ResolveSource(Source.Delegate, Key, SourcePath))
				{
					Data.AddPropertyBinding(SourcePath, Target);
				}
			}
			else
			{
				Ctx.Dst7003(Key, TEXT("A transition on a delegate names no delegate to listen to."));
			}
		}

		bool FBuilder::ResolveSource(const Model::FDstSourceRef& Ref, const FString& PlaceKey, FPropertyBindingPath& OutPath)
		{
			FGuid Id;
			switch (Ref.Kind)
			{
			case Model::EDstSourceKind::Parameters:
				Id = Data.GetRootParametersGuid();
				break;

			case Model::EDstSourceKind::Context:
				if (Data.Schema)
				{
					for (const FStateTreeExternalDataDesc& Desc : Data.Schema->GetContextDataDescs())
					{
						if (Desc.Name.ToString().Equals(Ref.Owner))
						{
							Id = Desc.ID;
							break;
						}
					}
				}
				if (!Id.IsValid())
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("The schema has no context `%s`."), *Ref.Owner));
					return false;
				}
				break;

			case Model::EDstSourceKind::Global:
			case Model::EDstSourceKind::Node:
				if (const FGuid* NodeId = NodeIds.Find(Ref.Owner))
				{
					Id = *NodeId;
				}
				else
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("A binding reads from `%s`, which is not a node of this tree."), *Ref.Owner));
					return false;
				}
				break;

			case Model::EDstSourceKind::StateParameters:
			case Model::EDstSourceKind::StateEvent:
			{
				UStateTreeState* const* Found = States.Find(Ref.Owner);
				if (!Found)
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("A binding reads from the state `%s`, which is not a state of this tree."), *Ref.Owner));
					return false;
				}
				Id = Ref.Kind == Model::EDstSourceKind::StateParameters ? (*Found)->Parameters.ID : (*Found)->GetEventID();
				break;
			}

			case Model::EDstSourceKind::TransitionEvent:
			{
				int32 Hash = INDEX_NONE;
				UStateTreeState* const* Found = Ref.Owner.FindLastChar(TEXT('#'), Hash) ? States.Find(Ref.Owner.Left(Hash)) : nullptr;
				const int32 TransitionIndex = Hash != INDEX_NONE ? FCString::Atoi(*Ref.Owner.Mid(Hash + 1)) : INDEX_NONE;
				if (!Found || !(*Found)->Transitions.IsValidIndex(TransitionIndex))
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("A binding reads from the event of `%s`, which is not a transition of this tree."), *Ref.Owner));
					return false;
				}
				Id = (*Found)->Transitions[TransitionIndex].GetEventID();
				break;
			}

			case Model::EDstSourceKind::None:
			default:
				Ctx.Dst7003(PlaceKey, TEXT("A binding has no source."));
				return false;
			}

			OutPath = MakeBindingPath(Id, Ref.Path);
			return true;
		}

		void FBuilder::NotifyBindingChanged(const FString& NodeKey, const FPropertyBindingPath& Source, const FPropertyBindingPath& Target)
		{
			// Nodes adapt their instance data to what is bound into them (Enum Compare takes its enum type from the binding).
			// The editor does this when a binding is made in the Details panel; code that binds has to do it itself, or the
			// tree compiles and runs wrong.
			FStateTreeEditorNode* Editor = FindNode(NodeKey);
			if (!Editor)
			{
				return;
			}
			FStateTreeNodeBase* Node = Editor->GetNode().GetPtr<FStateTreeNodeBase>();
			const FStateTreeDataView Instance = Editor->GetInstance();
			if (Node && Instance.IsValid())
			{
				const FStateTreeBindingLookup BindingLookup(&Data);
				Node->OnBindingChanged(Editor->ID, Instance, Source, Target, BindingLookup);
			}
		}

		void FBuilder::BindArg(const FString& OwnerKey, const Model::FDstArg& Arg, const FGuid& InstanceId, const FGuid& NodeStructId, const bool bNotify)
		{
			if (Arg.Kind == Model::EDstValueKind::Constant)
			{
				return;
			}

			const FGuid TargetId = Arg.Side == Model::EDstPropertySide::Node ? NodeStructId : InstanceId;
			const FPropertyBindingPath Target = MakeBindingPath(TargetId, Arg.Property);
			const bool bNotifyNode = bNotify && Arg.Side == Model::EDstPropertySide::Instance;

			switch (Arg.Kind)
			{
			case Model::EDstValueKind::Binding:
			case Model::EDstValueKind::Ref:
			{
				// A property reference is bound like any other property; only its text spelling differs.
				FPropertyBindingPath Source;
				if (ResolveSource(Arg.Source, OwnerKey, Source))
				{
					Data.AddPropertyBinding(Source, Target);
					if (bNotifyNode)
					{
						NotifyBindingChanged(OwnerKey, Source, Target);
					}
				}
				break;
			}

			case Model::EDstValueKind::Output:
			{
				// The node's property is copied into the source: the binding's source is the destination.
				FPropertyBindingPath Destination;
				if (ResolveSource(Arg.Source, OwnerKey, Destination))
				{
					Data.EditorBindings.AddOutputBinding(Destination, Target);
					if (bNotifyNode)
					{
						NotifyBindingChanged(OwnerKey, Destination, Target);
					}
				}
				break;
			}

			case Model::EDstValueKind::Function:
				if (Arg.Function.IsValid())
				{
					AddFunctionArg(OwnerKey, Target, *Arg.Function, bNotifyNode);
				}
				else
				{
					Ctx.Dst7003(OwnerKey, FString::Printf(TEXT("`%s` is fed by a property function the model does not hold."), *Arg.Property.ToString()));
				}
				break;

			default:
				break;
			}
		}

		void FBuilder::AddFunctionArg(const FString& OwnerKey, const FPropertyBindingPath& Target, const Model::FDstNode& Function, const bool bNotify)
		{
			FResolvedNodeType Type;
			FString Error;
			if (!ResolveNodeType(Function, Type, Error))
			{
				Ctx.Dst7003(Function.Key, Error);
				return;
			}
			if (!Type.Struct->IsChildOf(FStateTreePropertyFunctionBase::StaticStruct()))
			{
				Ctx.Dst7003(Function.Key, FString::Printf(TEXT("`%s` is not a property function."), *Function.StructType));
				return;
			}
			if (NodeIds.Contains(Function.Key))
			{
				Ctx.Dst7003(Function.Key, FString::Printf(TEXT("Two nodes have the key `%s`."), *Function.Key));
				return;
			}

			// The function's result property is in its instance data, whose type only an instance of the node can tell.
			FInstancedStruct Probe;
			Probe.InitializeAs(Type.Struct);
			const UStruct* InstanceType = Probe.Get<FStateTreeNodeBase>().GetInstanceDataType();
			const FProperty* Output = InstanceType ? FindFunctionOutputProperty(*InstanceType) : nullptr;
			if (!Output)
			{
				Ctx.Dst7003(Function.Key, FString::Printf(TEXT("`%s` has no result property (Result, bResult, Output, or the only Output one)."), *Function.StructType));
				return;
			}

			// The engine creates the function node inside the binding; its ID is random until it is replaced below.
			const FPropertyBindingPathSegment OutputSegment(Output->GetFName());
			Data.EditorBindings.AddFunctionBinding(Type.Struct, MakeArrayView(&OutputSegment, 1), Target);

			TArrayView<FStateTreePropertyPathBinding> Bindings = Data.EditorBindings.GetMutableBindings();
			FStateTreeEditorNode* FunctionNode = Bindings.IsEmpty() ? nullptr : Bindings.Last().GetMutablePropertyFunctionNode().GetPtr<FStateTreeEditorNode>();
			if (!FunctionNode)
			{
				Ctx.Dst7003(Function.Key, TEXT("The engine did not create the property function node."));
				return;
			}
			FStateTreePropertyPathBinding& Binding = Bindings.Last();

			FunctionNode->ID = Ctx.Assign(Function.Key, Function.Key, nullptr, Function.Key);
			Ctx.MapId(FunctionNode->GetNodeID(), Function.Key);
			// The binding's source is the function's instance data, addressed by the node ID.
			Binding.GetMutableSourcePath().SetStructID(FunctionNode->GetInstanceDataID());
			NodeIds.Add(Function.Key, FunctionNode->GetInstanceDataID());
			if (!Function.BindName.IsEmpty())
			{
				FunctionNode->SetNodeName(FName(*Function.BindName));
			}
			ApplyConstants(*FunctionNode, &Data, Function);

			// Adding bindings below can move this one; keep what is needed.
			const FGuid InstanceId = FunctionNode->GetInstanceDataID();
			const FGuid NodeStructId = FunctionNode->GetNodeID();
			const FPropertyBindingPath SourcePath = Binding.GetSourcePath();
			if (bNotify)
			{
				NotifyBindingChanged(OwnerKey, SourcePath, Target);
			}

			for (const Model::FDstArg& Arg : Function.Args)
			{
				BindArg(Function.Key, Arg, InstanceId, NodeStructId, /*bNotify*/ false);
			}
		}
	}

	//------------------------------------------------------------------------------------------------------------------//
	// Checking the model before the asset is touched
	//------------------------------------------------------------------------------------------------------------------//

	namespace
	{
		class FModelValidator
		{
		public:
			explicit FModelValidator(FGenContext& InCtx)
				: Ctx(InCtx)
			{
			}

			void Run()
			{
				CollectStates(Ctx.Tree.States, FString());
				for (const Model::FDstNode& Node : Ctx.Tree.Globals)
				{
					CheckNode(Node);
				}
				CheckParameters(Ctx.Tree.Parameters, FString());
				CheckStates(Ctx.Tree.States, FString());
			}

		private:
			void CollectStates(const TArray<Model::FDstState>& States, const FString& ParentPath)
			{
				for (const Model::FDstState& State : States)
				{
					const FString Path = ParentPath.IsEmpty() ? State.Name : ParentPath + TEXT("/") + State.Name;
					if (StatePaths.Contains(Path))
					{
						Ctx.Dst7003(Path, FString::Printf(TEXT("Two states have the path `%s`."), *Path));
					}
					StatePaths.Add(Path);
					CollectStates(State.Children, Path);
				}
			}

			void CheckStates(const TArray<Model::FDstState>& States, const FString& ParentPath)
			{
				for (const Model::FDstState& State : States)
				{
					const FString Path = ParentPath.IsEmpty() ? State.Name : ParentPath + TEXT("/") + State.Name;
					CheckState(State, Path);
					CheckStates(State.Children, Path);
				}
			}

			void CheckState(const Model::FDstState& State, const FString& Path)
			{
				if (State.Name.IsEmpty())
				{
					Ctx.Dst7003(Path, TEXT("A state has no name."));
				}
				if (!State.Tag.IsEmpty() && !IsKnownTag(State.Tag))
				{
					Ctx.Dst7003(Path, FString::Printf(TEXT("The gameplay tag `%s` is not in the project's tag table."), *State.Tag));
				}
				if (State.Event.IsSet())
				{
					CheckEvent(State.Event.GetValue(), Path);
				}

				if (State.Type == Model::EDstStateType::Linked)
				{
					if (!StatePaths.Contains(State.LinkTarget))
					{
						Ctx.Dst7003(Path, FString::Printf(TEXT("`%s` links to `%s`, which is not a state of this tree."), *Path, *State.LinkTarget));
					}
				}
				else if (State.Type == Model::EDstStateType::LinkedAsset)
				{
					if (State.LinkTarget.IsEmpty())
					{
						Ctx.Dst7003(Path, FString::Printf(TEXT("`%s` links to no StateTree asset."), *Path));
					}
				}
				else
				{
					CheckParameters(State.Parameters, Path);
				}

				for (const Model::FDstArg& Arg : State.ParameterBindings)
				{
					if (Arg.Kind == Model::EDstValueKind::Function && Arg.Function.IsValid())
					{
						CheckNode(*Arg.Function);
					}
				}
				for (const Model::FDstNode& Node : State.EnterConditions)
				{
					CheckNode(Node);
				}
				for (const Model::FDstNode& Node : State.Considerations)
				{
					CheckNode(Node);
				}
				for (const Model::FDstNode& Node : State.Tasks)
				{
					CheckNode(Node);
				}

				for (int32 Index = 0; Index < State.Transitions.Num(); ++Index)
				{
					const Model::FDstTransition& Transition = State.Transitions[Index];
					const FString Key = FString::Printf(TEXT("%s#%d"), *Path, Index);
					if (Transition.TargetKind == Model::EDstTransitionTarget::GotoState && !StatePaths.Contains(Transition.TargetState))
					{
						Ctx.Dst7003(Key, FString::Printf(TEXT("The transition goes to `%s`, which is not a state of this tree."), *Transition.TargetState));
					}
					if (Transition.Trigger == Model::EDstTransitionTrigger::OnEvent)
					{
						CheckEvent(Transition.Event, Key);
					}
					for (const Model::FDstNode& Node : Transition.Conditions)
					{
						CheckNode(Node);
					}
				}
			}

			void CheckEvent(const Model::FDstEvent& Event, const FString& PlaceKey)
			{
				if (!IsKnownTag(Event.Tag))
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("The gameplay tag `%s` is not in the project's tag table."), *Event.Tag),
						TEXT("Tags are never added automatically; add it to the project's tag settings."));
				}
				if (!Event.PayloadType.IsEmpty() && !FindStructByPath(Event.PayloadType))
				{
					Ctx.Dst7003(PlaceKey, FString::Printf(TEXT("The event payload type `%s` does not exist."), *Event.PayloadType));
				}
			}

			void CheckParameters(const TArray<Model::FDstParameter>& Parameters, const FString& StatePath)
			{
				for (const Model::FDstParameter& Parameter : Parameters)
				{
					FParsedParameterType Parsed;
					FString Error;
					if (!ParseParameterType(Parameter.Type, Parsed, Error))
					{
						Ctx.Dst7003(StatePath.IsEmpty() ? TEXT("Param:") + Parameter.Name : StatePath, FString::Printf(TEXT("Parameter `%s`: %s"), *Parameter.Name, *Error));
					}
				}
			}

			void CheckNode(const Model::FDstNode& Node)
			{
				if (Node.Key.IsEmpty())
				{
					Ctx.Dst7003(FString(), TEXT("A node of the model has no key."));
					return;
				}
				if (NodeKeys.Contains(Node.Key))
				{
					Ctx.Dst7003(Node.Key, FString::Printf(TEXT("Two nodes have the key `%s`."), *Node.Key));
				}
				NodeKeys.Add(Node.Key);

				FResolvedNodeType Type;
				FString Error;
				if (!ResolveNodeType(Node, Type, Error))
				{
					Ctx.Dst7003(Node.Key, Error);
				}

				// Nodes that feed a property of this one.
				for (const Model::FDstArg& Arg : Node.Args)
				{
					if (Arg.Kind == Model::EDstValueKind::Function && Arg.Function.IsValid())
					{
						CheckNode(*Arg.Function);
					}
				}
			}

			FGenContext& Ctx;
			TSet<FString> StatePaths;
			TSet<FString> NodeKeys;
		};
	}

	bool ValidateModel(FGenContext& Ctx)
	{
		const int32 ErrorsBefore = Ctx.NumErrors;
		if (Ctx.Tree.States.IsEmpty())
		{
			Ctx.Dst7003(FString(), TEXT("The tree has no states; a StateTree needs at least its root."));
			return false;
		}
		FModelValidator Validator(Ctx);
		Validator.Run();
		return Ctx.NumErrors == ErrorsBefore;
	}

	//------------------------------------------------------------------------------------------------------------------//
	// Public to the generator
	//------------------------------------------------------------------------------------------------------------------//

	bool BuildEditorData(FGenContext& Ctx, UStateTree& StateTree, UStateTreeEditorData& EditorData)
	{
		FBuilder Builder(Ctx, StateTree, EditorData);
		return Builder.Run();
	}
}
