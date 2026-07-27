// RUDE - RAGE <-> Unreal Development Environment
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "RudeToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

// ⚠ LOADING PHASE IS LOAD-BEARING (changed PostEngineInit -> Default, 2026-07-27).
// A commandlet class must exist by the time the engine resolves `-run=`, which happens BEFORE
// PostEngineInit - so at that phase `RudeCommandlet` was never found and the CLI could not run at
// all. `Default` is early enough, and ToolsetRegistry is itself a Default-phase plugin declared as
// our dependency, so it still loads first.
//
// Registration no longer ASSUMES that ordering, though: if the registry is not up yet we retry at
// post-engine-init. Order-independence is the point - a phase change must not be able to silently
// cost us the MCP surface, which is what a bare IsAvailable() check would have done.
class FRudeEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (!TryRegister())
		{
			PostInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
			{
				if (!TryRegister())
				{
					UE_LOG(LogTemp, Warning,
						TEXT("[RUDE] ToolsetRegistry unavailable even after engine init; "
						     "the agent/MCP surface is NOT registered. The CLI still works, "
						     "since it calls URudeToolset by reflection."));
				}
			});
		}
	}

	virtual void ShutdownModule() override
	{
		if (PostInitHandle.IsValid())
		{
			FCoreDelegates::OnPostEngineInit.Remove(PostInitHandle);
			PostInitHandle.Reset();
		}
		if (UToolsetRegistry::IsAvailable()
			&& UToolsetRegistry::IsToolsetClassRegistered(URudeToolset::StaticClass()))
		{
			UToolsetRegistry::UnregisterToolsetClass(URudeToolset::StaticClass());
		}
	}

private:
	bool TryRegister()
	{
		if (!UToolsetRegistry::IsAvailable())
		{
			return false;
		}
		if (!UToolsetRegistry::IsToolsetClassRegistered(URudeToolset::StaticClass()))
		{
			UToolsetRegistry::RegisterToolsetClass(URudeToolset::StaticClass());
			UE_LOG(LogTemp, Display, TEXT("[RUDE] Toolset registered."));
		}
		return true;
	}

	FDelegateHandle PostInitHandle;
};

IMPLEMENT_MODULE(FRudeEditorModule, RudeEditor)
