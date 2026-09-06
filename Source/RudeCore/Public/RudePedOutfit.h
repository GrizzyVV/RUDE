// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/SkeletalMesh.h"
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
