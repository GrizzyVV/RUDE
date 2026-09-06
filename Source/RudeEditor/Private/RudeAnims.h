// RUDE - RAGE <-> Unreal Development Environment
// WP10 ANIMS lane: the cutscene events sidecar. Everything the .cut says, verbatim, so an author (or the next
// tool) reads the game's own words; nothing here is interpreted. Editor-module asset (loads in the editor).
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RudeAnims.generated.h"

USTRUCT(BlueprintType)
struct FRudeCutEvent
{
	GENERATED_BODY()

	// pCutsceneLoadEventList | pCutsceneEventList
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString List;
	// the event item's type attribute (rage__cutfObjectIdEvent | rage__cutfEvent)
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString EventType;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") float Time = 0.f;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") int32 EventId = -1;
	// -1 when the event carries no iObjectId (rage__cutfEvent)
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") int32 ObjectId = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") int32 ArgsIndex = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString ArgsType;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString ArgsName;
	// the pCutsceneEventArgsList item this event points at, re-spelled from the source XML
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString ArgsXml;
	// the event item itself (IsChild, StickyId, pChildEvents ... everything)
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString EventXml;
};

USTRUCT(BlueprintType)
struct FRudeCutObject
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") int32 ObjectId = -1;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString Type;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString Name;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString StreamingName;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString Xml;
};

UCLASS(BlueprintType)
class URudeCutsceneEvents : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString CutName;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString SourceFile;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") float TotalDuration = 0.f;
	// vOffset / fRotation as the file spells them (RAGE metres / degrees, NOT converted)
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FVector SceneOffsetRage = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") float SceneRotationRage = 0.f;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") FString CutsceneFlags;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") TArray<FRudeCutObject> Objects;
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") TArray<FRudeCutEvent> Events;
	// every pCutsceneEventArgsList item, by index, re-spelled
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") TArray<FString> EventArgsXml;
	// every concatDataList row, re-spelled
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") TArray<FString> ConcatXml;
	// the animation parts used for the camera, in order
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Cutscene") TArray<FString> Parts;
};
