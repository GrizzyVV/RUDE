// RUDE - RAGE <-> Unreal Development Environment
#include "RudeSandboxPawn.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputCoreTypes.h"
#include "UObject/ConstructorHelpers.h"

ARudeSandboxPawn::ARudeSandboxPawn()
{
	PrimaryActorTick.bCanEverTick = false;
	GetCapsuleComponent()->InitCapsuleSize(34.f, 90.f);
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->bOrientRotationToMovement = true;      // the body faces where it walks; the camera is free
		Move->RotationRate = FRotator(0.f, 540.f, 0.f);
		Move->JumpZVelocity = 480.f;
		Move->AirControl = 0.35f;
		Move->MaxWalkSpeed = WalkSpeed;
		Move->MaxStepHeight = 45.f;                  // kerbs and stairs of a district
		Move->SetWalkableFloorAngle(50.f);
	}
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 380.f;
	SpringArm->SocketOffset = FVector(0.f, 0.f, 70.f);
	SpringArm->bUsePawnControlRotation = true;
	SpringArm->bDoCollisionTest = true;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(RootComponent);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetRelativeScale3D(FVector(0.6f, 0.6f, 1.7f));   // the engine cylinder is 100 cm tall
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cyl.Succeeded()) { Body->SetStaticMesh(Cyl.Object); }
}

void ARudeSandboxPawn::BeginPlay()
{
	Super::BeginPlay();
	if (UCharacterMovementComponent* Move = GetCharacterMovement()) { Move->MaxWalkSpeed = WalkSpeed; }
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Cyan,
			TEXT("[RUDE Sandbox] WASD move, mouse look, Shift sprint, Space jump. Console: Rude.Native Help"));
	}
}

void ARudeSandboxPawn::SetupPlayerInputComponent(UInputComponent* PIC)
{
	Super::SetupPlayerInputComponent(PIC);
	if (!PIC) { return; }
	// Raw keys, no project mappings: a digital key read as an axis is 1 while held, 0 otherwise.
	PIC->BindAxisKey(EKeys::W, this, &ARudeSandboxPawn::MoveForward);
	PIC->BindAxisKey(EKeys::S, this, &ARudeSandboxPawn::MoveBack);
	PIC->BindAxisKey(EKeys::D, this, &ARudeSandboxPawn::MoveRight);
	PIC->BindAxisKey(EKeys::A, this, &ARudeSandboxPawn::MoveLeft);
	PIC->BindAxisKey(EKeys::MouseX, this, &ARudeSandboxPawn::Turn);
	PIC->BindAxisKey(EKeys::MouseY, this, &ARudeSandboxPawn::LookUp);
	PIC->BindAxisKey(EKeys::Gamepad_LeftY, this, &ARudeSandboxPawn::MoveForward);
	PIC->BindAxisKey(EKeys::Gamepad_LeftX, this, &ARudeSandboxPawn::MoveRight);
	PIC->BindKey(EKeys::SpaceBar, IE_Pressed, this, &ARudeSandboxPawn::JumpPressed);
	PIC->BindKey(EKeys::SpaceBar, IE_Released, this, &ARudeSandboxPawn::JumpReleased);
	PIC->BindKey(EKeys::LeftShift, IE_Pressed, this, &ARudeSandboxPawn::SprintOn);
	PIC->BindKey(EKeys::LeftShift, IE_Released, this, &ARudeSandboxPawn::SprintOff);
}

void ARudeSandboxPawn::MoveForward(float V)
{
	if (FMath::IsNearlyZero(V) || !Controller) { return; }
	const FRotator YawRot(0.f, Controller->GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(YawRot).GetUnitAxis(EAxis::X), V);
}
void ARudeSandboxPawn::MoveBack(float V) { MoveForward(-V); }
void ARudeSandboxPawn::MoveRight(float V)
{
	if (FMath::IsNearlyZero(V) || !Controller) { return; }
	const FRotator YawRot(0.f, Controller->GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y), V);
}
void ARudeSandboxPawn::MoveLeft(float V) { MoveRight(-V); }

void ARudeSandboxPawn::Turn(float V)
{
	if (FMath::IsNearlyZero(V) || !Controller) { return; }
	FRotator R = Controller->GetControlRotation();
	R.Yaw = FRotator::NormalizeAxis(R.Yaw + V * 2.5f * LookSensitivity);
	Controller->SetControlRotation(R);
}
void ARudeSandboxPawn::LookUp(float V)
{
	if (FMath::IsNearlyZero(V) || !Controller) { return; }
	FRotator R = Controller->GetControlRotation();
	const float Signed = bInvertLookY ? -V : V;   // mouse up = look up by default
	R.Pitch = FMath::ClampAngle(FRotator::NormalizeAxis(R.Pitch) + Signed * 2.5f * LookSensitivity, -80.f, 75.f);
	Controller->SetControlRotation(R);
}

void ARudeSandboxPawn::SprintOn() { if (UCharacterMovementComponent* M = GetCharacterMovement()) { M->MaxWalkSpeed = SprintSpeed; } }
void ARudeSandboxPawn::SprintOff() { if (UCharacterMovementComponent* M = GetCharacterMovement()) { M->MaxWalkSpeed = WalkSpeed; } }
void ARudeSandboxPawn::JumpPressed() { Jump(); }
void ARudeSandboxPawn::JumpReleased() { StopJumping(); }
