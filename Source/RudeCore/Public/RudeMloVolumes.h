// RUDE - RAGE <-> Unreal Development Environment
// MLO AUTHORING (GDD Tier 1 interiors: import-AUTHOR-export). The two volume actors an author drags in a
// viewport to describe a NEW interior, and which ExportNewMlo reads to write a CMloArchetypeDef.
//
// The surface deliberately does NOT force one workflow: nothing here owns the entities. A room is a box you
// place; a portal is a thin box you place across a doorway; a prop is ANY static-mesh actor standing inside a
// room box. Delete a volume and the interior loses a room - there is no registry to keep in step.
//
// Why the fields live on the volume rather than in a config: a CMloRoomDef's fields ARE per-room (name, flags,
// blend, its two timecycle names, floorId, exteriorVisibiltyDepth - the game's own misspelling, kept), so the
// thing that carries them is the thing you select. Two fields are NOT here on purpose:
//   * bbMin/bbMax    - the box IS the bounds (the AABB of the box's eight transformed corners, so a rotated
//                      volume still writes an axis-aligned room; CMloRoomDef has no orientation).
//   * portalCount    - DERIVED. It equals the number of portals naming this room in 2,143/2,143 corpus rooms
//                      (maintainer lane `mlo_author` (`LAWS.md`) law 4), so an authored value could only ever
//                      be wrong. ExportNewMlo computes it.
// A portal likewise carries only its own scalars: roomFrom/roomTo come from the room volumes it TOUCHES
// (LAWS.md law 9: roomFrom is never limbo in 3,466/3,466, so a portal touching one room exits to limbo), and
// the four corners come from the mid-plane of its thinnest axis.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RudeMloVolumes.generated.h"

class UBoxComponent;

// One room of an interior: a box volume carrying the room's own CMloRoomDef fields.
// Defaults are the corpus modes (LAWS.md law 3): flags 96 (685/1,602 non-limbo rooms), blend 1 (1,598/1,602),
// floorId 0 (1,183/1,602), exteriorVisibiltyDepth -1 (1,602/1,602), timecycle names EMPTY (the game names one
// in 1,599/1,602 but every name is an unrecoverable joaat hash - the author supplies a real one or none).
UCLASS(ClassGroup = (RUDE), meta = (DisplayName = "RUDE MLO Room Volume"))
class RUDECORE_API ARudeMloRoomVolume : public AActor
{
	GENERATED_BODY()

public:
	ARudeMloRoomVolume();

	// The room's extent. Scale and rotate it in the viewport; the export writes the AABB of its corners.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|MLO")
	TObjectPtr<UBoxComponent> Box;

	// The CMloArchetypeDef <name> this room belongs to (matches the interior root's RUDE_MLO: tag).
	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	FString Interior;

	// <name>. Unique inside one interior in 541/541 archetypes - a repeat is refused at export.
	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	FString RoomName = TEXT("room");

	// THE limbo room: index 0, named "limbo" in 541/541 archetypes, the room every unroomed prop attaches to.
	// Exactly one per interior; when none is marked, ExportNewMlo synthesizes it from the union of the others.
	// Marking it FORCES law 3's five 541/541 values - the name `limbo` plus Flags 96, Blend 1, an EMPTY
	// TimecycleName and ExteriorVisibiltyDepth -1 - whatever this panel says, and the export counts the
	// overrides as `limboFieldsForced` so the change is visible. FloorId is left alone: it was never binned
	// for room 0.
	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	bool bLimbo = false;

	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	int32 Flags = 96;

	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	float Blend = 1.f;

	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	FString TimecycleName;

	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	FString SecondaryTimecycleName;

	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	int32 FloorId = 0;

	// The game's own spelling of the tag (2,143/2,143) - the typo is the contract.
	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Room")
	int32 ExteriorVisibiltyDepth = -1;
};

// One portal of an interior: a THIN box across a doorway. Its thinnest axis picks the plane; the four corners
// of that plane become <corners> (LAWS.md law 8: 4 coplanar corners spelled "x, y, z, NaN" in 13,864/13,864).
// roomFrom / roomTo come from the room volumes it overlaps.
UCLASS(ClassGroup = (RUDE), meta = (DisplayName = "RUDE MLO Portal Volume"))
class RUDECORE_API ARudeMloPortalVolume : public AActor
{
	GENERATED_BODY()

public:
	ARudeMloPortalVolume();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|MLO")
	TObjectPtr<UBoxComponent> Box;

	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Portal")
	FString Interior;

	// Corpus modal flags is 64 (1,164/3,466), then 0 (543). 0 is the default because no bit's MEANING was
	// measured - a default of 64 would be an unexplained behaviour the author never asked for.
	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Portal")
	int32 Flags = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Portal")
	int32 MirrorPriority = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Portal")
	int32 Opacity = 0;

	// A joaat hash in the file (values up to 3,148,486,306), so it is held wide enough to survive; 0 in
	// 3,406/3,466 portals.
	UPROPERTY(EditAnywhere, Category = "RUDE|MLO Portal")
	int64 AudioOcclusion = 0;

	// Optional overrides for the room pair when the volumes cannot decide (a portal touching three rooms).
	// Empty = derive from the touching volumes, which is the normal path.
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|MLO Portal")
	FString RoomFromName;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|MLO Portal")
	FString RoomToName;
};
