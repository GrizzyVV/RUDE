// RUDE - RAGE <-> Unreal Development Environment
// THE SANDBOX's editor side (GDD 1b). SandboxSetup readies a level for Play; EmitNativeSnippet is the
// "every export ships its invocation" rule - the Lua that calls the REAL FiveM native the PIE shim mocks
// (URudeNativeShim in RudeCore). The two are one contract: the shim's method names ARE the natives.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeSandboxGameMode.h"
#include "CollisionQueryParams.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

// ---- SandboxSetup (Matt) ------------------------------------------------------------------------------
FString URudeToolset::SandboxSetup(const FString& LevelPath, const FString& Location)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	if (!GEditor) { return Fail(TEXT("no GEditor")); }
	UWorld* World = GEditor->GetEditorWorldContext().World();
	FString Path = LevelPath.TrimStartAndEnd();
	if (!Path.IsEmpty() && (!World || !World->GetOutermost()->GetName().Equals(Path, ESearchCase::IgnoreCase)))
	{
		if (!FPackageName::IsValidLongPackageName(Path)) { return Fail(TEXT("give a content path, e.g. /Game/RUDE/Levels/Downtown")); }
		FString File;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Path, File, FPackageName::GetMapPackageExtension()) || !FPaths::FileExists(File))
		{
			return Fail(FString::Printf(TEXT("no map on disk for %s"), *Path));
		}
		if (!FEditorFileUtils::LoadMap(File, /*bLoadAsTemplate*/ false, /*bShowProgress*/ false)) { return Fail(TEXT("LoadMap failed")); }
		World = GEditor->GetEditorWorldContext().World();
	}
	if (!World) { return Fail(TEXT("no editor world")); }
	if (Path.IsEmpty()) { Path = World->GetOutermost()->GetName(); }
	if (!FPackageName::IsValidLongPackageName(Path) || Path.StartsWith(TEXT("/Temp"))) { return Fail(TEXT("the level is untitled - SaveLevel it to a content path first")); }

	// Where: "x,y,z" in UE cm (';' accepted: -ExecCmds splits on commas); empty = the origin, which on a
	// district is a street (GTA's 0,0 is Legion Square). Z snaps to the ground under the point when
	// something is there to stand on, so "0,0,0" lands on the pavement, not in a basement.
	FVector Loc = FVector::ZeroVector;
	{
		TArray<FString> P;
		Location.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
		if (P.Num() >= 3) { Loc = FVector(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2])); }
		else if (P.Num() != 0) { return Fail(TEXT("Location must be \"x,y,z\" in UE centimetres, or empty")); }
	}
	bool bGround = false;
	{
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(RudeSandboxSetup), /*bTraceComplex*/ true);
		if (World->LineTraceSingleByChannel(Hit, Loc + FVector(0.f, 0.f, 20000.f), Loc - FVector(0.f, 0.f, 20000.f), ECC_WorldStatic, Q))
		{
			Loc = Hit.ImpactPoint + FVector(0.f, 0.f, 120.f);
			bGround = true;
		}
	}
	// one spawn per level: a previous RUDE_SANDBOX_SPAWN is replaced
	int32 Replaced = 0;
	{
		TArray<AActor*> Old;
		for (TActorIterator<AActor> It(World); It; ++It) { if (It->ActorHasTag(FName(TEXT("RUDE_SANDBOX_SPAWN")))) { Old.Add(*It); } }
		for (AActor* A : Old) { World->EditorDestroyActor(A, true); ++Replaced; }
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerStart* Start = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), Loc, FRotator::ZeroRotator, SP);
	if (!Start) { return Fail(TEXT("PlayerStart spawn failed")); }
	Start->SetActorLabel(TEXT("RUDE_SandboxSpawn"));
	Start->Tags.Add(FName(TEXT("RUDE_SANDBOX_SPAWN")));
	Start->SetFolderPath(FName(TEXT("RUDE_SANDBOX")));
	// the game mode: the level's override, so Play needs no project setting
	AWorldSettings* WS = World->GetWorldSettings();
	if (!WS) { return Fail(TEXT("no world settings")); }
	WS->Modify();
	WS->DefaultGameMode = ARudeSandboxGameMode::StaticClass();
	WS->MarkPackageDirty();
	// the sky rig: BuildDistrictLevel makes one; a level built another way gets the same minimal rig here
	int32 Sky = 0;
	bool bSkyAdded = false;
	for (TActorIterator<AActor> It(World); It; ++It) { if (It->ActorHasTag(FName(TEXT("RUDE_SKY")))) { ++Sky; } }
	if (Sky == 0)
	{
		FActorSpawnParameters SkySP;
		if (ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-45.f, 30.f, 0.f), SkySP))
		{
			Sun->SetActorLabel(TEXT("RUDE_Sun"));
			Sun->Tags.Add(FName(TEXT("RUDE_SKY")));
			if (UDirectionalLightComponent* DLC = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				DLC->SetMobility(EComponentMobility::Movable);
				DLC->SetIntensity(8.f);
				DLC->bAtmosphereSunLight = true;
			}
			++Sky;
		}
		if (ASkyAtmosphere* Atm = World->SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator, SkySP))
		{
			Atm->SetActorLabel(TEXT("RUDE_SkyAtmosphere"));
			Atm->Tags.Add(FName(TEXT("RUDE_SKY")));
			++Sky;
		}
		if (ASkyLight* SkyL = World->SpawnActor<ASkyLight>(FVector::ZeroVector, FRotator::ZeroRotator, SkySP))
		{
			SkyL->SetActorLabel(TEXT("RUDE_SkyLight"));
			SkyL->Tags.Add(FName(TEXT("RUDE_SKY")));
			if (USkyLightComponent* SLC = SkyL->GetLightComponent())
			{
				SLC->SetMobility(EComponentMobility::Movable);
				SLC->bRealTimeCapture = true;
			}
			++Sky;
		}
		bSkyAdded = Sky > 0;
	}
	World->MarkPackageDirty();
	// save: SaveLevel's two legs (the interactive map save, then the headless package save if no file landed)
	bool bSaved = FEditorFileUtils::SaveMap(World, Path);
	FString MapFile;
	FPackageName::TryConvertLongPackageNameToFilename(Path, MapFile, FPackageName::GetMapPackageExtension());
	if (!FPaths::FileExists(MapFile))
	{
		World->GetOutermost()->MarkPackageDirty();
		bSaved = RudeSaveDirty(true, true) && FPaths::FileExists(MapFile);
	}
	else { RudeSaveDirty(false, true); }
	return FString::Printf(TEXT("{\"ok\":%s,\"level\":\"%s\",\"spawn\":[%.1f,%.1f,%.1f],\"groundSnapped\":%s,\"replaced\":%d,")
		TEXT("\"gameMode\":\"%s\",\"pawn\":\"ARudeSandboxPawn\",\"skyActors\":%d,\"skyAdded\":%s,\"saved\":%s,\"headlessSaved\":%d,\"headlessSaveFailed\":%d}"),
		bSaved ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Path), Loc.X, Loc.Y, Loc.Z, bGround ? TEXT("true") : TEXT("false"), Replaced,
		*RudeJsonEscape(ARudeSandboxGameMode::StaticClass()->GetPathName()), Sky, bSkyAdded ? TEXT("true") : TEXT("false"),
		bSaved ? TEXT("true") : TEXT("false"), GRudeLastSaved, GRudeLastSaveFailed);
}

// ---- EmitNativeSnippet (Matt + agent) -----------------------------------------------------------------
// The shim call and the shipped Lua are the same contract; this writes the Lua. Native names are FiveM's
// PascalCase spellings of the RAGE natives (the ALL_CAPS name follows each line). Where a flag value is
// convention rather than documentation it says so.
FString URudeToolset::EmitNativeSnippet(const FString& Kind, const FString& Name)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	const FString K = Kind.TrimStartAndEnd().ToLower();
	FString N = Name.TrimStartAndEnd();
	FString Native, Shim, Lua, Note, Side = TEXT("client");
	if (K.IsEmpty() || K == TEXT("list"))
	{
		return TEXT("{\"ok\":true,\"kinds\":[\"ipl\",\"cutscene\",\"entityset\",\"scenario\",\"clock\"],")
		       TEXT("\"name\":\"ipl: the ymap | cutscene: the cut | entityset: interior|set | scenario: the group | clock: HH:MM\"}");
	}
	if (K == TEXT("ipl") || K == TEXT("ymap"))
	{
		N = N.ToLower(); N.RemoveFromEnd(TEXT(".ymap"));
		if (N.IsEmpty()) { return Fail(TEXT("Name = the ymap (file stem)")); }
		Native = TEXT("RequestIpl / RemoveIpl");
		Shim = TEXT("Rude.Native EnableIpl ") + N;
		Lua = FString::Printf(TEXT("-- %s.ymap is script-controlled (CMapData flags bit 0): the game shows it only on request.\n")
		                      TEXT("RequestIpl('%s')      -- REQUEST_IPL: load and show the map's placements\n")
		                      TEXT("-- ...\n")
		                      TEXT("RemoveIpl('%s')       -- REMOVE_IPL: hide it again\n")
		                      TEXT("-- IsIplActive('%s')  -- IS_IPL_ACTIVE\n"), *N, *N, *N, *N);
		Note = TEXT("The shim's EnableIpl is the GDD's name for this hook; the FiveM native is RequestIpl (there is no EnableIpl native). A ymap that is NOT script-controlled streams by itself and needs no call.");
	}
	else if (K == TEXT("cutscene") || K == TEXT("cut"))
	{
		N = N.ToLower(); N.RemoveFromEnd(TEXT(".cut"));
		if (N.IsEmpty()) { return Fail(TEXT("Name = the cutscene (e.g. ah_1_int)")); }
		Native = TEXT("RequestCutscene / HasCutsceneLoaded / StartCutscene");
		Shim = TEXT("Rude.Native RequestCutscene ") + N + TEXT("  then  Rude.Native StartCutscene");
		Lua = FString::Printf(TEXT("RequestCutscene('%s', 8)                 -- REQUEST_CUTSCENE (8 = the flag value scripts commonly pass)\n")
		                      TEXT("while not HasCutsceneLoaded() do Wait(0) end   -- HAS_CUTSCENE_LOADED\n")
		                      TEXT("StartCutscene(0)                            -- START_CUTSCENE\n")
		                      TEXT("-- or StartCutsceneAtCoords(x, y, z, 0)     -- START_CUTSCENE_AT_COORDS\n")
		                      TEXT("-- StopCutscene(false)  -- STOP_CUTSCENE   |   RemoveCutscene()  -- REMOVE_CUTSCENE\n"), *N);
		Note = TEXT("A cutscene authored in RUDE needs its .cut written back first (the write path is not built); this snippet plays the game's own cut of that name.");
	}
	else if (K == TEXT("entityset") || K == TEXT("interior"))
	{
		FString Interior, Set;
		if (!N.Replace(TEXT(":"), TEXT("|")).Split(TEXT("|"), &Interior, &Set)) { Set = N; }
		Interior = Interior.TrimStartAndEnd(); Set = Set.TrimStartAndEnd();
		if (Set.IsEmpty()) { return Fail(TEXT("Name = \"interior|set\" (or just the set)")); }
		Native = TEXT("GetInteriorAtCoords / ActivateInteriorEntitySet / RefreshInterior");
		Shim = FString::Printf(TEXT("Rude.Native ActivateInteriorEntitySet %s %s"), Interior.IsEmpty() ? TEXT("\"\"") : *Interior, *Set);
		Lua = FString::Printf(TEXT("-- %s: a point inside the interior (GTA metres) finds its id\n")
		                      TEXT("local interior = GetInteriorAtCoords(x, y, z)          -- GET_INTERIOR_AT_COORDS\n")
		                      TEXT("if interior ~= 0 then\n")
		                      TEXT("    ActivateInteriorEntitySet(interior, '%s')   -- ACTIVATE_INTERIOR_ENTITY_SET\n")
		                      TEXT("    RefreshInterior(interior)                    -- REFRESH_INTERIOR\n")
		                      TEXT("end\n")
		                      TEXT("-- DeactivateInteriorEntitySet(interior, '%s')  -- DEACTIVATE_INTERIOR_ENTITY_SET\n")
		                      TEXT("-- IsInteriorEntitySetActive(interior, '%s')    -- IS_INTERIOR_ENTITY_SET_ACTIVE\n"),
		                      Interior.IsEmpty() ? TEXT("the interior") : *Interior, *Set, *Set, *Set);
		Note = TEXT("The shim needs actors tagged RUDE_MLO_EntitySet:<set> (ImportMlo does not tag entity sets yet). Entity set names come from the MLO archetype's <entitySets>.");
	}
	else if (K == TEXT("scenario") || K == TEXT("scenariogroup"))
	{
		if (N.IsEmpty()) { return Fail(TEXT("Name = the scenario group")); }
		Native = TEXT("SetScenarioGroupEnabled");
		Shim = TEXT("Rude.Native SetScenarioGroupEnabled ") + N + TEXT(" 1");
		Lua = FString::Printf(TEXT("SetScenarioGroupEnabled('%s', true)      -- SET_SCENARIO_GROUP_ENABLED\n")
		                      TEXT("-- SetScenarioGroupEnabled('%s', false)\n")
		                      TEXT("-- DoesScenarioGroupExist('%s')           -- DOES_SCENARIO_GROUP_EXIST\n")
		                      TEXT("-- SetScenarioTypeEnabled('WORLD_HUMAN_SMOKING', false)  -- SET_SCENARIO_TYPE_ENABLED\n"), *N, *N, *N);
		Note = TEXT("In PIE the group name is the REGION (a RUDE convention: the imported points carry iScenarioGroup as an index, and the region's group-name lookups are not imported yet). In game a group is a name the scenario point's group index resolves to.");
	}
	else if (K == TEXT("clock") || K == TEXT("time"))
	{
		int32 H = 12, M = 0;
		{
			TArray<FString> P;
			N.Replace(TEXT("."), TEXT(":")).ParseIntoArray(P, TEXT(":"), true);
			if (P.Num() >= 1 && P[0].IsNumeric()) { H = FMath::Clamp(FCString::Atoi(*P[0]), 0, 23); }
			if (P.Num() >= 2 && P[1].IsNumeric()) { M = FMath::Clamp(FCString::Atoi(*P[1]), 0, 59); }
		}
		Native = TEXT("NetworkOverrideClockTime / SetClockTime");
		Shim = FString::Printf(TEXT("Rude.Native SetClockTime %d %d"), H, M);
		Lua = FString::Printf(TEXT("NetworkOverrideClockTime(%d, %d, 0)   -- NETWORK_OVERRIDE_CLOCK_TIME: holds the hour against the server's clock sync\n")
		                      TEXT("-- SetClockTime(%d, %d, 0)            -- SET_CLOCK_TIME: the next sync overwrites it unless the server agrees\n")
		                      TEXT("-- PauseClock(true)                   -- PAUSE_CLOCK\n"), H, M, H, M);
		Note = TEXT("The sandbox clock runs (one game minute per two real seconds, GTA's rate) until PauseClock; the Lua sets the hour once.");
	}
	else { return Fail(TEXT("Kind = ipl | cutscene | entityset | scenario | clock (empty lists them)")); }
	return FString::Printf(TEXT("{\"ok\":true,\"kind\":\"%s\",\"name\":\"%s\",\"native\":\"%s\",\"side\":\"%s\",\"shim\":\"%s\",\"lua\":\"%s\",\"note\":\"%s\"}"),
		*RudeJsonEscape(K), *RudeJsonEscape(N), *RudeJsonEscape(Native), *Side, *RudeJsonEscape(Shim), *RudeJsonEscape(Lua), *RudeJsonEscape(Note));
}
