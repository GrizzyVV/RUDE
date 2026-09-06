// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "RudeVehicleAsset.generated.h"

// One <Physics><LOD1> child of a vehicle fragment: a bone frame + a bound (+ a mesh when the child carries
// geometry - the wheel prototype). Measured 2026-09-06 on blista / taxi / burrito (scratchpad/wp10/vehicles/
// LAWS.md): 21 / 26 / 28 children, one per group (burrito: 28 children over 22 groups), EVERY child with a
// <Drawable> header, only wheel_lf's with models; doors/bonnet/boot are skinned parts of the main drawable.
// Bound children pair with physics children by ordinal (Geometry per body part, Disc per wheel, one Box).
USTRUCT(BlueprintType)
struct FRudeVehicleChild
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") int32 ChildIndex = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") int32 GroupIndex = -1;
	// The group's name == the bone it hangs on (bonnet, boot, door_dside_f, wheel_lf ...).
	UPROPERTY(EditAnywhere, Category = "RUDE|Vehicle") FString GroupName;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") int32 ParentGroupIndex = 255;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") int32 BoneTag = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") FString BoneName;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") int32 BoneIndex = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") bool bHasGeometry = false;
	UPROPERTY(EditAnywhere, Category = "RUDE|Vehicle") TSoftObjectPtr<UStaticMesh> Mesh;
	// The component ImportVehicleComposite placed for it (Child_NN_<group>, or Wheel_* for the wheel bones).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") FString ComponentName;
	// The bound child of the same ordinal under <Archetype><Bounds><Children>: type + box AS SPELLED (RAGE metres).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Bound") FString BoundType;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Bound") FString BoundBoxMin;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Bound") FString BoundBoxMax;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Physics") FString PristineMass;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Physics") FString DamagedMass;
};

// One livery: the <veh>_sign_<n> texture pair (base ytd / +hi ytd). Index = n-1 (inferred from burrito: its
// carvariations rows enable liveries 0..2 and the ytd carries _sign_1.._sign_4; the runtime swap is not
// observable offline, so the mapping is carried as data, never asserted).
USTRUCT(BlueprintType)
struct FRudeVehicleLivery
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") int32 Index = -1;
	UPROPERTY(EditAnywhere, Category = "RUDE|Livery") FString TextureName;
	UPROPERTY(EditAnywhere, Category = "RUDE|Livery") FString HiTextureName;
	// True when any colors/Item/liveries row of carvariations enables this index.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") bool bEnabledByCarVariations = false;
	UPROPERTY(EditAnywhere, Category = "RUDE|Livery") TSoftObjectPtr<UTexture2D> Texture;
};

// A vehicle as data: the body mesh (LOD0 = _hi, LOD1.. = base lod groups), the child/bone table, the livery
// set and the three metadata rows (vehicles.meta -> handlingId -> handling.meta; carvariations by modelName)
// flattened BY FIELD NAME as the file spells them, plus each row's re-spelled item XML. Never in-scene; the
// composite actor carries a tag pointing at it. Export is out of scope for this asset (values are strings as
// read; nothing here is a byte-safe seam yet).
UCLASS(BlueprintType)
class RUDECORE_API URudeVehicle : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "RUDE|Vehicle") FString VehicleName;
	UPROPERTY(EditAnywhere, Category = "RUDE|Vehicle") TSoftObjectPtr<UStaticMesh> BodyMesh;
	// Source models on the body mesh (1 + the base lod groups that imported): LOD0 = _hi High when it exists.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") int32 LodCount = 0;
	// "<file>:<DrawableModels group>" per LOD, in LOD order.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") TArray<FString> LodSources;
	// The per-LOD drawable assets the body's LOD1.. were copied from (kept beside the body).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Vehicle") TArray<FString> LodAssets;

	// bone name -> the yft's bone tag (physics children name bones by tag).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Skeleton") TMap<FName, int32> BoneTags;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Skeleton") int32 BoneCount = 0;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Skeleton") int32 WheelBoneCount = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|Vehicle") TArray<FRudeVehicleChild> Children;
	// Bound type -> count over the archetype composite's children (Geometry / Disc / Box ...).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Bound") TMap<FString, int32> BoundTypeCounts;

	UPROPERTY(EditAnywhere, Category = "RUDE|Livery") TArray<FRudeVehicleLivery> Liveries;
	// The fragment shader that binds a _sign_ texture (index into <ShaderGroup><Shaders>), its preset and sampler.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") int32 LiveryShaderIndex = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") FString LiveryShaderPreset;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") FString LiverySampler;
	// The RUDE master parameter the sampler lands on (DiffuseSampler -> Diffuse); empty = the master has none.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") FString LiveryMaterialParameter;
	// Body material slots (<preset>__<geo>) whose geometry uses the livery shader, LOD0.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") TArray<FString> LiveryMaterialSlots;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") int32 CurrentLivery = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Livery") bool bHasLiveryFlag = false;

	// vehicles.meta row (CVehicleModelInfo), flattened "field" / "list/Item[n]/field" -> value as spelled.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") TMap<FString, FString> VehiclesMeta;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString HandlingId;
	// handling.meta row (CHandlingData + SubHandlingData items).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") TMap<FString, FString> Handling;
	// carvariations row (PSO hash tags resolved by joaat where the meta spelling is known).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") TMap<FString, FString> CarVariations;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceYft;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceHiYft;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceYtd;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceHiYtd;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceVehiclesMeta;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceHandling;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceCarVariations;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source") FString VehiclesMetaXml;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source") FString HandlingXml;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source") FString CarVariationsXml;
};
