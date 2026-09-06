// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "RudeScenarioAgent.generated.h"

class UStaticMeshComponent;

// One directed chain edge as the sandbox walks it: the two node positions (UE cm) plus the node ids read
// from the display actor's RUDE_SCEN_Edge tags, so the walk continues node-to-node by INDEX (the graph's
// own adjacency), never by a position tolerance. Only the first two points of an Edge_<i> spline are the
// edge; the third, when present, is the arrowhead barb ImportScenarioRegion folds back from the end.
USTRUCT(BlueprintType)
struct RUDECORE_API FRudeAgentEdge
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") int32 Index = -1;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") int32 From = -1;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") int32 To = -1;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") int32 Chain = -1;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") FVector A = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") FVector B = FVector::ZeroVector;
};

// The chain graph of one imported scenario region, read once from the level (the <region>_Chains actor
// tagged RUDE_SCEN_Chains + RUDE_SCEN:<region>, its Edge_<i> splines and RUDE_SCEN_Edge:<i,from,to,...> tags)
// and shared by every agent the shim spawns.
USTRUCT(BlueprintType)
struct RUDECORE_API FRudeScenarioGraph
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") FString Region;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") TArray<FRudeAgentEdge> Edges;
	// Scenario point positions (RUDE_SCEN_Point:<i> actors): an agent idles longer when it arrives near one.
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Agent") TArray<FVector> PointLocations;
	// node id -> indices into Edges that leave it
	TMap<int32, TArray<int32>> OutEdges;

	// Empty RegionFilter = the first chains actor found. False (with why) when the level has none.
	static bool Build(UWorld* World, const FString& RegionFilter, FRudeScenarioGraph& Out, FString& OutWhy);
};

// THE SANDBOX (GDD 1b.3): an ambient agent. A Character (capsule + engine cylinder body, no controller:
// bRunPhysicsWithNoController) that walks a chain edge to its end node, idles, then takes a random
// outgoing edge of that node; at a dead end it idles and restarts on a random edge. Loops forever.
// Drop one in a level by hand (it binds to the chains actor itself at BeginPlay) or let the shim's
// StartAmbientAgents spawn a crowd sharing one graph.
UCLASS(Blueprintable)
class RUDECORE_API ARudeScenarioAgent : public ACharacter
{
	GENERATED_BODY()

public:
	ARudeScenarioAgent();

	// Region (file stem) whose chains to walk; empty = the first chains actor in the level.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") FString Region;
	// What SetScenarioGroupEnabled matches (the shim sets it to the region; a RUDE convention, see NOTES).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") FString Group;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") float WalkSpeed = 160.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") float IdleSeconds = 1.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") float PointIdleSeconds = 6.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") float PointRadius = 250.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") float ArriveRadius = 60.f;
	// Gave up on an edge after this long (blocked by a prop): teleports to the edge's end and carries on.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") float StuckSeconds = 12.f;
	// Fly instead of walk (no gravity): for a level whose ground has no collision yet.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") bool bFly = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RUDE|Agent") bool bEnabled = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Agent") int32 CurrentEdge = -1;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Agent") int32 EdgesWalked = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Agent") int32 StuckCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Agent") FString State;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RUDE|Agent")
	TObjectPtr<UStaticMeshComponent> Body;

	// Shim path: share a graph, start on StartEdge (or random when -1). Call before FinishSpawning.
	void InitFromGraph(const TSharedPtr<FRudeScenarioGraph>& InGraph, int32 StartEdgeIdx, int32 Seed);
	// Self-serve path: read the graph from the level (an agent placed by hand).
	UFUNCTION(BlueprintCallable, Category = "RUDE|Agent") bool BindToChains();
	UFUNCTION(BlueprintCallable, Category = "RUDE|Agent") void SetAgentEnabled(bool bInEnabled);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	TSharedPtr<FRudeScenarioGraph> Graph;
	FRandomStream Rand;
	int32 PendingStartEdge = -1;
	float IdleLeft = 0.f;
	float OnEdgeSeconds = 0.f;

	void StartEdge(int32 EdgeIdx, bool bTeleport);
	int32 PickNextEdge(int32 FromNode) const;
	bool NearPoint(const FVector& Loc) const;
};
