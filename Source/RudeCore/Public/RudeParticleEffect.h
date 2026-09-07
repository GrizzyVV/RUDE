// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/TextRenderComponent.h"
#include "RudeParticleEffect.generated.h"

// One <EventEmitters><Item> of an effect rule: the (emitter rule, particle rule) pair the effect fires,
// with the ratio window it fires it over. Measured over the corpus's 1,240 ypt files: 27,676 event
// emitters across 10,268 effect rules, and 27,676/27,676 of both the EmitterRule and the ParticleRule
// names resolve inside the SAME file's own dictionaries - an effect never reaches out of its ypt.
USTRUCT(BlueprintType)
struct FRudeYptEventEmitter
{
	GENERATED_BODY()

	// The emitter rule this event spawns from (a Name in the same file's EmitterRuleDictionary).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString EmitterRule;

	// The particle rule the spawned particles are drawn with (same file's ParticleRuleDictionary).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString ParticleRule;

	// 0..6 in this corpus (10,268 / 7,580 / 4,889 / 2,656 / 1,326 / 687 / 270 of the 27,676). The
	// meaning of a value is NOT measured here - it is carried as the file spells it.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	int32 EventType = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float StartRatio = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float EndRatio = 1.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float PlaybackRateScalarMin = 1.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float PlaybackRateScalarMax = 1.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float ZoomScalarMin = 1.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float ZoomScalarMax = 1.f;

	// Kept as the file spells them ("0xFFFFFFFF"), not decoded: the channel order is unmeasured.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString ColourTintMin;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString ColourTintMax;

	// The evolution axes this event is driven by. Measured over the 27,676 events of the corpus's
	// 1,240 ypt files: 27,421 events carry an EvolutionList, naming 35,037 axes in 121 distinct
	// words - "LOD" is 21,276 of them.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	TArray<FString> Evolutions;

	// How many <EvolvedKeyframeProps><Item> the event carries (curve payload, not decoded here).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	int32 EvolvedKeyframeProps = 0;
};

// A referenced emitter- or particle-rule record, read shallow: the fields that describe the SHAPE and
// the DRAW, plus every scalar the item carries as a raw string. Nothing here is converted to a UE
// material or a UE emitter - see URudeParticleEffect's tier note.
USTRUCT(BlueprintType)
struct FRudeYptRuleRef
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Rule")
	FString Name;

	// "emitter" or "particle".
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Rule")
	FString Kind;

	// Emitter rules only. Measured over the 4,146 emitter rules of the base slot's ptfx.rpf: the
	// creation domain is Cylinder 1,619 / Sphere 1,413 / Box 1,114, and the target domain is
	// Cylinder 2,344 / Sphere 1,271 / Box 531. Three shapes, no fourth.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Rule")
	FString CreationDomainType;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Rule")
	FString TargetDomainType;

	// Particle rules only. 21,074 of the 21,837 particle rules name ptfx_sprite and 763 name
	// ptfx_trail - two shaders, and 17 techniques over them.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Rule")
	FString ShaderFile;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Rule")
	FString ShaderTechnique;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Rule")
	int32 DrawType = 0;

	// <AllBehaviours><Item><Type value="Age"/> - the behaviour vocabulary the rule turns on.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Rule")
	TArray<FString> Behaviours;

	// Every leaf field of the item, tag -> the text the file carries (a value attribute, an x/y/z(/w)
	// tuple joined with commas, or the element's own text). Unconverted on purpose.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Rule")
	TMap<FString, FString> RawFields;
};

// ONE effect rule out of one .ypt (a "particle effect" as the game's scripts name it: START_PARTICLE_FX
// takes this Name and the ypt's own name as the asset).
//
// ⛔ PREVIEW TIER, READ-ONLY. This is a MEASUREMENT of the game's record, not a conversion of it: no
// Niagara system is generated, no curve is evaluated, no material is built, and there is no writer -
// nothing in RUDE emits a .ypt. The placement preview (ARudeParticlePreview) is an APPROXIMATION of
// where an effect sits and roughly how big its culling volume is, and nothing else.
//
// Measured denominators (maintainer lane `vfx_move`, `LAWS.md`): 1,240 ypt files, 10,268 effect rules,
// 23,840 emitter rules, 21,837 particle rules, 990 drawables, 1,292 textures. Every effect rule in the
// corpus carries the same 43 fields and 8,441 of the 10,268 additionally carry EvolutionList.
UCLASS(BlueprintType)
class RUDECORE_API URudeParticleEffect : public UDataAsset
{
	GENERATED_BODY()

public:
	// ---- identity ---------------------------------------------------------------------------
	// The effect rule's own Name ("ent_amb_fbi_live_wires"). 10,268/10,268 effect-rule names in this
	// corpus are plain words - not one is a hash_XXXXXXXX placeholder.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString EffectName;

	// The ypt (effect dictionary) it lives in ("scr_agencyheistb"). Both halves are needed to name an
	// effect: 2,549 distinct effect-rule names appear 10,268 times, so a name alone is ambiguous.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString YptName;

	// Always "preview" - the tier this asset is honest about being.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString Tier;

	// ---- provenance -------------------------------------------------------------------------
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString CorpusRoot;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSlot;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceFile;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSha1;

	// ---- the effect rule's own fields, as measured -------------------------------------------
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	int32 RefCount = 0;

	// "4.2" on every effect rule in this corpus; carried as text because it is a file version, not a number.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString FileVersion;

	// The file spells this 0xFFFFFFFF for "loop forever"; kept as text so no sign convention is invented.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString NumLoops;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float DurationMin = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float DurationMax = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float PreUpdateTime = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float PreUpdateTimeInterval = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float PlaybackRateScalarMin = 1.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float PlaybackRateScalarMax = 1.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	bool bIsShortLived = false;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	bool bHasNoShadows = false;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	bool bSortEventsByDistance = false;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	int32 DrawListID = 0;

	// RAGE metres -> UE centimetres with the house Y mirror (AGENTS §6.1).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FVector RandomOffsetPosCm = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FVector ViewportCullingSphereOffsetCm = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float ViewportCullingSphereRadiusCm = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float DistanceCullingFadeDistCm = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float DistanceCullingCullDistCm = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float LodEvoDistanceMinCm = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float LodEvoDistanceMaxCm = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float CollisionRangeCm = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	float CollisionProbeDistanceCm = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	int32 CollisionType = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	int32 ViewportCullingMode = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	int32 DistanceCullingMode = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	int32 ZoomLevel = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString GameFlags;

	// ---- the graph the effect actually is -----------------------------------------------------
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	TArray<FRudeYptEventEmitter> EventEmitters;

	// Every emitter/particle rule the events above name, read shallow, in first-reference order.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	TArray<FRudeYptRuleRef> Rules;

	// The effect rule's own evolution axes.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	TArray<FString> Evolutions;

	// Keyframe-prop name -> how many keyframes it carries. 40,055 of the 51,340 effect-rule keyframe
	// props in the corpus carry ZERO keys, so an empty list here is the common case, not a read failure.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Effect")
	TMap<FString, int32> KeyframePropKeys;

	// Every leaf field of the effect rule, tag -> the text the file carries. The raw field map: it is
	// what makes an unread field visible instead of silently dropped.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Effect")
	TMap<FString, FString> RawFields;

	// ---- honesty ------------------------------------------------------------------------------
	// Set by the importer, always. Reads: PREVIEW TIER - measured, not converted; no writer exists.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Effect")
	FString TierNote;
};

// A placement stand-in for one effect: a wireframe sphere at the effect's own culling radius and a text
// label. ⛔ It is an APPROXIMATION of WHERE an effect sits and ROUGHLY how far it reaches. It does not
// simulate, emit, or render the effect, and it is not a conversion of one. Nothing about the particles
// themselves is reproduced.
UCLASS(BlueprintType, ClassGroup = (RUDE))
class RUDECORE_API ARudeParticlePreview : public AActor
{
	GENERATED_BODY()

public:
	// Defined inline on purpose: RudeCore gains a header and no new .cpp, so the module's file list
	// does not have to be invalidated for this class to link (AGENTS §5 build trap).
	ARudeParticlePreview()
	{
		PrimaryActorTick.bCanEverTick = false;
		Pivot = CreateDefaultSubobject<USceneComponent>(TEXT("Pivot"));
		RootComponent = Pivot;
		Extent = CreateDefaultSubobject<USphereComponent>(TEXT("Extent"));
		Extent->SetupAttachment(Pivot);
		Extent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Extent->SetCollisionProfileName(TEXT("NoCollision"));
		Extent->ShapeColor = FColor(255, 160, 0);
		Extent->bDrawOnlyIfSelected = false;
		Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
		Label->SetupAttachment(Pivot);
		Approximation = TEXT("APPROXIMATION - placement preview only. Not a conversion: no particles are simulated, emitted or rendered, and RUDE has no .ypt writer.");
	}

	// The measured effect this stands in for.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	TSoftObjectPtr<URudeParticleEffect> Effect;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	FString EffectName;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	FString YptName;

	// The creation-domain shape of the first emitter rule the effect fires ("Sphere"/"Box"/"Cylinder"),
	// or empty when the effect fires none. Recorded, NOT drawn - the marker is a sphere either way.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	FString CreationDomain;

	// The radius the marker was drawn at, and where it came from.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	float RadiusCm = 100.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	FString RadiusSource;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	FString Approximation;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	TObjectPtr<USceneComponent> Pivot;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	TObjectPtr<USphereComponent> Extent;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|Preview")
	TObjectPtr<UTextRenderComponent> Label;
};
