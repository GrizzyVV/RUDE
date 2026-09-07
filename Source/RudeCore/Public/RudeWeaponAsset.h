// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/StaticMesh.h"
#include "RudeWeaponAsset.generated.h"

// One component the weapon's meta row lists at one attach point: the weaponcomponents.meta row BY FIELD
// NAME as spelled, the two bones the join runs through, and the mesh (when the component names a model).
//
// THE JOIN, MEASURED 2026-09-06 over the whole weapon set (maintainer lane `weapons` (`LAWS.md`)):
//   * The WEAPON's own skeleton carries the attach bones (WAPClip 185 drawables, WAPFlshLasr 163,
//     WAPSupp 151, WAPScop 115, WAPGrip 76, WAPScop_2 69, ...); the COMPONENT's drawable carries ONE
//     AAP* bone and it is that drawable's FIRST bone, 516/516, always parent -1.
//   * That AAP root is identity rotation AND zero translation in 516/516: a component's geometry is
//     authored AT its attach frame's origin, so the mesh drops straight onto the weapon's WAP bone
//     frame with no correction. (⚠ this is the opposite of a ped prop, which needs its anchor bone's
//     bind rotation inverted - maintainer lane `pedprops` law 7.)
//   * 198 of 875 weapon-set drawables carry WAP bones and 516 carry AAP bones; NO drawable carries both.
//     The two prefixes are the two halves of one join, never mixed.
USTRUCT(BlueprintType)
struct FRudeWeaponComponent
{
	GENERATED_BODY()

	// The meta name the weapon's <AttachPoints> lists (COMPONENT_AT_AR_SUPP).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString Name;
	// The item's type as the file spells it (CWeaponComponentClipInfo / ...ScopeInfo / ...SuppressorInfo /
	// ...FlashLightInfo / ...VariantModelInfo / CWeaponComponentInfo). 8 types over 697 copies.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString Type;
	// <Model> of the component row, lower-cased: the ydr to import (empty = a component with no model).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString Model;
	// The WEAPON bone this component hangs on - the <AttachBone> of the weapon's attach point (WAPSupp).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString AttachPoint;
	// The COMPONENT bone as its own meta row declares it (AAPSupp). Empty on the rows that have none.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString AttachBone;
	// The AAP* bone actually found on the component's drawable. Empty = the drawable carries none
	// (measured: the declared name is on the drawable in 509/602 model-bearing refs, some AAP bone in
	// 525/602; the rest are name variants like AAP_CAMO or a single bone named after the model).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString ResolvedBone;
	// <Default>true</Default> in the weapon's attach point: the component the game shows with no edit.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") bool bDefault = false;
	// Ordinal of the attach point in the weapon row, and the weapon-skeleton bone index of AttachPoint
	// (-1 = the weapon's own skeleton does NOT have that bone - 16/272 attach points, all WAPClip on
	// shotguns and launchers; the mesh then rides the weapon's origin and is counted unmapped).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") int32 AttachPointIndex = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") int32 BoneIndex = -1;
	UPROPERTY(EditAnywhere, Category = "RUDE|Weapon") TSoftObjectPtr<UStaticMesh> Mesh;
	// The component ImportWeapon placed on the actor for it (hidden unless bDefault).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString ComponentName;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") bool bVisible = false;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") int32 Vertices = 0;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") int32 Triangles = 0;

	// The named rows of the component's meta item (present in 681 of 697 copies).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString LocName;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString LocDesc;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString AccuracyModifier;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString DamageModifier;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString CreateObject;
	// Type-specific rows: ClipSize on 254 clip items, MuzzleBone on 35 suppressors, CameraHash on 44 scopes.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString ClipSize;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString MuzzleBone;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString CameraHash;
	// Everything else the row spells, flattened "field" / "list/Item[n]/field" -> value as spelled.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") TMap<FString, FString> Fields;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceFile;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source") FString Xml;
};

// One <AttachPoints><Item>: the weapon bone and the component set that can sit on it.
USTRUCT(BlueprintType)
struct FRudeWeaponAttachPoint
{
	GENERATED_BODY()

	// <AttachBone> as spelled. 415 of 415 measured attach-point items carry one.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString Bone;
	// Index in the weapon drawable's skeleton, -1 = not on this skeleton (see FRudeWeaponComponent).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") int32 BoneIndex = -1;
	// Indices into URudeWeapon::Components, in the order the meta lists them.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") TArray<int32> Components;
	// The one <Default>true</Default> of this point, if any: 93 of 273 attach points have exactly one
	// and NOT ONE has two, so "the default" is a single value by the game's own data.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString DefaultComponent;
	// The component currently shown on the actor (SetWeaponComponent writes it; empty = none).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Weapon") FString CurrentComponent;
};

// A weapon as data: the weapon drawable, its component table joined WAP -> AAP, and the weapons.meta /
// weaponcomponents.meta rows flattened BY FIELD NAME as the files spell them (plus each row's re-spelled
// item XML), exactly as URudeVehicle carries vehicles.meta / handling / carvariations.
//
// Never in-scene; the composite actor carries a tag pointing at it. Export is out of scope for this asset
// (values are strings as read; nothing here is a byte-safe seam yet).
UCLASS(BlueprintType)
class RUDECORE_API URudeWeapon : public UDataAsset
{
	GENERATED_BODY()

public:
	// <Name> of the CWeaponInfo row (WEAPON_PISTOL). 184 distinct names over 320 item copies.
	UPROPERTY(EditAnywhere, Category = "RUDE|Weapon") FString WeaponName;
	// <Model> lower-cased (w_pi_pistol) - the ydr. 124 of 184 weapon rows name a model.
	UPROPERTY(EditAnywhere, Category = "RUDE|Weapon") FString ModelName;
	UPROPERTY(EditAnywhere, Category = "RUDE|Weapon") TSoftObjectPtr<UStaticMesh> WeaponMesh;

	// The named rows of the weapon item, as spelled. Every one of these is present in 320/320 CWeaponInfo
	// copies measured, so a weapon that lacks one is a data surprise worth seeing as an empty string.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString Audio;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString Slot;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString DamageType;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString FireType;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString WheelSlot;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString Group;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString AmmoInfoName;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString AimingInfo;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString ClipSize;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString Damage;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString AccuracySpread;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString TimeBetweenShots;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString ReloadTimeMP;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString ReloadTimeSP;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString Speed;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") FString Penetration;
	// The whole row, flattened (220 distinct field names over the measured copies; the named ones above
	// are also here, so nothing is lost by naming a subset).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") TMap<FString, FString> WeaponMeta;
	// The CAmmoInfo* row <AmmoInfo> points at, when the corpus has it (94 distinct ammo field names).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Meta") TMap<FString, FString> AmmoMeta;

	UPROPERTY(EditAnywhere, Category = "RUDE|Weapon") TArray<FRudeWeaponAttachPoint> AttachPoints;
	UPROPERTY(EditAnywhere, Category = "RUDE|Weapon") TArray<FRudeWeaponComponent> Components;

	// bone name -> the drawable's bone tag, and the counts the verdict reports.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Skeleton") TMap<FName, int32> BoneTags;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Skeleton") int32 BoneCount = 0;
	// Bones of the weapon skeleton whose name starts with WAP (the attachment sockets it offers).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Skeleton") int32 WapBoneCount = 0;

	// True when the corpus has <model>_hi.ydr and it is what the actor shows, with the base drawable as its
	// LOD1. MEASURED 2026-09-06: 204 of the 875 weapon-set drawables have a _hi twin, the _hi carries more
	// vertices in 194/204 (w_ar_carbinerifle 3,961 -> 20,251), and the twin carries the same skeleton. This
	// is the weapon lane's detail toggle - weapons have NO lod groups inside a file (875/875 High only).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Lod") bool bHiDrawable = false;
	// Meshes on this actor (weapon + components) whose LOD0 is a _hi drawable.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Lod") int32 HiLodMeshes = 0;

	// Texture dictionaries this composite bound against: the drawable's own ytd first, then the other
	// weapon dictionaries in play. gtxd.ymt has ZERO w_ rows (0 of its rows), so a weapon does NOT ride
	// the map's texture-parent chain - see LAWS.md law 12.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Texture") TArray<FString> TextureDictionaries;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Texture") int32 TexturesMissing = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceYdr;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceYtd;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source") FString SourceWeaponsMeta;
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source") FString WeaponsMetaXml;
};
