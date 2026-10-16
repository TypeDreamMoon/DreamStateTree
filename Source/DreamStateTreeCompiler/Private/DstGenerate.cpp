// Copyright DreamMoon. All Rights Reserved.
//
// The generator's entry points: the asset (created, or rebuilt in place with its UStateTree and its editor data object
// kept), the schema, the compile through the engine's compiler manager, the placing of the engine's messages in the text,
// the extensions' checks, the package metadata and the save. The editor data itself is filled in DstGenerateBuild.cpp.

#include "DstGenerate.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamStateTreeExtension.h"
#include "DstExtract.h"
#include "DstGenerateInternal.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeCompilerManager.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorModule.h"
#include "StateTreeEditorSchema.h"
#include "StateTreeSchema.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler
{
	const FName DstSourceMetadataKey(TEXT("DreamStateTree.Source"));
	const FName DstSourcePathMetadataKey(TEXT("DreamStateTree.SourcePath"));

	FGuid MakeDstGuid(const FString& AssetPath, const FString& Key, const TCHAR* Salt)
	{
		FString Full = AssetPath + TEXT("|") + Key;
		if (Salt && Salt[0] != TEXT('\0'))
		{
			Full += TEXT("|");
			Full += Salt;
		}

		const FTCHARToUTF8 Utf8(*Full);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);

		uint32 Parts[4];
		FMemory::Memcpy(Parts, Digest, sizeof(Parts));
		FGuid Guid(Parts[0], Parts[1], Parts[2], Parts[3]);
		// The all-zero GUID means "none" to the engine.
		if (!Guid.IsValid())
		{
			Guid.D = 1;
		}
		return Guid;
	}

	namespace GeneratePrivate
	{
		namespace
		{
			/** The compiler log keeps its messages protected; this exposes them for reading. */
			struct FDstCompilerLog : public FStateTreeCompilerLog
			{
				TConstArrayView<FStateTreeCompilerLogMessage> GetAllMessages() const { return Messages; }
			};

			/** `/Game/AI/ST_Brawler.ST_Brawler` -> package and asset name. The asset is named like its package. */
			bool SplitAssetPath(const FString& AssetPath, FString& OutPackage, FString& OutName, FString& OutError)
			{
				FString Package;
				FString Name;
				if (!AssetPath.Split(TEXT("."), &Package, &Name))
				{
					Package = AssetPath;
					Name = FPackageName::GetShortName(Package);
				}
				if (Package.IsEmpty() || Name.IsEmpty())
				{
					OutError = FString::Printf(TEXT("`%s` is not an asset path (expected `/Game/Folder/Name.Name`)."), *AssetPath);
					return false;
				}

				FText Reason;
				const bool bValidPackage = FPackageName::IsValidLongPackageName(Package, /*bIncludeReadOnlyRoots*/ false, &Reason)
					// The scratch root of tests and previews.
					|| Package.StartsWith(TEXT("/Temp/"), ESearchCase::CaseSensitive);
				if (!bValidPackage)
				{
					OutError = FString::Printf(TEXT("`%s` is not a package path an asset can be created in: %s"), *Package, *Reason.ToString());
					return false;
				}
				if (Name != FPackageName::GetShortName(Package))
				{
					OutError = FString::Printf(TEXT("The asset `%s` must be named like its package `%s`."), *Name, *Package);
					return false;
				}

				OutPackage = Package;
				OutName = Name;
				return true;
			}

			/** The asset's own schema properties go back to the class defaults before the text's are applied. */
			void ResetSchemaToDefaults(UStateTreeSchema& Schema)
			{
				const UObject* Defaults = Schema.GetClass()->GetDefaultObject();
				for (TFieldIterator<FProperty> It(Schema.GetClass()); It; ++It)
				{
					const FProperty* Property = *It;
					if (!Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					{
						Property->CopyCompleteValue_InContainer(&Schema, Defaults);
					}
				}
			}

			/**
			 * What the Details panel does after it changed a property: schemas keep derived data in step with their properties
			 * there (the component schema copies the actor class into its context description).
			 */
			void NotifyPropertyChanged(UObject& Object, const FProperty& Property)
			{
				FProperty* Changed = const_cast<FProperty*>(&Property);
				FEditPropertyChain Chain;
				Chain.AddTail(Changed);
				Chain.SetActivePropertyNode(Changed);
				Chain.SetActiveMemberPropertyNode(Changed);
				const FPropertyChangedEvent ChangedEvent(Changed, EPropertyChangeType::ValueSet);
				FPropertyChangedChainEvent ChainEvent(Chain, ChangedEvent);
				Object.PostEditChangeChainProperty(ChainEvent);
			}

			/** The editor data as text, to compare it before and after the compile. Empty, with the reason, when it cannot be read. */
			FString DumpAsset(const UStateTree& Asset, FString& OutProblem)
			{
				Model::FDstTree Extracted;
				Lang::FDstDiagnosticSink Scratch;
				if (!ExtractDst(Asset, Extracted, Scratch))
				{
					const Lang::FDstDiagnostic* First = Scratch.FirstError();
					OutProblem = First ? First->Message : FString(TEXT("the asset could not be read back"));
					return FString();
				}
				return Model::DumpDstModel(Extracted);
			}

			/** The engine's compile messages, each at the line that wrote the thing it names. */
			void ReportCompileLog(FGenContext& Ctx, const FDstCompilerLog& Log, const bool bCompiled)
			{
				bool bReportedError = false;
				for (const FStateTreeCompilerLogMessage& Message : Log.GetAllMessages())
				{
					// The item's ID first (a node, a parameter, an event), then the state it was found in.
					FString PlaceKey = Ctx.FindPlaceKey(Message.Item.ID);
					if (PlaceKey.IsEmpty() && Message.State)
					{
						PlaceKey = Message.State->GetPath();
					}
					const Lang::FDstSpan Span = Ctx.SpanFor(PlaceKey);
					const FString Text = Message.Item.Name.IsNone()
						? Message.Message
						: FString::Printf(TEXT("%s: %s"), *Message.Item.Name.ToString(), *Message.Message);

					// Severities count up from the worst; anything at Error or below (the removed CriticalError) is an error.
					if (Message.Severity <= static_cast<int32>(EMessageSeverity::Error))
					{
						++Ctx.NumErrors;
						bReportedError = true;
						Ctx.Sink.Error(TEXT("DST7003"), Span, Text);
					}
					else if (Message.Severity <= static_cast<int32>(EMessageSeverity::Warning))
					{
						// PerformanceWarning and Warning.
						Ctx.Sink.Warning(TEXT("DST7004"), Span, Text);
					}
					else
					{
						Ctx.Sink.Info(TEXT("DST7004"), Span, Text);
					}
				}

				if (!bCompiled && !bReportedError)
				{
					++Ctx.NumErrors;
					Ctx.Sink.Error(TEXT("DST7003"), Ctx.SpanFor(FString()), TEXT("The engine's compile of the StateTree failed without saying why."),
						TEXT("Look for LogStateTree messages in the output log."));
				}
			}

			/** The checks extensions bring for the schema's trees (UDreamStateTreeExtension::Validate), at the lines they point to. */
			void RunExtensions(FGenContext& Ctx, const UStateTreeEditorData& EditorData)
			{
				if (!EditorData.Schema)
				{
					return;
				}
				for (const UDreamStateTreeExtension* Extension : UDreamStateTreeExtension::FindFor(*EditorData.Schema))
				{
					if (!Extension)
					{
						continue;
					}
					FDreamStateTreeDiagnostics Diagnostics;
					Extension->Validate(EditorData, Diagnostics);
					for (const FDreamStateTreeDiagnostics::FEntry& Entry : Diagnostics.GetEntries())
					{
						const Lang::FDstSpan Span = Ctx.SpanFor(Ctx.FindPlaceKey(Entry.Where));
						// The extension's own code stays visible; the DST code says where the report came from.
						const FString Text = FString::Printf(TEXT("%s: %s"), *Entry.Code.ToString(), *Entry.Message);
						switch (Entry.Severity)
						{
						case EDreamStateTreeSeverity::Error:
							++Ctx.NumErrors;
							Ctx.Sink.Error(TEXT("DST7007"), Span, Text);
							break;
						case EDreamStateTreeSeverity::Warning:
							Ctx.Sink.Warning(TEXT("DST7007"), Span, Text);
							break;
						case EDreamStateTreeSeverity::Info:
						default:
							Ctx.Sink.Info(TEXT("DST7007"), Span, Text);
							break;
						}
					}
				}
			}

			void WriteSourceMetadata(UStateTree& Asset, const FDstGenerateOptions& Options)
			{
				FMetaData& MetaData = Asset.GetPackage()->GetMetaData();
				if (Options.SourceText.IsEmpty())
				{
					MetaData.RemoveValue(&Asset, DstSourceMetadataKey);
				}
				else
				{
					MetaData.SetValue(&Asset, DstSourceMetadataKey, *Options.SourceText);
				}
				if (Options.SourcePath.IsEmpty())
				{
					MetaData.RemoveValue(&Asset, DstSourcePathMetadataKey);
				}
				else
				{
					MetaData.SetValue(&Asset, DstSourcePathMetadataKey, *Options.SourcePath);
				}
			}

			bool SaveAsset(FGenContext& Ctx, UStateTree& Asset)
			{
				UPackage* Package = Asset.GetPackage();
				const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags = SAVE_NoError;
				Args.Error = GWarn;
				Args.bSlowTask = false;
				// Save returns the result struct; SavePackage only a bool.
				const FSavePackageResultStruct Saved = UPackage::Save(Package, &Asset, *Filename, Args);
				if (!Saved.IsSuccessful())
				{
					++Ctx.NumErrors;
					Ctx.Sink.Error(TEXT("DST7006"), Ctx.SpanFor(FString()), FString::Printf(TEXT("%s cannot be saved to %s."), *Package->GetName(), *Filename),
						TEXT("Is the file read-only, or locked by source control?"));
					return false;
				}
				return true;
			}
		}
	}

	bool GenerateDst(const Model::FDstTree& Tree, const FDstPlaces& Places, const FDstGenerateOptions& Options, FDstGenerateResult& OutResult,
		Lang::FDstDiagnosticSink& Sink)
	{
		using namespace GeneratePrivate;

		OutResult = FDstGenerateResult();
		const int32 ErrorsBefore = Sink.NumErrors();
		FGenContext Ctx(Tree, Places, Sink, OutResult);
		const Lang::FDstSpan HeaderSpan = Ctx.SpanFor(FString());

		// The path, the schema and what already sits at the path are settled before anything is created or changed.
		FString PackageName;
		FString AssetName;
		FString Error;
		if (!SplitAssetPath(Tree.AssetPath, PackageName, AssetName, Error))
		{
			Sink.Error(TEXT("DST7001"), HeaderSpan, Error);
			return false;
		}

		UClass* SchemaClass = FindClassByPath(Tree.SchemaClass);
		if (!SchemaClass || !SchemaClass->IsChildOf(UStateTreeSchema::StaticClass()) || SchemaClass->HasAnyClassFlags(CLASS_Abstract))
		{
			Sink.Error(TEXT("DST7001"), HeaderSpan, FString::Printf(TEXT("%s cannot be created: `%s` is not a StateTree schema class."), *Tree.AssetPath, *Tree.SchemaClass));
			return false;
		}

		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Existing && FPackageName::DoesPackageExist(PackageName))
		{
			LoadPackage(nullptr, *PackageName, LOAD_NoWarn | LOAD_Quiet);
			Existing = FindObject<UObject>(nullptr, *ObjectPath);
		}
		UStateTree* StateTree = nullptr;
		if (Existing)
		{
			StateTree = Cast<UStateTree>(Existing);
			if (!StateTree)
			{
				Sink.Error(TEXT("DST7002"), HeaderSpan, FString::Printf(TEXT("%s already exists as a %s, not a StateTree."), *ObjectPath, *Existing->GetClass()->GetName()),
					TEXT("Choose another Name in the file header, or delete that asset."));
				return false;
			}
		}

		// A rebuild replaces the content in place; what can be found wrong in the model is found first.
		if (!ValidateModel(Ctx))
		{
			return false;
		}

		// The asset: new, or the existing UStateTree with its editor data object (an open editor holds both).
		if (!StateTree)
		{
			UPackage* Package = CreatePackage(*PackageName);
			if (Package)
			{
				StateTree = NewObject<UStateTree>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			}
			if (!StateTree)
			{
				Sink.Error(TEXT("DST7001"), HeaderSpan, FString::Printf(TEXT("%s cannot be created: the package cannot be made."), *Tree.AssetPath));
				return false;
			}
			OutResult.bCreated = true;
			FAssetRegistryModule::AssetCreated(StateTree);
		}
		OutResult.Asset = StateTree;

		// The editor data and editor schema classes a schema asks for, as the factory resolves them.
		FStateTreeEditorModule& EditorModule = FStateTreeEditorModule::GetModule();
		TSubclassOf<const UStateTreeSchema> SchemaSubclass;
		SchemaSubclass = SchemaClass;
		const TNonNullSubclassOf<UStateTreeEditorData> EditorDataClass = EditorModule.GetEditorDataClass(SchemaSubclass.Get());
		const TNonNullSubclassOf<UStateTreeEditorSchema> EditorSchemaClass = EditorModule.GetEditorSchemaClass(SchemaSubclass.Get());

		UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
		if (EditorData && !EditorData->IsA(EditorDataClass.Get()))
		{
			// Only a different kind of editor data forces a new object.
			UE::StateTree::Compiler::RenameObjectToTransientPackage(EditorData, FString::Printf(TEXT("TRASH_%s"), *EditorData->GetName()));
			EditorData = nullptr;
		}
		if (!EditorData)
		{
			EditorData = NewObject<UStateTreeEditorData>(StateTree, EditorDataClass, FName(), RF_Transactional);
			StateTree->EditorData = EditorData;
		}

		UStateTreeEditorSchema* EditorSchema = EditorData->EditorSchema;
		if (EditorSchema && !EditorSchema->IsA(EditorSchemaClass.Get()))
		{
			UE::StateTree::Compiler::RenameObjectToTransientPackage(EditorSchema, FString::Printf(TEXT("TRASH_%s"), *EditorSchema->GetName()));
			EditorSchema = nullptr;
		}
		if (!EditorSchema)
		{
			EditorData->EditorSchema = NewObject<UStateTreeEditorSchema>(EditorData, EditorSchemaClass, FName(), RF_Transactional);
		}

		// The schema object stays when the class is the same; its properties are set again from the text.
		UStateTreeSchema* Schema = EditorData->Schema;
		if (Schema && Schema->GetClass() != SchemaClass)
		{
			UE::StateTree::Compiler::RenameObjectToTransientPackage(Schema, FString::Printf(TEXT("TRASH_%s"), *Schema->GetName()));
			Schema = nullptr;
		}
		if (!Schema)
		{
			Schema = NewObject<UStateTreeSchema>(EditorData, SchemaClass, FName(), RF_Transactional);
			EditorData->Schema = Schema;
		}
		else
		{
			ResetSchemaToDefaults(*Schema);
		}

		TArray<FString> SchemaKeys;
		Tree.SchemaProperties.GetKeys(SchemaKeys);
		SchemaKeys.Sort();
		for (const FString& Key : SchemaKeys)
		{
			const FProperty* Property = SchemaClass->FindPropertyByName(FName(*Key));
			if (!Property)
			{
				Ctx.Dst7003(FString(), FString::Printf(TEXT("The schema `%s` has no property `%s`."), *SchemaClass->GetName(), *Key));
				continue;
			}
			if (!ImportPropertyText(*Property, Schema, Schema, Tree.SchemaProperties[Key], Error))
			{
				Ctx.Dst7003(FString(), Error);
				continue;
			}
			NotifyPropertyChanged(*Schema, *Property);
		}
		if (Sink.NumErrors() != ErrorsBefore)
		{
			StateTree->GetPackage()->MarkPackageDirty();
			return false;
		}

		// The tree itself.
		if (!BuildEditorData(Ctx, *StateTree, *EditorData))
		{
			// Not compiled and not saved: what is in memory is not a tree anyone should run.
			StateTree->GetPackage()->MarkPackageDirty();
			return false;
		}

		// The compile may fix things up in the editor data (ValidateStateTree runs first and drops what the schema does not
		// allow). Whatever it changes is something the generator should have written differently.
		FString BeforeProblem;
		const FString Before = DumpAsset(*StateTree, BeforeProblem);

		FDstCompilerLog Log;
		const bool bCompiled = UE::StateTree::Compiler::FCompilerManager::CompileSynchronously(StateTree, Log);
		OutResult.bCompiled = bCompiled;
		ReportCompileLog(Ctx, Log, bCompiled);

		FString AfterProblem;
		const FString After = DumpAsset(*StateTree, AfterProblem);
		if (BeforeProblem.IsEmpty() && AfterProblem.IsEmpty())
		{
			const FString Difference = DescribeFirstDifference(Before, After);
			if (!Difference.IsEmpty())
			{
				OutResult.bCompileChangedEditorData = true;
				Sink.Error(TEXT("DST7005"), HeaderSpan,
					FString::Printf(TEXT("The engine's compile changed the editor data the generator wrote (%s)."), *Difference),
					TEXT("The generator wrote something the engine had to correct; this is a bug in the generator or in the model that lowered to it."));
			}
		}
		else
		{
			Sink.Warning(TEXT("DST7005"), HeaderSpan,
				FString::Printf(TEXT("Could not check whether the compile changed the editor data: %s."), BeforeProblem.IsEmpty() ? *AfterProblem : *BeforeProblem));
		}

		// Checks that belong to the schema's extensions, once the tree is built and compiled.
		if (bCompiled)
		{
			RunExtensions(Ctx, *EditorData);
		}

		if (Sink.NumErrors() == ErrorsBefore)
		{
			WriteSourceMetadata(*StateTree, Options);
			StateTree->GetPackage()->MarkPackageDirty();
			if (Options.bSave)
			{
				SaveAsset(Ctx, *StateTree);
			}
		}
		else
		{
			StateTree->GetPackage()->MarkPackageDirty();
		}

		return Sink.NumErrors() == ErrorsBefore;
	}
}
