// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "RudeSandboxPawn.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UStaticMeshComponent;

// THE SANDBOX (GDD 1b.1): the on-foot player. A Character with a third-person spring-arm camera,
// WASD + mouse, Shift to sprint, Space to jump. Input is bound in code to RAW KEYS (BindAxisKey /
// BindKey) so a host project with no input configuration of its own still walks: RUDE is a plugin
// and may not assume the project's DefaultInput.ini or an Enhanced Input mapping context.
// Spawned by ARudeSandboxGameMode at the RUDE_SANDBOX_SPAWN PlayerStart that SandboxSetup placed.
// Mouse look writes the control rotation directly (2.5 deg per mouse unit), so it does not depend on
// the project's legacy input-scale setting; flip bInvertLookY if it feels backwards.
UCLASS(Blueprintable)
class RUDECORE_API ARudeSandboxPawn : public ACharacter
{
	GENERATED_BODY()

public:
	ARudeSandboxPawn();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Sandbox")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Sandbox")
	TObjectPtr<UCameraComponent> Camera;

	// A visible stand-in body (engine cylinder) so the pawn reads in the viewport without a skeletal mesh.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Sandbox")
	TObjectPtr<UStaticMeshComponent> Body;

	// cm/s. GTA on-foot: walk ~1.4 m/s, jog ~3.2 m/s, sprint ~6-7 m/s.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Sandbox") float WalkSpeed = 320.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Sandbox") float SprintSpeed = 700.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Sandbox") float LookSensitivity = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Sandbox") bool bInvertLookY = false;

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void BeginPlay() override;

private:
	void MoveForward(float V);
	void MoveBack(float V);
	void MoveRight(float V);
	void MoveLeft(float V);
	void Turn(float V);
	void LookUp(float V);
	void SprintOn();
	void SprintOff();
	void JumpPressed();
	void JumpReleased();
};
