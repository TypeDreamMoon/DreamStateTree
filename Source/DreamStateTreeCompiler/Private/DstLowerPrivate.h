// Copyright DreamMoon. All Rights Reserved.
//
// What the files of the semantic stage (DstLower*.cpp) share: the lowerer class, the scopes bindings are resolved in,
// the small value types they pass around, and the helpers that need nothing but reflection.
//
// Reading order of the files:
//   DstLower.cpp           LowerDst, FDstPlaces, EnsureDstBindNames
//   DstLowerDocument.cpp   header, schema, sections (Settings, Parameters, Globals, Tags, Colors), libraries
//   DstLowerTypes.cpp      names -> engine types, parameter types and property bags, property classification
//   DstLowerBinding.cpp    what a path means: roots, visibility, the property it ends in, type compatibility
//   DstLowerNode.cpp       node calls: type, schema, arguments, settings, Input/Output/Context checks
//   DstLowerValue.cpp      one argument value: constant, binding, ref, output, arithmetic, property function call
//   DstLowerCondition.cpp  condition and consideration lists: operands, indents, comparisons
//   DstLowerState.cpp      states, clauses, transitions, links, subtrees
//   DstLowerCheck.cpp      checks over the finished model

#pragma once

#include "CoreMinimal.h"
#include "DstLower.h"
#include "DstNames.h"
#include "DstSugar.h"
#include "DstValues.h"
#include "GameplayTagContainer.h"
#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Model/DstModel.h"
#include "StateTreeSchema.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"

class FProperty;
class UClass;
class UDreamStateTreeExtension;
class UEnum;
class UScriptStruct;
class UStruct;
struct FPropertyBagPropertyDesc;

namespace UE::DreamStateTree::Compiler::Lowering
{
	// ------------------------------------------------------------------------------------------
	// Diagnostics
	// ------------------------------------------------------------------------------------------

	/**
	 * Forwards to the run's sink. The only thing it adds is the file a diagnostic belongs to: inside a library's
	 * subtree the span refers to the library, not to the main file. The call shape stays `Sink.Error(TEXT("DSTnnnn"), ...)`
	 * so that every code can be found by searching for it.
	 */
	class FLowerSink
	{
	public:
		explicit FLowerSink(Lang::FDstDiagnosticSink& InReal) : Real(InReal) {}

		bool Error(const TCHAR* Code, const Lang::FDstSpan& Span, FString Message, FString Hint = FString());
		void Warning(const TCHAR* Code, const Lang::FDstSpan& Span, FString Message, FString Hint = FString());

		/** Empty for the main file; a library's path while one of its subtrees is lowered. */
		FString CurrentFile;
		Lang::FDstDiagnosticSink& Real;
	};

	/** Parentheses the author wrote are plain grouping. */
	inline const Lang::FDstExpr& StripParens(const Lang::FDstExpr& Expr)
	{
		const Lang::FDstExpr* Current = &Expr;
		while (const Lang::FDstParenExpr* Paren = Current->As<Lang::FDstParenExpr>())
		{
			if (!Paren->Inner)
			{
				break;
			}
			Current = Paren->Inner.Get();
		}
		return *Current;
	}

	// ------------------------------------------------------------------------------------------
	// Types of properties
	// ------------------------------------------------------------------------------------------

	/** What kind of value a property holds, for choosing Compare nodes and property functions. */
	enum class EValueDomain : uint8
	{
		Other,
		Bool,
		/** int32, int64, uint32, a byte that is not an enum. */
		Int,
		/** float, double. */
		Float,
		Name,
		String,
		Enum,
		Object,
		Struct,
	};

	EValueDomain GetValueDomain(const FProperty* Property);
	/** `float`, `Vector`, `Actor`, `EMode`: for messages. */
	FString DescribeProperty(const FProperty* Property);
	/** The enum an enum-like property (FEnumProperty, FByteProperty with an enum) holds; null for anything else. */
	const UEnum* GetPropertyEnum(const FProperty* Property);

	/** Category Input / Output / Parameter / Context of an instance data property. */
	enum class EPropertyUse : uint8 { Parameter, Input, Output, Context };
	EPropertyUse GetPropertyUse(const FProperty& Property);
	bool IsPropertyRefProperty(const FProperty& Property);
	bool IsDelegateDispatcherProperty(const FProperty* Property);
	bool IsDelegateListenerProperty(const FProperty* Property);
	bool IsAnyEnumProperty(const FProperty* Property);
	bool IsGameplayTagProperty(const FProperty* Property);
	/** A property with meta Optional: an Input that may stay unbound. */
	bool IsOptionalProperty(const FProperty& Property);

	/** Closest names to Wanted among Candidates, best first, at most Max; case-insensitive edit distance. */
	TArray<FString> FindSimilarNames(const FString& Wanted, const TArray<FString>& Candidates, int32 Max = 3);
	/** `Did you mean `A`, `B`?` or an empty string. */
	FString MakeSuggestionHint(const FString& Wanted, const TArray<FString>& Candidates);

	// ------------------------------------------------------------------------------------------
	// Names -> engine types (parameter types, payload types)
	// ------------------------------------------------------------------------------------------

	const UScriptStruct* ResolveStructByName(const FString& Written);
	const UEnum* ResolveEnumByName(const FString& Written);
	UClass* ResolveClassByName(const FString& Written);

	/**
	 * A parameter type as written (`float`, `Vector`, `Actor`, `Class<Actor>`, `Array<int>`) -> the canonical string of
	 * FDstParameter::Type. False with OutError set when a name resolves to nothing.
	 */
	bool ParseParameterType(const FString& Written, FString& OutCanonical, FString& OutError);

	/** The canonical type string of a property bag description (the inverse of ParseParameterType). */
	FString BagDescToCanonicalType(const FPropertyBagPropertyDesc& Desc);

	/** The parameters of a tree, a subtree or a linked asset with the property bag that gives them real properties. */
	struct FParamSet
	{
		TArray<Model::FDstParameter> Params;
		TStrongObjectPtr<UPropertyBag> Bag;

		/** Rebuilds Bag from Params' types. False with OutError when a canonical type does not resolve. */
		bool Build(FString& OutError);
		const FProperty* FindProperty(const FString& Name) const;
		const Model::FDstParameter* Find(const FString& Name) const;
		/** A container holding the zero value of every parameter; null before Build. */
		const void* GetDefaultMemory() const;
		TArray<FString> GetNames() const;

	private:
		TSharedPtr<FStructOnScope> Defaults;
	};

	/** One declared parameter, from a `Parameters` entry or a subtree's signature. */
	struct FParamDecl
	{
		FString TypeName;
		Lang::FDstSpan TypeSpan;
		FString Name;
		Lang::FDstSpan NameSpan;
		const Lang::FDstExpr* Default = nullptr;
		const Lang::FDstMeta* Meta = nullptr;
		Lang::FDstSpan Span;
	};

	// ------------------------------------------------------------------------------------------
	// Nodes
	// ------------------------------------------------------------------------------------------

	/** A node type with everything the argument checks need, built once per type. */
	struct FNodeInfo
	{
		const FDstNodeType* Type = nullptr;
		const UScriptStruct* NodeStruct = nullptr;
		/** The instance data: a UScriptStruct for C++ nodes, the generated UClass for Blueprint nodes. Null for a node without data. */
		const UStruct* InstanceStruct = nullptr;
		bool bBlueprint = false;
		TSharedPtr<FStructOnScope> NodeDefaults;
		TSharedPtr<FStructOnScope> InstanceDefaults;
		const UObject* InstanceCDO = nullptr;

		/** The container holding the defaults of an instance data property. */
		const void* GetInstanceDefaults() const;
		const void* GetNodeDefaults() const;
	};
	using FNodeInfoPtr = TSharedPtr<FNodeInfo>;

	/** A node type's short name for EnsureDstBindNames and messages: `Delay`, `STT_Wander`. */
	FString GetNodeTypeShortName(const FDstNodeType& Type);

	// ------------------------------------------------------------------------------------------
	// Scopes
	// ------------------------------------------------------------------------------------------

	/** A node others can bind to by its name. */
	struct FBindableNode
	{
		FString Name;
		FString Key;
		Model::EDstSourceKind Kind = Model::EDstSourceKind::Node;
		const UStruct* InstanceStruct = nullptr;
		bool bIsTask = false;
		/** The node has been lowered, so its instance struct is known. */
		bool bLowered = false;
	};

	/** The event a state or a transition requires. */
	struct FEventInfo
	{
		bool bValid = false;
		Model::EDstSourceKind Kind = Model::EDstSourceKind::StateEvent;
		/** The state path, or `<state path>#<transition index>`. */
		FString Owner;
		FGameplayTag Tag;
		const UScriptStruct* Payload = nullptr;
	};

	/** One state of the chain the lowering is inside of. */
	struct FStateFrame
	{
		FString Name;
		FString Path;
		bool bSubtreeRoot = false;
		FEventInfo Event;
		/** The named tasks lowered so far, in order. */
		TArray<FBindableNode> Tasks;
		/** Subtree parameters (the root of a subtree only). */
		const FParamSet* Params = nullptr;
		/** `ConsumeEvent` from the state's settings, applied when its `on` is lowered. */
		TOptional<bool> PendingConsumeEvent;
	};

	/** What a binding target is, which decides what it can read (the engine's own accessibility rules). */
	enum class EBindTarget : uint8
	{
		Global,
		EnterCondition,
		Consideration,
		Task,
		TransitionCondition,
		/** An argument of `link`: the linked state's parameters; sees neither the state's own event nor its tasks. */
		LinkParameter,
		/** The `Delegate(...)` of a transition: sees every task of the state. */
		TransitionDelegate,
	};

	struct FBindScope
	{
		EBindTarget Target = EBindTarget::Task;
		/** Global targets: how many globals come before the node; the others see all of them. */
		int32 GlobalLimit = MAX_int32;
		/** The event of the transition whose conditions these are, when it is an event transition. */
		const FEventInfo* TransitionEvent = nullptr;

		bool AcceptsTaskOutputs() const { return Target == EBindTarget::Task || Target == EBindTarget::TransitionDelegate; }
	};

	/** Where a path starts. */
	enum class EBindOrigin : uint8 { TreeParameter, StateParameter, Context, Global, Task, StateEvent, TransitionEvent };

	/** What a binding path resolved to. */
	struct FResolvedSource
	{
		Model::FDstSourceRef Ref;
		/** The property the path ends in; null when it names a whole root (a context object). */
		const FProperty* Leaf = nullptr;
		/** The type of the whole root, when Leaf is null. */
		const UStruct* RootStruct = nullptr;
		/** The first property of the path (the parameter, the node's output...), for the access rules. */
		const FProperty* RootProperty = nullptr;
		EBindOrigin Origin = EBindOrigin::TreeParameter;
		const FBindableNode* Node = nullptr;
		/** `Params.Speed`, for messages. */
		FString Display;
	};

	/** A constant the folding understands. */
	struct FFoldedConstant
	{
		enum class EKind : uint8 { None, Number, Bool };
		EKind Kind = EKind::None;
		double Number = 0.0;
		bool bBool = false;
		bool bWhole = false;
	};

	/** What an argument's value is, deciding how it is lowered. */
	enum class EValueClass : uint8
	{
		Constant,
		/** A path whose first name is a binding root. */
		Binding,
		Ref,
		/** Arithmetic or logic with something that is not constant in it. */
		Operation,
		/** A call: a property function (or an asset path, which is a constant). */
		Call,
		/** A tuple or a list with a binding or an operation inside, or a reference to a tree with a parameter fed from elsewhere. */
		Mixed,
	};

	/** A node being built: what the argument lowering needs to write into. */
	struct FNodeBuild
	{
		Model::FDstNode* Node = nullptr;
		FNodeInfoPtr Info;
		FBindScope Scope;
		/** Top-level properties given so far, as `I:Name` / `N:Name`. */
		TArray<FString> Given;
		/** The span of the whole call, for diagnostics about what is missing. */
		Lang::FDstSpan CallSpan;
		FString DisplayName;
		/** The properties are a property bag's (a linked state's parameters): no Category applies to them. */
		bool bBagProperties = false;
		bool bInputsMayBeConstant = false;
	};

	// ------------------------------------------------------------------------------------------
	// Condition lists
	// ------------------------------------------------------------------------------------------

	struct FCondGroup;

	/** One operand of a group: a leaf expression or a nested group. */
	struct FCondOperand
	{
		const Lang::FDstExpr* Leaf = nullptr;
		TUniquePtr<FCondGroup> Group;
	};

	/** Operands joined from left to right; Ops[i] joins Operands[i] and Operands[i + 1]. */
	struct FCondGroup
	{
		TArray<FCondOperand> Operands;
		TArray<Model::EDstOperand> Ops;
	};

	/** A leaf with the operand and indent the engine's list wants for it. */
	struct FCondItem
	{
		const Lang::FDstExpr* Leaf = nullptr;
		Model::EDstOperand Operand = Model::EDstOperand::Copy;
		int32 Indent = 0;
	};

	// ------------------------------------------------------------------------------------------
	// The lowerer
	// ------------------------------------------------------------------------------------------

	/** Everything one LowerDst call knows. The member functions are spread over the DstLower*.cpp files. */
	class FDstLowerer
	{
	public:
		FDstLowerer(const Lang::FDstCompilationUnit& InUnit, FDstLowerResult& InOut, Lang::FDstDiagnosticSink& InSink, const FDstLowerOptions& InOptions);

		bool Run();

		// ---- DstLowerDocument.cpp
		bool ReadHeader(const Lang::FDstDocument& Doc);
		void ReadSchemaArgument(const Lang::FDstArgument& Argument);
		void BuildSchemaObject();
		const FProperty* FindContextProperty(const FString& ContextName) const;
		void CollectTagPrefixes();
		void CheckLibraries();
		void LowerSections(const Lang::FDstDocument& Doc);
		void LowerSettingsSection(const Lang::FDstSectionStmt& Section);
		void LowerParametersSection(const Lang::FDstSectionStmt& Section);
		void LowerColorsSection(const Lang::FDstSectionStmt& Section);
		void PreScanGlobals(const Lang::FDstSectionStmt& Section);
		void LowerGlobalsSection();
		void ReadParameterMeta(const Lang::FDstMeta* Meta, TMap<FString, FString>& OutMeta);
		/** Lowers a parameter list (a section's, or a subtree's signature) into OutSet. */
		bool LowerParameterList(const TArray<FParamDecl>& Decls, FParamSet& OutSet, const FString& PlaceKeyPrefix);
		const Lang::FDstSourceText* GetCurrentSource() const;
		FString SourceOf(const Lang::FDstSpan& Span) const;
		void AddPlace(const FString& Key, const Lang::FDstSpan& Span);
		FString DescribeExpr(const Lang::FDstExpr& Expr) const;
		FString PathOf(const FString& ParentPath, const FString& Name) const;

		// ---- DstLowerTypes.cpp
		FNodeInfoPtr GetNodeInfo(const FDstNodeType& Type);
		FNodeInfoPtr GetNodeInfoForStruct(const UScriptStruct* Struct);

		// ---- DstLowerBinding.cpp
		/** Whether the first name of a path is something a path may start with. */
		bool IsBindingRoot(const FString& Name, const TArray<FString>& Segments) const;
		bool IsBindingPath(const Lang::FDstExpr& Expr, const FBindScope& Scope) const;
		/**
		 * A global node or a named task by name. Null when the name is none, or when it is one that cannot be read here
		 * (then bOutKnown is set, and an error has been reported when bReport).
		 */
		const FBindableNode* FindNodeRoot(const FString& Name, const FBindScope& Scope, const Lang::FDstSpan& Span, bool bReport, bool* bOutKnown = nullptr);
		bool WalkPath(const FProperty* Start, const TArray<FString>& Segments, int32 FirstIndex, const Lang::FDstSpan& Span,
			const FString& OwnerDisplay, Model::FDstPropertyPath& InOutPath, const FProperty*& OutLeaf);
		bool ResolveInStruct(const UStruct* Root, const TArray<FString>& Segments, int32 FirstIndex, const Lang::FDstSpan& Span,
			const FString& OwnerDisplay, FResolvedSource& OutSource);
		bool ResolveEvent(const Lang::FDstExpr& Expr, const TArray<FString>& Segments, int32 EventIndex, const FBindScope& Scope,
			const FStateFrame* ExplicitFrame, FResolvedSource& OutSource);
		bool ResolvePath(const Lang::FDstExpr& Expr, const FBindScope& Scope, FResolvedSource& OutSource);
		bool CheckBindable(const FResolvedSource& Source, const FProperty& Target, const Lang::FDstSpan& Span, const FString& TargetDisplay);
		bool ResolveDelegateSource(const Lang::FDstExpr& Expr, const FBindScope& Scope, FResolvedSource& OutSource, Model::EDstCompletion& OutCompletion);

		// ---- DstLowerNode.cpp
		/**
		 * Resolves the callee of a node call to a node type of the expected category (or the second one). A name that exists
		 * only in other categories sets bOutWrongKind when it is given, else it is DST4021.
		 */
		bool ResolveNodeType(const Lang::FDstExpr& Callee, EDstNodeCategory Expected, EDstNodeCategory AlsoExpected, const Lang::FDstSpan& Span,
			const FDstNodeType*& OutType, bool* bOutWrongKind = nullptr);
		bool CheckSchemaAllowsNode(const FDstNodeType& Type, const Lang::FDstSpan& Span);
		bool MakeSugarNode(const TCHAR* StructPath, const FString& Key, Model::EDstNodeRole Role, const Lang::FDstSpan& Span,
			Model::FDstNode& OutNode, FNodeInfoPtr& OutInfo, const FDstNodeType*& OutType);
		bool LowerNode(const Lang::FDstExpr& Expr, const FString& BindName, const Lang::FDstSpan& BindSpan, const FString& Key,
			Model::EDstNodeRole Role, EDstNodeCategory Expected, const FBindScope& Scope, Model::FDstNode& OutNode, FNodeInfoPtr* OutInfo = nullptr,
			const Lang::FDstMeta* ExtraSettings = nullptr);
		bool LowerNodeArguments(FNodeBuild& Build, const TArray<Lang::FDstArgumentPtr>& Arguments);
		bool LowerNodeSettings(FNodeBuild& Build, const Lang::FDstMeta* Meta, const Lang::FDstSpan& Fallback);
		bool FindArgumentProperty(const FNodeInfo& Info, const FString& Name, Model::EDstPropertySide& OutSide, const FProperty*& OutProperty) const;
		void CheckUnboundProperties(FNodeBuild& Build);
		void FinishNode(FNodeBuild& Build);
		void MarkGiven(FNodeBuild& Build, Model::EDstPropertySide Side, const FProperty& Property);
		bool WasGiven(const FNodeBuild& Build, Model::EDstPropertySide Side, const FProperty& Property) const;
		static void SortArgs(Model::FDstNode& Node);

		// ---- DstLowerValue.cpp
		bool LowerValue(FNodeBuild& Build, Model::EDstPropertySide Side, const FProperty& Property, const Lang::FDstExpr& Value);
		bool LowerOutputArgument(FNodeBuild& Build, const Lang::FDstArgument& Argument);
		bool LowerConstant(FNodeBuild& Build, Model::EDstPropertySide Side, const FProperty& Property, const Lang::FDstExpr& Value);
		/** A value partly constant and partly fed from elsewhere: the fed parts become arguments, the constant part the property's constant. */
		bool LowerMixed(FNodeBuild& Build, Model::EDstPropertySide Side, const FProperty& Property, const Lang::FDstExpr& Value);
		/**
		 * One value whose property is reached by Path from the side's root. The parts fed from elsewhere are added to Build.Node as
		 * arguments at their own paths; OutConstant gets the rest as an expression of constants (a tuple, a list, a reference
		 * with the fed parts left at their defaults). Null in OutConstant when the whole value is fed. False after an error.
		 */
		bool SplitValue(FNodeBuild& Build, Model::EDstPropertySide Side, const Model::FDstPropertyPath& Path, const FProperty& Property,
			const Lang::FDstExpr& Value, const FString& Display, TUniquePtr<Lang::FDstExpr>& OutConstant);
		bool SplitTuple(FNodeBuild& Build, Model::EDstPropertySide Side, const Model::FDstPropertyPath& Path, const FProperty& Property,
			const Lang::FDstTupleExpr& Tuple, const FString& Display, TUniquePtr<Lang::FDstExpr>& OutConstant);
		bool SplitList(FNodeBuild& Build, Model::EDstPropertySide Side, const Model::FDstPropertyPath& Path, const FProperty& Property,
			const Lang::FDstListExpr& List, const FString& Display, TUniquePtr<Lang::FDstExpr>& OutConstant);
		bool SplitReference(FNodeBuild& Build, Model::EDstPropertySide Side, const Model::FDstPropertyPath& Path, const Lang::FDstExpr& Value,
			const FString& Display, TUniquePtr<Lang::FDstExpr>& OutConstant);
		/**
		 * Whether Value names a tree for a reference property: `ST_Shoot`, `Path(...)`, either called with parameters, or `None`.
		 * A path that starts at a binding root is a binding of the whole reference, not that.
		 */
		bool TryGetReferenceParts(const Lang::FDstExpr& Value, const FBindScope& Scope, const Lang::FDstExpr*& OutAsset,
			const TArray<Lang::FDstArgumentPtr>*& OutArguments) const;
		bool LowerRef(FNodeBuild& Build, Model::EDstPropertySide Side, const FProperty& Property, const Lang::FDstRefExpr& Ref);
		bool LowerBinding(FNodeBuild& Build, Model::EDstPropertySide Side, const FProperty& Property, const Lang::FDstExpr& Value);
		bool LowerOperation(FNodeBuild& Build, Model::EDstPropertySide Side, const Model::FDstPropertyPath& TargetPath, const FProperty& Leaf, const Lang::FDstExpr& Value);
		bool LowerFunctionCall(FNodeBuild& Build, Model::EDstPropertySide Side, const Model::FDstPropertyPath& TargetPath, const FProperty& Leaf, const Lang::FDstExpr& Value);
		bool BuildOperationNode(const Lang::FDstExpr& Expression, const FProperty& Target, const FNodeBuild& Owner, const FString& PropertyName,
			TSharedPtr<Model::FDstNode>& OutFunction);
		/** Property is the property the value is for, when known: a reference to a tree is written as a call that is no property function. */
		EValueClass ClassifyValue(const Lang::FDstExpr& Value, const FBindScope& Scope, const FProperty* Property = nullptr) const;
		bool TryFold(const Lang::FDstExpr& Expr, FFoldedConstant& OutValue, EValueDomain Domain = EValueDomain::Other) const;
		/** A literal expression holding a folded constant, spanning Span. */
		TUniquePtr<Lang::FDstExpr> MakeConstantExpr(const FFoldedConstant& Value, const Lang::FDstSpan& Span) const;
		bool ConstantToText(const Lang::FDstExpr& Value, const FProperty& Property, FString& OutText, const Lang::FDstSpan& Span);
		void AddConstantArg(FNodeBuild& Build, Model::EDstPropertySide Side, const FProperty& Property, const FString& Text);
		void AddBindingArg(FNodeBuild& Build, Model::EDstPropertySide Side, const Model::FDstPropertyPath& Path, Model::EDstValueKind Kind, const Model::FDstSourceRef& Source);

		// ---- DstLowerCondition.cpp
		/** Lowers the `if` expressions of one list (a state's, or a transition's) into OutNodes; several clauses are joined with `and`. */
		bool LowerConditionList(const TArray<const Lang::FDstExpr*>& Clauses, const FString& KeyPrefix, Model::EDstNodeRole Role,
			const FBindScope& Scope, TArray<Model::FDstNode>& OutNodes);
		bool LowerConsiderationList(const Lang::FDstExpr& Expr, const FString& KeyPrefix, const FBindScope& Scope, TArray<Model::FDstNode>& OutNodes);
		bool BuildGroup(const Lang::FDstExpr& Expr, bool bConsideration, FCondGroup& Group, bool& bOk);
		bool AddOperand(const Lang::FDstExpr& Expr, bool bConsideration, FCondGroup& Group, bool& bOk);
		void FlattenGroup(const FCondGroup& Group, int32 Depth, Model::EDstOperand Connect, TArray<FCondItem>& Items);
		bool LowerConditionLeaf(const Lang::FDstExpr& Leaf, const FString& Key, Model::EDstNodeRole Role, const FBindScope& Scope, Model::FDstNode& OutNode);
		bool LowerCompare(const Lang::FDstBinaryExpr& Compare, bool bInvert, const FString& Key, Model::EDstNodeRole Role, const FBindScope& Scope, Model::FDstNode& OutNode);
		bool LowerEnumRight(FNodeBuild& Build, const FNodeInfo& Info, const FProperty& RightProperty, const UEnum* Enum, const Lang::FDstExpr& Value);
		bool LowerBoolPath(const Lang::FDstExpr& PathExpr, bool bInvert, const FString& Key, Model::EDstNodeRole Role, const FBindScope& Scope, Model::FDstNode& OutNode);
		bool LowerConstantConsideration(const Lang::FDstExpr& Expr, const FString& Key, const FBindScope& Scope, Model::FDstNode& OutNode);
		bool SetInvert(FNodeBuild& Build, const Lang::FDstSpan& Span);

		// ---- DstLowerState.cpp
		struct FSubtreeInfo
		{
			FString Name;
			const Lang::FDstStateStmt* Stmt = nullptr;
			FString File;
			bool bLibrary = false;
			bool bQueued = false;
			bool bLowered = false;
			FParamSet Signature;
			bool bSignatureBuilt = false;
			bool bSignatureFailed = false;
		};
		/** A `->` to a state by name, resolved once every state exists. */
		struct FPendingTarget
		{
			FString FromPath;
			int32 Index = 0;
			TArray<FString> Segments;
			Lang::FDstSpan Span;
			FString File;
		};

		void CollectSubtreesAndTaskNames();
		FSubtreeInfo* FindSubtree(const FString& Name, bool* bOutAmbiguous = nullptr);
		FSubtreeInfo* FindSubtreeByStmt(const Lang::FDstStateStmt& Stmt);
		bool EnsureSignature(FSubtreeInfo& Info);
		bool LowerStates(const Lang::FDstDocument& Doc);
		void DrainLibrarySubtrees();
		bool LowerStateStmt(const Lang::FDstStateStmt& Stmt, const FString& ParentPath, Model::FDstState& OutState);
		bool LowerStateSettings(const Lang::FDstStateStmt& Stmt, Model::FDstState& State, FStateFrame& Frame);
		bool ResolveTag(const Lang::FDstExpr& Expr, FGameplayTag& OutTag, bool bReport = true);
		bool ReadEventSpec(const Lang::FDstExpr& Expr, Model::FDstEvent& OutEvent, const UScriptStruct*& OutPayload);
		bool LowerEventClause(const Lang::FDstClauseStmt& Clause, Model::FDstState& State, FStateFrame& Frame);
		bool LowerPickClause(const Lang::FDstClauseStmt& Clause, bool bHasChildren, Model::FDstState& State);
		bool LowerTasks(const TArray<const Lang::FDstClauseStmt*>& DoClauses, Model::FDstState& State, FStateFrame& Frame);
		const FParamSet* LoadAssetSignature(const FString& ObjectPath, FString& OutError);
		bool LinkedAssetsReach(const FString& FromAsset, const FString& TargetAsset, int32 Depth);
		bool LowerLinkClause(const Lang::FDstClauseStmt& Clause, Model::FDstState& State, FStateFrame& Frame);
		bool LowerLinkArguments(const Lang::FDstCallExpr* Call, const FParamSet& Target, Model::FDstState& State, FStateFrame& Frame,
			const FString& TargetName, const Lang::FDstSpan& Span);
		bool LowerTransitionTrigger(const Lang::FDstExpr& Trigger, Model::FDstTransition& OutTransition, FEventInfo& OutEvent, const FString& Owner, const FBindScope& Scope);
		bool LowerTransitionOptions(const Lang::FDstMeta& OptionsMeta, Model::FDstTransition& OutTransition);
		bool LowerTransition(const Lang::FDstClauseStmt& Clause, int32 Index, Model::FDstState& State, FStateFrame& Frame, Model::FDstTransition& OutTransition);
		void ResolvePendingTargets();

		// ---- DstLowerCheck.cpp
		void CheckStructure();
		void CheckEventReachability();
		void CheckDuplicateStateNames(const TArray<Model::FDstState>& States, const FString& ParentPath);

		// ---- state shared by every file
		const Lang::FDstCompilationUnit& Unit;
		FDstLowerResult& Out;
		FLowerSink Sink;
		FDstLowerOptions Options;

		const Lang::FDstDocument* Document = nullptr;
		bool bIsLibraryMain = false;

		// schema
		const UClass* SchemaClass = nullptr;
		TStrongObjectPtr<UStateTreeSchema> Schema;
		TArray<FString> Namespaces;
		struct FContext
		{
			FString Name;
			const UStruct* Struct = nullptr;
		};
		TArray<FContext> Contexts;
		/** Extensions that apply to the schema (their default payloads); default objects, so they outlive the run. */
		TArray<const UDreamStateTreeExtension*> Extensions;

		FDstValueContext ValueContext;

		// parameters
		FParamSet TreeParams;
		TArray<TUniquePtr<FSubtreeInfo>> Subtrees;
		TArray<FSubtreeInfo*> LibraryQueue;
		/** Signatures of StateTree assets read for `link asset`, by object path. */
		TMap<FString, TUniquePtr<FParamSet>> AssetSignatures;

		// globals
		TArray<FBindableNode> GlobalNodes;
		struct FGlobalEntry
		{
			const Lang::FDstAssignStmt* Stmt = nullptr;
			const FDstNodeType* Type = nullptr;
			Model::EDstNodeRole Role = Model::EDstNodeRole::GlobalTask;
			int32 ModelIndex = INDEX_NONE;
		};
		TArray<FGlobalEntry> GlobalEntries;

		// colors
		TArray<FString> ColorNames;

		// scopes
		TArray<FStateFrame*> Frames;
		/** Every task name written anywhere, for telling "unknown" from "not readable here". */
		TArray<FString> KnownTaskNames;
		TArray<FPendingTarget> PendingTargets;

		// node info cache
		TMap<const FDstNodeType*, FNodeInfoPtr> NodeInfoCache;
		/** Types the registry does not list (a sugar node by struct path), kept alive for NodeInfoCache's pointers. */
		TArray<TUniquePtr<FDstNodeType>> SyntheticTypes;
	};
}
