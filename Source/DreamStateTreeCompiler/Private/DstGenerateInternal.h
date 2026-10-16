// Copyright DreamMoon. All Rights Reserved.
//
// What the generator's files share: the context that carries the model, the diagnostics and the ID book-keeping, and the
// helpers that turn the model's texts (type names, struct paths, export text) into engine objects.
//
//   DstGenerate.cpp         the public entry points, the asset (create or rebuild in place), compile, diagnostics, save
//   DstGenerateBuild.cpp    the editor data: parameters, globals, states, nodes, transitions, links, bindings
//   DstGenerateSupport.cpp  the helpers declared here

#pragma once

#include "CoreMinimal.h"
#include "DstGenerate.h"
#include "DstLower.h"
#include "Lang/DstDiagnostic.h"
#include "Model/DstModel.h"
#include "PropertyBindingPath.h"
#include "StructUtils/PropertyBag.h"

class FProperty;
class UClass;
class UEnum;
class UObject;
class UScriptStruct;
class UStateTree;
class UStateTreeEditorData;
class UStruct;

namespace UE::DreamStateTree::Compiler::GeneratePrivate
{
	/** Name of the color every editor data starts with; it has no ID, which is also what a state without a color refers to. */
	extern const TCHAR* const DefaultColorName;

	/**
	 * Carries the model, the diagnostics and the ID book-keeping through one generation. Every ID the generator assigns goes
	 * through Assign: it is derived from the model key, recorded in Result.Ids and remembered with the place in the text
	 * that wrote the object, so a compile message or an extension's report that names the ID lands on the right line.
	 */
	struct FGenContext
	{
		FGenContext(const Model::FDstTree& InTree, const FDstPlaces& InPlaces, Lang::FDstDiagnosticSink& InSink, FDstGenerateResult& InResult)
			: Tree(InTree)
			, Places(InPlaces)
			, Sink(InSink)
			, Result(InResult)
		{
		}

		const Model::FDstTree& Tree;
		const FDstPlaces& Places;
		Lang::FDstDiagnosticSink& Sink;
		FDstGenerateResult& Result;

		/** Errors this generator reported; the sink may hold others from the stages before. */
		int32 NumErrors = 0;

		/**
		 * MakeDstGuid(Tree.AssetPath, HashKey, Salt), recorded as Result.Ids[IdKey] and as the ID of PlaceKey. IdKey and
		 * HashKey differ only for the salted IDs, which share their key with the object they belong to.
		 */
		FGuid Assign(const FString& IdKey, const FString& HashKey, const TCHAR* Salt, const FString& PlaceKey);
		/** Remembers that an ID (one the engine derives, or one the engine copied) belongs to PlaceKey. */
		void MapId(const FGuid& Id, const FString& PlaceKey);
		/** The place key an ID was mapped to; empty (the file header) when it is not known. */
		FString FindPlaceKey(const FGuid& Id) const;

		/** The text span of a place key. A property function key falls back to the node it feeds. */
		Lang::FDstSpan SpanFor(const FString& PlaceKey) const;

		/** An error found while building the engine objects from the model. */
		void Dst7003(const FString& PlaceKey, const FString& Message, const FString& Hint = FString());
		/** A warning of the same kind. */
		void Dst7004(const FString& PlaceKey, const FString& Message, const FString& Hint = FString());

		/** Places the semantic stage knows under another key than the model's (`Global:<name>` for `Global[0]`). */
		TMap<FString, FString> AltPlaceKeys;

	private:
		TMap<FGuid, FString> IdToPlace;
	};

	//------------------------------------------------------------------------------------------------------------------//
	// Texts to engine things
	//------------------------------------------------------------------------------------------------------------------//

	/** The struct at an object path (`/Script/StateTreeModule.StateTreeDelayTask`); loads a user-defined struct asset. */
	const UScriptStruct* FindStructByPath(const FString& Path);
	/** The class at an object path (`/Script/Engine.Actor`, `/Game/AI/STT_Wander.STT_Wander_C`); loads a Blueprint class. */
	UClass* FindClassByPath(const FString& Path);
	/** The enum at an object path; loads a user-defined enum asset. */
	UEnum* FindEnumByPath(const FString& Path);

	/** A model parameter type (`float`, `Struct:/Script/CoreUObject.Vector`, `Array<int32>`) as a property bag type. */
	struct FParsedParameterType
	{
		EPropertyBagPropertyType ValueType = EPropertyBagPropertyType::None;
		const UObject* TypeObject = nullptr;
		FPropertyBagContainerTypes Containers;
	};
	bool ParseParameterType(const FString& Type, FParsedParameterType& Out, FString& OutError);

	/** Sets a property of a container from its export text (PPF_None, the form DstExportDefault writes). */
	bool ImportPropertyText(const FProperty& Property, void* Container, UObject* Owner, const FString& Text, FString& OutError);

	/** The property a constant is for, and the container it lives in, walking nested structs. */
	bool ResolveConstantTarget(const UStruct* Struct, void* Memory, const Model::FDstPropertyPath& Path, const FProperty*& OutProperty,
		void*& OutContainer, FString& OutError);

	/** A binding path from the model's path: the segments as written, with the instance types the model knows. */
	FPropertyBindingPath MakeBindingPath(const FGuid& StructId, const Model::FDstPropertyPath& Path);
	/**
	 * The same, for a path that starts in the data RootStruct describes (held at RootMemory): a segment of the model's
	 * PropertyBag instance type, the parameter bag of a reference to a tree, takes the struct that data holds there. The
	 * segment is left without an instance struct when the data does not reach it.
	 */
	FPropertyBindingPath MakeBindingPath(const FGuid& StructId, const Model::FDstPropertyPath& Path, const UStruct* RootStruct, const void* RootMemory);

	/** What a node of the model is in the engine. */
	struct FResolvedNodeType
	{
		/** The node struct; for a Blueprint node, the engine's wrapper struct. */
		const UScriptStruct* Struct = nullptr;
		/** The Blueprint node's class; null for C++ nodes. */
		UClass* BlueprintClass = nullptr;
	};
	bool ResolveNodeType(const Model::FDstNode& Node, FResolvedNodeType& Out, FString& OutError);

	/**
	 * The instance data property a property function writes its result to: the one named Result, bResult or Output, or the
	 * only property of category Output. Null when it has none.
	 */
	const FProperty* FindFunctionOutputProperty(const UStruct& InstanceStruct);

	/** The first line where two dumps differ, "line 12: before `...`, after `...`"; empty when they are equal. */
	FString DescribeFirstDifference(const FString& Before, const FString& After);

	//------------------------------------------------------------------------------------------------------------------//
	// The build
	//------------------------------------------------------------------------------------------------------------------//

	/**
	 * Checks what can be checked about the model before anything is touched: node types, Blueprint classes, tags, payload
	 * structs, parameter types, state references. A rebuild replaces the asset's content in place, so these are found first.
	 */
	bool ValidateModel(FGenContext& Ctx);

	/**
	 * Empties EditorData and fills it from the model: settings, colors, parameters, globals, states, nodes, transitions and
	 * links, and, once all of those exist, the bindings. The schema is already in place. False when an error was reported.
	 */
	bool BuildEditorData(FGenContext& Ctx, UStateTree& StateTree, UStateTreeEditorData& EditorData);
}
