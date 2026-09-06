// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "RudeSandboxGameMode.generated.h"

// THE SANDBOX's game mode: DefaultPawn = ARudeSandboxPawn, and the player starts at the actor tagged
// RUDE_SANDBOX_SPAWN (the PlayerStart SandboxSetup placed) before falling back to any PlayerStart.
// SandboxSetup writes this class as the level's GameMode override, so pressing Play needs no project
// settings changed.
UCLASS(Blueprintable)
class RUDECORE_API ARudeSandboxGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARudeSandboxGameMode();
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
};
