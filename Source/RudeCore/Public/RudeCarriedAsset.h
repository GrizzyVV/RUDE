// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RudeCarriedAsset.generated.h"

// One corpus file carried as-is: the PASSTHROUGH tier (WP10). No edit-native fields - the honest state for a
// lane RUDE can list and inspect but does not yet author (yed, yld, yfd, ypdb, ynv, mrf, ypt, ...). The
// interchange XML rides verbatim (inline while it fits the cap; the ledger path otherwise) beside a summary of
// its top-level arrays and the ledger provenance, so the file can be found again and re-emitted byte-for-byte.
// The pattern is URudeArchetype's SourceXml seam, minus the fields.
UCLASS(BlueprintType)
class RUDECORE_API URudeCarriedAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	// Ledger lane word (ynv, ypt, ...).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Carried")
	FString LaneType;

	// Asset name as the ledger spells it (may hold characters a package name cannot, e.g. navmesh[102][102]).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Carried")
	FString Name;

	// XML root element (NavMesh, MoveNetwork, ParticleEffectsList, ...).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Carried")
	FString RootTag;

	// The list-view line: top-level children with their direct-child counts ("Polygons:2564 Portals:4 Points:1062 ...").
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Carried")
	FString Summary;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Carried")
	int64 SizeBytes = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Carried")
	int32 TopLevelChildren = 0;

	// false = the ledger kept the game's binary (no interchange XML to carry).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Carried")
	bool bConverted = false;

	// false = the XML was over the inline cap; re-read it from CorpusRoot/SourceSlot/SourceFile.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Carried")
	bool bXmlInline = false;

	// ---- provenance ----
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString CorpusRoot;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSlot;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceFile;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSha1;

	// The file's own bytes (UTF-8 decoded), verbatim.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;
};
