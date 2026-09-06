// RUDE - RAGE <-> Unreal Development Environment
// One entity of an MLO interior (a CEntityDef inside a ytyp's CMloArchetypeDef - the archetype's own
// <entities>, or one entity set's <entities>) as it rides on the UE actor ImportMlo spawns for it.
//
// Why this is NOT URudeEntityComponent: that component's identity is (ymap, ordinal) and every ymap
// tool sweeps the level for it - ExportLevelYmaps groups every one it finds under a source ymap,
// MoveRudeEntity / LodAudit / SetLodParent / ImportCarGenerators iterate it too. An interior entity
// carrying it (or a subclass of it) would be exported into a ymap. Its identity is
// (interior archetype, entity set, ordinal), its file is a ytyp, and it has no LOD lineage at all
// (measured 2026-09-06 over 67,440/67,440 MLO entities: parentIndex -1, numChildren 0,
// lodLevel ORPHANHD - scratchpad/wp11/mlo_export/LAWS.md).
//
// The actor's transform IS the placement (MLO-local: the interior root sits at the world origin;
// export reads the transform RELATIVE to the root, so a root moved as a whole changes nothing).
// The export re-emits SourceXml VERBATIM while the actor still sits at SourceTransform; a moved
// entity has ONLY its <position>/<rotation> lines re-spelled inside that slice. Nothing else is
// mirrored as fields: the round trip is the slice.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RudeMloEntityComponent.generated.h"

UCLASS(ClassGroup = (RUDE), meta = (BlueprintSpawnableComponent))
class RUDECORE_API URudeMloEntityComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	// The CMloArchetypeDef <name> as the corpus spells it (lower-case name or hash_XXXXXXXX).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MLO")
	FString Interior;

	// Empty = the archetype's own <entities>; else the <name> of the entity set this one belongs to.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MLO")
	FString SetName;

	// Ordinal inside its <entities> list (-1 = authored in UE / a duplicate). Rooms' and portals'
	// <attachedObjects> index the top-level list by THIS number, so it is identity, not bookkeeping.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MLO")
	int32 SourceIndex = -1;

	// The room (index into <rooms>) whose attachedObjects lists this entity; for a set entity the
	// value of its <locations> slot. -1 = none (a portal door, or unroomed).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MLO")
	int32 RoomIndex = -1;

	// The portal (index into <portals>) whose attachedObjects lists this entity; -1 = none.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MLO")
	int32 PortalIndex = -1;

	// Archetype the placement refers to, as spelled in the file.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MLO")
	FString ArchetypeName;

	// The ytyp (asset name, lower-case) the interior was read from, and the file's own path at
	// import time - the export splices THAT file's bytes and refuses a different copy.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYtyp;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceFile;

	// The entity's own <Item type="CEntityDef"> slice EXACTLY as the file spells it (its indentation
	// included; ends with the newline after </Item>). Empty for an entity built with no template.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;

	// The MLO-local transform it was placed with (UE cm, the import lane's Y mirror).
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FTransform SourceTransform;
};
