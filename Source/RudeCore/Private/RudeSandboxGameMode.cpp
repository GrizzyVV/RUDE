// RUDE - RAGE <-> Unreal Development Environment
#include "RudeSandboxGameMode.h"
#include "RudeSandboxPawn.h"
#include "Engine/World.h"
#include "EngineUtils.h"

ARudeSandboxGameMode::ARudeSandboxGameMode()
{
	DefaultPawnClass = ARudeSandboxPawn::StaticClass();
	bStartPlayersAsSpectators = false;
}

AActor* ARudeSandboxGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->ActorHasTag(FName(TEXT("RUDE_SANDBOX_SPAWN")))) { return *It; }
		}
	}
	return Super::ChoosePlayerStart_Implementation(Player);
}
