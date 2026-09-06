// RUDE - RAGE <-> Unreal Development Environment
// Private, shared between RudeToolset.cpp and RudeLevelTools.cpp (and RudeBuildArea.cpp). Nothing here
// is part of the tool surface; it is the plumbing the lanes share so the monolith can be split.
#pragma once

#include "CoreMinimal.h"

class FXmlNode;
class AActor;
class UWorld;
class UStaticMesh;
class FJsonObject;

// JSON string escape for text that rides inside a verdict or a manifest.
FString RudeJsonEscape(const FString& In);
// XML text escape / re-spelling of an FXmlNode subtree (FXmlFile has no writer and flattens text).
void RudeXmlEscapeInto(FString& O, const FString& In);
void RudeXmlNodeToString(const FXmlNode* N, FString& O, int32 Depth);
// One actor per entity with a filled URudeEntityComponent (ImportScene ACTORS, BuildDistrictLevel, PlaceArchetype).
AActor* RudeSpawnEntityActor(UWorld* World, const FString& YmapName, const TSharedPtr<FJsonObject>& Ent,
                             const FTransform& Xf, UStaticMesh* Mesh, bool bProxy, uint32 TimeMask);
// Headless-safe dirty-package save (UPackage::Save when there is no Slate); counts land in the globals.
bool RudeSaveDirty(bool bMaps, bool bContent);
extern int32 GRudeLastSaved, GRudeLastSaveFailed;
// Sum of an integer field across a JSON verdict list (batch tools fold per-item verdicts with it).
int32 RudeSumField(const FString& Json, const TCHAR* Key);

// ---- import plumbing shared by RudeToolset.cpp (single-file lanes) and RudeMapLanes.cpp (map/area) ----
// Every scoping signal a caller can PROVE for texture resolution; nothing here is inferred from a path.
struct FRudeTextureScope
{
	FString ArchetypeTxd;                              // tier 2 (lowercase; empty = none)
	TArray<FString> ParentTxdChain;                    // tier 3, nearest ancestor first
	const TArray<FString>* YtypNeighbours = nullptr;   // tier 4 (sorted, unique, owned by the index)
	FString AssetSlot;                                 // tier 5 (empty = unknown)
	const TMap<FString, FString>* DictSlots = nullptr; // tier 5: dictionary -> winning slot

	bool IsEmpty() const
	{
		return ArchetypeTxd.IsEmpty() && ParentTxdChain.Num() == 0
			&& (YtypNeighbours == nullptr || YtypNeighbours->Num() == 0)
			&& (AssetSlot.IsEmpty() || DictSlots == nullptr);
	}
};

// Build a UStaticMesh (plus per-slot MaterialInstances) from ONE drawable-shaped XML node; the body every
// import lane shares. Scope = nullptr means "no scope" (the single-file tools' behaviour).
FString ImportDrawableNode(const FXmlNode* DrawableRoot, const FString& MeshName, const FString& DestFolder,
                           const FRudeTextureScope* Scope = nullptr);
// ImportYdr / ImportYddEntry bodies with the texture scope the public UFUNCTIONs have no parameter for.
FString RudeImportYdrScoped(const FString& XmlPath, const FString& DestFolder, const FRudeTextureScope* TextureScope);
FString RudeImportYddEntryScoped(const FString& XmlPath, const FString& EntryName, const FString& DestFolder,
                                 const FRudeTextureScope* TextureScope);
// Jenkins one-at-a-time over the lowercased name (the game's joaat).
uint32 RudeJoaat(const FString& Name);
