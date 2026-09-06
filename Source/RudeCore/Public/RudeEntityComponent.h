// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RudeEntityComponent.generated.h"

class AActor;

// One element the importer did not bind to a property, kept exactly as spelled so export can
// re-emit it. Name = element tag; Text = its text content; Attributes = "k=v" pairs in file order.
USTRUCT(BlueprintType)
struct RUDECORE_API FRudeCarriedField
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "RUDE")
	FString Name;

	UPROPERTY(VisibleAnywhere, Category = "RUDE")
	FString Text;

	UPROPERTY(VisibleAnywhere, Category = "RUDE")
	TArray<FString> Attributes;
};

// A placed RAGE entity (a ymap `CEntityDef`) as it rides on the UE actor that represents it.
//
// The actor's transform IS the placement (position / rotation / scale go through the one
// transform module at import and export; they are not duplicated here). Everything else the
// game stores per placement lives on this component, split by how much RUDE understands it:
//   edit-native  - decoded, name proven, a real UE control (the ymap spec's T1 rows)
//   as spelled   - decoded, meaning or control unproven; shown under Advanced, exported verbatim (T2)
//   carried      - anything the importer did not bind; re-emitted as read
// Identity (which file, which slot, which ordinal) is provenance for the Repair/Update flow and
// for export: an entity that came from `dt1_02.ymap` goes back into `dt1_02.ymap`.
UCLASS(ClassGroup = (RUDE), meta = (BlueprintSpawnableComponent))
class RUDECORE_API URudeEntityComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// ---- identity / provenance ------------------------------------------------------------
	// Archetype the placement refers to, as spelled in the file (lower-case name or hash_XXXXXXXX).
	UPROPERTY(EditAnywhere, Category = "RUDE|Entity")
	FString ArchetypeName;

	// The item's type as the file spells it: CEntityDef (a prop) or CMloInstanceDef (a whole interior
	// placed). Wave 1 rebuilds only CEntityDef on edit; an edited MLO instance exports as read.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Entity")
	FString ItemType = TEXT("CEntityDef");

	// The ymap (asset name) this entity was read from; empty for an entity authored in UE.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYmap;

	// The build slot that copy of the ymap came from ("00_base", "20_dlc/062_patchday27ng", ...).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSlot;

	// Ordinal inside the file's <entities> list (-1 = authored in UE). parentIndex values in
	// sibling entities refer to THIS number, so it is identity, not bookkeeping.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	int32 SourceIndex = -1;

	// ---- edit-native (T1) -----------------------------------------------------------------
	UPROPERTY(EditAnywhere, Category = "RUDE|LOD", meta = (ClampMin = "0"))
	float LodDist = 0.f;

	UPROPERTY(EditAnywhere, Category = "RUDE|LOD", meta = (ClampMin = "0"))
	float ChildLodDist = 0.f;

	// LODTYPES_DEPTH_HD / _LOD / _SLOD1 / _SLOD2 / _SLOD3 / _SLOD4 / _ORPHANHD, as the game spells it.
	UPROPERTY(EditAnywhere, Category = "RUDE|LOD")
	FString LodLevel = TEXT("LODTYPES_DEPTH_HD");

	// Index of the parent entity in the PARENT map's list (-1 = none). Lineage, never hierarchy.
	UPROPERTY(EditAnywhere, Category = "RUDE|LOD")
	int32 ParentIndex = -1;

	// The LOD parent as a LINK: the next-coarser entity this one hands over to. Resolved at import by
	// the rule measured on downtown (ENGINEERING_LOG law 24: the parent ymap first, then this ymap,
	// exactly one level coarser - 2,813/2,813 unique) and the thing you EDIT to re-parent. At export
	// parentIndex, numChildren and HD/ORPHANHD are DERIVED from links (laws 25, 27); lodDist and
	// childLodDist never are - they are authored (law 26).
	// SOFT on purpose: every ymap is its own runtime Data Layer and a HARD actor reference across
	// layers is a MapCheck error (measured 2026-09-06); a soft one resolves while the layer is loaded.
	UPROPERTY(EditAnywhere, Category = "RUDE|LOD")
	TSoftObjectPtr<AActor> LodParent;

	// Back-links, maintained by the build and by SetLodParent; LodAudit re-derives them.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|LOD")
	TArray<TSoftObjectPtr<AActor>> LodChildren;

	// Stored numChildren != children present in this level (they live in ymaps outside it): the
	// count stays verbatim and re-parenting under this entity is refused (law 25).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|LOD")
	bool bLodPartial = false;

	// CMapData/parent of the source ymap: the only OTHER file a parent may live in (law 24).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYmapParent;


	// PRI_REQUIRED / PRI_OPTIONAL_HIGH / _MEDIUM / _LOW, as the game spells it.
	UPROPERTY(EditAnywhere, Category = "RUDE|LOD")
	FString PriorityLevel = TEXT("PRI_REQUIRED");

	// The <extensions> subtree verbatim (door, light effect, spawn-point override ...). Wave 1
	// carries it whole; later waves lift each kind onto its own component.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Extensions")
	FString ExtensionsXml;

	// ---- as spelled (T2) ------------------------------------------------------------------
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Advanced (as spelled)")
	uint32 Flags = 0;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Advanced (as spelled)")
	uint32 Guid = 0;

	// Derived by the game's exporter from the lineage; RUDE recomputes it on export.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Advanced (as spelled)")
	int32 NumChildren = 0;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Advanced (as spelled)")
	float AmbientOcclusionMultiplier = 255.f;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Advanced (as spelled)")
	float ArtificialAmbientOcclusion = 255.f;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Advanced (as spelled)")
	uint32 TintValue = 0;

	// ---- carried --------------------------------------------------------------------------
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Carried")
	TArray<FRudeCarriedField> Carried;

	// ---- the byte-safe seam ---------------------------------------------------------------
	// The entity's own <Item type="CEntityDef"> as the file spelled it, and the transform it was
	// placed with. Export re-emits SourceXml VERBATIM while the actor still sits at
	// SourceTransform with SourceFieldsKey unchanged - an untouched entity goes back out exactly
	// as it came in; only an edited one is rebuilt from the fields above.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FTransform SourceTransform;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceFieldsKey;

	// One string over every exportable field, so "did anything change" is one comparison.
	FString FieldsKey() const
	{
		return FString::Printf(TEXT("%s|%g|%g|%s|%d|%s|%s|%u|%u|%d|%g|%g|%u"),
			*ArchetypeName, LodDist, ChildLodDist, *LodLevel, ParentIndex, *PriorityLevel,
			*ExtensionsXml, Flags, Guid, NumChildren, AmbientOcclusionMultiplier,
			ArtificialAmbientOcclusion, TintValue);
	}
};
