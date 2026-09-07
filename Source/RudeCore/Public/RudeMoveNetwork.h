// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RudeMoveNetwork.generated.h"

// One MoveNetworkTriggers / MoveNetworkFlags entry: a name and the bit it occupies in the network's
// trigger or flag word. Measured over the corpus's 162 .mrf files: 1,050 triggers and 945 flags, of
// which 1,018/1,050 and 908/945 are hash_XXXXXXXX - the network names its own inputs by HASH, and the
// plaintext is not in the file. That is why this asset carries the hash verbatim and invents nothing.
USTRUCT(BlueprintType)
struct FRudeMoveNamedBit
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString Name;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 BitPosition = -1;

	// True when Name is a hash_XXXXXXXX placeholder rather than a word.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	bool bHashedName = false;
};

// One <Conditions><Item> on a transition. Measured: 9,136 conditions over 8,000 transitions, in 12
// kinds - MoveNetworkFlag 4,523 / MoveNetworkTrigger 2,681 / EventOccurred 524 / ParameterGreaterThan
// 319 / ParameterLessThan 215 / ParameterGreaterOrEqual 195 / ParameterInsideRange 180 /
// ParameterLessOrEqual 168 / TimeGreaterThan 147 / ParameterOutsideRange 94 / BoolParameterEquals 82 /
// TimeLessThan 8. No thirteenth kind occurs.
USTRUCT(BlueprintType)
struct FRudeMoveCondition
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString Type;

	// Set for the bit-testing kinds (MoveNetworkFlag / MoveNetworkTrigger); -1 otherwise.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 BitPosition = -1;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	bool bInvert = false;

	// One line as the graph printer spells it: the kind, the bit and the inverted marker, e.g.
	// "MoveNetworkFlag bit 3" or "MoveNetworkTrigger bit 7 (inverted)". The trigger/flag NAME is not
	// part of it - the printer never looks the bit up in the trigger/flag tables.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString Summary;

	// Every leaf field of the condition item, unconverted.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|MoVE")
	TMap<FString, FString> RawFields;
};

// One <Transitions><Item>: an edge from the node that owns it to a named sibling state. Measured:
// 8,000 transitions, ALL 8,000 carrying the same 18 fields, plus SynchronizerTagFlags on the 493 whose
// SynchronizerType is Tag. BlendModifier is SlowInSlowOut 7,776 / SlowOut 211 / SlowIn 10 / None 3;
// SynchronizerType is None 7,490 / Tag 493 / Phase 17.
USTRUCT(BlueprintType)
struct FRudeMoveTransition
{
	GENERATED_BODY()

	// Index into URudeMoveNetwork::Nodes of the node that owns this transition.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 FromNode = -1;

	// The <TargetState ref="..."> name as spelled.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString TargetState;

	// Resolved index into Nodes, or -1 when no sibling of the owning state machine carries that name.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 TargetNode = -1;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	float Duration = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString BlendModifier;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString SynchronizerType;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString FrameFilter;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString DurationParameterName;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString ProgressParameterName;

	// Combination rule UNMEASURED: the file states no operator between a transition's conditions, so
	// they are carried as a list and never combined. (PrintMoveNetwork writes " AND " between them as
	// a RENDERING choice only; nothing in RUDE acts on it.)
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	TArray<FRudeMoveCondition> Conditions;

	// Every LEAF field of the transition, as the file spells it. Measured: a transition has 19 tags,
	// 18 of them leaves (Conditions is the only container) - 17 on all 8,000 transitions plus
	// SynchronizerTagFlags on 493 - and 10 of those leaves are UnkFlag* words.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|MoVE")
	TMap<FString, FString> RawFields;
};

// One node of the network, flattened out of the XML tree with its parent and depth kept. Measured:
// 33,636 graph nodes over 162 files in 23 kinds; the five commonest are Clip 6,329, State 5,852,
// PushParameter 4,397, Remap 4,345 and Finish 4,184. (A raw type= census counts 42,772 in 35 kinds -
// 9,136 of those are transition CONDITIONS, which are edges' payload, not nodes.)
USTRUCT(BlueprintType)
struct FRudeMoveNode
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 Index = -1;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 Parent = -1;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 Depth = 0;

	// The type= attribute ("StateMachine", "State", "Clip", "BlendN", ...).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString Type;

	// The node's <Name>. 25,908 of the 26,565 non-empty names in the corpus are hash_XXXXXXXX.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString Name;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	bool bHashedName = false;

	// Which slot of the parent it sat in ("RootState", "InitialNode", "States", "Child0", ...).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString Role;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 NodeIndex = -1;

	// Clip nodes only. ContainerType is VariableClipSet 4,322 / ClipSet 724 / ClipDictionary 184 /
	// Unk3 34 over the 5,264 clip records measured.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString ClipContainerType;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString ClipContainerName;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString ClipName;

	// StateMachine nodes only: the <InitialState ref="..."> name.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString InitialState;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString EntryParameterName;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString ExitParameterName;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	TArray<int32> Children;

	// Indices into URudeMoveNetwork::Transitions of the edges leaving this node.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	TArray<int32> Transitions;

	// Every leaf field of the node, unconverted.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|MoVE")
	TMap<FString, FString> RawFields;
};

// ONE .mrf (a MoVE network: the animation state graph a task drives) as a readable graph.
//
// ⛔ READ-ONLY TIER. This is a MEASUREMENT of the network, not a projection of it: no AnimBlueprint,
// no AnimGraph node, no state machine is generated, and there is no writer - nothing in RUDE emits a
// .mrf. A full AnimBlueprint projection is a later epic; this asset is what makes the graph
// inspectable in the meantime.
//
// Two facts bound what a reader can honestly do with it. First, the names are hashes: 25,908 of the
// 26,565 non-empty <Name> texts in the corpus are hash_XXXXXXXX, so a state's identity is a number the
// file does not spell out. Second, the leaf animation a Clip node plays is named through a clip SET,
// not a clip: 5,046 of the 5,264 clip records reference a ClipSet or VariableClipSet, whose contents
// live in clip_sets.ymt and not in the .mrf.
UCLASS(BlueprintType)
class RUDECORE_API URudeMoveNetwork : public UDataAsset
{
	GENERATED_BODY()

public:
	// ---- identity ---------------------------------------------------------------------------
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString NetworkName;

	// Always "read-only".
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
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

	// ---- the graph --------------------------------------------------------------------------
	// Node 0 is the RootState. 124 of the 162 networks root in a StateMachine and 38 in a bare State.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	TArray<FRudeMoveNode> Nodes;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	TArray<FRudeMoveTransition> Transitions;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	TArray<FRudeMoveNamedBit> Triggers;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	TArray<FRudeMoveNamedBit> Flags;

	// ---- counts the reader can check without walking the graph --------------------------------
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString RootType;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 NumStates = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 NumStateMachines = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 NumClips = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 NumConditions = 0;

	// Transitions whose TargetState resolved to a sibling node, and those that did not.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 TransitionsResolved = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 TransitionsUnresolved = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 HashedNames = 0;

	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	int32 PlainNames = 0;

	// Node type -> count, for this network.
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	TMap<FString, int32> NodeTypeCounts;

	// ---- honesty ------------------------------------------------------------------------------
	UPROPERTY(VisibleAnywhere, Category = "RUDE|MoVE")
	FString TierNote;
};
