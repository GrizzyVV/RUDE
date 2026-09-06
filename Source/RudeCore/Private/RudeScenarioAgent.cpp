// RUDE - RAGE <-> Unreal Development Environment
#include "RudeScenarioAgent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/ConstructorHelpers.h"

// ---- the graph ---------------------------------------------------------------------------------------
bool FRudeScenarioGraph::Build(UWorld* World, const FString& RegionFilter, FRudeScenarioGraph& Out, FString& OutWhy)
{
	Out = FRudeScenarioGraph();
	if (!World) { OutWhy = TEXT("no world"); return false; }
	const FString Want = RegionFilter.TrimStartAndEnd().ToLower();
	AActor* Chains = nullptr;
	TArray<AActor*> PointActors;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		FString Region;
		bool bIsChains = false, bIsPoint = false;
		for (const FName& T : A->Tags)
		{
			const FString S = T.ToString();
			if (S == TEXT("RUDE_SCEN_Chains")) { bIsChains = true; }
			else if (S.StartsWith(TEXT("RUDE_SCEN_Point:"))) { bIsPoint = true; }
			else if (S.StartsWith(TEXT("RUDE_SCEN:"))) { Region = S.Mid(10); }
		}
		if (!bIsChains && !bIsPoint) { continue; }
		if (!Want.IsEmpty() && !Region.Equals(Want, ESearchCase::IgnoreCase)) { continue; }
		if (bIsChains && !Chains) { Chains = A; Out.Region = Region; }
		if (bIsPoint) { PointActors.Add(A); }
	}
	if (!Chains)
	{
		OutWhy = Want.IsEmpty() ? FString(TEXT("no RUDE_SCEN_Chains actor in the level - ImportScenarioRegion first"))
		                        : FString::Printf(TEXT("no RUDE_SCEN_Chains actor tagged RUDE_SCEN:%s"), *Want);
		return false;
	}
	// edge records: RUDE_SCEN_Edge:<i>,<from>,<to>,<action>,<navMode>,<navSpeed>,<chain>
	TMap<int32, FRudeAgentEdge> Records;
	for (const FName& T : Chains->Tags)
	{
		const FString S = T.ToString();
		if (!S.StartsWith(TEXT("RUDE_SCEN_Edge:"))) { continue; }
		TArray<FString> F;
		S.Mid(15).ParseIntoArray(F, TEXT(","), true);
		if (F.Num() < 3) { continue; }
		FRudeAgentEdge E;
		E.Index = FCString::Atoi(*F[0]);
		E.From = FCString::Atoi(*F[1]);
		E.To = FCString::Atoi(*F[2]);
		E.Chain = F.Num() >= 7 ? FCString::Atoi(*F[6]) : -1;
		Records.Add(E.Index, E);
	}
	// the drawn splines give the positions; only an edge that has one is walkable (a filtered import
	// draws fewer, and the walk must not invent routes the level does not show)
	TArray<USplineComponent*> Splines;
	Chains->GetComponents<USplineComponent>(Splines);
	for (USplineComponent* Sp : Splines)
	{
		const FString N = Sp->GetName();
		if (!N.StartsWith(TEXT("Edge_")) || Sp->GetNumberOfSplinePoints() < 2) { continue; }
		const int32 Idx = FCString::Atoi(*N.Mid(5));
		FRudeAgentEdge E;
		if (const FRudeAgentEdge* R = Records.Find(Idx)) { E = *R; }
		else { E.Index = Idx; E.From = -1000000 - Idx; E.To = -2000000 - Idx; }   // no record: an isolated edge
		E.A = Sp->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
		E.B = Sp->GetLocationAtSplinePoint(1, ESplineCoordinateSpace::World);
		const int32 At = Out.Edges.Add(E);
		Out.OutEdges.FindOrAdd(E.From).Add(At);
	}
	for (AActor* P : PointActors) { Out.PointLocations.Add(P->GetActorLocation()); }
	if (Out.Edges.Num() == 0) { OutWhy = TEXT("the chains actor has no Edge_* splines"); return false; }
	return true;
}

// ---- the agent ---------------------------------------------------------------------------------------
ARudeScenarioAgent::ARudeScenarioAgent()
{
	PrimaryActorTick.bCanEverTick = true;
	GetCapsuleComponent()->InitCapsuleSize(30.f, 90.f);
	// agents pass through each other and the player: a rehearsal of routes, not a crowd simulation
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	bUseControllerRotationYaw = false;
	AutoPossessAI = EAutoPossessAI::Disabled;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->bRunPhysicsWithNoController = true;   // no AIController: the agent feeds its own input
		Move->bOrientRotationToMovement = true;
		Move->RotationRate = FRotator(0.f, 360.f, 0.f);
		Move->MaxWalkSpeed = WalkSpeed;
		Move->MaxFlySpeed = WalkSpeed;
		Move->MaxStepHeight = 45.f;
		Move->SetWalkableFloorAngle(50.f);
	}
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(RootComponent);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetRelativeScale3D(FVector(0.5f, 0.5f, 1.7f));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cyl.Succeeded()) { Body->SetStaticMesh(Cyl.Object); }
}

void ARudeScenarioAgent::InitFromGraph(const TSharedPtr<FRudeScenarioGraph>& InGraph, int32 StartEdgeIdx, int32 Seed)
{
	Graph = InGraph;
	Rand.Initialize(Seed);
	PendingStartEdge = StartEdgeIdx;
}

bool ARudeScenarioAgent::BindToChains()
{
	TSharedPtr<FRudeScenarioGraph> G = MakeShared<FRudeScenarioGraph>();
	FString Why;
	if (!FRudeScenarioGraph::Build(GetWorld(), Region, *G, Why)) { State = Why; return false; }
	Graph = G;
	Region = G->Region;
	if (Group.IsEmpty()) { Group = Region; }
	Rand.Initialize(GetUniqueID());
	PendingStartEdge = -1;
	return true;
}

void ARudeScenarioAgent::SetAgentEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	SetActorHiddenInGame(!bInEnabled);
	SetActorTickEnabled(bInEnabled);
	if (!bInEnabled) { if (UCharacterMovementComponent* M = GetCharacterMovement()) { M->StopMovementImmediately(); } }
}

void ARudeScenarioAgent::BeginPlay()
{
	Super::BeginPlay();
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxWalkSpeed = WalkSpeed;
		Move->MaxFlySpeed = WalkSpeed;
		if (bFly) { Move->SetMovementMode(MOVE_Flying); }
	}
	if (!Graph.IsValid()) { BindToChains(); }
	if (Graph.IsValid() && Graph->Edges.Num() > 0)
	{
		const int32 Start = Graph->Edges.IsValidIndex(PendingStartEdge) ? PendingStartEdge : Rand.RandRange(0, Graph->Edges.Num() - 1);
		StartEdge(Start, true);
	}
}

void ARudeScenarioAgent::StartEdge(int32 EdgeIdx, bool bTeleport)
{
	CurrentEdge = EdgeIdx;
	OnEdgeSeconds = 0.f;
	State = TEXT("walk");
	if (bTeleport && Graph.IsValid() && Graph->Edges.IsValidIndex(EdgeIdx))
	{
		SetActorLocation(Graph->Edges[EdgeIdx].A + FVector(0.f, 0.f, 100.f), false, nullptr, ETeleportType::TeleportPhysics);
	}
}

int32 ARudeScenarioAgent::PickNextEdge(int32 FromNode) const
{
	if (!Graph.IsValid()) { return -1; }
	const TArray<int32>* Outs = Graph->OutEdges.Find(FromNode);
	if (!Outs || Outs->Num() == 0) { return -1; }
	return (*Outs)[Rand.RandRange(0, Outs->Num() - 1)];
}

bool ARudeScenarioAgent::NearPoint(const FVector& Loc) const
{
	if (!Graph.IsValid()) { return false; }
	const double R2 = (double)PointRadius * (double)PointRadius;
	for (const FVector& P : Graph->PointLocations) { if (FVector::DistSquared2D(P, Loc) <= R2) { return true; } }
	return false;
}

void ARudeScenarioAgent::Tick(float Dt)
{
	Super::Tick(Dt);
	if (!bEnabled || !Graph.IsValid() || Graph->Edges.Num() == 0) { return; }
	if (IdleLeft > 0.f)
	{
		IdleLeft -= Dt;
		if (IdleLeft > 0.f) { return; }
		if (CurrentEdge < 0) { StartEdge(Rand.RandRange(0, Graph->Edges.Num() - 1), true); return; }
		State = TEXT("walk");
	}
	if (!Graph->Edges.IsValidIndex(CurrentEdge)) { StartEdge(Rand.RandRange(0, Graph->Edges.Num() - 1), true); return; }
	const FRudeAgentEdge& E = Graph->Edges[CurrentEdge];
	FVector D = E.B - GetActorLocation();
	if (!bFly) { D.Z = 0.f; }
	OnEdgeSeconds += Dt;
	const bool bStuck = OnEdgeSeconds > StuckSeconds;
	if (D.Size() <= ArriveRadius || bStuck)
	{
		if (bStuck)
		{
			++StuckCount;
			SetActorLocation(E.B + FVector(0.f, 0.f, 100.f), false, nullptr, ETeleportType::TeleportPhysics);
		}
		++EdgesWalked;
		const int32 Next = PickNextEdge(E.To);
		IdleLeft = NearPoint(E.B) ? PointIdleSeconds : IdleSeconds;
		State = Next < 0 ? TEXT("idle (chain end)") : TEXT("idle");
		CurrentEdge = Next;   // -1 = dead end: after the idle, restart on a random edge
		OnEdgeSeconds = 0.f;
		return;
	}
	AddMovementInput(D.GetSafeNormal(), 1.f);
}
