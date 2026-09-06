// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RudePathNodeComponent.generated.h"

// One link out of a path node (a ynd <Links><Item>): the node it reaches, as cell + ordinal, and the
// three flag bytes and the length the game stores per link. Measured over 5 cells (WP10 LAWS.md):
// 12,776 links, every in-cell link has its reciprocal on the far node (12,524/12,524), so one road
// segment is stored twice - once per end. Cross-cell links go only to the four edge neighbours
// (cell +-1, +-32). LinkLength is an integer close to the 3D distance in metres (mean +0.26..+0.54 m
// over the actual distance; an exact rule was NOT found), so RUDE never rewrites it.
USTRUCT(BlueprintType)
struct RUDECORE_API FRudePathLink
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "RUDE")
	int32 ToAreaID = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE")
	int32 ToNodeID = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags0 = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags1 = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags2 = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE", meta = (ClampMin = "0", ClampMax = "255"))
	int32 LinkLength = 0;
};

// A vehicle/ped path node (one <Item> of a ynd's <Nodes>) as it rides on the UE actor that represents it.
//
// The actor's location IS the node position (GTA metres <-> UE cm with Y mirrored, through the same rule
// the entity lane uses). Everything else the game stores per node lives here:
//   identity   AreaID (= the cell), NodeID (= ordinal in the file; link targets refer to it - never edited)
//   fields     StreetName, Flags0..Flags5 (bytes; meanings unproven, exported as numbers), Links
//   junction   when a JunctionRefs row names this node: the junction's heightmap block, carried for
//              inspection; Wave 1 writes Junctions/JunctionRefs back VERBATIM, so edits here do not
//              reach the file (the export counts them as junctionEditsNotWritten)
//   provenance which cell file, which build slot, which ordinal, the item's own bytes (SourceXml), the
//              transform it was placed with and the fields key it was read with - the byte-safe seam:
//              while transform and key are unchanged the node goes out exactly as it came in.
UCLASS(ClassGroup = (RUDE), meta = (BlueprintSpawnableComponent))
class RUDECORE_API URudePathNodeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// ---- identity ---------------------------------------------------------------------------
	// The cell index this node belongs to (= the file's number: nodes464 -> 464). Never edited.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Path Node")
	int32 AreaID = 0;

	// Ordinal in the file's <Nodes> list. Links in this and neighbouring cells refer to it: identity.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Path Node")
	int32 NodeID = 0;

	// Index >= VehicleNodeCount in the file: the ped (footpath) partition. Measured: vehicle nodes
	// first, ped nodes after, each partition sorted by Y ascending (5/5 cells).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Path Node")
	bool bPedNode = false;

	// ---- fields (as spelled: names proven, meanings not) ------------------------------------
	// Empty or a hash_XXXXXXXX (5,988 nodes measured: 4,043 hashes, 1,945 empty, 0 plain names).
	UPROPERTY(EditAnywhere, Category = "RUDE|Path Node")
	FString StreetName;

	UPROPERTY(EditAnywhere, Category = "RUDE|Flags", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags0 = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|Flags", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags1 = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|Flags", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags2 = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|Flags", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags3 = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|Flags", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags4 = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|Flags", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Flags5 = 0;

	// Links out of this node, in file order.
	UPROPERTY(EditAnywhere, Category = "RUDE|Links")
	TArray<FRudePathLink> Links;

	// ---- junction (carried) -----------------------------------------------------------------
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	bool bJunction = false;

	// Ordinal in the file's <Junctions> list (JunctionRefs: JunctionID). Measured sequential 0..n-1.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	int32 JunctionID = -1;

	// The junction heightmap's origin corner in GTA metres (x, y): the node sits 2..39 m to the +x/+y
	// side of it in every measured case (236/236 non-negative offsets).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	FVector2D JunctionPosition = FVector2D::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	float JunctionMinZ = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	float JunctionMaxZ = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	int32 JunctionSizeX = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	int32 JunctionSizeY = 0;

	// SizeY rows of SizeX hex bytes (00..FF), as spelled; measured 236/236 junctions match their sizes.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	TArray<FString> JunctionHeightmapRows;

	// JunctionRefs/Unk0: 236/236 measured are 0.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Junction")
	int32 JunctionUnk0 = 0;

	// ---- provenance / the byte-safe seam ----------------------------------------------------
	// The cell file (asset name, "nodes464") this node was read from; empty for a node authored in UE.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYnd;

	// The build slot that copy came from ("10_update" for every downtown cell measured).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSlot;

	// Ordinal in the file (-1 = authored in UE; Wave 1 does not write those).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	int32 SourceIndex = -1;

	// The node's own "  <Item>...</Item>\n" as the file spelled it.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FTransform SourceTransform;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceFieldsKey;

	// The junction item and the JunctionRefs row verbatim (only on a junction node).
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceJunctionXml;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceJunctionRefXml;

	int32 FlagAt(int32 K) const
	{
		switch (K) { case 0: return Flags0; case 1: return Flags1; case 2: return Flags2; case 3: return Flags3; case 4: return Flags4; default: return Flags5; }
	}

	// One string over every exportable field, so "did anything change" is one comparison. Position is
	// the actor transform and is compared separately (SourceTransform).
	FString FieldsKey() const
	{
		FString K = FString::Printf(TEXT("%s|%d|%d|%d|%d|%d|%d|"), *StreetName, Flags0, Flags1, Flags2, Flags3, Flags4, Flags5);
		for (const FRudePathLink& L : Links)
		{
			K += FString::Printf(TEXT("%d:%d:%d:%d:%d:%d;"), L.ToAreaID, L.ToNodeID, L.Flags0, L.Flags1, L.Flags2, L.LinkLength);
		}
		return K;
	}
};
