// Copyright DreamMoon. All Rights Reserved.

#include "DreamStateTreeEditorMenus.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "DreamStateTreeEditor"

class FDreamStateTreeEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMessageLogModule& MessageLog = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
		FMessageLogInitializationOptions LogOptions;
		LogOptions.bShowFilters = true;
		LogOptions.bAllowClear = true;
		MessageLog.RegisterLogListing(TEXT("DreamStateTree"), LOCTEXT("LogLabel", "Dream State Tree"), LogOptions);

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
		{
			FToolMenuOwnerScoped OwnerScoped(this);
			DreamStateTreeEditorMenus::RegisterMenus();
		}));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FMessageLogModule* MessageLog = FModuleManager::GetModulePtr<FMessageLogModule>(TEXT("MessageLog")))
		{
			MessageLog->UnregisterLogListing(TEXT("DreamStateTree"));
		}
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDreamStateTreeEditorModule, DreamStateTreeEditor)
