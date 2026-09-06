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
