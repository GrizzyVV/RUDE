// RUDE - RAGE <-> Unreal Development Environment
#include "RudeDriveablePawn.h"
#include "RudeNativeShim.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Controller.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputCoreTypes.h"

// ---- wheel classes -------------------------------------------------------------------------------------------------
URudeDriveWheelFront::URudeDriveWheelFront(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AxleType = EAxleType::Front;
	bAffectedBySteering = true;
	bAffectedByHandbrake = false;
	bAffectedByEngine = true;
	MaxSteerAngle = 35.f;
	SweepShape = ESweepShape::Raycast;
}

URudeDriveWheelRear::URudeDriveWheelRear(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AxleType = EAxleType::Rear;
	bAffectedBySteering = false;
	bAffectedByHandbrake = true;
	bAffectedByEngine = true;
	MaxSteerAngle = 0.f;
	SweepShape = ESweepShape::Raycast;
}

// ---- movement -------------------------------------------------------------------------------------------------------
URudeDriveMovement::URudeDriveMovement(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// S while rolling forward brakes; S from rest reverses (Chaos's arcade rule; the on-foot pawn's WASD reads the same)
	bReverseAsBrake = true;
}

void URudeDriveMovement::SetupVehicle(TUniquePtr<Chaos::FSimpleWheeledVehicle>& PVehicle)
{
	Super::SetupVehicle(PVehicle);
	if (!PVehicle.IsValid()) { return; }
	const int32 N = FMath::Min(PVehicle->Wheels.Num(), PVehicle->Suspension.Num());
	OwnedWheelConfigs.SetNum(N);
	OwnedSuspensionConfigs.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		Chaos::FSimpleWheelSim& W = PVehicle->Wheels[i];
		Chaos::FSimpleSuspensionSim& S = PVehicle->Suspension[i];
		// start from what Super built (the class defaults + the differential's EngineEnabled / TorqueRatio pass),
		// then own it: the sim's SetupPtr is public for exactly this
		OwnedWheelConfigs[i] = W.Setup();
		OwnedSuspensionConfigs[i] = S.Setup();
		W.SetupPtr = &OwnedWheelConfigs[i];
		S.SetupPtr = &OwnedSuspensionConfigs[i];
		if (!WheelParams.IsValidIndex(i)) { continue; }
		const FRudeDriveWheelParams& P = WheelParams[i];
		Chaos::FSimpleWheelConfig& WC = OwnedWheelConfigs[i];
		WC.WheelRadius = P.RadiusCm;
		WC.WheelWidth = P.WidthCm;
		WC.WheelMass = P.MassKg;
		WC.MaxSteeringAngle = FMath::RoundToInt(P.MaxSteerDeg);
		WC.SteeringEnabled = P.bFront && P.MaxSteerDeg > 0.f;
		WC.MaxBrakeTorque = P.BrakeTorqueNm;
		WC.HandbrakeTorque = P.HandbrakeTorqueNm;
		WC.HandbrakeEnabled = P.HandbrakeTorqueNm > 0.f;
		WC.FrictionMultiplier = P.FrictionMultiplier;
		// the sim keeps public copies of the flags it consults per tick - set both
		W.SetWheelRadius(P.RadiusCm);
		W.SteeringEnabled = WC.SteeringEnabled;
		W.HandbrakeEnabled = WC.HandbrakeEnabled;
		W.FrictionMultiplier = WC.FrictionMultiplier;
		W.MaxSteeringAngle = (float)WC.MaxSteeringAngle;
		Chaos::FSimpleSuspensionConfig& SC = OwnedSuspensionConfigs[i];
		SC.SpringRate = Chaos::MToCm(P.SpringRate);
		SC.SpringPreload = Chaos::MToCm(P.SpringPreload);
		SC.DampingRatio = P.DampingRatio;
		SC.SetSuspensionMaxRaise(P.MaxRaiseCm);
		SC.SetSuspensionMaxDrop(P.MaxDropCm);
	}
	// Super derived the damping rates, resting forces and resting positions from the CLASS numbers; derive again
	// from the numbers above (SetupSuspension reads SpringRate / DampingRatio / MaxRaise / MaxDrop through the sims).
	SetupSuspension(PVehicle);
}

// ---- wheel animation ------------------------------------------------------------------------------------------------
void FRudeDriveAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
	Wheels.Reset();
	const AWheeledVehiclePawn* Vehicle = InAnimInstance ? Cast<AWheeledVehiclePawn>(InAnimInstance->GetOwningActor()) : nullptr;
	const UChaosWheeledVehicleMovementComponent* Mv = Vehicle ? Cast<UChaosWheeledVehicleMovementComponent>(Vehicle->GetVehicleMovement()) : nullptr;
	const USkeletalMeshComponent* Mesh = InAnimInstance ? InAnimInstance->GetSkelMeshComponent() : nullptr;
	if (!Mv || !Mesh) { return; }
	for (int32 i = 0; i < Mv->WheelSetups.Num() && i < Mv->Wheels.Num(); ++i)
	{
		FRudeDriveWheelAnim A;
		A.BoneIndex = Mesh->GetBoneIndex(Mv->WheelSetups[i].BoneName);
		if (const UChaosVehicleWheel* W = Mv->Wheels[i].Get())
		{
			A.SpinDeg = W->GetRotationAngle();
			A.SteerDeg = W->GetSteerAngle();
			A.OffsetCm = W->GetSuspensionOffset();
		}
		Wheels.Add(A);
	}
}

bool FRudeDriveAnimProxy::Evaluate(FPoseContext& Output)
{
	Output.ResetToRefPose();
	const FBoneContainer& BC = Output.Pose.GetBoneContainer();
	for (const FRudeDriveWheelAnim& A : Wheels)
	{
		if (A.BoneIndex == INDEX_NONE) { continue; }
		const FCompactPoseBoneIndex CI = BC.MakeCompactPoseIndex(FMeshPoseBoneIndex(A.BoneIndex));
		if (CI.GetInt() < 0) { continue; }
		FTransform& T = Output.Pose[CI];
		// local frame: Z = up (steer axis), X = the axle (spin axis). Q = Steer * Spin * Ref applies Ref, then Spin,
		// then Steer (FQuat A*B applies B first) - the same "spin the steered axle" the engine's node composes.
		const FQuat Steer(FVector::ZAxisVector, FMath::DegreesToRadians(A.SteerDeg));
		const FQuat Spin(FVector::XAxisVector, FMath::DegreesToRadians(A.SpinDeg));
		T.SetRotation((Steer * Spin * T.GetRotation()).GetNormalized());
		T.AddToTranslation(FVector(0.f, 0.f, A.OffsetCm));
	}
	return true;
}

// ---- the pawn -------------------------------------------------------------------------------------------------------
ARudeDriveablePawn::ARudeDriveablePawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<URudeDriveMovement>(AWheeledVehiclePawn::VehicleMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;
	USkeletalMeshComponent* M = GetMesh();
	M->BodyInstance.bSimulatePhysics = true;       // the parent leaves it off (its template Blueprint turns it on)
	M->SetAnimInstanceClass(URudeDriveAnimInstance::StaticClass());
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(M);
	SpringArm->TargetArmLength = 750.f;
	SpringArm->SocketOffset = FVector(0.f, 0.f, 180.f);
	SpringArm->SetRelativeRotation(FRotator(-12.f, 0.f, 0.f));
	SpringArm->bUsePawnControlRotation = false;    // a chase camera: follows the car's yaw, ignores its pitch/roll
	SpringArm->bInheritPitch = false;
	SpringArm->bInheritRoll = false;
	SpringArm->bInheritYaw = true;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 6.f;
	SpringArm->bEnableCameraRotationLag = true;
	SpringArm->CameraRotationLagSpeed = 6.f;
	SpringArm->bDoCollisionTest = true;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;
}

URudeDriveMovement* ARudeDriveablePawn::GetDriveMovement() const
{
	return Cast<URudeDriveMovement>(GetVehicleMovement());
}

void ARudeDriveablePawn::BeginPlay()
{
	Super::BeginPlay();
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 8.f, FColor::Cyan,
			FString::Printf(TEXT("[RUDE Drive] %s ready - Rude.Native EnterVehicle %s  (W/S throttle-brake, A/D steer, Space handbrake, F exit, R upright)"),
				*VehicleName, *VehicleName));
	}
}

void ARudeDriveablePawn::SetupPlayerInputComponent(UInputComponent* PIC)
{
	Super::SetupPlayerInputComponent(PIC);
	if (!PIC) { return; }
	PIC->BindAxisKey(EKeys::W, this, &ARudeDriveablePawn::AxisW);
	PIC->BindAxisKey(EKeys::S, this, &ARudeDriveablePawn::AxisS);
	PIC->BindAxisKey(EKeys::A, this, &ARudeDriveablePawn::AxisA);
	PIC->BindAxisKey(EKeys::D, this, &ARudeDriveablePawn::AxisD);
	PIC->BindAxisKey(EKeys::Gamepad_RightTriggerAxis, this, &ARudeDriveablePawn::AxisW);
	PIC->BindAxisKey(EKeys::Gamepad_LeftTriggerAxis, this, &ARudeDriveablePawn::AxisS);
	PIC->BindAxisKey(EKeys::Gamepad_LeftX, this, &ARudeDriveablePawn::AxisD);
	PIC->BindKey(EKeys::SpaceBar, IE_Pressed, this, &ARudeDriveablePawn::HandbrakeOn);
	PIC->BindKey(EKeys::SpaceBar, IE_Released, this, &ARudeDriveablePawn::HandbrakeOff);
	PIC->BindKey(EKeys::F, IE_Pressed, this, &ARudeDriveablePawn::ExitPressed);
	PIC->BindKey(EKeys::R, IE_Pressed, this, &ARudeDriveablePawn::UprightPressed);
}

void ARudeDriveablePawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Controller) { return; }
	if (UChaosVehicleMovementComponent* Mv = GetVehicleMovement())
	{
		Mv->SetThrottleInput(FMath::Clamp(KeyW - KeyS, -1.f, 1.f));   // negative = brake / reverse (bReverseAsBrake)
		Mv->SetSteeringInput(FMath::Clamp(KeyD - KeyA, -1.f, 1.f));
	}
}

void ARudeDriveablePawn::UnPossessed()
{
	Super::UnPossessed();
	KeyW = KeyS = KeyA = KeyD = 0.f;
	if (UChaosVehicleMovementComponent* Mv = GetVehicleMovement())
	{
		Mv->SetThrottleInput(0.f);
		Mv->SetSteeringInput(0.f);
		Mv->SetHandbrakeInput(true);   // parked
	}
}

void ARudeDriveablePawn::HandbrakeOn() { if (UChaosVehicleMovementComponent* Mv = GetVehicleMovement()) { Mv->SetHandbrakeInput(true); } }
void ARudeDriveablePawn::HandbrakeOff() { if (UChaosVehicleMovementComponent* Mv = GetVehicleMovement()) { Mv->SetHandbrakeInput(false); } }

void ARudeDriveablePawn::ExitPressed()
{
	if (URudeNativeShim* Shim = URudeNativeShim::Get(this)) { Shim->ExitVehicle(); }
}

// R: put the car back on its wheels where it is (a test-drive rolls; a rolled test-drive should not end the test)
void ARudeDriveablePawn::UprightPressed()
{
	const FRotator R(0.f, GetActorRotation().Yaw, 0.f);
	SetActorLocationAndRotation(GetActorLocation() + FVector(0.f, 0.f, 120.f), R, false, nullptr, ETeleportType::TeleportPhysics);
	if (USkeletalMeshComponent* M = GetMesh())
	{
		M->SetPhysicsLinearVelocity(FVector::ZeroVector);
		M->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
	}
	if (UChaosVehicleMovementComponent* Mv = GetVehicleMovement()) { Mv->ResetVehicle(); }
}
