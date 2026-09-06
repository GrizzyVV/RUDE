// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeBase.h"
#include "ChaosVehicleWheel.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "WheeledVehiclePawn.h"
#include "RudeDriveablePawn.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UInputComponent;

// THE CHAOS TEST-DRIVE (GDD Tier 2, WP11; design + the handling.meta -> Chaos table: maintainer lane `drive` (`DESIGN.md`)).
//
// A driveable is built by URudeToolset::BuildDriveable from an imported composite: a USkeletalMesh carrying the yft's
// bones (body vertices weighted 1.0 to `chassis`, each placed wheel mesh weighted 1.0 to its wheel_* bone), a
// PhysicsAsset with one chassis body, and this pawn with one FChaosWheelSetup per wheel bone. Two facts shape it:
//   * Chaos reads a wheel's numbers from the WHEEL CLASS's default object (SetupVehicle: WheelClass.GetDefaultObject())
//     and every runtime setter writes through AccessSetup() INTO that CDO - shared by every vehicle of the class. So
//     the per-vehicle numbers live on this pawn's URudeDriveMovement (FRudeDriveWheelParams, saved with the actor) and
//     SetupVehicle re-points each wheel/suspension sim at a config the component OWNS before writing them.
//   * RUDE space puts a car's nose at UE -Y (GTA +Y forward through the pinned (x,-y,z) mirror); Chaos steers along
//     the pawn's +X. BuildDriveable folds a +90 deg yaw into the ROOT bone, so in this mesh the nose is +X, the
//     driver's door is -Y, and a wheel bone's local X is still its axle (the game's wheels spin about local X).
// Input is bound to RAW KEYS like ARudeSandboxPawn (a plugin cannot assume the project's input config).

// ---- the wheel classes ----------------------------------------------------------------------------------------------
// Front: steers, no handbrake. Rear: handbrake, no steer. Both driven; the differential decides which axle gets torque.
// The numbers here are Chaos's own defaults; the per-vehicle values arrive through FRudeDriveWheelParams.
UCLASS()
class RUDECORE_API URudeDriveWheelFront : public UChaosVehicleWheel
{
	GENERATED_BODY()
public:
	URudeDriveWheelFront(const FObjectInitializer& ObjectInitializer);
};

UCLASS()
class RUDECORE_API URudeDriveWheelRear : public UChaosVehicleWheel
{
	GENERATED_BODY()
public:
	URudeDriveWheelRear(const FObjectInitializer& ObjectInitializer);
};

// One wheel's numbers, in Chaos's own units (cm, kg, Nm, degrees; SpringRate in the wheel class's unit where 250 is
// Chaos's default for a 1.5 t car - it is scaled by Chaos::MToCm inside, exactly as UChaosVehicleWheel does).
USTRUCT(BlueprintType)
struct RUDECORE_API FRudeDriveWheelParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") FName BoneName;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") bool bFront = false;
	// The wheel mesh was the other side's prototype mirrored across its axle (a record, not a parameter).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Drive") bool bMirrored = false;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float RadiusCm = 32.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float WidthCm = 20.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float MassKg = 20.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float MaxSteerDeg = 0.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float BrakeTorqueNm = 1500.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float HandbrakeTorqueNm = 0.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float FrictionMultiplier = 2.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float SpringRate = 250.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float SpringPreload = 50.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float DampingRatio = 0.5f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float MaxRaiseCm = 10.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") float MaxDropCm = 10.f;
};

// The movement component: Chaos's wheeled vehicle with per-instance wheel numbers (see the class note above).
UCLASS(ClassGroup = (RUDE), meta = (BlueprintSpawnableComponent))
class RUDECORE_API URudeDriveMovement : public UChaosWheeledVehicleMovementComponent
{
	GENERATED_BODY()
public:
	URudeDriveMovement(const FObjectInitializer& ObjectInitializer);

	// One entry per WheelSetups[] entry, same order. Missing entries keep the wheel class's defaults.
	UPROPERTY(EditAnywhere, Category = "RUDE|Drive") TArray<FRudeDriveWheelParams> WheelParams;
	// "meta field -> Chaos parameter = value (tag)" per mapped line, written by BuildDriveable: what the Details
	// panel and the verdict show so nobody has to guess where a number came from.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Drive") TArray<FString> MappingNotes;

	virtual void SetupVehicle(TUniquePtr<Chaos::FSimpleWheeledVehicle>& PVehicle) override;

private:
	// The configs the sims read through their (public) SetupPtr after SetupVehicle: owned here, never the CDO's.
	TArray<Chaos::FSimpleWheelConfig> OwnedWheelConfigs;
	TArray<Chaos::FSimpleSuspensionConfig> OwnedSuspensionConfigs;
};

// ---- the wheel animation: no anim graph, the pose is written in C++ from the sim ------------------------------
struct FRudeDriveWheelAnim
{
	int32 BoneIndex = INDEX_NONE;
	float SpinDeg = 0.f;
	float SteerDeg = 0.f;
	float OffsetCm = 0.f;
};

// PreUpdate (game thread) reads each wheel's spin / steer / suspension offset; Evaluate (worker thread) writes the ref
// pose with the wheel bones rotated: steer about the bone's local Z, spin about its local X (the axle), the offset
// along local Z - the same three numbers the engine's FAnimNode_WheelController applies, in the frame this mesh has.
class RUDECORE_API FRudeDriveAnimProxy : public FAnimInstanceProxy
{
public:
	FRudeDriveAnimProxy() {}
	FRudeDriveAnimProxy(UAnimInstance* InInstance) : FAnimInstanceProxy(InInstance) {}
	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;
	TArray<FRudeDriveWheelAnim> Wheels;
};

UCLASS(transient)
class RUDECORE_API URudeDriveAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override { return new FRudeDriveAnimProxy(this); }
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override { delete InProxy; }
};

// ---- the pawn -------------------------------------------------------------------------------------------------------
UCLASS(Blueprintable)
class RUDECORE_API ARudeDriveablePawn : public AWheeledVehiclePawn
{
	GENERATED_BODY()
public:
	ARudeDriveablePawn(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Drive") TObjectPtr<USpringArmComponent> SpringArm;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Drive") TObjectPtr<UCameraComponent> Camera;
	// The game's model name (blista); `Rude.Native EnterVehicle <VehicleName>` finds the pawn by it (and by tag).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "RUDE|Drive") FString VehicleName;
	// The URudeVehicle DataAsset the pawn was built from (content path), for the record.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Drive") FString VehicleAssetPath;

	URudeDriveMovement* GetDriveMovement() const;

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void UnPossessed() override;

private:
	// digital keys read as axes (1 while held); combined in Tick so W and S never fight over one input
	float KeyW = 0.f, KeyS = 0.f, KeyA = 0.f, KeyD = 0.f;
	void AxisW(float V) { KeyW = V; }
	void AxisS(float V) { KeyS = V; }
	void AxisA(float V) { KeyA = V; }
	void AxisD(float V) { KeyD = V; }
	void HandbrakeOn();
	void HandbrakeOff();
	void ExitPressed();
	void UprightPressed();
};
