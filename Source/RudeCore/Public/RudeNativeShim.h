// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RudeNativeShim.generated.h"

RUDECORE_API DECLARE_LOG_CATEGORY_EXTERN(LogRudeSandbox, Log, All);

class ULevelSequence;
class ULevelSequencePlayer;
class ALevelSequenceActor;
class ADirectionalLight;
class ARudeScenarioAgent;
class ARudeDriveablePawn;
struct FRudeScenarioGraph;

// One line of the script-event log (GDD 1b.4): which hook fired, with what, and what the sandbox did.
USTRUCT(BlueprintType)
struct RUDECORE_API FRudeNativeEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Native") float Time = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Native") FString Native;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Native") FString Args;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Native") FString Result;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Native") bool bOk = true;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FRudeNativeCalled, const FString&, Native, const FString&, Args, const FString&, Result);

// THE NATIVE SHIM (GDD 1b.2): one command surface, two backends. In PIE these methods are the mock of
// the real FiveM natives they are named after; the identical call ships in the resource's Lua
// (URudeToolset::EmitNativeSnippet writes that line). Testing in PIE IS rehearsing the script you ship.
//
//   EnableIpl / RequestIpl / RemoveIpl / IsIplActive     <- REQUEST_IPL / REMOVE_IPL / IS_IPL_ACTIVE
//        the ymap's runtime Data Layer (DL_<ymap>, BuildDistrictLevel's projection) Activated / Unloaded,
//        plus the placed actors of that ymap shown / hidden (script ymaps are saved hidden: the fallback
//        and the finish of the layer path, once its actors stream in)
//   RequestCutscene / HasCutsceneLoaded / StartCutscene / StopCutscene / RemoveCutscene
//        <- the CUTSCENE natives: plays the Level Sequence ImportCutscene wrote (/Game/RUDE/Cutscenes/<cut>/LS_<cut>)
//   SetClockTime / NetworkOverrideClockTime / PauseClock / GetClockHours / GetClockMinutes
//        <- SET_CLOCK_TIME / NETWORK_OVERRIDE_CLOCK_TIME / PAUSE_CLOCK / GET_CLOCK_HOURS / GET_CLOCK_MINUTES
//        drives the RUDE_SKY sun's pitch and sweeps the RUDE_TIME:<mask> components (the game's own 24-bit
//        hour masks - SetWorldHour's rule, at runtime). The clock RUNS between calls (GTA: 1 game minute
//        per 2 real seconds) unless paused.
//   ActivateInteriorEntitySet / DeactivateInteriorEntitySet   <- ACTIVATE_/DEACTIVATE_INTERIOR_ENTITY_SET
//        shows / hides actors tagged RUDE_MLO_EntitySet:<set> inside RUDE_MLO:<interior>
//   SetScenarioGroupEnabled / IsScenarioGroupEnabled / SetScenarioTypeEnabled
//        <- SET_SCENARIO_GROUP_ENABLED / IS_SCENARIO_GROUP_ENABLED / SET_SCENARIO_TYPE_ENABLED
//        agents of that group (= region, a RUDE convention) stop and hide; the region's markers follow
//   StartAmbientAgents / StopAmbientAgents      (no native: the sandbox's own crowd)
//   EnterVehicle / ExitVehicle                  <- SET_PED_INTO_VEHICLE / TASK_LEAVE_VEHICLE (WP11: possess the
//        ARudeDriveablePawn BuildDriveable spawned, tagged RUDE_DRIVEABLE:<name>; the on-foot pawn parks beside
//        the car, hidden, and comes back at the driver's door on exit - F in the car does the same)
// Every call appends to EventLog, prints on screen, logs to LogRudeSandbox and broadcasts OnNativeCalled.
// Console: `Rude.Native <Native> [args]` (RudeNativeShim.cpp) and, where the engine routes Exec to
// world subsystems, the bare method names.
UCLASS()
class RUDECORE_API URudeNativeShim : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// ---- subsystem -------------------------------------------------------------------------------
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(URudeNativeShim, STATGROUP_Tickables); }
	// The shim of the running game world. The editor's console during PIE hands the EDITOR world; this
	// falls through to the PIE world so `Rude.Native` works from either console.
	static URudeNativeShim* Get(const UObject* WorldContext);

	// ---- IPL / ymap ------------------------------------------------------------------------------
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool EnableIpl(const FString& Name);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool RequestIpl(const FString& Name);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool RemoveIpl(const FString& Name);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool IsIplActive(const FString& Name) const;
	// The ymaps this level can toggle: RUDE_SCRIPT_YMAP actors present + DL_* layers whose runtime state is Unloaded.
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") FString ListIpls();

	// ---- cutscene ---------------------------------------------------------------------------------
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool RequestCutscene(const FString& Name);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool HasCutsceneLoaded() const;
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool StartCutscene(const FString& Name);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void StopCutscene();
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void RemoveCutscene();

	// ---- clock ------------------------------------------------------------------------------------
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void SetClockTime(int32 Hours, int32 Minutes, int32 Seconds);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void NetworkOverrideClockTime(int32 Hours, int32 Minutes, int32 Seconds);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void PauseClock(bool bPaused);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") int32 GetClockHours() const;
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") int32 GetClockMinutes() const;

	// ---- interiors --------------------------------------------------------------------------------
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool ActivateInteriorEntitySet(const FString& Interior, const FString& EntitySet);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool DeactivateInteriorEntitySet(const FString& Interior, const FString& EntitySet);

	// ---- scenarios --------------------------------------------------------------------------------
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void SetScenarioGroupEnabled(const FString& Group, bool bEnabled);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool IsScenarioGroupEnabled(const FString& Group) const;
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void SetScenarioTypeEnabled(const FString& Type, bool bEnabled);
	// Count agents (default 8, max 200) on the region's chains (empty Region = the first region in the level).
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") int32 StartAmbientAgents(int32 Count, const FString& Region);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void StopAmbientAgents();

	// ---- vehicles (WP11 THE CHAOS TEST-DRIVE) ------------------------------------------------------------------
	// Name = the model (blista) or empty for the first driveable in the level. Possesses the ARudeDriveablePawn; the
	// on-foot pawn is parked beside it (hidden, collision off) and comes back at the driver's door on ExitVehicle.
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool EnterVehicle(const FString& Name);
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") bool ExitVehicle();
	UFUNCTION(BlueprintCallable, Category = "RUDE|Native") bool IsInVehicle() const;

	// ---- the event log ----------------------------------------------------------------------------
	UFUNCTION(BlueprintCallable, Category = "RUDE|Native") TArray<FRudeNativeEvent> GetEventLog() const { return EventLog; }
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void DumpEventLog();
	UFUNCTION(Exec, BlueprintCallable, Category = "RUDE|Native") void ClearEventLog();
	// The console surface: a native by name with string args; returns the log line it produced.
	UFUNCTION(BlueprintCallable, Category = "RUDE|Native") FString Dispatch(const FString& Native, const TArray<FString>& Args);

	UPROPERTY(BlueprintAssignable, Category = "RUDE|Native") FRudeNativeCalled OnNativeCalled;

	// Fractional hour of day, 0-24 (public: the time-of-day slider reads and writes it).
	UPROPERTY(BlueprintReadWrite, Category = "RUDE|Native") float Hour = 12.f;
	// Game minutes per real second while the clock runs. GTA: 0.5 (one game minute every two seconds).
	UPROPERTY(BlueprintReadWrite, Category = "RUDE|Native") float MinutesPerRealSecond = 0.5f;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Native") bool bClockPaused = false;
	UPROPERTY(BlueprintReadWrite, Category = "RUDE|Native") bool bOnScreen = true;
	UPROPERTY(BlueprintReadWrite, Category = "RUDE|Native") bool bAgentsFly = false;
	UPROPERTY(BlueprintReadOnly, Category = "RUDE|Native") TArray<FRudeNativeEvent> EventLog;

private:
	TSet<FString> ActiveIpls;
	TMap<FString, double> PendingIplSweeps;   // ymap -> deadline: actors a layer streams in were saved hidden
	TSet<FString> ActiveEntitySets;
	TSet<FString> DisabledGroups;
	TSet<FString> DisabledTypes;
	TWeakObjectPtr<ADirectionalLight> Sun;
	int32 LastMaskHour = -1;
	int32 LastGated = 0;
	float SweepAccum = 0.f;
	FString LoadedCutsceneName;
	UPROPERTY() TObjectPtr<ULevelSequence> LoadedCutscene;
	UPROPERTY() TObjectPtr<ULevelSequencePlayer> CutscenePlayer;
	UPROPERTY() TObjectPtr<ALevelSequenceActor> CutsceneActor;
	UPROPERTY() TArray<TObjectPtr<ARudeScenarioAgent>> Agents;
	TSharedPtr<FRudeScenarioGraph> Graph;
	TWeakObjectPtr<APawn> OnFootPawn;          // who was walking before EnterVehicle
	TWeakObjectPtr<ARudeDriveablePawn> DrivingPawn;

	void Log(const FString& Native, const FString& Args, const FString& Result, bool bOk = true);
	bool SetIpl(const FString& Native, const FString& Name, bool bOn);
	bool SetYmapLayerState(const FString& Ymap, bool bLoad, FString& OutWhat);
	int32 SweepYmapActors(const FString& Ymap, bool bShow);
	bool SetEntitySet(const FString& Native, const FString& Interior, const FString& EntitySet, bool bOn);
	void ApplyClock(bool bForceMasks);
	int32 SweepTimeMasks(int32 Hr);
	void StopCutsceneInternal();
};
