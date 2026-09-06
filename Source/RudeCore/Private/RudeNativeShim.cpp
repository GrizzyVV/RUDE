// RUDE - RAGE <-> Unreal Development Environment
#include "RudeNativeShim.h"
#include "RudeEntityComponent.h"
#include "RudeScenarioAgent.h"
#include "RudeDriveablePawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieSceneSequencePlaybackSettings.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"

DEFINE_LOG_CATEGORY(LogRudeSandbox);

namespace RudeShim
{
	static FString NormYmap(const FString& In)
	{
		FString Y = In.TrimStartAndEnd().ToLower();
		Y.RemoveFromEnd(TEXT(".ymap"));
		return Y;
	}
	// ImportCutscene's asset-name rule (RudeAnims.cpp AssetNameOf): non-alphanumerics become '_'
	static FString AssetNameOf(const FString& In)
	{
		FString O;
		for (const TCHAR C : In) { O.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_')); }
		return O.IsEmpty() ? FString(TEXT("_")) : O;
	}
	static bool ParseBool(const FString& S, bool Def)
	{
		const FString L = S.TrimStartAndEnd().ToLower();
		if (L.IsEmpty()) { return Def; }
		return L == TEXT("1") || L == TEXT("true") || L == TEXT("on") || L == TEXT("yes");
	}
	static void SetActorShown(AActor* A, bool bShow)
	{
		A->SetActorHiddenInGame(!bShow);
		TArray<UPrimitiveComponent*> Prims;
		A->GetComponents<UPrimitiveComponent>(Prims);
		for (UPrimitiveComponent* P : Prims) { P->SetHiddenInGame(!bShow, true); P->SetVisibility(bShow, true); }
	}
}

// ---- subsystem ---------------------------------------------------------------------------------------
bool URudeNativeShim::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* W = Cast<UWorld>(Outer);
	return W && W->IsGameWorld();
}

bool URudeNativeShim::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void URudeNativeShim::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogRudeSandbox, Display, TEXT("[RUDE Sandbox] native shim up in %s - Rude.Native Help"), *GetNameSafe(GetWorld()));
}

void URudeNativeShim::Deinitialize()
{
	StopCutsceneInternal();
	Agents.Empty();
	Super::Deinitialize();
}

URudeNativeShim* URudeNativeShim::Get(const UObject* WorldContext)
{
	UWorld* W = (GEngine && WorldContext) ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (URudeNativeShim* S = W ? W->GetSubsystem<URudeNativeShim>() : nullptr) { return S; }
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* GW = Ctx.World();
			if (!GW || !GW->IsGameWorld()) { continue; }
			if (URudeNativeShim* S = GW->GetSubsystem<URudeNativeShim>()) { return S; }
		}
	}
	return nullptr;
}

void URudeNativeShim::Log(const FString& Native, const FString& Args, const FString& Result, bool bOk)
{
	FRudeNativeEvent E;
	E.Time = GetWorld() ? (float)GetWorld()->GetTimeSeconds() : 0.f;
	E.Native = Native; E.Args = Args; E.Result = Result; E.bOk = bOk;
	EventLog.Add(E);
	const FString Line = FString::Printf(TEXT("[%7.2f] %s(%s) -> %s"), E.Time, *Native, *Args, *Result);
	UE_LOG(LogRudeSandbox, Display, TEXT("%s"), *Line);
	if (bOnScreen && GEngine) { GEngine->AddOnScreenDebugMessage(-1, 8.f, bOk ? FColor::Green : FColor::Orange, Line); }
	OnNativeCalled.Broadcast(Native, Args, Result);
}

void URudeNativeShim::Tick(float Dt)
{
	Super::Tick(Dt);
	if (!bClockPaused && MinutesPerRealSecond > 0.f)
	{
		Hour = FMath::Fmod(Hour + Dt * MinutesPerRealSecond / 60.f, 24.f);
		ApplyClock(false);
	}
	if (PendingIplSweeps.Num() > 0 && GetWorld())
	{
		SweepAccum += Dt;
		if (SweepAccum >= 0.5f)
		{
			SweepAccum = 0.f;
			const double Now = GetWorld()->GetTimeSeconds();
			for (auto It = PendingIplSweeps.CreateIterator(); It; ++It)
			{
				const int32 N = SweepYmapActors(It.Key(), true);
				if (N > 0) { Log(TEXT("EnableIpl"), It.Key(), FString::Printf(TEXT("streamed in: %d actors shown"), N)); }
				if (N > 0 || Now > It.Value()) { It.RemoveCurrent(); }
			}
		}
	}
	if (bOnScreen && GEngine)
	{
		const int32 H = FMath::Clamp((int32)Hour, 0, 23), M = FMath::Clamp((int32)((Hour - H) * 60.f), 0, 59);
		GEngine->AddOnScreenDebugMessage(7001, 1.f, FColor::White,
			FString::Printf(TEXT("[RUDE Sandbox] clock %02d:%02d%s  ipls:%d  agents:%d  cutscene:%s  log:%d   (Rude.Native Help)"),
				H, M, bClockPaused ? TEXT(" (paused)") : TEXT(""), ActiveIpls.Num(), Agents.Num(),
				CutscenePlayer && CutscenePlayer->IsPlaying() ? *LoadedCutsceneName : TEXT("-"), EventLog.Num()));
	}
}

// ---- IPL / ymap --------------------------------------------------------------------------------------
bool URudeNativeShim::SetYmapLayerState(const FString& Ymap, bool bLoad, FString& OutWhat)
{
	UDataLayerManager* DLM = UDataLayerManager::GetDataLayerManager(GetWorld());
	if (!DLM) { OutWhat = TEXT("none(no DataLayerManager)"); return false; }
	const FString Want = TEXT("DL_") + Ymap;
	UDataLayerInstance* Found = nullptr;
	DLM->ForEachDataLayerInstance([&Found, &Want](UDataLayerInstance* DL)
	{
		if (DL && DL->GetDataLayerShortName().Equals(Want, ESearchCase::IgnoreCase)) { Found = DL; return false; }
		return true;
	});
	if (!Found) { OutWhat = TEXT("none(") + Want + TEXT(" not in this world)"); return false; }
	const EDataLayerRuntimeState S = bLoad ? EDataLayerRuntimeState::Activated : EDataLayerRuntimeState::Unloaded;
	const bool bOk = DLM->SetDataLayerInstanceRuntimeState(Found, S, false);
	OutWhat = FString::Printf(TEXT("%s->%s%s"), *Want, bLoad ? TEXT("Activated") : TEXT("Unloaded"), bOk ? TEXT("") : TEXT("(refused)"));
	return bOk;
}

int32 URudeNativeShim::SweepYmapActors(const FString& Ymap, bool bShow)
{
	UWorld* World = GetWorld();
	if (!World) { return 0; }
	int32 Touched = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		const URudeEntityComponent* R = A->FindComponentByClass<URudeEntityComponent>();
		if (!R || RudeShim::NormYmap(R->SourceYmap) != Ymap) { continue; }
		if (bShow)
		{
			// SetYmapVisible's rule: a LOD shell SetLodView hid stays hidden
			bool bLodHidden = false;
			for (const FName& T : A->Tags)
			{
				const FString S = T.ToString();
				if (S.StartsWith(TEXT("RUDE_LOD:")) && !S.EndsWith(TEXT("_HD"))) { bLodHidden = true; }
			}
			if (bLodHidden) { continue; }
		}
		RudeShim::SetActorShown(A, bShow);
		++Touched;
	}
	return Touched;
}

bool URudeNativeShim::SetIpl(const FString& Native, const FString& Name, bool bOn)
{
	const FString Y = RudeShim::NormYmap(Name);
	if (Y.IsEmpty()) { Log(Native, Name, TEXT("give a ymap name"), false); return false; }
	FString Layer;
	const bool bLayer = SetYmapLayerState(Y, bOn, Layer);
	const int32 Touched = SweepYmapActors(Y, bOn);
	if (bOn) { ActiveIpls.Add(Y); if (bLayer && GetWorld()) { PendingIplSweeps.Add(Y, GetWorld()->GetTimeSeconds() + 15.0); } }
	else { ActiveIpls.Remove(Y); PendingIplSweeps.Remove(Y); }
	const bool bOk = bLayer || Touched > 0;
	Log(Native, Y, FString::Printf(TEXT("layer:%s actors:%d%s"), *Layer, Touched,
		bOk ? TEXT("") : TEXT(" - nothing by that name (Rude.Native ListIpls)")), bOk);
	return bOk;
}

bool URudeNativeShim::EnableIpl(const FString& Name) { return SetIpl(TEXT("EnableIpl"), Name, true); }
bool URudeNativeShim::RequestIpl(const FString& Name) { return SetIpl(TEXT("RequestIpl"), Name, true); }
bool URudeNativeShim::RemoveIpl(const FString& Name) { return SetIpl(TEXT("RemoveIpl"), Name, false); }
bool URudeNativeShim::IsIplActive(const FString& Name) const { return ActiveIpls.Contains(RudeShim::NormYmap(Name)); }

FString URudeNativeShim::ListIpls()
{
	UWorld* World = GetWorld();
	TSet<FString> Names;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (!It->ActorHasTag(FName(TEXT("RUDE_SCRIPT_YMAP")))) { continue; }
			if (const URudeEntityComponent* R = It->FindComponentByClass<URudeEntityComponent>()) { Names.Add(RudeShim::NormYmap(R->SourceYmap)); }
		}
		if (UDataLayerManager* DLM = UDataLayerManager::GetDataLayerManager(World))
		{
			DLM->ForEachDataLayerInstance([&Names, DLM](UDataLayerInstance* DL)
			{
				const FString N = DL ? DL->GetDataLayerShortName() : FString();
				if (N.StartsWith(TEXT("DL_")) && DLM->GetDataLayerInstanceRuntimeState(DL) == EDataLayerRuntimeState::Unloaded) { Names.Add(N.Mid(3).ToLower()); }
				return true;
			});
		}
	}
	TArray<FString> Sorted = Names.Array();
	Sorted.Sort();
	const FString Joined = FString::Join(Sorted, TEXT(" "));
	Log(TEXT("ListIpls"), TEXT(""), FString::Printf(TEXT("%d script ymaps: %s"), Sorted.Num(), *Joined));
	return Joined;
}

// ---- cutscene ----------------------------------------------------------------------------------------
bool URudeNativeShim::RequestCutscene(const FString& Name)
{
	const FString N = Name.TrimStartAndEnd();
	ULevelSequence* LS = nullptr;
	FString Tried;
	if (N.StartsWith(TEXT("/"))) { LS = LoadObject<ULevelSequence>(nullptr, *N); Tried = N; }
	else if (!N.IsEmpty())
	{
		const FString A = RudeShim::AssetNameOf(N.ToLower());
		const FString P1 = FString::Printf(TEXT("/Game/RUDE/Cutscenes/%s/LS_%s.LS_%s"), *A, *A, *A);
		const FString P2 = FString::Printf(TEXT("/Game/RUDE/Cutscenes/%s.%s"), *A, *A);
		LS = LoadObject<ULevelSequence>(nullptr, *P1); Tried = P1;
		if (!LS) { LS = LoadObject<ULevelSequence>(nullptr, *P2); Tried += TEXT(" | ") + P2; }
	}
	if (!LS) { Log(TEXT("RequestCutscene"), N, TEXT("no Level Sequence at ") + Tried, false); return false; }
	LoadedCutscene = LS;
	LoadedCutsceneName = N;
	Log(TEXT("RequestCutscene"), N, TEXT("loaded ") + LS->GetPathName());
	return true;
}

bool URudeNativeShim::HasCutsceneLoaded() const { return LoadedCutscene != nullptr; }

bool URudeNativeShim::StartCutscene(const FString& Name)
{
	const FString N = Name.TrimStartAndEnd();
	if (!N.IsEmpty() && !N.Equals(LoadedCutsceneName, ESearchCase::IgnoreCase)) { if (!RequestCutscene(N)) { return false; } }
	if (!LoadedCutscene) { Log(TEXT("StartCutscene"), N, TEXT("nothing requested - RequestCutscene <name> first"), false); return false; }
	StopCutsceneInternal();
	FMovieSceneSequencePlaybackSettings Settings;
	Settings.bAutoPlay = false;
	Settings.bPauseAtEnd = false;
	ALevelSequenceActor* Actor = nullptr;
	ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(GetWorld(), LoadedCutscene, Settings, Actor);
	if (!Player) { Log(TEXT("StartCutscene"), LoadedCutsceneName, TEXT("CreateLevelSequencePlayer failed"), false); return false; }
	CutscenePlayer = Player;
	CutsceneActor = Actor;
	Player->Play();
	Log(TEXT("StartCutscene"), LoadedCutsceneName, FString::Printf(TEXT("playing, %.1f s"), Player->GetDuration().AsSeconds()));
	return true;
}

void URudeNativeShim::StopCutsceneInternal()
{
	if (CutscenePlayer) { CutscenePlayer->Stop(); CutscenePlayer = nullptr; }
	if (CutsceneActor) { CutsceneActor->Destroy(); CutsceneActor = nullptr; }
}

void URudeNativeShim::StopCutscene()
{
	const bool bWas = CutscenePlayer != nullptr;
	StopCutsceneInternal();
	Log(TEXT("StopCutscene"), LoadedCutsceneName, bWas ? TEXT("stopped") : TEXT("nothing playing"));
}

void URudeNativeShim::RemoveCutscene()
{
	StopCutsceneInternal();
	Log(TEXT("RemoveCutscene"), LoadedCutsceneName, LoadedCutscene ? TEXT("released") : TEXT("nothing loaded"));
	LoadedCutscene = nullptr;
	LoadedCutsceneName.Empty();
}

// ---- clock -------------------------------------------------------------------------------------------
int32 URudeNativeShim::SweepTimeMasks(int32 Hr)
{
	// SetWorldHour's rule at runtime: bit N of a RUDE_TIME:<mask> tag means visible during hour N
	UWorld* World = GetWorld();
	if (!World) { return 0; }
	const uint32 Bit = 1u << FMath::Clamp(Hr, 0, 23);
	int32 Gated = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<UStaticMeshComponent*> Comps;   // ISM components are static mesh components
		It->GetComponents<UStaticMeshComponent>(Comps);
		for (UStaticMeshComponent* C : Comps)
		{
			for (const FName& Tag : C->ComponentTags)
			{
				FString T = Tag.ToString();
				if (!T.StartsWith(TEXT("RUDE_TIME:"))) { continue; }
				T.RightChopInline(10);
				const uint32 Mask = (uint32)FCString::Strtoui64(*T, nullptr, 10);
				const bool bVisible = (Mask & Bit) != 0;
				C->SetVisibility(bVisible, true);
				C->SetHiddenInGame(!bVisible);
				++Gated;
				break;
			}
		}
	}
	return Gated;
}

void URudeNativeShim::ApplyClock(bool bForceMasks)
{
	UWorld* World = GetWorld();
	if (!World) { return; }
	if (!Sun.IsValid())
	{
		for (TActorIterator<ADirectionalLight> It(World); It; ++It) { if (It->ActorHasTag(FName(TEXT("RUDE_SKY")))) { Sun = *It; break; } }
	}
	// 06:00 on the horizon, 12:00 overhead, 18:00 horizon, midnight straight below (a plain sine, no latitude)
	const float T = Hour / 24.f;
	const float Elev = 90.f * FMath::Sin((T - 0.25f) * 2.f * PI);
	if (Sun.IsValid())
	{
		FRotator R = Sun->GetActorRotation();
		R.Pitch = -Elev;
		Sun->SetActorRotation(R);
		if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(Sun->GetLightComponent())) { C->SetIntensity(Elev > 0.f ? 8.f : 0.05f); }
	}
	const int32 Hr = FMath::Clamp((int32)Hour, 0, 23);
	if (bForceMasks || Hr != LastMaskHour) { LastMaskHour = Hr; LastGated = SweepTimeMasks(Hr); }
}

void URudeNativeShim::SetClockTime(int32 Hours, int32 Minutes, int32 Seconds)
{
	const int32 H = ((Hours % 24) + 24) % 24;
	Hour = FMath::Fmod((float)H + FMath::Clamp(Minutes, 0, 59) / 60.f + FMath::Clamp(Seconds, 0, 59) / 3600.f, 24.f);
	ApplyClock(true);
	Log(TEXT("SetClockTime"), FString::Printf(TEXT("%d,%d,%d"), Hours, Minutes, Seconds),
		FString::Printf(TEXT("hour=%.2f sun=%s masked=%d"), Hour, Sun.IsValid() ? TEXT("RUDE_SKY") : TEXT("none(no RUDE_SKY light)"), LastGated));
}

void URudeNativeShim::NetworkOverrideClockTime(int32 Hours, int32 Minutes, int32 Seconds)
{
	SetClockTime(Hours, Minutes, Seconds);
	EventLog.Last().Native = TEXT("NetworkOverrideClockTime");
}

void URudeNativeShim::PauseClock(bool bPaused)
{
	bClockPaused = bPaused;
	Log(TEXT("PauseClock"), bPaused ? TEXT("true") : TEXT("false"), bPaused ? TEXT("clock stopped") : TEXT("clock running"));
}

int32 URudeNativeShim::GetClockHours() const { return FMath::Clamp((int32)Hour, 0, 23); }
int32 URudeNativeShim::GetClockMinutes() const { return FMath::Clamp((int32)((Hour - (float)GetClockHours()) * 60.f), 0, 59); }

// ---- interiors ---------------------------------------------------------------------------------------
bool URudeNativeShim::SetEntitySet(const FString& Native, const FString& Interior, const FString& EntitySet, bool bOn)
{
	UWorld* World = GetWorld();
	const FString I = Interior.TrimStartAndEnd().ToLower();
	const FString S = EntitySet.TrimStartAndEnd().ToLower();
	if (S.IsEmpty()) { Log(Native, Interior, TEXT("give an entity set name"), false); return false; }
	const FString SetTag = TEXT("rude_mlo_entityset:") + S;
	const FString MloTag = TEXT("rude_mlo:") + I;
	int32 Touched = 0;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			bool bInInterior = I.IsEmpty(), bInSet = false;
			for (const FName& T : It->Tags)
			{
				const FString L = T.ToString().ToLower();
				if (L == SetTag) { bInSet = true; }
				else if (L == MloTag) { bInInterior = true; }
			}
			if (!bInSet || !bInInterior) { continue; }
			RudeShim::SetActorShown(*It, bOn);
			++Touched;
		}
	}
	const FString Key = I + TEXT("|") + S;
	if (bOn) { ActiveEntitySets.Add(Key); } else { ActiveEntitySets.Remove(Key); }
	Log(Native, I + TEXT(",") + S, Touched > 0 ? FString::Printf(TEXT("actors:%d"), Touched)
		: FString(TEXT("no actor tagged RUDE_MLO_EntitySet:<set> (ImportMlo does not tag entity sets yet - see NOTES)")), Touched > 0);
	return Touched > 0;
}

bool URudeNativeShim::ActivateInteriorEntitySet(const FString& Interior, const FString& EntitySet) { return SetEntitySet(TEXT("ActivateInteriorEntitySet"), Interior, EntitySet, true); }
bool URudeNativeShim::DeactivateInteriorEntitySet(const FString& Interior, const FString& EntitySet) { return SetEntitySet(TEXT("DeactivateInteriorEntitySet"), Interior, EntitySet, false); }

// ---- scenarios ---------------------------------------------------------------------------------------
void URudeNativeShim::SetScenarioGroupEnabled(const FString& Group, bool bEnabled)
{
	const FString G = Group.TrimStartAndEnd().ToLower();
	int32 AgentsTouched = 0, Markers = 0;
	for (ARudeScenarioAgent* Ag : Agents)
	{
		if (!Ag) { continue; }
		if (G == TEXT("all") || Ag->Group.ToLower() == G || Ag->Region.ToLower() == G) { Ag->SetAgentEnabled(bEnabled); ++AgentsTouched; }
	}
	if (UWorld* World = GetWorld())
	{
		const FString Tag = TEXT("rude_scen:") + G;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			for (const FName& T : It->Tags)
			{
				if (T.ToString().ToLower() == Tag) { RudeShim::SetActorShown(*It, bEnabled); ++Markers; break; }
			}
		}
	}
	if (bEnabled) { DisabledGroups.Remove(G); } else { DisabledGroups.Add(G); }
	Log(TEXT("SetScenarioGroupEnabled"), G + (bEnabled ? TEXT(",true") : TEXT(",false")),
		FString::Printf(TEXT("agents:%d markers:%d"), AgentsTouched, Markers), AgentsTouched + Markers > 0);
}

bool URudeNativeShim::IsScenarioGroupEnabled(const FString& Group) const { return !DisabledGroups.Contains(Group.TrimStartAndEnd().ToLower()); }

void URudeNativeShim::SetScenarioTypeEnabled(const FString& Type, bool bEnabled)
{
	const FString Ty = Type.TrimStartAndEnd().ToLower();
	int32 Markers = 0;
	if (UWorld* World = GetWorld())
	{
		const FString Tag = TEXT("rude_scen_type:") + Ty;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			for (const FName& T : It->Tags)
			{
				if (T.ToString().ToLower() == Tag) { RudeShim::SetActorShown(*It, bEnabled); ++Markers; break; }
			}
		}
	}
	if (bEnabled) { DisabledTypes.Remove(Ty); } else { DisabledTypes.Add(Ty); }
	Log(TEXT("SetScenarioTypeEnabled"), Ty + (bEnabled ? TEXT(",true") : TEXT(",false")), FString::Printf(TEXT("markers:%d (agents carry no type yet)"), Markers), Markers > 0);
}

int32 URudeNativeShim::StartAmbientAgents(int32 Count, const FString& Region)
{
	UWorld* World = GetWorld();
	if (!World) { return 0; }
	const FString Want = Region.TrimStartAndEnd().ToLower();
	if (!Graph.IsValid() || (!Want.IsEmpty() && !Graph->Region.Equals(Want, ESearchCase::IgnoreCase)))
	{
		TSharedPtr<FRudeScenarioGraph> G = MakeShared<FRudeScenarioGraph>();
		FString Why;
		if (!FRudeScenarioGraph::Build(World, Want, *G, Why)) { Log(TEXT("StartAmbientAgents"), Region, Why, false); return 0; }
		Graph = G;
	}
	const int32 N = FMath::Clamp(Count <= 0 ? 8 : Count, 1, 200);
	FRandomStream Rand(Agents.Num() * 7919 + N + 1);
	int32 Spawned = 0;
	for (int32 i = 0; i < N; ++i)
	{
		const int32 EdgeIdx = Rand.RandRange(0, Graph->Edges.Num() - 1);
		const FTransform Xf(FRotator::ZeroRotator, Graph->Edges[EdgeIdx].A + FVector(0.f, 0.f, 100.f));
		ARudeScenarioAgent* Ag = World->SpawnActorDeferred<ARudeScenarioAgent>(ARudeScenarioAgent::StaticClass(), Xf, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Ag) { continue; }
		Ag->Region = Graph->Region;
		Ag->Group = Graph->Region;
		Ag->bFly = bAgentsFly;
		Ag->InitFromGraph(Graph, EdgeIdx, Rand.RandRange(1, 1 << 30));
		Ag->FinishSpawning(Xf);
		Agents.Add(Ag);
		++Spawned;
	}
	Log(TEXT("StartAmbientAgents"), FString::Printf(TEXT("%d,%s"), N, *Graph->Region),
		FString::Printf(TEXT("%d agents on %d edges, %d points%s"), Spawned, Graph->Edges.Num(), Graph->PointLocations.Num(), bAgentsFly ? TEXT(" (flying)") : TEXT("")), Spawned > 0);
	return Spawned;
}

void URudeNativeShim::StopAmbientAgents()
{
	int32 N = 0;
	for (ARudeScenarioAgent* Ag : Agents) { if (Ag) { Ag->Destroy(); ++N; } }
	Agents.Empty();
	Log(TEXT("StopAmbientAgents"), TEXT(""), FString::Printf(TEXT("%d agents removed"), N));
}

// ---- the event log -----------------------------------------------------------------------------------
// ---- vehicles (WP11 THE CHAOS TEST-DRIVE) --------------------------------------------------------------
bool URudeNativeShim::IsInVehicle() const
{
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	return PC && Cast<ARudeDriveablePawn>(PC->GetPawn()) != nullptr;
}

bool URudeNativeShim::EnterVehicle(const FString& Name)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC) { Log(TEXT("EnterVehicle"), Name, TEXT("no player controller in this world"), false); return false; }
	const FString Want = Name.TrimStartAndEnd().ToLower();
	ARudeDriveablePawn* Target = nullptr;
	TArray<FString> Present;
	for (TActorIterator<ARudeDriveablePawn> It(World); It; ++It)
	{
		const FString N = It->VehicleName.ToLower();
		Present.Add(N);
		if (Want.IsEmpty() || N == Want || It->ActorHasTag(FName(*(TEXT("RUDE_DRIVEABLE:") + Want))))
		{
			Target = *It;
			if (!Want.IsEmpty()) { break; }
		}
	}
	if (!Target)
	{
		Log(TEXT("EnterVehicle"), Want, FString::Printf(TEXT("no driveable '%s' in this level (BuildDriveable makes one from an imported composite); present: [%s]"),
			*Want, *FString::Join(Present, TEXT(","))), false);
		return false;
	}
	if (PC->GetPawn() == Target) { Log(TEXT("EnterVehicle"), Target->VehicleName, TEXT("already driving it")); return true; }
	if (APawn* Current = PC->GetPawn())
	{
		if (!Current->IsA<ARudeDriveablePawn>()) { OnFootPawn = Current; }
	}
	// park the walker beside the car - hidden and collision-free, so the test-drive cannot run it over
	if (APawn* P = OnFootPawn.Get())
	{
		const FTransform VT = Target->GetActorTransform();
		P->SetActorLocation(VT.TransformPosition(FVector(0.f, -350.f, 60.f)), false, nullptr, ETeleportType::TeleportPhysics);
		if (ACharacter* C = Cast<ACharacter>(P)) { if (UCharacterMovementComponent* M = C->GetCharacterMovement()) { M->StopMovementImmediately(); } }
		P->SetActorEnableCollision(false);
		P->SetActorHiddenInGame(true);
	}
	PC->Possess(Target);
	if (UChaosVehicleMovementComponent* Mv = Target->GetVehicleMovement()) { Mv->SetHandbrakeInput(false); Mv->SetSleeping(false); }
	DrivingPawn = Target;
	Log(TEXT("EnterVehicle"), Target->VehicleName, FString::Printf(TEXT("driving %s - W/S throttle-brake, A/D steer, Space handbrake, F exit, R upright"), *Target->GetName()));
	return true;
}

bool URudeNativeShim::ExitVehicle()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	ARudeDriveablePawn* V = PC ? Cast<ARudeDriveablePawn>(PC->GetPawn()) : nullptr;
	if (!V) { Log(TEXT("ExitVehicle"), TEXT(""), TEXT("not in a vehicle"), false); return false; }
	PC->UnPossess();   // the pawn's UnPossessed zeroes the inputs and sets the handbrake
	APawn* P = OnFootPawn.Get();
	if (!P)
	{
		// nothing to come back to (entered from a spectator, or the walker was destroyed): ask the game mode for a new one
		if (AGameModeBase* GM = World->GetAuthGameMode()) { GM->RestartPlayer(PC); P = PC->GetPawn(); }
	}
	const FTransform VT = V->GetActorTransform();
	// the driver's door: GTA cars are left-hand drive; in this mesh the left side is -Y
	const FVector Door = VT.TransformPosition(FVector(0.f, -250.f, 0.f)) + FVector(0.f, 0.f, 120.f);
	if (P)
	{
		P->SetActorEnableCollision(true);
		P->SetActorHiddenInGame(false);
		P->SetActorLocation(Door, false, nullptr, ETeleportType::TeleportPhysics);
		P->SetActorRotation(FRotator(0.f, VT.Rotator().Yaw, 0.f));
		if (ACharacter* C = Cast<ACharacter>(P)) { if (UCharacterMovementComponent* M = C->GetCharacterMovement()) { M->SetMovementMode(MOVE_Walking); } }
		if (PC->GetPawn() != P) { PC->Possess(P); }
	}
	DrivingPawn = nullptr;
	OnFootPawn = nullptr;
	Log(TEXT("ExitVehicle"), V->VehicleName, P ? TEXT("on foot beside the driver's door") : TEXT("unpossessed; no on-foot pawn to return to"), P != nullptr);
	return P != nullptr;
}

void URudeNativeShim::DumpEventLog()
{
	UE_LOG(LogRudeSandbox, Display, TEXT("[RUDE Sandbox] event log: %d entries"), EventLog.Num());
	for (const FRudeNativeEvent& E : EventLog)
	{
		UE_LOG(LogRudeSandbox, Display, TEXT("  [%7.2f] %s(%s) -> %s%s"), E.Time, *E.Native, *E.Args, *E.Result, E.bOk ? TEXT("") : TEXT("  [FAILED]"));
	}
	if (bOnScreen && GEngine)
	{
		const int32 From = FMath::Max(0, EventLog.Num() - 12);
		for (int32 i = From; i < EventLog.Num(); ++i)
		{
			const FRudeNativeEvent& E = EventLog[i];
			GEngine->AddOnScreenDebugMessage(-1, 12.f, E.bOk ? FColor::Silver : FColor::Orange, FString::Printf(TEXT("[%7.2f] %s(%s) -> %s"), E.Time, *E.Native, *E.Args, *E.Result));
		}
	}
}

void URudeNativeShim::ClearEventLog() { EventLog.Empty(); }

FString URudeNativeShim::Dispatch(const FString& Native, const TArray<FString>& Args)
{
	const FString N = Native.TrimStartAndEnd().ToLower();
	auto Arg = [&Args](int32 i) { return Args.IsValidIndex(i) ? Args[i] : FString(); };
	auto ArgB = [&Arg](int32 i, bool Def) { return RudeShim::ParseBool(Arg(i), Def); };
	auto ArgI = [&Arg](int32 i, int32 Def) { const FString S = Arg(i); return S.IsNumeric() ? FCString::Atoi(*S) : Def; };
	const int32 Before = EventLog.Num();
	if (N == TEXT("enableipl")) { EnableIpl(Arg(0)); }
	else if (N == TEXT("requestipl")) { RequestIpl(Arg(0)); }
	else if (N == TEXT("removeipl")) { RemoveIpl(Arg(0)); }
	else if (N == TEXT("isiplactive")) { const bool b = IsIplActive(Arg(0)); Log(TEXT("IsIplActive"), Arg(0), b ? TEXT("true") : TEXT("false")); }
	else if (N == TEXT("listipls")) { ListIpls(); }
	else if (N == TEXT("requestcutscene")) { RequestCutscene(Arg(0)); }
	else if (N == TEXT("hascutsceneloaded")) { Log(TEXT("HasCutsceneLoaded"), TEXT(""), HasCutsceneLoaded() ? TEXT("true") : TEXT("false")); }
	else if (N == TEXT("startcutscene")) { StartCutscene(Arg(0)); }
	else if (N == TEXT("stopcutscene")) { StopCutscene(); }
	else if (N == TEXT("removecutscene")) { RemoveCutscene(); }
	else if (N == TEXT("setclocktime")) { SetClockTime(ArgI(0, 12), ArgI(1, 0), ArgI(2, 0)); }
	else if (N == TEXT("networkoverrideclocktime")) { NetworkOverrideClockTime(ArgI(0, 12), ArgI(1, 0), ArgI(2, 0)); }
	else if (N == TEXT("pauseclock")) { PauseClock(ArgB(0, true)); }
	else if (N == TEXT("getclockhours")) { Log(TEXT("GetClockHours"), TEXT(""), FString::FromInt(GetClockHours())); }
	else if (N == TEXT("activateinteriorentityset")) { ActivateInteriorEntitySet(Arg(0), Arg(1)); }
	else if (N == TEXT("deactivateinteriorentityset")) { DeactivateInteriorEntitySet(Arg(0), Arg(1)); }
	else if (N == TEXT("setscenariogroupenabled")) { SetScenarioGroupEnabled(Arg(0), ArgB(1, true)); }
	else if (N == TEXT("isscenariogroupenabled")) { Log(TEXT("IsScenarioGroupEnabled"), Arg(0), IsScenarioGroupEnabled(Arg(0)) ? TEXT("true") : TEXT("false")); }
	else if (N == TEXT("setscenariotypeenabled")) { SetScenarioTypeEnabled(Arg(0), ArgB(1, true)); }
	else if (N == TEXT("startambientagents")) { StartAmbientAgents(ArgI(0, 8), Arg(1)); }
	else if (N == TEXT("stopambientagents")) { StopAmbientAgents(); }
	else if (N == TEXT("entervehicle")) { EnterVehicle(Arg(0)); }
	else if (N == TEXT("exitvehicle")) { ExitVehicle(); }
	else if (N == TEXT("isinvehicle")) { Log(TEXT("IsInVehicle"), TEXT(""), IsInVehicle() ? TEXT("true") : TEXT("false")); }
	else if (N == TEXT("log") || N == TEXT("dumpeventlog")) { DumpEventLog(); return FString::Printf(TEXT("%d entries"), EventLog.Num()); }
	else if (N == TEXT("clearlog") || N == TEXT("cleareventlog")) { ClearEventLog(); return TEXT("cleared"); }
	else if (N == TEXT("onscreen")) { bOnScreen = ArgB(0, true); return bOnScreen ? TEXT("on") : TEXT("off"); }
	else if (N == TEXT("fly")) { bAgentsFly = ArgB(0, true); return bAgentsFly ? TEXT("new agents fly") : TEXT("new agents walk"); }
	else
	{
		const FString Help = TEXT("Rude.Native <native> [args]:  EnableIpl|RequestIpl <ymap> | RemoveIpl <ymap> | IsIplActive <ymap> | ListIpls | "
			"RequestCutscene <cut> | StartCutscene [cut] | StopCutscene | RemoveCutscene | HasCutsceneLoaded | "
			"SetClockTime <h> <m> [s] | NetworkOverrideClockTime <h> <m> [s] | PauseClock <0|1> | GetClockHours | "
			"ActivateInteriorEntitySet <interior> <set> | DeactivateInteriorEntitySet <interior> <set> | "
			"SetScenarioGroupEnabled <group|all> <0|1> | SetScenarioTypeEnabled <type> <0|1> | StartAmbientAgents [count] [region] | StopAmbientAgents | "
			"EnterVehicle [model] | ExitVehicle | IsInVehicle | "
			"Log | ClearLog | OnScreen <0|1> | Fly <0|1>");
		if (N != TEXT("help") && !N.IsEmpty()) { Log(Native, FString::Join(Args, TEXT(" ")), TEXT("unknown native"), false); }
		UE_LOG(LogRudeSandbox, Display, TEXT("%s"), *Help);
		if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 15.f, FColor::Yellow, Help); }
		return Help;
	}
	return EventLog.Num() > Before ? EventLog.Last().Result : FString(TEXT("(no log entry)"));
}

// ---- the console surface ------------------------------------------------------------------------------
// `Rude.Native EnableIpl dt1_05` from the PIE viewport console or the editor's Output Log console.
static void RudeNativeConsole(const TArray<FString>& Args, UWorld* World)
{
	URudeNativeShim* Shim = URudeNativeShim::Get(World);
	if (!Shim)
	{
		UE_LOG(LogRudeSandbox, Warning, TEXT("[Rude.Native] no running game world - press Play first (SandboxSetup readies a level)"));
		return;
	}
	if (Args.Num() == 0) { Shim->Dispatch(TEXT("help"), {}); return; }
	TArray<FString> Rest(Args.GetData() + 1, Args.Num() - 1);
	Shim->Dispatch(Args[0], Rest);
}

static FAutoConsoleCommandWithWorldAndArgs GRudeNativeCommand(
	TEXT("Rude.Native"),
	TEXT("Call a mocked FiveM native in PIE: Rude.Native <Native> [args]. 'Rude.Native Help' lists them."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RudeNativeConsole));
