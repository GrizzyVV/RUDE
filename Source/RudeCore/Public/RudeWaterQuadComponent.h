// RUDE - RAGE <-> Unreal Development Environment
// One quad of the game's water table as a marker actor's component: the fields edit-native, the
// source slice kept for a verbatim round trip - the same shape URudeCarGenComponent uses.
//
// WHAT THE FILE HOLDS (measured 2026-09-11 on 00_base/common.rpf/data/levels/gta5/water.xml,
// 329,478 bytes): THREE sections, and every item in all three carries the same four bounds.
//   WaterQuads   504  minX maxX minY maxY | Type IsInvisible HasLimitedDepth z a1 a2 a3 a4 NoStencil
//   CalmingQuads 542  minX maxX minY maxY | fDampening
//   WaveQuads    116  minX maxX minY maxY | Amplitude XDirection YDirection
// 1,162 items, 1,162 of each bound - so the bounds are universal and the rest is per-kind. Nothing
// here is inferred: the counts come from the file and the field lists are its own element names.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RudeWaterQuadComponent.generated.h"

UENUM(BlueprintType)
enum class ERudeWaterQuadKind : uint8
{
	Water   UMETA(DisplayName = "Water"),
	Calming UMETA(DisplayName = "Calming"),
	Wave    UMETA(DisplayName = "Wave"),
};

UCLASS(ClassGroup = (RUDE), meta = (BlueprintSpawnableComponent))
class RUDECORE_API URudeWaterQuadComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	// which section of water.xml this row came from, and must go back to
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Water")
	ERudeWaterQuadKind Kind = ERudeWaterQuadKind::Water;

	// ---- WaterQuads only ----
	UPROPERTY(EditAnywhere, Category = "RUDE|Water")
	int32 Type = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Water")
	bool bIsInvisible = false;
	UPROPERTY(EditAnywhere, Category = "RUDE|Water")
	bool bHasLimitedDepth = false;
	UPROPERTY(EditAnywhere, Category = "RUDE|Water")
	bool bNoStencil = false;
	// the four corner alphas, in the file's own order
	UPROPERTY(EditAnywhere, Category = "RUDE|Water", meta = (ClampMin = "0", ClampMax = "255"))
	int32 A1 = 26;
	UPROPERTY(EditAnywhere, Category = "RUDE|Water", meta = (ClampMin = "0", ClampMax = "255"))
	int32 A2 = 26;
	UPROPERTY(EditAnywhere, Category = "RUDE|Water", meta = (ClampMin = "0", ClampMax = "255"))
	int32 A3 = 26;
	UPROPERTY(EditAnywhere, Category = "RUDE|Water", meta = (ClampMin = "0", ClampMax = "255"))
	int32 A4 = 26;

	// ---- CalmingQuads only ----
	UPROPERTY(EditAnywhere, Category = "RUDE|Water")
	float Dampening = 0.f;

	// ---- WaveQuads only ----
	UPROPERTY(EditAnywhere, Category = "RUDE|Water")
	float Amplitude = 0.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Water")
	float XDirection = 0.f;
	UPROPERTY(EditAnywhere, Category = "RUDE|Water")
	float YDirection = 0.f;

	// ---- the source slice: what a verbatim carry needs -------------------------------------------
	// The quad's bounds and height live on the ACTOR's transform + scale, not here, so that moving the
	// marker in the viewport IS the edit. These remember what the file said, so an untouched quad can
	// be written back as its own bytes and a moved one can be told apart from a rounding difference.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceFile;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	int32 SourceIndex = -1;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	float SourceMinX = 0.f;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	float SourceMaxX = 0.f;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	float SourceMinY = 0.f;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	float SourceMaxY = 0.f;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	float SourceZ = 0.f;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceFieldsKey;

	// Only the per-kind FIELDS - the geometry is compared separately, against the transform.
	FString FieldsKey() const
	{
		switch (Kind)
		{
		case ERudeWaterQuadKind::Calming:
			return FString::Printf(TEXT("calming|%g"), Dampening);
		case ERudeWaterQuadKind::Wave:
			return FString::Printf(TEXT("wave|%g|%g|%g"), Amplitude, XDirection, YDirection);
		default:
			return FString::Printf(TEXT("water|%d|%d|%d|%d|%d|%d|%d|%d"), Type, bIsInvisible ? 1 : 0,
				bHasLimitedDepth ? 1 : 0, bNoStencil ? 1 : 0, A1, A2, A3, A4);
		}
	}
};
