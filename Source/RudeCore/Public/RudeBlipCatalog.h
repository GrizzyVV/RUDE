// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/Texture2D.h"
#include "RudeBlipCatalog.generated.h"

// One blip sprite as minimap.gfx exports it (SWF ExportAssets tag 56: characterId + name).
USTRUCT(BlueprintType)
struct FRudeBlipEntry
{
	GENERATED_BODY()

	// The export name ("radar_airport"). Measured: 954 distinct radar_* exports in the game-loaded
	// minimap.gfx (update slot), 410 in the base one.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Blip")
	FString Name;

	// The character id INSIDE the SWF. This is NOT the game's numeric blip sprite id (the id
	// SET_BLIP_SPRITE takes); that mapping lives in game code, not in any corpus file.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Blip")
	int32 SwfCharacterId = 0;

	// The sheet this sprite is cut from. UNRESOLVED in this lane (null): finding it means following
	// the sprite's DefineShape fill to its bitmap character to the GFX external-image tag that names
	// the sheet. Left empty rather than guessed.
	UPROPERTY(EditAnywhere, Category = "RUDE|Blip")
	TSoftObjectPtr<UTexture2D> Sheet;
};

// The blip catalog: every sprite name minimap.gfx exports, plus the sheet textures minimap.ytd
// declares (imported through ImportYtd when the corpus carries their pixels).
UCLASS(BlueprintType)
class RUDECORE_API URudeBlipCatalog : public UDataAsset
{
	GENERATED_BODY()

public:
	// In the gfx's own export order.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Blips")
	TArray<FRudeBlipEntry> Blips;

	// Name (lower-case) -> index into Blips.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Blips")
	TMap<FName, int32> IndexByName;

	// The blip sheets minimap.ytd declares (blips_texturesheet, _ng, _ng_2, _ng_3), as soft refs to the
	// textures ImportYtd lands under <DestFolder>/minimap/. A ref whose package does not exist means the
	// corpus had no pixel sidecar for that sheet (verdict: sheetsWithPixels).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Blips")
	TArray<TSoftObjectPtr<UTexture2D>> Sheets;

	// Sheet name -> declared size, from the ytd manifest.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Blips")
	TMap<FString, FIntPoint> SheetSizes;

	// Exports that are not radar_* (health_hit, sonar_sweep, the sheet names themselves, ...).
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Blips")
	TArray<FString> OtherExports;

	// ---- provenance ----
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceGfxSlot;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceGfxFile;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	int32 GfxVersion = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYtdSlot;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceYtdFile;
};
