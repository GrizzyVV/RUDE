// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"   // RUDE_PEDPROPS: the rigid prop mesh
#include "Engine/Texture2D.h"
#include "Animation/Skeleton.h"
#include "RudePedOutfit.generated.h"

// One texture variation of a ped drawable: the ytd texture the game swaps in for letter a/b/c... The ydd's
// shader binds letter a; the ymt's aTexData row says how many letters exist and which texId each rides
// (measured on a_m_m_business_01: texId 0 -> _uni, 1 -> _whi, 2 -> _bla; carried as data, never a table).
USTRUCT(BlueprintType)
struct FRudePedTexture
{
	GENERATED_BODY()

	// The letter in <comp>_diff_<ddd>_<letter>_<race>.
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	FString Letter;

	// The ytd texture resolved for this letter by prefix (empty = the ped's ytd has no such name).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	FString TextureName;

	// CPVTextureData.texId as spelled.
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	int32 TexId = 0;

	// CPVTextureData.distribution as spelled (255 on every row measured).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	int32 Distribution = 255;

	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	TSoftObjectPtr<UTexture2D> Texture;
};

// One drawable of a component: the dictionary entry <comp>_<ddd>_<class> and its texture letters.
USTRUCT(BlueprintType)
struct FRudePedDrawable
{
	GENERATED_BODY()

	// Dictionary entry name (a hash_XXXXXXXX entry is resolved back to <comp>_<ddd>_<class> when joaat allows).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	FString Name;

	// Position in the ymt's aDrawblData3 (= the ddd in the name).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	int32 DrawableIndex = 0;

	// r / u / m as spelled in the entry name (meaning unverified; carried).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	FString Class;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Ped")
	int32 PropMask = 0;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Ped")
	int32 NumAlternatives = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	TArray<FRudePedTexture> Textures;

	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	TSoftObjectPtr<USkeletalMesh> Mesh;

	// The import's own counts (the comparator's numbers): High LOD group only.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	int32 Vertices = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	int32 Triangles = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	int32 VerticesWithoutWeights = 0;
	// RUDE_PEDLOD_BEGIN drawable
	// The entry's four <LodDist*> floats AS SPELLED (+0x70..0x7C). MEASURED a constant 9998,9998,9998,9998 on
	// 2,119/2,125 game binary entries and 1,148/1,152 corpus entries: the ped's LOD switch distance is NOT
	// stored here (maintainer lane `ped_lods` (`LAWS.md`) law 2). ExportPedReplace hands these four values to
	// ExportYddBinary (`LODDIST=` in Options), which writes them at +0x70..0x7C - so an entry that deviates
	// (4 corpus entries do, e.g. a three-group `uppr_000_u` reading 100/9998/9998/9998) re-exports its OWN
	// values. Empty = never imported from a ped; the writer then uses the modal 9998 x4.
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Ped")
	TArray<float> LodDist;

	// LOD groups the entry shipped and the import built: 1 (High only), 2 (+Medium), 3 (+Low). Measured over
	// 1,152 corpus entries: 823 carry three, 182 two, 147 one; none carries a VeryLow group.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	int32 LodGroups = 1;

	// Per-LOD vertex / triangle counts of the imported mesh, index 0 = High (= Vertices / Triangles above).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	TArray<int32> LodVertices;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	TArray<int32> LodTriangles;
	// RUDE_PEDLOD_END drawable
};

// One component slot of the ped (availComp position) with its drawables in ymt order.
USTRUCT(BlueprintType)
struct FRudePedComponent
{
	GENERATED_BODY()

	// availComp slot 0..11.
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	int32 ComponentIndex = 0;

	// head / berd / hair / uppr / lowr / hand / feet / teef / accs / task / decl / jbib
	// (0,2,3,4,8,9 measured on two peds; the rest is the conventional order).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	FString Slot;

	// CPVComponentData.numAvailTex (= the sum of the drawables' texture rows on every ped measured).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	int32 NumAvailTex = 0;

	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	TArray<FRudePedDrawable> Drawables;
};

// RUDE_PEDPROPS_BEGIN outfit structs
// One prop (hat / glasses / earpiece / watch / bracelet) of the ped: the <ped>_p.ydd entry p_<anchor>_<ddd> joined
// with its ymt CPedPropMetaData row. Measured (maintainer lane `pedprops` (`LAWS.md`), 709 peds / 1,763 entries): every
// entry is RIGID (HasSkin 0 on 977/977 base models, no BoneIDs, no Skeleton, no Bounds, High group only), modeled in
// ped axes with the origin at the anchor bone; anchorId 0 head / 1 eyes / 2 ears / 6 left wrist / 7 right wrist are
// the only ids the game's data spells (1,169/1,169 anchor rows).
USTRUCT(BlueprintType)
struct FRudePedProp
{
	GENERATED_BODY()

	// Dictionary entry name, p_<anchor>_<ddd> (1,763/1,763 spell it; a hash_ entry never occurred).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped Props")
	FString Name;

	// CPedPropMetaData.anchorId as spelled (0 head, 1 eyes, 2 ears, 6 lwrist, 7 rwrist measured).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped Props")
	int32 AnchorId = -1;

	// The anchor word of the entry name (head / eyes / ears / lwrist / rwrist) = the ymt's ANCHOR_* enumerant.
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped Props")
	FString Anchor;

	// The yft bone the prop rides: SKEL_Head for head/eyes/ears, SKEL_L_Hand / SKEL_R_Hand for the wrists - RUDE's
	// table (the game's own is code, not data); NAME_None when the skeleton lacks it (counted anchorsUnmapped).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped Props")
	FName AnchorBone;

	// CPedPropMetaData.propId (= the ddd of the entry name; 1,169/1,169 anchor groups consistent).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped Props")
	int32 PropIndex = -1;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Ped Props")
	int32 PropFlags = 0;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Ped Props")
	int32 Flags = 0;

	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "RUDE|Ped Props")
	FString AudioId;

	// texData rows: letter a.. -> p_<anchor>_diff_<ddd>_<letter> in <ped>_p.ytd (no race suffix; case-insensitive).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped Props")
	TArray<FRudePedTexture> Textures;

	// The rigid mesh (every measured prop is rigid). A skinned prop entry is counted at import, not built (v1).
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped Props")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped Props")
	int32 Vertices = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped Props")
	int32 Triangles = 0;

	// The entry's shader presets in geometry order (ped / ped_alpha: a lens is ped_alpha, bucket 1).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped Props")
	TArray<FString> ShaderPresets;
};
// RUDE_PEDPROPS_END outfit structs

// A ped's variation matrix as data: which component slots exist, which drawables each has, which texture
// letters each drawable has, and the skeleton they all bind to. Built by ImportPed from <ped>.ymt
// (CPedVariationInfo) joined against <ped>.ydd and <ped>.ytd. Never in-scene; the preview actor reads it.
UCLASS(BlueprintType)
class RUDECORE_API URudePedOutfit : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	FString PedName;

	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	TSoftObjectPtr<USkeleton> Skeleton;

	// bone name -> the yft's bone tag (the id animation channels use); the anim lane maps by name, then tag
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	TMap<FName, int32> BoneTags;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	int32 BoneCount = 0;

	// availComp as spelled: 12 entries, 255 = slot absent, else the index into the ymt's aComponentData3.
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	TArray<int32> AvailComp;

	UPROPERTY(EditAnywhere, Category = "RUDE|Ped")
	TArray<FRudePedComponent> Components;

	// RUDE_PEDPROPS_BEGIN outfit members
	// The prop matrix (<ped>.ymt propInfo joined against <ped>_p.ydd / <ped>_p.ytd); empty when the ped has none.
	UPROPERTY(EditAnywhere, Category = "RUDE|Ped Props")
	TArray<FRudePedProp> Props;

	// CPedVariationInfo.propInfo.numAvailProps as spelled (= the row count on 709/709 peds measured).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped Props")
	int32 NumAvailProps = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourcePropYdd;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourcePropYtd;
	// RUDE_PEDPROPS_END outfit members

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	bool bHasTexVariations = false;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Ped")
	bool bHasDrawblVariations = false;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYft;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYdd;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYtd;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYmt;
};
