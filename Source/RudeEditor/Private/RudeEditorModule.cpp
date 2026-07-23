// RUDE - RAGE <-> Unreal Development Environment
#include "Modules/ModuleManager.h"
#include "RudeToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FRudeEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (UToolsetRegistry::IsAvailable())
		{
			UToolsetRegistry::RegisterToolsetClass(URudeToolset::StaticClass());
			UE_LOG(LogTemp, Display, TEXT("[RUDE] Toolset registered."));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[RUDE] ToolsetRegistry unavailable; toolset not registered."));
		}
	}

	virtual void ShutdownModule() override
	{
		if (UToolsetRegistry::IsAvailable()
			&& UToolsetRegistry::IsToolsetClassRegistered(URudeToolset::StaticClass()))
		{
			UToolsetRegistry::UnregisterToolsetClass(URudeToolset::StaticClass());
		}
	}
};

IMPLEMENT_MODULE(FRudeEditorModule, RudeEditor)
