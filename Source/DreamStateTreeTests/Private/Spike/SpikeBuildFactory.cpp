// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeBuildHelpers.h"
#include "Spike/SpikeDump.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/StateTreeComponentSchema.h"
#include "Editor.h"
#include "IAssetTools.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorSchema.h"
#include "StateTreeFactory.h"
#include "UObject/Package.h"

// Creates a tree the way the Content Browser does (UStateTreeFactory, with and without AssetTools) and looks at what
// the factory sets up on its own: the editor data, the schema, the editor schema, a root state, a first compile.

namespace UE::DreamStateTree::Spike
{
	namespace BuildFactoryPrivate
	{
		FString NameOf(const UObject* Object)
		{
			return Object ? Object->GetClass()->GetName() : FString(TEXT("null"));
		}

		/** Checks what a freshly created tree has and compiles it once more. */
		FString InspectNewTree(FAutomationTestBase& Test, UStateTree& Tree, const UClass* ExpectedSchema, FDstBuildReport& Report)
		{
			UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(Tree.EditorData);
			Test.TestNotNull(TEXT("The factory made editor data"), EditorData);
			if (!EditorData)
			{
				return TEXT("no editor data");
			}

			const bool bSchemaOk = EditorData->Schema && EditorData->Schema->GetClass() == ExpectedSchema;
			Test.TestTrue(TEXT("The editor data has the requested schema"), bSchemaOk);
			Test.TestNotNull(TEXT("The factory made an editor schema"), EditorData->EditorSchema.Get());

			FString RootName(TEXT("none"));
			if (EditorData->SubTrees.Num() > 0 && EditorData->SubTrees[0])
			{
				RootName = EditorData->SubTrees[0]->Name.ToString();
			}
			Test.TestTrue(TEXT("The factory made exactly one root state"), EditorData->SubTrees.Num() == 1);

			const bool bReadyAfterFactory = Tree.IsReadyToRun();
			const FDstBuildCompileOutcome Direct = DstBuildCompile(Tree);
			const FDstBuildCompileOutcome ViaManager = DstBuildCompileViaManager(Tree);
			Test.TestTrue(TEXT("The empty tree compiles"), Direct.bSucceeded && ViaManager.bSucceeded);

			Report.Line(FString::Printf(TEXT("  editor data class: %s"), *NameOf(EditorData)));
			Report.Line(FString::Printf(TEXT("  schema class: %s"), *NameOf(EditorData->Schema)));
			Report.Line(FString::Printf(TEXT("  editor schema class: %s"), *NameOf(EditorData->EditorSchema)));
			Report.Line(FString::Printf(TEXT("  root states: %d (first: %s)"), EditorData->SubTrees.Num(), *RootName));
			Report.Line(FString::Printf(TEXT("  ready to run right after the factory: %s"), bReadyAfterFactory ? TEXT("yes") : TEXT("no")));
			Report.Line(TEXT("  compile (compiler struct): ") + Direct.ToString());
			Report.Line(TEXT("  compile (compiler manager): ") + ViaManager.ToString());
			Report.Block(TEXT("  editor data"), UE::DreamStateTree::Spike::DumpEditorData(*EditorData));

			return FString::Printf(TEXT("EditorData=%s, Schema=%s, EditorSchema=%s, root states=%d, ready after factory=%s, empty tree compile=%s"),
				*NameOf(EditorData), *NameOf(EditorData->Schema), *NameOf(EditorData->EditorSchema), EditorData->SubTrees.Num(),
				bReadyAfterFactory ? TEXT("yes") : TEXT("no"), *Direct.Summary());
		}
	}
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstSpikeBuild_Factory, FDstSpikeBuildTestBase, "DreamStateTree.Spike.Build.Factory",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeBuild_Factory::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	FDstBuildAssetKeeper Keeper;
	FDstBuildLogCapture LogCapture;
	FDstBuildReport Report;

	const TSubclassOf<UStateTreeSchema> SchemaClasses[] = {
		UStateTreeComponentSchema::StaticClass(),
		UDstTestSchema::StaticClass(),
	};

	// A package of the same name may still be in memory from an earlier run; CreateAsset would ask about overwriting it.
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);

	for (const TSubclassOf<UStateTreeSchema>& SchemaClass : SchemaClasses)
	{
		const FString SchemaName = SchemaClass->GetName();

		// 1. The factory called directly, as a script or a test would.
		{
			Report.Heading(FString::Printf(TEXT("Factory, called directly, schema %s"), *SchemaName));
			LogCapture.Reset();

			UStateTreeFactory* Factory = NewObject<UStateTreeFactory>(GetTransientPackage());
			Factory->SetSchemaClass(SchemaClass.Get());

			const FString AssetName = FString::Printf(TEXT("Direct_%s_%s"), *SchemaName, *UniqueSuffix);
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/DreamStateTreeTests/Factory/%s"), *AssetName));
			UObject* Created = Factory->FactoryCreateNew(UStateTree::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
			UStateTree* Tree = Cast<UStateTree>(Created);
			if (!TestNotNull(TEXT("FactoryCreateNew returned a tree"), Tree))
			{
				Report.Line(TEXT("The factory returned nothing (its first compile failed or no schema was set)."));
				Report.Block(TEXT("engine log"), FString::Join(LogCapture.GetLines(), TEXT("\n")));
				continue;
			}
			Keeper.Keep(Tree);

			const FString Summary = UE::DreamStateTree::Spike::BuildFactoryPrivate::InspectNewTree(*this, *Tree, SchemaClass.Get(), Report);
			Report.Block(TEXT("  engine log"), FString::Join(LogCapture.GetLines(), TEXT("\n")));
			AddInfo(FString::Printf(TEXT("[Factory] Direct, %s: %s"), *SchemaName, *Summary));
		}

		// 2. Through AssetTools, the way the Content Browser's New menu does it.
		{
			Report.Heading(FString::Printf(TEXT("AssetTools CreateAsset, schema %s"), *SchemaName));
			if (!GEditor)
			{
				Report.Line(TEXT("Skipped: no editor."));
				AddInfo(TEXT("[Factory] AssetTools route skipped: no editor in this run"));
				continue;
			}
			LogCapture.Reset();

			UStateTreeFactory* Factory = NewObject<UStateTreeFactory>(GetTransientPackage());
			Factory->SetSchemaClass(SchemaClass.Get());

			const FString AssetName = FString::Printf(TEXT("AssetTools_%s_%s"), *SchemaName, *UniqueSuffix);
			IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
			UObject* Created = AssetTools.CreateAsset(AssetName, TEXT("/Game/DreamStateTreeTests/Factory"), UStateTree::StaticClass(), Factory);
			UStateTree* Tree = Cast<UStateTree>(Created);
			if (!TestNotNull(TEXT("IAssetTools::CreateAsset returned a tree"), Tree))
			{
				Report.Block(TEXT("engine log"), FString::Join(LogCapture.GetLines(), TEXT("\n")));
				continue;
			}
			Keeper.Keep(Tree);

			const FString Summary = UE::DreamStateTree::Spike::BuildFactoryPrivate::InspectNewTree(*this, *Tree, SchemaClass.Get(), Report);
			Report.Line(FString::Printf(TEXT("  package: %s, dirty: %s"), *Tree->GetOutermost()->GetName(), Tree->GetOutermost()->IsDirty() ? TEXT("yes") : TEXT("no")));
			Report.Block(TEXT("  engine log"), FString::Join(LogCapture.GetLines(), TEXT("\n")));
			AddInfo(FString::Printf(TEXT("[Factory] AssetTools, %s: %s"), *SchemaName, *Summary));

			// CreateAsset registers the asset with the asset registry; take it out again so the test leaves nothing behind.
			FAssetRegistryModule::AssetDeleted(Tree);
		}
	}

	const FString ReportPath = SaveReport(TEXT("Build"), TEXT("Factory"), Report.ToString());
	AddInfo(FString::Printf(TEXT("Report: %s"), *ReportPath));
	return true;
}
