// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/StaticMesh.h"
#include "RudeArchetype.generated.h"

// One ytyp archetype as a palette entry: never in-scene, the definition placements refer to.
// Fields by tier (ytyp spec 2026-09-03): 13 edit-native, 2 as-spelled, the <extensions> subtree
// carried verbatim; a time archetype adds its hour mask; an MLO archetype carries its rooms /
// portals / entity-sets / entities subtrees verbatim (Wave 1 does not rebuild those on edit).
// Vectors and distances are spelled in RAGE units (metres, RAGE axes) - this is data, not a placement.
// The byte-safe seam is the same as the entity's: SourceXml + SourceFieldsKey.
UCLASS(BlueprintType)
class RUDECORE_API URudeArchetype : public UDataAsset
{
	GENERATED_BODY()

public:
	// CBaseArchetypeDef / CTimeArchetypeDef / CMloArchetypeDef, as the file spells it.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Archetype")
	FString ArchetypeKind = TEXT("CBaseArchetypeDef");

	UPROPERTY(EditAnywhere, Category = "RUDE|Archetype")
	FString Name;

	UPROPERTY(EditAnywhere, Category = "RUDE|Archetype")
	FString AssetName;

	// ASSET_TYPE_DRAWABLE / ASSET_TYPE_FRAGMENT / ASSET_TYPE_DRAWABLEDICTIONARY / ASSET_TYPE_ASSETLESS
	UPROPERTY(EditAnywhere, Category = "RUDE|Archetype")
	FString AssetType = TEXT("ASSET_TYPE_DRAWABLE");

	UPROPERTY(EditAnywhere, Category = "RUDE|Archetype")
	FString TextureDictionary;

	UPROPERTY(EditAnywhere, Category = "RUDE|Archetype")
	FString PhysicsDictionary;

	UPROPERTY(EditAnywhere, Category = "RUDE|Archetype")
	FString DrawableDictionary;

	UPROPERTY(EditAnywhere, Category = "RUDE|Archetype")
	FString ClipDictionary;

	UPROPERTY(EditAnywhere, Category = "RUDE|Streaming", meta = (ClampMin = "0"))
	float LodDist = 0.f;

	UPROPERTY(EditAnywhere, Category = "RUDE|Streaming", meta = (ClampMin = "0"))
	float HdTextureDist = 0.f;

	// Bounds as spelled (RAGE metres). Rebuilt from the mesh on export when the mesh was edited.
	UPROPERTY(EditAnywhere, Category = "RUDE|Bounds")
	FVector BbMin = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "RUDE|Bounds")
	FVector BbMax = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "RUDE|Bounds")
	FVector BsCentre = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "RUDE|Bounds", meta = (ClampMin = "0"))
	float BsRadius = 0.f;

	// Hour mask of a CTimeArchetypeDef (bit N = visible at hour N); 0 = not a time archetype.
	UPROPERTY(EditAnywhere, Category = "RUDE|Time")
	uint32 TimeFlags = 0;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Advanced (as spelled)")
	uint32 Flags = 0;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Advanced (as spelled)")
	uint32 SpecialAttribute = 0;

	// <extensions> subtree verbatim.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Extensions")
	FString ExtensionsXml;

	// CMloArchetypeDef only: everything after the base fields (mloFlags, entities, rooms, portals,
	// entitySets ...) verbatim. Wave 1 carries it; the interior lane lifts it later.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Interior")
	FString MloXml;

	// The imported drawable, when RUDE has it.
	UPROPERTY(EditAnywhere, Category = "RUDE|Archetype")
	TSoftObjectPtr<UStaticMesh> Mesh;

	// ---- provenance + the byte-safe seam ----
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYtyp;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSlot;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	int32 SourceIndex = -1;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceFieldsKey;

	FString FieldsKey() const
	{
		return FString::Printf(TEXT("%s|%s|%s|%s|%s|%s|%s|%g|%g|%s|%s|%s|%g|%u|%u|%u|%s"),
			*Name, *AssetName, *AssetType, *TextureDictionary, *PhysicsDictionary, *DrawableDictionary,
			*ClipDictionary, LodDist, HdTextureDist, *BbMin.ToString(), *BbMax.ToString(), *BsCentre.ToString(),
			BsRadius, TimeFlags, Flags, SpecialAttribute, *ExtensionsXml);
	}
};
