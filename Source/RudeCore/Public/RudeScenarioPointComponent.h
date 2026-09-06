// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RudeScenarioPointComponent.generated.h"

// One scenario point (a CScenarioPoint out of a CScenarioPointRegion's top-level MyPoints list) as it
// rides on the marker actor ImportScenarioRegion placed for it. The actor's transform IS the point's
// position and heading (RAGE->UE: metres*100 with the Y mirror; heading w -> UE yaw = -(deg(w) + 90)).
// Everything else the file stores per point lives here, in the file's own field order, and is exported
// from here - the exporter never re-opens the region to rebuild a point.
//
// Identity: which region, which build slot, which ORDINAL in the file's MyPoints list. The format gives a
// point no name and no guid; the ordinal is the only handle, and the region's AccelGridNodeIndices index
// the list by it (measured on downtown 2026-09-06: 320 cells, low-15-bit values non-decreasing 0..823 =
// the point count), so the ordinal is identity, not bookkeeping - a deletion would shift every later one.
UCLASS(ClassGroup = (RUDE), meta = (BlueprintSpawnableComponent))
class RUDECORE_API URudeScenarioPointComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// ---- identity / provenance ------------------------------------------------------------
	// The region (file stem) this point was read from, e.g. "downtown".
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceRegion;

	// The build slot that copy of the region came from ("00_base", "10_update"); empty for an ad-hoc file.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSlot;

	// Ordinal inside the file's top-level <Points>/<MyPoints> list (-1 = authored in UE / duplicated).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	int32 SourceIndex = -1;

	// ---- the eleven ints + Flags, as the file spells them (edit-native; iType/ModelSetId are indices
	// into the region's own LookUps lists, which is why the resolved NAMES ride beside them read-only)
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Scenario")
	FString TypeName;          // LookUps/TypeNames[IType] (display only; IType is what is written)

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Scenario")
	FString ModelSetName;      // Ped/VehicleModelSetNames[ModelSetId] (display only)

	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 IType = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 ModelSetId = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 Interior = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 RequiredIMapId = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 Probability = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 AvailableInMpSp = 1;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 TimeStartOverride = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 TimeEndOverride = 24;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 Radius = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 TimeTillPedLeaves = 0;
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario") int32 ScenarioGroup = 0;

	// <Flags> text as spelled ("NoSpawn, HighPriority"); empty = the file's "<Flags />".
	UPROPERTY(EditAnywhere, Category = "RUDE|Scenario")
	FString Flags;

	// ---- the byte-safe seam ---------------------------------------------------------------
	// The point's own "   <Item>...</Item>\n" slice of the source file's bytes, the transform it was
	// placed with, and the field key at import. Export re-emits SourceXml VERBATIM while the actor
	// still sits at SourceTransform with FieldsKey() unchanged; only an edited point is rebuilt.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FTransform SourceTransform;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceFieldsKey;

	// The w attribute exactly as the file spelled it, and the UE yaw it became. A point that was moved
	// but NOT rotated writes SourceHeadingText back byte-for-byte (a float->degrees->float round trip
	// would otherwise re-spell 1 in ~10 headings in the last digit).
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceHeadingText;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	float SourceYaw = 0.f;

	// One string over every exportable non-transform field, so "did anything change" is one comparison.
	FString FieldsKey() const
	{
		return FString::Printf(TEXT("%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%s"),
			IType, ModelSetId, Interior, RequiredIMapId, Probability, AvailableInMpSp,
			TimeStartOverride, TimeEndOverride, Radius, TimeTillPedLeaves, ScenarioGroup, *Flags);
	}
};
