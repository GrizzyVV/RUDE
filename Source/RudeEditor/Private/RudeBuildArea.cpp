// RUDE - RAGE <-> Unreal Development Environment
//
// BUILD AREA - STAGE 1 ONLY: prove the Level-Instance mechanism in isolation.
// docs/BUILD_AREA_DESIGN.md section 8 stage 1 - "build only PackAreaLevelInstance ... then:
// R1 instance counts, R2 numeric transforms, R7 package-on-disk, R12 folder probe. This closes
// section 5c's open Q3 BEFORE any orchestration exists." Nothing in this file orchestrates:
// no alias resolution, no import, no level creation, no lighting, no manifest. Those are stages
// 2-6 and deliberately absent, so a stage-1 failure cannot be confused with an import failure.
//
// ============================ WHAT WAS READ IN THE ENGINE SOURCE ============================
// Every claim below is from B:\UE_5.8 (Build.version: 5.8.0, ++UE5+Release-5.8, CL 55116800),
// read for this file - not from documentation and not from memory.
//
// (1) THE PIVOT - the one decision that must be right (UX_AND_FEATURE_NOTES 5c).
//     Runtime/Engine/Public/LevelInstance/LevelInstanceTypes.h:92
//         ELevelInstancePivotType PivotType = ELevelInstancePivotType::CenterMinZ;   <-- DEFAULT
//     Runtime/Engine/Private/LevelInstance/LevelInstanceSubsystem.cpp:1380-1397
//         WorldOrigin  -> LevelInstanceLocation = FVector(0,0,0)
//         CenterMinZ   -> LevelInstanceLocation = bounding-box centre, Z = box.Min.Z
//     ...and then :1551-1559
//         NewLevelInstanceActor->SetActorLocation(LevelInstanceLocation);
//         LevelStreaming->LevelTransform = NewLevelInstanceActor->GetActorTransform();
//         LoadedLevel->bAlreadyMovedActors = true;      // FLevelUtils::RemoveEditorTransform on
//                                                       // save rewrites every contained actor
//                                                       // transform RELATIVE to LevelTransform
//     So the re-base is not cosmetic: with CenterMinZ every actor's SAVED transform is shifted by
//     -centre and the compensating offset lives on the Level Instance actor. RAGE placements are
//     ABSOLUTE world coordinates and the export lane converts actor transforms straight back to
//     them, so a re-based area exports plausible-looking, uniformly-wrong positions - a defect
//     invisible in the editor and only visible in game. WorldOrigin makes LevelTransform identity,
//     which makes the rewrite a no-op. That is the mechanism; the verdict measures it anyway.
//
// (2) THE GUARD. LevelInstanceSubsystem.cpp:1321-1341 CanCreateLevelInstanceFrom = "array not
//     empty" + CanMoveActorToLevel per actor. CanMoveActorToLevel (:1938-2011) vetoes only pivot
//     actors and Level-Instance actors in specific edit states. MEASURED: it does NOT veto
//     ISM-bearing actors and does NOT require a saved world - so the guard will pass for our
//     spawn and its value here is catching the empty/degenerate case loudly.
//
// (3) ⛔ THE MODAL DIALOG - the surprise, and the reason this tool is not headless-clean.
//     LevelInstanceSubsystem.cpp:1439 hardcodes  CreateNewStreamingLevelParams.bUseSaveAs = true;
//     -> Editor/UnrealEd/Private/EditorLevelUtils.cpp:781-784  FEditorFileUtils::SaveLevelAs(...)
//     -> Editor/UnrealEd/Private/FileHelpers.cpp:1780-1796 SaveLevelAs IGNORES the caller's
//        filename (it recomputes DefaultFilename from the new world's own package)
//     -> FileHelpers.cpp:1466 OpenSaveAsDialog -> :1405 CreateModalSaveAssetDialog.
//     CONSEQUENCE, MEASURED: in UE 5.8 CreateLevelInstanceFrom ALWAYS opens a modal "Save Level
//     As" browser, and FNewLevelInstanceParams::LevelPackageName has NO effect on the plain
//     (non-PackedLevelActor) path - the destination is whatever the operator picks. This tool
//     therefore (a) pre-seeds the dialog's starting folder, (b) says so before it blocks, and
//     (c) reports the package that was ACTUALLY created plus levelMatchesRequest, rather than
//     asserting the requested path. Under -unattended the modal is CANCELLED, not shown
//     (Runtime/Slate/.../SlateApplication.cpp:2134), so a headless run FAILS LOUDLY with
//     "Failed to create new Level" in the log instead of hanging. Both behaviours are honest;
//     neither is a silent success. (ENGINEERING_LOG "UE / plugin build & MCP gotchas": when
//     anything hangs, LOOK AT THE SCREEN first - here you would see this dialog.)
//
// (4) THE ACTORS DO NOT SURVIVE AS POINTERS. EditorLevelUtils.cpp:826 MoveActorsToLevel ->
//     :86-275 CopyOrMoveActorsToLevel implements the move as CUT + PASTE
//     (EditorServer.cpp:3380-3388 edactCopySelected + edactDeleteSelected, then
//     edactPasteSelected). The source actors are DESTROYED and new actors are created in the new
//     level. So every "before" measurement in this file is captured into plain values BEFORE the
//     call and the "after" side re-enumerates through the subsystem. Touching a captured AActor*
//     after the call would be reading freed memory.
//     Label survival (INFERRED, not measured): the cut deletes the originals before the paste, so
//     the FCachedActorLabels the paste builds (EditorActor.cpp:589) no longer contains them and
//     SetActorLabelUnique (:661) has nothing to disambiguate against. This file does not TRUST
//     that - unpaired counts are reported as numbers so a relabel shows up as data, not a lie.
//
// (5) R12, FOLDERS, ANSWERED FROM SOURCE. LevelInstanceSubsystem.cpp:1463-1468 calls
//     Actor->SetFolderPath_Recursively(NAME_None) on every actor in the new level. Outliner
//     folders do NOT survive packing. That is why BUILD_AREA_DESIGN section 3e clears by TAG, and
//     why the verdict reports foldersSurviving (expected 0) instead of assuming it.
//
// Tags on claims in this file: MEASURED = read in the cited source; INFERRED = reasoned from it;
// UNKNOWN = named, not resolved. Design decisions here are the agent's (this file's author),
// not Matt's - BUILD_AREA_DESIGN is the spec, the calls below are the reading of it.

#include "RudeToolset.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "EditorDirectories.h"
#include "Editor/UnrealEd/Public/EditorLevelUtils.h"
#include "FileHelpers.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceTypes.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace RudeBuildArea
{
	// The area folder every RUDE area level lives under (BUILD_AREA_DESIGN section 3a, which is
	// itself UX_AND_FEATURE_NOTES 5c's own path). Stage 6's BuildArea must use this same constant.
	static const TCHAR* AreasRoot = TEXT("/Game/RUDE/Areas");

	// The marker ImportScene stamps on its spawn today. RudeToolset.cpp:3440 sets it as the
	// OUTLINER FOLDER and :3405 clears by that folder; ImportMlo (:2842) marks with actor TAGS
	// instead; BUILD_AREA_DESIGN section 1c plans to add the tag to the map lane as well. Matching
	// both spellings (see CollectAreaActors) is this file's call, so stage 1 runs against the code
	// as it ships TODAY and keeps running unchanged after the stage-2 refactor adds the tag.
	static const TCHAR* DefaultMarker = TEXT("RUDE_LS");

	// BUILD_AREA_DESIGN section 3a: "<slug> = catalog alias, non-alphanumerics -> _". Kept literal
	// and shared so BuildArea and PackAreaLevelInstance can never disagree about where an area's
	// level lives - a slug fork would silently create two levels for one district.
	static FString AreaSlug(const FString& AreaName)
	{
		FString Slug;
		Slug.Reserve(AreaName.Len());
		for (const TCHAR C : AreaName)
		{
			Slug.AppendChar(FChar::IsAlnum(C) ? C : TEXT('_'));
		}
		return Slug;
	}

	// Verdicts are consumed by agents and CI, so a stray quote or backslash in a label must not
	// turn a verdict into unparseable JSON.
	static FString Esc(const FString& In)
	{
		return In.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
	}

	static FString Fail(const FString& Why)
	{
		UE_LOG(LogTemp, Error, TEXT("[RUDE] PackAreaLevelInstance: %s"), *Why);
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Esc(Why));
	}

	// One actor's packable state, captured as VALUES. See note (4): the AActor* is dead after the
	// pack, so nothing here may be a pointer.
	struct FActorShot
	{
		FVector Location = FVector::ZeroVector;
		int32   IsmComponents = 0;
		int64   Instances = 0;
		// component name -> up to 3 sampled instance transforms, in WORLD space. Sampling first /
		// middle / last rather than only index 0 costs nothing and catches a per-instance scramble
		// that a single sample would miss.
		TMap<FString, TArray<FTransform>> Samples;
	};

	// Fill a snapshot from an actor. UInstancedStaticMeshComponent is the base of
	// UHierarchicalInstancedStaticMeshComponent, so this counts both families.
	static void ShootActor(const AActor* Actor, FActorShot& Out)
	{
		Out.Location = Actor->GetActorLocation();
		TArray<UInstancedStaticMeshComponent*> Isms;
		Actor->GetComponents(Isms);
		Out.IsmComponents = Isms.Num();
		for (const UInstancedStaticMeshComponent* Ism : Isms)
		{
			if (!Ism)
			{
				continue;
			}
			const int32 Count = Ism->GetInstanceCount();
			Out.Instances += Count;
			if (Count <= 0)
			{
				continue;
			}
			TArray<int32> Picks;
			Picks.Add(0);
			if (Count > 2) { Picks.Add(Count / 2); }
			if (Count > 1) { Picks.Add(Count - 1); }

			TArray<FTransform>& Dst = Out.Samples.Add(Ism->GetName());
			for (const int32 Idx : Picks)
			{
				FTransform Xf;
				// bWorldSpace = true: the whole point of R2 is the WORLD coordinate, because that
				// is what the export lane converts back to RAGE metres.
				if (Ism->GetInstanceTransform(Idx, Xf, /*bWorldSpace*/ true))
				{
					Dst.Add(Xf);
				}
			}
		}
	}
}

// ============================================================================================
// PackAreaLevelInstance - stage 1's whole deliverable.
// ============================================================================================
namespace RudeBuildArea
{
	// ⭐ THE HEADLESS PATH - build what CreateLevelInstanceFrom builds, without its modal.
	// WHY THIS EXISTS: UE 5.8 hardcodes bUseSaveAs=true inside the subsystem
	// (LevelInstanceSubsystem.cpp:1439), and ULevelInstanceSubsystem exposes no other creation
	// entry point, so "Build Full Map runs headless via an agent" (UX section 10) is impossible
	// until we assemble it ourselves. Every call below was read in the engine headers first:
	//   EditorLevelUtils.h:172-219  FCreateNewStreamingLevelForWorldParams (bUseSaveAs, ActorsToMove,
	//                               bUseExternalActors, DefaultFilename)
	//   EditorLevelUtils.h:337      RemoveLevelFromWorld
	//   LevelInstanceActor.h:74     ALevelInstance::SetWorldAsset
	// ⚠ THE ORDER IS LOAD-BEARING: the streaming level must be detached AFTER the actors are moved
	// and saved into it, and BEFORE the Level Instance is spawned - otherwise the same actors exist
	// twice (once as a sublevel, once through the LI) and every instance count double-reports.
	static ILevelInstanceInterface* CreateLevelInstanceHeadless(UWorld* World,
		const TArray<AActor*>& ActorsToMove, const FString& PackageName,
		TSubclassOf<AActor> LevelInstanceClass)
	{
		// ⛔ DefaultFilename is a FILESYSTEM PATH, not a package name. EditorLevelUtils.cpp:787
		// hands it straight to FEditorFileUtils::SaveLevel and then runs FilenameToLongPackageName
		// on the RESULT - so passing "/Game/..." makes the save quietly write nothing, and the
		// engine later refuses SetWorldAsset with "package ... does not exist". (Measured
		// 2026-07-28: that exact refusal.)
		const FString LevelFilename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetMapPackageExtension());
		UEditorLevelUtils::FCreateNewStreamingLevelForWorldParams P(
			ULevelStreamingDynamic::StaticClass(), LevelFilename);
		P.bUseSaveAs = false;              // ⛔ the whole point: no modal
		P.ActorsToMove = &ActorsToMove;
		P.bUseExternalActors = true;       // OFPA, same as the dialog path (5c)
		P.bCreateWorldPartition = false;   // an AREA is a plain level; the LI is what the WP world sees

		ULevelStreaming* Streaming = UEditorLevelUtils::CreateNewStreamingLevelForWorld(*World, P);
		if (!Streaming)
		{
			UE_LOG(LogTemp, Error, TEXT("[RUDE] headless: CreateNewStreamingLevelForWorld returned "
				"null for '%s' - the level asset was not created"), *PackageName);
			return nullptr;
		}
		ULevel* Created = Streaming->GetLoadedLevel();
		const FString CreatedPackage = Streaming->GetWorldAssetPackageName();
		if (!Created)
		{
			UE_LOG(LogTemp, Error, TEXT("[RUDE] headless: the streaming level for '%s' has no loaded "
				"ULevel - refusing to spawn a Level Instance that would point at nothing"),
				*CreatedPackage);
			return nullptr;
		}
		// ⛔ MATCH THE ENGINE: CLEAR OUTLINER FOLDERS ON THE PACKED ACTORS.
		// LevelInstanceSubsystem.cpp:1463-1468 calls SetFolderPath_Recursively(NAME_None) on every
		// actor it packs, and this path must not diverge: CollectAreaActors matches by FOLDER as
		// well as by tag, so an actor that keeps "RUDE_LS" inside a packed level would be
		// re-collected by the next pack and counted twice. (Measured 2026-07-28: the headless run
		// reported foldersSurviving 111 against the engine path's 0 - the verdict caught it.)
		for (AActor* Moved : TActorRange<AActor>(Created->GetWorld()))
		{
			if (Moved && Moved->GetLevel() == Created)
			{
				Moved->SetFolderPath_Recursively(NAME_None);
			}
		}

		// ⛔⛔ SAVE AGAIN - THE ENGINE SAVED IT EMPTY. EditorLevelUtils.cpp:783-828 does the work in
		// this order: SaveLevel (the level is still EMPTY) -> AddLevelToWorld -> MoveActorsToLevel.
		// So the asset on disk contains NO actors; the move exists only in memory. Detaching at
		// that point throws the actors away, which is exactly what the first run measured:
		// actorsPacked 0, instancesAfter 0, 111 unpaired. Saving maps AND content here also
		// captures the OFPA external-actor packages the move created.
		// ⛔ THE GUARD IS LOAD-BEARING HEADLESS: under `-unattended` (but without
		// GIsRunningUnattendedScript) PromptForCheckoutAndSave returns PR_Cancelled and writes
		// NOTHING — FileHelpers.cpp:4659-4667. That is why this warning used to fire on every
		// headless pack. Setting the flag routes to the engine's own modal-free save path, the
		// same TGuardValue the engine uses at FileHelpers.cpp:5919.
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript,
			FApp::IsUnattended() ? true : GIsRunningUnattendedScript);
		if (!FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave*/ false,
			/*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true, /*bFastSave*/ false,
			/*bNotifyNoPackagesSaved*/ false, /*bCanBeDeclined*/ false))
		{
			UE_LOG(LogTemp, Warning, TEXT("[RUDE] headless: SaveDirtyPackages reported failure "
				"after moving actors into '%s' - continuing to the on-disk check, which is the "
				"gate that actually matters"), *CreatedPackage);
		}

		// ⛔ PROVE THE ASSET IS ON DISK BEFORE DETACHING. SetWorldAsset validates existence
		// (ULevelInstanceSubsystem::CanUseWorldAsset), so an unsaved level fails there - but by
		// then the sublevel is already removed and the actors are gone with it. Check first, and
		// refuse while the level is still attached and recoverable.
		if (!FPackageName::DoesPackageExist(CreatedPackage))
		{
			UE_LOG(LogTemp, Error, TEXT("[RUDE] headless: '%s' was not written to disk - refusing "
				"to detach the sublevel (the actors are still in it and recoverable)"), *CreatedPackage);
			return nullptr;
		}
		// Detach the sublevel: World Partition does not stream sublevels (5c), and leaving it
		// attached would duplicate every actor we just moved.
		if (!UEditorLevelUtils::RemoveLevelFromWorld(Created))
		{
			UE_LOG(LogTemp, Error, TEXT("[RUDE] headless: RemoveLevelFromWorld failed for '%s' - "
				"refusing to continue, because the actors would exist BOTH as a sublevel and "
				"through the Level Instance"), *CreatedPackage);
			return nullptr;
		}

		// The LI actor sits at the ORIGIN: with WorldOrigin semantics the contained actors keep
		// their absolute RAGE coordinates, so any non-zero transform here would offset the area.
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transactional;
		UClass* Cls = LevelInstanceClass ? LevelInstanceClass.Get() : ALevelInstance::StaticClass();
		AActor* Actor = World->SpawnActor<AActor>(Cls, FTransform::Identity, Spawn);
		ILevelInstanceInterface* LI = Cast<ILevelInstanceInterface>(Actor);
		if (!LI)
		{
			UE_LOG(LogTemp, Error, TEXT("[RUDE] headless: spawned %s is not a Level Instance"),
				Cls ? *Cls->GetName() : TEXT("null"));
			if (Actor) { World->DestroyActor(Actor); }
			return nullptr;
		}
		if (!LI->SetWorldAsset(TSoftObjectPtr<UWorld>(FSoftObjectPath(
			CreatedPackage + TEXT(".") + FPackageName::GetShortName(CreatedPackage)))))
		{
			UE_LOG(LogTemp, Error, TEXT("[RUDE] headless: SetWorldAsset('%s') refused"), *CreatedPackage);
			World->DestroyActor(Actor);
			return nullptr;
		}
		LI->UpdateLevelInstanceFromWorldAsset();
		// ⛔ FORCE THE LOAD, SYNCHRONOUSLY. A Level Instance loads its world ASYNCHRONOUSLY, so
		// everything downstream - the R1 instance count, the R2 transform sampling, anything a
		// caller does next - would measure an EMPTY level and report a perfect-looking zero.
		// (Measured 2026-07-28: actorsInLevel 114 in the asset, instancesAfter 0 in the verdict,
		// 111 unmatched. The asset was right; the measurement was early.) BlockLoad makes the
		// verdict describe the same moment the level exists in.
		if (ULevelInstanceSubsystem* Sub = World->GetSubsystem<ULevelInstanceSubsystem>())
		{
			Sub->BlockLoadLevelInstance(LI);
		}
		UE_LOG(LogTemp, Display, TEXT("[RUDE] headless: created '%s' and attached a Level Instance "
			"at the origin, no dialog"), *CreatedPackage);
		return LI;
	}
}

FString URudeToolset::PackAreaLevelInstance(const FString& AreaName, const FString& ActorTag,
                                            const FString& Mode)
{
	// Mode is EXPLICIT: "HEADLESS" builds the level without the engine's modal Save-As;
	// anything else uses the proven dialog path. Never inferred, never fallen back to -
	// the two paths land the level in DIFFERENT places and that must not be a surprise.
	const bool bHeadless = Mode.TrimStartAndEnd().Equals(TEXT("HEADLESS"), ESearchCase::IgnoreCase);
	using namespace RudeBuildArea;

	const FString Area = AreaName.TrimStartAndEnd();
	if (Area.IsEmpty())
	{
		return Fail(TEXT("AreaName is empty - name the area whose spawned actors should be packed "
		                 "(it becomes the level name, e.g. 'cargoship' -> /Game/RUDE/Areas/cargoship)"));
	}

	const FString Slug = AreaSlug(Area);
	if (Slug.IsEmpty())
	{
		return Fail(FString::Printf(TEXT("AreaName '%s' slugs to nothing"), *Area));
	}
	const FString Requested = FString::Printf(TEXT("%s/%s"), AreasRoot, *Slug);
	{
		FText Why;
		if (!FPackageName::IsValidLongPackageName(Requested, /*bIncludeReadOnlyRoots*/ false, &Why))
		{
			return Fail(FString::Printf(TEXT("'%s' is not a valid package name: %s"),
				*Requested, *Why.ToString()));
		}
	}
	// Non-destructive default (BUILD_AREA_DESIGN section 3e): stage 1 has no REBUILD lane, and
	// re-creating a same-named level in one session also walks into the delete-tombstone trap
	// (ENGINEERING_LOG "UE / plugin build & MCP gotchas"). Refuse instead of overwriting.
	if (FPackageName::DoesPackageExist(Requested))
	{
		return Fail(FString::Printf(
			TEXT("%s already exists - stage 1 will not overwrite an area level. Delete it in the "
			     "Content Browser (and restart the editor before re-creating the same name), or "
			     "pack under a different area name"), *Requested));
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return Fail(TEXT("no editor world"));
	}
	ULevelInstanceSubsystem* Subsystem = World->GetSubsystem<ULevelInstanceSubsystem>();
	if (!Subsystem)
	{
		return Fail(TEXT("this world has no ULevelInstanceSubsystem - open a normal editor map "
		                 "before packing"));
	}

	// ---- 1) collect ------------------------------------------------------------------------
	// The selector. Empty = the marker ImportScene uses today. Matched as EITHER an actor tag OR
	// the outliner folder path, because the two RUDE spawners disagree about which one they stamp
	// (see DefaultMarker). Both counts are reported so "which one matched" is never a guess.
	const FString MarkerStr = ActorTag.TrimStartAndEnd().IsEmpty() ? FString(DefaultMarker)
	                                                               : ActorTag.TrimStartAndEnd();
	const FName Marker(*MarkerStr);

	TArray<AActor*> ToPack;
	TMap<FString, FActorShot> Before;       // actor label is the pairing key for the after-side
	int32 MatchedByTag = 0, MatchedByFolder = 0, SkippedLevelInstances = 0, DuplicateLabels = 0;
	int32 SkippedAlreadyPacked = 0, ActorsInWorld = 0;
	TSet<const ULevel*> LevelsSpanned;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		++ActorsInWorld;
		if (!A)
		{
			continue;
		}
		const bool bTag = A->Tags.Contains(Marker);
		const bool bFolder = (A->GetFolderPath() == Marker);
		if (!bTag && !bFolder)
		{
			continue;
		}
		// A Level Instance actor cannot be packed into a Level Instance in the general case
		// (CanMoveActorToLevel, LevelInstanceSubsystem.cpp:1947+) and packing one would nest
		// areas by accident. Excluded on purpose, and counted so the exclusion is visible.
		if (Cast<ILevelInstanceInterface>(A))
		{
			++SkippedLevelInstances;
			continue;
		}
		// ⛔ And an actor that ALREADY LIVES INSIDE a Level Instance must never be re-packed.
		// TActorIterator walks every level of the world, including the streamed levels of Level
		// Instances created by earlier runs. Today that is harmless because the engine strips the
		// outliner folder on pack (note 5) and the map spawner marks by folder only - but TAGS
		// survive the pack, so the moment BUILD_AREA_DESIGN section 1c adds the RUDE_LS tag to
		// SpawnSceneManifest, packing area B would silently swallow area A's contents out of A's
		// level. Guarding now costs one call and removes a defect that would be very hard to see.
		if (Subsystem->GetParentLevelInstance(A))
		{
			++SkippedAlreadyPacked;
			continue;
		}
		if (bTag) { ++MatchedByTag; } else { ++MatchedByFolder; }

		const FString Label = A->GetActorLabel();
		if (Before.Contains(Label))
		{
			// Pairing is by label; duplicates make the delta unpairable for those actors. Counted
			// and surfaced through deltaValid rather than quietly dropped.
			++DuplicateLabels;
		}
		else
		{
			ShootActor(A, Before.Add(Label));
		}
		ToPack.Add(A);
		LevelsSpanned.Add(A->GetLevel());
	}

	if (ToPack.Num() == 0)
	{
		return Fail(FString::Printf(
			TEXT("no actors matched '%s' (searched actor tags AND outliner folder paths across "
			     "%d actors in the open world) - import the area first, e.g. RUDE.Run ImportArea"),
			*MarkerStr, ActorsInWorld));
	}
	// The cut/paste move takes a fast single-level path only when every actor lives in one level
	// (EditorServer.cpp:3359 CanPerformQuickCopy); the multi-level path can prompt, which stalls a
	// scripted run. Refusing is my call: stage 1 is a measurement, and a measurement taken through
	// a different code path than production would prove the wrong thing.
	if (LevelsSpanned.Num() > 1)
	{
		return Fail(FString::Printf(
			TEXT("the %d matched actors span %d levels - pack from a single level"),
			ToPack.Num(), LevelsSpanned.Num()));
	}

	// ⛔ COUNT EVERY ACTOR THAT WILL BE PACKED, not just the uniquely-labelled ones. `Before` is
	// keyed by label for PAIRING, so duplicate labels collapse into one entry - but the after-count
	// walks the packed level and counts them all. Summing the map would therefore under-count the
	// BEFORE side and make R1 look like instances were GAINED. Sum from ToPack instead, and let
	// r1Valid say whether the pairing-based delta below can be trusted at all.
	// (Review finding, 2026-07-28.)
	int64 InstancesBefore = 0;
	int32 IsmBefore = 0;
	for (AActor* A : ToPack)
	{
		FActorShot Shot;
		ShootActor(A, Shot);
		InstancesBefore += Shot.Instances;
		IsmBefore += Shot.IsmComponents;
	}

	// ---- 2) guard --------------------------------------------------------------------------
	{
		FText Reason;
		if (!Subsystem->CanCreateLevelInstanceFrom(ToPack, &Reason))
		{
			return Fail(FString::Printf(TEXT("CanCreateLevelInstanceFrom refused: %s"),
				*Reason.ToString()));
		}
	}

	// ---- 3) pack ---------------------------------------------------------------------------
	// Best-effort courtesy for the modal described in note (3): point the Save-As browser at the
	// folder the area belongs in. The dialog still owns the final answer - LevelPackageName does
	// not reach it on this path - so this only saves the operator from navigating.
	{
		FString AreasDirOnDisk;
		if (FPackageName::TryConvertLongPackageNameToFilename(
			FPackageName::GetLongPackagePath(Requested), AreasDirOnDisk))
		{
			FEditorDirectories::Get().SetLastDirectory(ELastDirectory::LEVEL, AreasDirOnDisk);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] PackAreaLevelInstance '%s': packing %d actors / %d ISM components / %lld "
		     "instances -> %s. A modal 'Save Level As' WILL open (UE 5.8 always uses Save-As here) "
		     "- confirm that path. Under -unattended it is cancelled and this call fails."),
		*Area, ToPack.Num(), IsmBefore, InstancesBefore, *Requested);

	FNewLevelInstanceParams Params;
	Params.Type = ELevelInstanceCreationType::LevelInstance;   // never PackedLevelActor: a packed
	                                                           // actor bakes to a Blueprint and is
	                                                           // not the editable level 5c asked for
	Params.PivotType = ELevelInstancePivotType::WorldOrigin;   // ⛔⛔ THE decision. See note (1).
	Params.PivotActor = nullptr;                               // only read when PivotType == Actor
	Params.LevelPackageName = Requested;                       // honoured on the packed path only
	                                                           // (note 3) - set anyway so this call
	                                                           // states its intent and stays correct
	                                                           // if a later engine wires it through
	Params.bAlwaysShowDialog = false;                          // read only by the editor MENU path
	Params.bPromptForSave = false;                             // (LevelInstanceEditorModule.cpp:349)
	Params.SetExternalActors(true);                            // OFPA - matches the WP world and
	                                                           // keeps areas diffable (5c)
	// Params.LevelInstanceClass left null: LevelInstanceSubsystem.cpp:1491-1494 then picks
	// ALevelInstance for this Type. Deterministic, and it ignores the per-project user setting the
	// editor menu applies - a scripted pack must not depend on a user preference.

	// ⭐ TWO PATHS, chosen EXPLICITLY - never a silent fallback (a fallback here would quietly
	// change WHERE the level lands, which is the one thing this tool must report truthfully).
	//   DIALOG   (default) - the engine's own CreateLevelInstanceFrom. Proven in-engine
	//                        2026-07-28: 13,152 instances survived exactly, pivot at origin.
	//                        Costs a modal Save-As, so it cannot run headless.
	//   HEADLESS           - build the level ourselves. Required by Matt's "Build Full Map runs
	//                        headless via an agent" (UX section 10), because the subsystem
	//                        exposes NO scripted entry point (only CreateLevelInstanceFrom).
	ILevelInstanceInterface* NewLI = nullptr;
	if (bHeadless)
	{
		NewLI = CreateLevelInstanceHeadless(World, ToPack, Requested, nullptr);
		ToPack.Empty();
		if (!NewLI)
		{
			return Fail(TEXT("headless level creation failed - see the log for "
			                 "LogEditorLevelUtils/LogLevelInstance. Nothing was packed"));
		}
	}
	else
	{
		NewLI = Subsystem->CreateLevelInstanceFrom(ToPack, Params);

		// ⛔ From here on every pointer in ToPack is DEAD (note 4). Do not dereference them.
		ToPack.Empty();

		if (!NewLI)
		{
			return Fail(TEXT("CreateLevelInstanceFrom returned null - the level was not created. "
			                 "Most likely the 'Save Level As' dialog was cancelled (UE 5.8 always "
			                 "uses Save-As here). Pass Mode=HEADLESS to build the level without a "
			                 "dialog. Check the log for LogLevelInstance/LogSlate lines"));
		}
	}
	AActor* LIActor = Cast<AActor>(NewLI);
	if (!LIActor)
	{
		return Fail(TEXT("the created Level Instance is not an AActor - engine contract changed"));
	}

	// ---- 4) measure ------------------------------------------------------------------------
	const FString Created = NewLI->GetWorldAssetPackage();
	const bool bMatches = Created.Equals(Requested, ESearchCase::CaseSensitive);
	if (!bMatches)
	{
		// Not a failure - the operator owns the path (note 3) - but it MUST be visible, because
		// everything downstream (BuildArea, Build Full Map) addresses areas by this package name.
		UE_LOG(LogTemp, Warning,
			TEXT("[RUDE] PackAreaLevelInstance: requested %s but the level was created at %s"),
			*Requested, *Created);
	}

	// The contents live in a streamed level; make sure it is resident before counting it, or a
	// zero count would read as "the ISMs did not survive" when it only means "not loaded yet".
	if (!NewLI->IsLoaded() && !NewLI->IsEditing())
	{
		Subsystem->BlockLoadLevelInstance(NewLI);
	}

	// ⚠ The new level is a WORLD, so ForEachActorInLevelInstance also hands back the level's own
	// housekeeping actors (AWorldSettings, the default brush, AWorldDataLayers on a partitioned
	// level). Counting those as "packed" would inflate actorsPacked and make an actor LOSS look
	// like a clean run - so only labels we actually sent are treated as ours, and everything else
	// is reported separately instead of being folded in.
	TMap<FString, FActorShot> After;
	int32 ActorsInLevel = 0, FoldersSurviving = 0, IsmAfter = 0, UnmatchedIsmActors = 0;
	int64 InstancesAfter = 0;
	Subsystem->ForEachActorInLevelInstance(NewLI, [&](AActor* A) -> bool
	{
		if (!A)
		{
			return true;
		}
		++ActorsInLevel;
		const FString Label = A->GetActorLabel();
		if (!Before.Contains(Label))
		{
			// An ISM-bearing actor we cannot name is the signature of a RELABEL on paste - the one
			// assumption in note (4) that is inferred rather than measured. Counted, never hidden.
			TArray<UInstancedStaticMeshComponent*> Stray;
			A->GetComponents(Stray);
			if (Stray.Num() > 0)
			{
				++UnmatchedIsmActors;
			}
			return true;
		}
		// The engine strips folders on pack (note 5). Counting survivors turns R12 into a number.
		if (!A->GetFolderPath().IsNone())
		{
			++FoldersSurviving;
		}
		FActorShot Shot;
		ShootActor(A, Shot);
		InstancesAfter += Shot.Instances;
		IsmAfter += Shot.IsmComponents;
		After.Add(Label, MoveTemp(Shot));
		return true;
	});

	// ⚠ NAMING, corrected 2026-07-28: this is the MOVE-INTEGRITY gate, NOT the pivot proof. The
// engine deliberately preserves world positions when it packs a Level Instance regardless of
// pivot type (LevelInstanceSubsystem.cpp:1555-1557 - actors keep world positions and are made
// relative on save), so maxWorldDeltaCm reads 0.0 under CenterMinZ TOO. What actually proves
// PivotType=WorldOrigin is `levelInstanceAtOrigin` + the LI actor's own location being 0,0,0.
// Pair by label; measure the actor's own world location AND the sampled ISM
	// instance world transforms, because our map actors sit at the origin and carry every real
	// RAGE coordinate inside their ISM instances (RudeToolset.cpp:3432 spawns the actor with no
	// transform, :3500 adds instances with bWorldSpace=true). An actor-location-only check would
	// therefore be a weak test of exactly the thing that matters.
	double MaxDeltaCm = 0.0, MaxRotDeg = 0.0, MaxScaleDelta = 0.0;
	int32 UnpairedBefore = 0, UnpairedComponents = 0, SampledInstances = 0;

	for (const TPair<FString, FActorShot>& B : Before)
	{
		const FActorShot* A = After.Find(B.Key);
		if (!A)
		{
			++UnpairedBefore;
			continue;
		}
		MaxDeltaCm = FMath::Max(MaxDeltaCm, (double)(A->Location - B.Value.Location).Size());
		for (const TPair<FString, TArray<FTransform>>& BC : B.Value.Samples)
		{
			const TArray<FTransform>* AC = A->Samples.Find(BC.Key);
			if (!AC || AC->Num() != BC.Value.Num())
			{
				++UnpairedComponents;
				continue;
			}
			for (int32 i = 0; i < BC.Value.Num(); ++i)
			{
				const FTransform& Bx = BC.Value[i];
				const FTransform& Ax = (*AC)[i];
				++SampledInstances;
				MaxDeltaCm = FMath::Max(MaxDeltaCm,
					(double)(Ax.GetLocation() - Bx.GetLocation()).Size());
				MaxRotDeg = FMath::Max(MaxRotDeg,
					(double)FMath::RadiansToDegrees(Ax.GetRotation().AngularDistance(Bx.GetRotation())));
				MaxScaleDelta = FMath::Max(MaxScaleDelta,
					(double)(Ax.GetScale3D() - Bx.GetScale3D()).GetAbsMax());
			}
		}
	}
	// deltaValid says whether maxWorldDeltaCm covers EVERYTHING that was packed. When it is false
	// the number is still reported, but it is a PARTIAL measurement and must not be read as proof
	// that nothing moved - the unpaired* counts say what it missed.
	const bool bDeltaValid = (UnpairedBefore == 0 && UnpairedComponents == 0
		&& UnmatchedIsmActors == 0 && DuplicateLabels == 0 && SampledInstances > 0);

	const FVector LILoc = LIActor->GetActorLocation();
	const FRotator LIRot = LIActor->GetActorRotation();
	// The WorldOrigin mechanism check, independent of the per-actor deltas: the LI actor itself
	// must sit at world origin with no rotation, because LevelTransform IS that transform
	// (LevelInstanceSubsystem.cpp:1558) and anything else re-bases the contents on save.
	const bool bAtOrigin = LILoc.IsNearlyZero(0.01) && LIRot.IsNearlyZero(0.01);

	const bool bOnDisk = FPackageName::DoesPackageExist(Created);

	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] PackAreaLevelInstance '%s' -> %s | actors %d->%d | ISM %d->%d | instances "
		     "%lld->%lld | maxWorldDelta %.4f cm (valid=%s) | pivot WorldOrigin, LI at %s"),
		*Area, *Created, Before.Num() + DuplicateLabels, After.Num(), IsmBefore, IsmAfter,
		InstancesBefore, InstancesAfter, MaxDeltaCm,
		bDeltaValid ? TEXT("true") : TEXT("false"), *LILoc.ToString());

	return FString::Printf(
		TEXT("{\"ok\":true,\"level\":\"%s\",\"levelRequested\":\"%s\",\"levelMatchesRequest\":%s,")
		TEXT("\"levelOnDisk\":%s,\"actorsCollected\":%d,\"actorsPacked\":%d,\"actorsInLevel\":%d,")
		TEXT("\"matchedByTag\":%d,\"matchedByFolder\":%d,\"skippedLevelInstances\":%d,")
		TEXT("\"skippedAlreadyPacked\":%d,")
		TEXT("\"ismComponents\":%d,\"ismComponentsBefore\":%d,")
		TEXT("\"instancesBefore\":%lld,\"instancesAfter\":%lld,")
		TEXT("\"pivotType\":\"WorldOrigin\",\"levelInstanceLocationCm\":\"%.3f,%.3f,%.3f\",")
		TEXT("\"levelInstanceAtOrigin\":%s,\"maxWorldDeltaCm\":%.5f,\"maxRotDeltaDeg\":%.5f,")
		TEXT("\"maxScaleDelta\":%.5f,\"sampledInstances\":%d,\"deltaValid\":%s,")
		TEXT("\"unpairedBefore\":%d,\"unpairedComponents\":%d,\"unmatchedIsmActors\":%d,")
		TEXT("\"duplicateLabels\":%d,\"foldersSurviving\":%d}"),
		*Esc(Created), *Esc(Requested), bMatches ? TEXT("true") : TEXT("false"),
		bOnDisk ? TEXT("true") : TEXT("false"), Before.Num() + DuplicateLabels, After.Num(),
		ActorsInLevel,
		MatchedByTag, MatchedByFolder, SkippedLevelInstances,
		SkippedAlreadyPacked,
		IsmAfter, IsmBefore,
		InstancesBefore, InstancesAfter,
		LILoc.X, LILoc.Y, LILoc.Z,
		bAtOrigin ? TEXT("true") : TEXT("false"), MaxDeltaCm, MaxRotDeg,
		MaxScaleDelta, SampledInstances, bDeltaValid ? TEXT("true") : TEXT("false"),
		UnpairedBefore, UnpairedComponents, UnmatchedIsmActors,
		DuplicateLabels, FoldersSurviving);
}
