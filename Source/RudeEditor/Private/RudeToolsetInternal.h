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
// ---- generated-master staleness (RudeToolset.cpp) ----
// THE definition of "stale" for a master RUDE generates, so that there is exactly one and every
// generator that upgrades a master in place (EnsureGeneratedMaster, EnsureDetailMaster,
// EnsureCutoutMaster) and the reporter (RudeDoctor) all call it instead of each keeping a copy.
// Unreadable = the material would not load, or the name is not one this rule covers; that is
// counted separately and never reported as healthy. OutWhy is a plain phrase naming the failed
// condition, empty when Healthy.
enum class ERudeMasterHealth : uint8 { Healthy, Stale, Unreadable };
ERudeMasterHealth RudeGeneratedMasterHealth(class UMaterial* M, const FString& AssetName, FString& OutWhy);

// ---- fxmanifest merge (RudeMapLanes.cpp) ----
// Merge the directives a RUDE export REQUIRES into an fxmanifest.lua that may already carry a
// person's own `client_script` / `files` lines. Keeps the existing bytes VERBATIM and appends only
// the directives the file does not already declare. OutPreserved = lines the existing file had (all
// carried through), OutAlready = required directives it already declared, OutAdded = directives
// appended. False only when the write itself failed (OutError says why).
bool RudeMergeManifest(const FString& Path, const TArray<FString>& RequiredLines,
                       int32& OutPreserved, int32& OutAlready, int32& OutAdded, FString& OutError);

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

// ---- LOD lineage (RudeLevelTools.cpp; ENGINEERING_LOG laws 24-28) ----
// Resolve every entity's parentIndex into LodParent / LodChildren by the measured rule: the ymap named
// by CMapData/parent first, then the entity's own ymap, accepting the candidate exactly one LOD level
// coarser. YmapParent maps ymap (lower) -> its CMapData/parent. Counts: links made, links that resolved
// to nothing (tagged RUDE_LOD_UNRESOLVED), parents whose stored numChildren != children present
// (bLodPartial, tagged RUDE_LOD_PARTIAL).
void RudeResolveLodLineage(UWorld* World, const TMap<FString, FString>& YmapParent,
                           int32& OutLinks, int32& OutUnresolved, int32& OutPartial);

// ---- entity lights (RudeLevelTools.cpp) ----
// Every CLightAttrDef instance in the entity's carried <extensions> becomes a UE light component on the
// actor (tags RUDE_LIGHT:<index>, RUDE_LIGHT_KEY:<hash of the fields it mirrors>). Returns the count.
int32 RudeAttachEntityLights(AActor* Actor, class URudeEntityComponent* R);
// Before export keying: any light component whose mirrored fields changed rewrites its instance inside
// R->ExtensionsXml (position, colour, intensity, falloff, exponent, cone angles, direction) so the
// entity keys as edited and only those fields move. Returns the number of instances rewritten.
int32 RudeSyncEntityLights(AActor* Actor, class URudeEntityComponent* R);

// ---- MLO interiors: raw slices of one CMloArchetypeDef (RudeMloExport.cpp; maintainer lane `mlo_export` (`LAWS.md`)) ----
// The ytyp's OWN BYTES cut along the lines the game's writer emits (measured 2026-09-06 over 541 MLO archetypes:
// one shape). Offsets are into `Arch` (the archetype's "  <Item type=\"CMloArchetypeDef\">" slice) except
// ArchStart/ArchEnd, which are into the document. Every item slice starts at its own indentation and ends with the
// newline after its </Item>, so concatenating a block's slices reproduces the block's inner bytes exactly.
struct FRudeMloRawRoom
{
	FString Item;                     // "    <Item>...    </Item>\n"
	int32 AoStart = -1, AoEnd = -1;   // inside Item: the "     <attachedObjects...>" element incl. its trailing newline
	TArray<int32> Attached;           // the ordinals it lists (empty for "<attachedObjects />")
};
struct FRudeMloRawSet
{
	FString Name;                     // the set's <name> as spelled
	FString Item;                     // "    <Item>...    </Item>\n"
	int32 LocStart = -1, LocEnd = -1; // inside Item: the "     <locations...>" element incl. its trailing newline
	int32 EntStart = -1, EntEnd = -1; // inside Item: "     <entities>\n" .. "     </entities>\n" (or "     <entities />\n")
	bool bEntitiesEmpty = false;
	TArray<int32> Locations;          // one room index per entity (2,272/2,272 sets measured)
	TArray<FString> Items;            // "      <Item type=\"CEntityDef\">...      </Item>\n" slices, by ordinal
};
struct FRudeMloRaw
{
	int32 ArchStart = -1, ArchEnd = -1;   // in the document
	FString Arch;
	int32 EntStart = -1, EntEnd = -1;     // in Arch: "   <entities>\n" .. "   </entities>\n" (or "   <entities />\n")
	bool bEntitiesEmpty = false;
	TArray<FString> Items;                // "    <Item type=\"CEntityDef\">...    </Item>\n" slices, by ordinal
	int32 RoomsStart = -1, RoomsEnd = -1; // in Arch: the whole "   <rooms itemType=\"CMloRoomDef\">" block (-1 = none)
	TArray<FRudeMloRawRoom> Rooms;
	int32 SetsStart = -1, SetsEnd = -1;   // in Arch: the whole "   <entitySets itemType=\"CMloEntitySet\">" block (-1 = none)
	TArray<FRudeMloRawSet> Sets;
};
// Cut Doc along the measured lines for the CMloArchetypeDef named MloName (case-insensitive). False with a reason
// when the file does not have the measured shape (CRLF, a leftover byte between items, a set without <locations>):
// the caller refuses, never guesses.
bool RudeMloSliceRaw(const FString& Doc, const FString& MloName, FRudeMloRaw& Out, FString& OutError);
// The scalar-list rendering the game's writer uses, shared with the MLO AUTHORING lane (RudeMloAuthor.cpp):
// empty -> "<Tag />", 1..10 values inline on one line, 11+ wrapped ten per line one indent deeper
// (maintainer lane `mlo_export` (`LAWS.md`) law 8: 926/926 short lists inline, 1,193/1,193 long
// attachedObjects and 472/472 long locations lists in full lines of ten). A room's <attachedObjects> is
// spelled by this function whether the list came from a splice or from an authored interior.
FString RudeMloIntList(const FString& Indent, const TCHAR* Tag, const TArray<int32>& V);
// RudeNum (RudeLevelTools.cpp, file-local) for other translation units: a float32 as the game's files spell it.
FString RudeNumText(double V);
