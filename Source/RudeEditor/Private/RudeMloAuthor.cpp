// RUDE - RAGE <-> Unreal Development Environment
// MLO interior AUTHORING lane (GDD Tier 1 interiors: import-AUTHOR-export). ImportMlo brings an existing
// interior in and ExportMloYtyp splices an EDITED one back into its own file; this file is the third leg -
// a NEW interior that never existed in the game, written as a FiveM resource the game loads.
//
// The surface is deliberately workflow-free. There is no interior editor mode, no wizard and no registry:
//   * a ROOM is an ARudeMloRoomVolume box you place and scale - it carries the room's own CMloRoomDef fields;
//   * a PORTAL is a thin ARudeMloPortalVolume box across a doorway - the room volumes it touches give
//     roomFrom/roomTo, the mid-plane of its thinnest axis gives the four corners;
//   * an ENTITY is ANY static-mesh actor standing inside a room box whose mesh resolves to an archetype.
// Delete a volume and the interior loses a room. Drag a prop from one room into another and it moves rooms.
// Nothing has to be told; ExportNewMlo reads the level.
//
// Every literal below is a measured law of the game's own files - maintainer lane `mlo_author` (`LAWS.md`),
// counted over 424 MLO ytyps / 541 CMloArchetypeDef / 2,143 rooms / 3,466 portals / 67,440 entities and the
// 1,745 ymaps that carry a CMloInstanceDef. The laws this file leans on hardest:
//   law 1  the archetype's 22 children are ONE order in 541/541, and flags/specialAttribute/hdTextureDist/
//          assetType/the three dictionaries/extensions are fixed values in 541/541.
//   law 2  bbMin/bbMax/bsCentre/bsRadius are ZERO in 541/541 - the corpus cannot teach the rule, so RUDE
//          writes the union of the rooms and the props (NOTES.md: the lane's one deliberate deviation).
//   law 3  room 0 is `limbo` in 541/541, with flags 96, blend 1, an EMPTY timecycle and depth -1.
//   law 4  portalCount is DERIVED from the portal list in 2,143/2,143 - never authored.
//   law 6  membership is the attachedObjects ORDINAL list, not geometry (an entity sits inside its own room's
//          box in only 30,779/66,332), so containment is an authoring convenience and the ordinals are truth.
//   law 8  a portal is 4 coplanar corners spelled "x, y, z, NaN" (13,864/13,864).
//   law 9  roomFrom is NEVER limbo (0/3,466); the corner z-order L H H L is the plurality (1,762/3,206) and
//          the winding is NOT recoverable (towards roomFrom 1,855 / towards roomTo 1,609 / degenerate 2 of
//          3,466) - RUDE winds towards roomTo, the MINORITY half, for determinism and says so.
//   law 10 one CMloInstanceDef per ymap (1,745/1,745), 22 fields in one order, numExitPortals = the portals
//          that touch limbo, and contentFlags 0x40 <-> a non-empty <physicsDictionaries> (1,745/1,745).
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "RudeArchetype.h"
#include "RudeEntityComponent.h"
#include "RudeMloEntityComponent.h"
#include "RudeMloVolumes.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---- names -------------------------------------------------------------------------------------
// The archetype name is lower-case a-z0-9_ in 540/541 corpus MLOs and the instance's archetypeName is
// lower-case in 1,745/1,745; anything else is refused rather than escaped, because the value is also a
// FILE NAME (stream/<name>.ytyp) and a joaat key.
static bool RudeAuthIsCleanAssetName(const FString& S)
{
	if (S.IsEmpty() || S.Len() > 96) { return false; }
	for (const TCHAR C : S)
	{
		const bool bOk = (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_';
		if (!bOk) { return false; }
	}
	return true;
}

// Room and timecycle names carry upper case in 1,389/2,143 rooms, so case is free - but they land in element
// TEXT with no escaping, so an XML-special or whitespace is refused.
static bool RudeAuthIsCleanText(const FString& S)
{
	for (const TCHAR C : S)
	{
		if (C == '<' || C == '>' || C == '&' || C == '"' || C == '\'' || C == '\n' || C == '\r' || C == '\t' || C == ' ')
		{
			return false;
		}
	}
	return true;
}

// ---- geometry ----------------------------------------------------------------------------------
// The AABB of a box component's eight corners in the interior root's frame (UE cm). A CMloRoomDef has NO
// orientation, so a rotated volume must be written as its enclosing axis-aligned box - never smaller than
// what the author drew (LAWS.md law 11).
static void RudeAuthLocalAabb(const UBoxComponent* BoxComp, const FTransform& RootXf, FVector& OutMin, FVector& OutMax)
{
	const FVector E = BoxComp->GetUnscaledBoxExtent();
	const FTransform Rel = BoxComp->GetComponentTransform().GetRelativeTransform(RootXf);
	OutMin = FVector(TNumericLimits<double>::Max());
	OutMax = FVector(-TNumericLimits<double>::Max());
	for (int32 i = 0; i < 8; ++i)
	{
		const FVector P = Rel.TransformPosition(FVector((i & 1) ? E.X : -E.X, (i & 2) ? E.Y : -E.Y, (i & 4) ? E.Z : -E.Z));
		OutMin = OutMin.ComponentMin(P);
		OutMax = OutMax.ComponentMax(P);
	}
}

// UE cm -> RAGE metres with the import lane's Y mirror. A BOX mirrors by mirroring its corners, so min and
// max swap on Y (LAWS.md law 11).
static FVector RudeAuthToRage(const FVector& P) { return FVector(P.X / 100.0, -P.Y / 100.0, P.Z / 100.0); }

static void RudeAuthBoxToRage(const FVector& Min, const FVector& Max, FVector& OutMin, FVector& OutMax)
{
	OutMin = FVector(Min.X / 100.0, -Max.Y / 100.0, Min.Z / 100.0);
	OutMax = FVector(Max.X / 100.0, -Min.Y / 100.0, Max.Z / 100.0);
}

static bool RudeAuthAabbOverlap(const FVector& AMin, const FVector& AMax, const FVector& BMin, const FVector& BMax, double Tol)
{
	return AMin.X - Tol <= BMax.X && AMax.X + Tol >= BMin.X
		&& AMin.Y - Tol <= BMax.Y && AMax.Y + Tol >= BMin.Y
		&& AMin.Z - Tol <= BMax.Z && AMax.Z + Tol >= BMin.Z;
}

static bool RudeAuthAabbContains(const FVector& Min, const FVector& Max, const FVector& P)
{
	return P.X >= Min.X && P.X <= Max.X && P.Y >= Min.Y && P.Y <= Max.Y && P.Z >= Min.Z && P.Z <= Max.Z;
}

// ---- the interior in the level -----------------------------------------------------------------
static FString RudeAuthTagValue(const AActor* A, const TCHAR* Prefix)
{
	const int32 L = FCString::Strlen(Prefix);
	for (const FName& T : A->Tags)
	{
		const FString S = T.ToString();
		if (S.StartsWith(Prefix, ESearchCase::IgnoreCase)) { return S.Mid(L); }
	}
	return FString();
}

// Every interior root in the level (ImportMlo's convention, shared: tag RUDE_MLO_ROOT + RUDE_MLO:<name>).
static AActor* RudeAuthFindRoot(UWorld* World, const FString& Wanted, TArray<FString>& OutPresent, int32& OutRoots)
{
	AActor* Found = nullptr;
	OutRoots = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(FName(TEXT("RUDE_MLO_ROOT")))) { continue; }
		const FString Name = RudeAuthTagValue(*It, TEXT("RUDE_MLO:"));
		if (Name.IsEmpty()) { continue; }
		++OutRoots;
		OutPresent.Add(Name);
		if (!Found && Name.Equals(Wanted, ESearchCase::IgnoreCase)) { Found = *It; }
	}
	return Found;
}

// The archetype a placed actor stands for: the interior component first (an ImportMlo / AddMloProp actor),
// then the ymap component (a PlaceArchetype actor), then the mesh's own asset name - which IS the archetype
// name for RUDE-imported drawables, because the import lane names the asset after the drawable.
static FString RudeAuthArchetypeOf(const AActor* A, FString& OutHow)
{
	if (const URudeMloEntityComponent* M = A->FindComponentByClass<URudeMloEntityComponent>())
	{
		if (!M->ArchetypeName.IsEmpty()) { OutHow = TEXT("mloComponent"); return M->ArchetypeName.ToLower(); }
	}
	if (const URudeEntityComponent* R = A->FindComponentByClass<URudeEntityComponent>())
	{
		if (!R->ArchetypeName.IsEmpty()) { OutHow = TEXT("entityComponent"); return R->ArchetypeName.ToLower(); }
	}
	if (const UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>())
	{
		if (UStaticMesh* Mesh = SMC->GetStaticMesh())
		{
			OutHow = TEXT("meshName");
			return Mesh->GetName().ToLower();
		}
	}
	OutHow = TEXT("none");
	return FString();
}

// ---- the gathered interior ---------------------------------------------------------------------
struct FRudeAuthRoom
{
	ARudeMloRoomVolume* Volume = nullptr;   // null = the synthesized limbo
	FString Name = TEXT("limbo");
	FVector LocalMin = FVector::ZeroVector, LocalMax = FVector::ZeroVector;   // UE cm, root-relative
	FVector WorldMin = FVector::ZeroVector, WorldMax = FVector::ZeroVector;   // UE cm, world (for the ymap extents)
	int32 Flags = 96;
	float Blend = 1.f;
	FString Timecycle, Timecycle2;
	int32 FloorId = 0;
	int32 Depth = -1;
	TArray<int32> Attached;
	int32 PortalCount = 0;
};

struct FRudeAuthPortal
{
	ARudeMloPortalVolume* Volume = nullptr;
	int32 From = -1, To = -1;
	FVector Corners[4];   // RAGE metres, MLO-local, in the order written
	bool bAmbiguousPlane = false;
};

struct FRudeAuthEntity
{
	AActor* Actor = nullptr;
	FString Archetype;
	FString How;
	FTransform Local;     // UE cm, root-relative
	FVector World = FVector::ZeroVector;
	int32 Room = 0;
	bool bHidden = false;
};

// ---- spawn helpers (agent; the gate builds an interior with these) ------------------------------
FString URudeToolset::NewMloInterior(const FString& InteriorName, const FString& LocationCm)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Name = InteriorName.TrimStartAndEnd().ToLower();
	if (!RudeAuthIsCleanAssetName(Name))
	{
		return Fail(TEXT("InteriorName must be lower-case a-z, 0-9 and _ (it is the archetype name, the ytyp file name and a joaat key)"));
	}
	TArray<FString> P;
	LocationCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
	const FVector Loc = (P.Num() == 3)
		? FVector(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2]))
		: FVector::ZeroVector;
	// re-running replaces the root only (rooms, portals and props keep standing); the interior is its tag
	const FName IdTag(*(TEXT("RUDE_MLO:") + Name));
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(IdTag) && It->Tags.Contains(FName(TEXT("RUDE_MLO_ROOT"))))
		{
			return FString::Printf(TEXT("{\"ok\":true,\"interior\":\"%s\",\"root\":\"%s\",\"created\":false}"),
				*RudeJsonEscape(Name), *RudeJsonEscape(It->GetActorLabel()));
		}
	}
	AActor* Root = World->SpawnActor<AActor>();
	if (!Root) { return Fail(TEXT("root actor spawn failed")); }
	USceneComponent* RootComp = NewObject<USceneComponent>(Root, TEXT("Root"));
	Root->SetRootComponent(RootComp);
	RootComp->SetMobility(EComponentMobility::Static);
	RootComp->RegisterComponent();
	Root->AddInstanceComponent(RootComp);
	Root->SetActorLocation(Loc);
	Root->SetActorLabel(TEXT("MLO_") + Name);
	Root->SetFolderPath(FName(TEXT("RUDE_MLO")));
	Root->Tags.Add(IdTag);
	Root->Tags.Add(FName(TEXT("RUDE_MLO_ROOT")));
	// no RUDE_MLO_Ytyp tag: this interior has no source file, and ExportMloYtyp must not try to splice one
	Root->Tags.Add(FName(TEXT("RUDE_MLO_AUTHORED")));
	Root->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"interior\":\"%s\",\"root\":\"%s\",\"created\":true,\"originCm\":[%g,%g,%g]}"),
		*RudeJsonEscape(Name), *RudeJsonEscape(Root->GetActorLabel()), Loc.X, Loc.Y, Loc.Z);
}

// "key=value;key=value" - unknown keys are REFUSED, never ignored: a typo in a field name that silently did
// nothing would ship an interior whose flags the author believes they set.
static bool RudeAuthParseFields(const FString& Spec, TMap<FString, FString>& Out, const TCHAR* Allowed[], int32 NumAllowed, FString& OutErr)
{
	TArray<FString> Pairs;
	Spec.Replace(TEXT(","), TEXT(";")).ParseIntoArray(Pairs, TEXT(";"), true);
	for (const FString& Pair : Pairs)
	{
		FString K, V;
		if (!Pair.Split(TEXT("="), &K, &V))
		{
			OutErr = FString::Printf(TEXT("field '%s' is not key=value"), *Pair.TrimStartAndEnd());
			return false;
		}
		K.TrimStartAndEndInline();
		V.TrimStartAndEndInline();
		bool bKnown = false;
		for (int32 i = 0; i < NumAllowed; ++i)
		{
			if (K.Equals(Allowed[i], ESearchCase::IgnoreCase)) { K = Allowed[i]; bKnown = true; break; }
		}
		if (!bKnown)
		{
			FString List;
			for (int32 i = 0; i < NumAllowed; ++i) { List += FString::Printf(TEXT("%s%s"), i ? TEXT(", ") : TEXT(""), Allowed[i]); }
			OutErr = FString::Printf(TEXT("unknown field '%s' (known: %s)"), *K, *List);
			return false;
		}
		Out.Add(K, V);
	}
	return true;
}

FString URudeToolset::AddMloRoom(const FString& InteriorName, const FString& RoomName,
                                 const FString& CenterCm, const FString& ExtentCm, const FString& Fields)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Interior = InteriorName.TrimStartAndEnd().ToLower();
	const FString Room = RoomName.TrimStartAndEnd();
	if (Interior.IsEmpty() || Room.IsEmpty()) { return Fail(TEXT("InteriorName and RoomName are required")); }
	if (!RudeAuthIsCleanText(Room)) { return Fail(TEXT("RoomName may not contain whitespace or XML specials")); }
	TArray<FString> Present;
	int32 Roots = 0;
	AActor* Root = RudeAuthFindRoot(World, Interior, Present, Roots);
	if (!Root) { return Fail(FString::Printf(TEXT("no interior '%s' - run NewMloInterior first (present: %s)"), *Interior, *FString::Join(Present, TEXT(", ")))); }
	TArray<FString> C, E;
	CenterCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(C, TEXT(","), true);
	ExtentCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(E, TEXT(","), true);
	if (C.Num() != 3 || E.Num() != 3) { return Fail(TEXT("CenterCm and ExtentCm are both x,y,z in UE centimetres (ExtentCm = HALF sizes)")); }
	const FVector Centre(FCString::Atod(*C[0]), FCString::Atod(*C[1]), FCString::Atod(*C[2]));
	const FVector Extent(FCString::Atod(*E[0]), FCString::Atod(*E[1]), FCString::Atod(*E[2]));
	// 1,589/1,591 finite corpus room boxes are non-degenerate; a flat room is an authoring mistake, not a shape
	if (Extent.X <= 0.0 || Extent.Y <= 0.0 || Extent.Z <= 0.0) { return Fail(TEXT("ExtentCm must be positive on all three axes (it is a half-size)")); }
	static const TCHAR* Allowed[] = { TEXT("limbo"), TEXT("flags"), TEXT("blend"), TEXT("timecycle"),
	                                  TEXT("secondaryTimecycle"), TEXT("floorId"), TEXT("exteriorDepth") };
	TMap<FString, FString> F;
	FString FieldErr;
	if (!RudeAuthParseFields(Fields, F, Allowed, (int32)UE_ARRAY_COUNT(Allowed), FieldErr)) { return Fail(FieldErr); }

	ARudeMloRoomVolume* V = World->SpawnActor<ARudeMloRoomVolume>();
	if (!V) { return Fail(TEXT("room volume spawn failed")); }
	V->Interior = Interior;
	V->RoomName = Room;
	if (const FString* S = F.Find(TEXT("limbo"))) { V->bLimbo = S->Equals(TEXT("true"), ESearchCase::IgnoreCase) || *S == TEXT("1"); }
	if (Room.Equals(TEXT("limbo"), ESearchCase::IgnoreCase)) { V->bLimbo = true; }
	if (const FString* S = F.Find(TEXT("flags"))) { V->Flags = FCString::Atoi(**S); }
	if (const FString* S = F.Find(TEXT("blend"))) { V->Blend = (float)FCString::Atod(**S); }
	if (const FString* S = F.Find(TEXT("timecycle"))) { V->TimecycleName = *S; }
	if (const FString* S = F.Find(TEXT("secondaryTimecycle"))) { V->SecondaryTimecycleName = *S; }
	if (const FString* S = F.Find(TEXT("floorId"))) { V->FloorId = FCString::Atoi(**S); }
	if (const FString* S = F.Find(TEXT("exteriorDepth"))) { V->ExteriorVisibiltyDepth = FCString::Atoi(**S); }
	if (!RudeAuthIsCleanText(V->TimecycleName) || !RudeAuthIsCleanText(V->SecondaryTimecycleName))
	{
		World->DestroyActor(V);
		return Fail(TEXT("timecycle names may not contain whitespace or XML specials"));
	}
	if (V->bLimbo)
	{
		// law 3: room 0 is `limbo` with flags 96, blend 1, an EMPTY timecycleName and
		// exteriorVisibiltyDepth -1 in 541/541 - so THE LIMBO ROOM DOES NOT TAKE THE AUTHOR'S VALUES for
		// those four, and the returned JSON shows what it got instead. `floorId` is deliberately NOT forced:
		// room 0's floorId was never binned (the 1,183/1,602 mode is the NON-limbo figure), and an
		// unmeasured field stays the author's. ExportNewMlo forces the same four again, because a volume
		// can also be placed by hand and edited in the details panel without this tool ever running.
		V->RoomName = TEXT("limbo");
		V->Flags = 96;
		V->Blend = 1.f;
		V->TimecycleName.Reset();
		V->ExteriorVisibiltyDepth = -1;
	}
	V->Box->SetBoxExtent(Extent, false);
	V->SetActorLocation(Root->GetActorLocation() + Centre);
	V->SetActorLabel(FString::Printf(TEXT("MLO_%s_room_%s"), *Interior, *V->RoomName));
	V->SetFolderPath(FName(*(TEXT("RUDE_MLO/") + Interior)));
	V->Tags.Add(FName(*(TEXT("RUDE_MLO:") + Interior)));
	V->Tags.Add(FName(TEXT("RUDE_MLO_RoomVolume")));
	V->AttachToActor(Root, FAttachmentTransformRules::KeepWorldTransform);
	V->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"interior\":\"%s\",\"room\":\"%s\",\"limbo\":%s,\"actor\":\"%s\",\"flags\":%d,\"floorId\":%d}"),
		*RudeJsonEscape(Interior), *RudeJsonEscape(V->RoomName), V->bLimbo ? TEXT("true") : TEXT("false"),
		*RudeJsonEscape(V->GetActorLabel()), V->Flags, V->FloorId);
}

FString URudeToolset::AddMloPortal(const FString& InteriorName, const FString& CenterCm,
                                   const FString& ExtentCm, const FString& Fields)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Interior = InteriorName.TrimStartAndEnd().ToLower();
	if (Interior.IsEmpty()) { return Fail(TEXT("InteriorName is required")); }
	TArray<FString> Present;
	int32 Roots = 0;
	AActor* Root = RudeAuthFindRoot(World, Interior, Present, Roots);
	if (!Root) { return Fail(FString::Printf(TEXT("no interior '%s' - run NewMloInterior first (present: %s)"), *Interior, *FString::Join(Present, TEXT(", ")))); }
	TArray<FString> C, E;
	CenterCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(C, TEXT(","), true);
	ExtentCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(E, TEXT(","), true);
	if (C.Num() != 3 || E.Num() != 3) { return Fail(TEXT("CenterCm and ExtentCm are both x,y,z in UE centimetres (ExtentCm = HALF sizes; the SMALLEST axis is the portal plane)")); }
	const FVector Centre(FCString::Atod(*C[0]), FCString::Atod(*C[1]), FCString::Atod(*C[2]));
	const FVector Extent(FCString::Atod(*E[0]), FCString::Atod(*E[1]), FCString::Atod(*E[2]));
	if (Extent.X <= 0.0 || Extent.Y <= 0.0 || Extent.Z <= 0.0) { return Fail(TEXT("ExtentCm must be positive on all three axes")); }
	static const TCHAR* Allowed[] = { TEXT("flags"), TEXT("mirrorPriority"), TEXT("opacity"),
	                                  TEXT("audioOcclusion"), TEXT("roomFrom"), TEXT("roomTo") };
	TMap<FString, FString> F;
	FString FieldErr;
	if (!RudeAuthParseFields(Fields, F, Allowed, (int32)UE_ARRAY_COUNT(Allowed), FieldErr)) { return Fail(FieldErr); }

	ARudeMloPortalVolume* V = World->SpawnActor<ARudeMloPortalVolume>();
	if (!V) { return Fail(TEXT("portal volume spawn failed")); }
	V->Interior = Interior;
	if (const FString* S = F.Find(TEXT("flags"))) { V->Flags = FCString::Atoi(**S); }
	if (const FString* S = F.Find(TEXT("mirrorPriority"))) { V->MirrorPriority = FCString::Atoi(**S); }
	if (const FString* S = F.Find(TEXT("opacity"))) { V->Opacity = FCString::Atoi(**S); }
	if (const FString* S = F.Find(TEXT("audioOcclusion"))) { V->AudioOcclusion = FCString::Strtoi64(**S, nullptr, 10); }
	if (const FString* S = F.Find(TEXT("roomFrom"))) { V->RoomFromName = *S; }
	if (const FString* S = F.Find(TEXT("roomTo"))) { V->RoomToName = *S; }
	V->Box->SetBoxExtent(Extent, false);
	V->SetActorLocation(Root->GetActorLocation() + Centre);
	V->SetActorLabel(FString::Printf(TEXT("MLO_%s_portal"), *Interior));
	V->SetFolderPath(FName(*(TEXT("RUDE_MLO/") + Interior)));
	V->Tags.Add(FName(*(TEXT("RUDE_MLO:") + Interior)));
	V->Tags.Add(FName(TEXT("RUDE_MLO_PortalVolume")));
	V->AttachToActor(Root, FAttachmentTransformRules::KeepWorldTransform);
	V->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"interior\":\"%s\",\"actor\":\"%s\",\"flags\":%d,\"thinAxisExtentCm\":%g}"),
		*RudeJsonEscape(Interior), *RudeJsonEscape(V->GetActorLabel()), V->Flags, FMath::Min3(Extent.X, Extent.Y, Extent.Z));
}

FString URudeToolset::AddMloProp(const FString& InteriorName, const FString& ArchetypeName,
                                 const FString& LocationCm, const FString& RotationDeg, const FString& PaletteFolder)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Interior = InteriorName.TrimStartAndEnd().ToLower();
	const FString Arch = ArchetypeName.TrimStartAndEnd().ToLower();
	if (Interior.IsEmpty() || Arch.IsEmpty()) { return Fail(TEXT("InteriorName and ArchetypeName are required")); }
	if (!RudeAuthIsCleanText(Arch)) { return Fail(TEXT("ArchetypeName may not contain whitespace or XML specials")); }
	TArray<FString> Present;
	int32 Roots = 0;
	AActor* Root = RudeAuthFindRoot(World, Interior, Present, Roots);
	if (!Root) { return Fail(FString::Printf(TEXT("no interior '%s' - run NewMloInterior first (present: %s)"), *Interior, *FString::Join(Present, TEXT(", ")))); }
	TArray<FString> L, R;
	LocationCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(L, TEXT(","), true);
	RotationDeg.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(R, TEXT(","), true);
	if (L.Num() != 3) { return Fail(TEXT("LocationCm must be x,y,z in UE centimetres, relative to the interior root")); }
	const FVector Loc(FCString::Atod(*L[0]), FCString::Atod(*L[1]), FCString::Atod(*L[2]));
	const FRotator Rot(R.Num() == 3 ? FCString::Atod(*R[0]) : 0.0,
	                   R.Num() == 3 ? FCString::Atod(*R[1]) : 0.0,
	                   R.Num() == 3 ? FCString::Atod(*R[2]) : 0.0);

	// the palette is consulted HERE (authoring time), so ExportNewMlo never has to guess a mesh's archetype
	UStaticMesh* Mesh = nullptr;
	FString PaletteHit;
	const FString Folder = PaletteFolder.TrimStartAndEnd();
	if (!Folder.IsEmpty())
	{
		if (const URudeArchetype* A = LoadObject<URudeArchetype>(nullptr, *(Folder / Arch + TEXT(".") + Arch)))
		{
			PaletteHit = Folder / Arch;
			Mesh = A->Mesh.LoadSynchronous();
		}
	}
	UStaticMesh* ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Mesh) { Mesh = ProxyCube; }
	if (!Mesh) { return Fail(TEXT("no mesh for the archetype and no /Engine/BasicShapes/Cube proxy available")); }

	AActor* A = World->SpawnActor<AActor>();
	if (!A) { return Fail(TEXT("prop spawn failed")); }
	UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(A, TEXT("Mesh"));
	SMC->SetStaticMesh(Mesh);
	SMC->SetMobility(EComponentMobility::Static);
	A->SetRootComponent(SMC);
	SMC->RegisterComponent();
	A->AddInstanceComponent(SMC);
	A->SetActorTransform(FTransform(Rot, Root->GetActorLocation() + Loc, FVector::OneVector));
	A->SetActorLabel(Arch);
	A->SetFolderPath(FName(*(TEXT("RUDE_MLO/") + Interior)));
	A->Tags.Add(FName(*(TEXT("RUDE_MLO:") + Interior)));
	A->Tags.Add(FName(TEXT("RUDE_MLO_Entity")));
	if (PaletteHit.IsEmpty()) { A->Tags.Add(FName(TEXT("RUDE_PROXY"))); }
	URudeMloEntityComponent* M = NewObject<URudeMloEntityComponent>(A, TEXT("RudeMloEntity"));
	M->Interior = Interior;
	M->SourceIndex = -1;          // authored: no ordinal until the export assigns one
	M->RoomIndex = -1;            // resolved from containment at export
	M->ArchetypeName = Arch;
	M->SourceTransform = A->GetActorTransform().GetRelativeTransform(Root->GetActorTransform());
	M->RegisterComponent();
	A->AddInstanceComponent(M);
	A->AttachToActor(Root, FAttachmentTransformRules::KeepWorldTransform);
	A->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":true,\"interior\":\"%s\",\"archetype\":\"%s\",\"actor\":\"%s\",\"palette\":\"%s\",\"mesh\":%s}"),
		*RudeJsonEscape(Interior), *RudeJsonEscape(Arch), *RudeJsonEscape(A->GetActorLabel()),
		*RudeJsonEscape(PaletteHit), PaletteHit.IsEmpty() ? TEXT("false") : TEXT("true"));
}

// ---- ExportNewMlo ------------------------------------------------------------------------------
FString URudeToolset::ExportNewMlo(const FString& InteriorName, const FString& OutDir, const FString& CorpusRoot)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Name = InteriorName.TrimStartAndEnd().ToLower();
	if (!RudeAuthIsCleanAssetName(Name)) { return Fail(TEXT("InteriorName must be lower-case a-z, 0-9 and _")); }
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("give an OutDir for the FiveM resource")); }
	TArray<FString> Present;
	int32 Roots = 0;
	AActor* Root = RudeAuthFindRoot(World, Name, Present, Roots);
	if (!Root) { return Fail(FString::Printf(TEXT("no interior '%s' in the level (present: %s)"), *Name, *FString::Join(Present, TEXT(", ")))); }
	const FTransform RootXf = Root->GetActorTransform();

	int32 Refusals = 0, Adopted = 0, OrphanVolumes = 0;
	FString Refused;
	auto Refuse = [&Refusals, &Refused](const FString& Why)
	{
		++Refusals;
		if (Refused.Len() < 1200)
		{
			Refused += FString::Printf(TEXT("%s\"%s\""), Refused.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(Why));
		}
	};
	// A volume with an empty Interior is adopted ONLY when this level holds exactly one interior - and the
	// count is reported, so an adoption is never invisible.
	auto Belongs = [&Name, &Roots, &Adopted, &OrphanVolumes](const FString& Declared, const AActor* Owner) -> bool
	{
		if (Declared.Equals(Name, ESearchCase::IgnoreCase)) { return true; }
		if (!Declared.IsEmpty()) { return false; }
		const FString Tagged = RudeAuthTagValue(Owner, TEXT("RUDE_MLO:"));
		if (Tagged.Equals(Name, ESearchCase::IgnoreCase)) { return true; }
		if (!Tagged.IsEmpty()) { return false; }
		if (Roots != 1)
		{
			// it names no interior and the level holds several: unassignable. Counted so it is never a
			// silent drop - a room that quietly did not make it into the file is exactly the failure
			// nothing downstream can see.
			++OrphanVolumes;
			return false;
		}
		++Adopted;
		return true;
	};

	// ---- 1) rooms. limbo is index 0 in 541/541 (law 3); everything else sorted by name so two runs of the
	//         same level produce the same ordinals (rooms and portals index entities BY ordinal - law 6).
	TArray<FRudeAuthRoom> Rooms;
	TArray<ARudeMloRoomVolume*> RoomVols;
	ARudeMloRoomVolume* LimboVol = nullptr;
	int32 LimboMarked = 0;
	for (TActorIterator<ARudeMloRoomVolume> It(World); It; ++It)
	{
		ARudeMloRoomVolume* V = *It;
		if (V->Box.Get() == nullptr) { continue; }
		if (!Belongs(V->Interior, V)) { continue; }
		const bool bIsLimbo = V->bLimbo || V->RoomName.Equals(TEXT("limbo"), ESearchCase::IgnoreCase);
		if (bIsLimbo) { ++LimboMarked; LimboVol = V; continue; }
		RoomVols.Add(V);
	}
	if (LimboMarked > 1) { return Fail(FString::Printf(TEXT("%d room volumes are marked limbo - an interior has exactly one (room 0, 541/541)"), LimboMarked)); }
	RoomVols.Sort([](const ARudeMloRoomVolume& A, const ARudeMloRoomVolume& B) { return A.RoomName < B.RoomName; });

	auto FillRoom = [&RootXf](FRudeAuthRoom& Out, ARudeMloRoomVolume* V)
	{
		Out.Volume = V;
		Out.Name = V->RoomName;
		Out.Flags = V->Flags;
		Out.Blend = V->Blend;
		Out.Timecycle = V->TimecycleName;
		Out.Timecycle2 = V->SecondaryTimecycleName;
		Out.FloorId = V->FloorId;
		Out.Depth = V->ExteriorVisibiltyDepth;
		RudeAuthLocalAabb(V->Box.Get(), RootXf, Out.LocalMin, Out.LocalMax);
		RudeAuthLocalAabb(V->Box.Get(), FTransform::Identity, Out.WorldMin, Out.WorldMax);
	};

	Rooms.SetNum(1);   // slot 0 = limbo, filled below
	for (ARudeMloRoomVolume* V : RoomVols)
	{
		FRudeAuthRoom R;
		FillRoom(R, V);
		Rooms.Add(R);
	}
	if (Rooms.Num() < 2) { return Fail(FString::Printf(TEXT("interior '%s' has no non-limbo room volume - add at least one ARudeMloRoomVolume (AddMloRoom)"), *Name)); }
	{
		TSet<FString> SeenNames;
		for (int32 i = 1; i < Rooms.Num(); ++i)
		{
			const FString Key = Rooms[i].Name.ToLower();
			if (SeenNames.Contains(Key)) { Refuse(FString::Printf(TEXT("two rooms are named '%s' - room names are unique in 541/541 corpus interiors"), *Rooms[i].Name)); }
			SeenNames.Add(Key);
			const FVector Size = Rooms[i].LocalMax - Rooms[i].LocalMin;
			if (Size.X <= 1e-3 || Size.Y <= 1e-3 || Size.Z <= 1e-3)
			{
				Refuse(FString::Printf(TEXT("room '%s' has a degenerate box (%g x %g x %g cm)"), *Rooms[i].Name, Size.X, Size.Y, Size.Z));
			}
		}
	}
	// limbo: the author's volume, else synthesized as the union of the rooms (law 5: the game's limbo box is
	// hand-authored and differs from the union in 525/536, so it is NOT recoverable - a superset is the safe
	// synthesis because every prop that falls outside a room still lands inside room 0's box)
	bool bLimboSynth = false;
	int32 LimboFieldsForced = 0;
	if (LimboVol)
	{
		FillRoom(Rooms[0], LimboVol);
		// law 3's five 541/541 room-0 values - the name plus four fields - are forced here as well as in
		// AddMloRoom: the volume is an actor whose RoomName / Flags / Blend / TimecycleName /
		// ExteriorVisibiltyDepth are all EditAnywhere, so an author can place one by hand, or retype them
		// in the details panel, and never call AddMloRoom at all.
		// Each override is COUNTED (limboFieldsForced) so the change is never invisible in the verdict.
		if (!Rooms[0].Name.Equals(TEXT("limbo"), ESearchCase::IgnoreCase)) { ++LimboFieldsForced; }
		if (Rooms[0].Flags != 96) { ++LimboFieldsForced; }
		if (Rooms[0].Blend != 1.f) { ++LimboFieldsForced; }
		if (!Rooms[0].Timecycle.IsEmpty()) { ++LimboFieldsForced; }
		if (Rooms[0].Depth != -1) { ++LimboFieldsForced; }
		Rooms[0].Name = TEXT("limbo");     // 541/541
		Rooms[0].Flags = 96;               // 541/541
		Rooms[0].Blend = 1.f;              // 541/541
		Rooms[0].Timecycle.Reset();        // EMPTY in 541/541
		Rooms[0].Depth = -1;               // 541/541
	}
	else
	{
		bLimboSynth = true;
		Rooms[0].Volume = nullptr;
		Rooms[0].Name = TEXT("limbo");
		Rooms[0].Flags = 96;          // 541/541
		Rooms[0].Blend = 1.f;         // 541/541
		Rooms[0].FloorId = 0;
		Rooms[0].Depth = -1;          // 541/541
		Rooms[0].LocalMin = Rooms[1].LocalMin;
		Rooms[0].LocalMax = Rooms[1].LocalMax;
		Rooms[0].WorldMin = Rooms[1].WorldMin;
		Rooms[0].WorldMax = Rooms[1].WorldMax;
		for (int32 i = 2; i < Rooms.Num(); ++i)
		{
			Rooms[0].LocalMin = Rooms[0].LocalMin.ComponentMin(Rooms[i].LocalMin);
			Rooms[0].LocalMax = Rooms[0].LocalMax.ComponentMax(Rooms[i].LocalMax);
			Rooms[0].WorldMin = Rooms[0].WorldMin.ComponentMin(Rooms[i].WorldMin);
			Rooms[0].WorldMax = Rooms[0].WorldMax.ComponentMax(Rooms[i].WorldMax);
		}
	}
	// A room's name and its two timecycle names land in element TEXT with NO escaping (§6 below writes
	// <name>%s</name>), exactly like an entity's archetypeName, which is checked at :~800. AddMloRoom
	// validates them at authoring time - but RoomName / TimecycleName / SecondaryTimecycleName are
	// EditAnywhere UPROPERTYs on a volume an author can also drag in by hand and retype in the details
	// panel, so the tool is not the only door. Validated again HERE, where the file is actually spelled:
	// one space or `&` would otherwise ship a malformed .ytyp under ok:true.
	for (const FRudeAuthRoom& R : Rooms)
	{
		if (R.Name.IsEmpty() || !RudeAuthIsCleanText(R.Name))
		{
			Refuse(FString::Printf(TEXT("room name '%s' is empty or carries whitespace / an XML special - a room name is written as unescaped element text"), *R.Name));
		}
		if (!RudeAuthIsCleanText(R.Timecycle) || !RudeAuthIsCleanText(R.Timecycle2))
		{
			Refuse(FString::Printf(TEXT("room '%s': a timecycle name carries whitespace or an XML special ('%s' / '%s') - both are written as unescaped element text"),
				*R.Name, *R.Timecycle, *R.Timecycle2));
		}
	}
	TMap<FString, int32> RoomByName;
	for (int32 i = 0; i < Rooms.Num(); ++i) { RoomByName.Add(Rooms[i].Name.ToLower(), i); }

	// ---- 2) portals: the room volumes each one touches, and the four corners of its thin face
	TArray<FRudeAuthPortal> Portals;
	TArray<ARudeMloPortalVolume*> PortalVols;
	for (TActorIterator<ARudeMloPortalVolume> It(World); It; ++It)
	{
		if (It->Box.Get() != nullptr && Belongs(It->Interior, *It)) { PortalVols.Add(*It); }
	}
	PortalVols.Sort([](const ARudeMloPortalVolume& A, const ARudeMloPortalVolume& B) { return A.GetName() < B.GetName(); });
	int32 PortalsWithoutTwoRooms = 0, PortalsAmbiguousPlane = 0;
	for (ARudeMloPortalVolume* V : PortalVols)
	{
		FRudeAuthPortal P;
		P.Volume = V;
		FVector PMin, PMax;
		RudeAuthLocalAabb(V->Box.Get(), RootXf, PMin, PMax);
		TArray<int32> Touch;
		for (int32 r = 1; r < Rooms.Num(); ++r)   // limbo is never roomFrom (0/3,466) and overlaps everything
		{
			if (RudeAuthAabbOverlap(PMin, PMax, Rooms[r].LocalMin, Rooms[r].LocalMax, 1.0)) { Touch.Add(r); }
		}
		// explicit overrides win: the only way to express a portal whose volume overlaps three rooms
		int32 OverFrom = -1, OverTo = -1;
		if (!V->RoomFromName.IsEmpty()) { if (const int32* I = RoomByName.Find(V->RoomFromName.ToLower())) { OverFrom = *I; } }
		if (!V->RoomToName.IsEmpty()) { if (const int32* I = RoomByName.Find(V->RoomToName.ToLower())) { OverTo = *I; } }
		if (!V->RoomFromName.IsEmpty() && OverFrom < 0) { Refuse(FString::Printf(TEXT("portal '%s': roomFrom '%s' is not a room of this interior"), *V->GetActorLabel(), *V->RoomFromName)); }
		if (!V->RoomToName.IsEmpty() && OverTo < 0) { Refuse(FString::Printf(TEXT("portal '%s': roomTo '%s' is not a room of this interior"), *V->GetActorLabel(), *V->RoomToName)); }
		if (OverFrom >= 0 || OverTo >= 0)
		{
			P.From = OverFrom >= 0 ? OverFrom : (Touch.Num() > 0 ? Touch[0] : -1);
			// the roomTo fallback consults the touched volumes exactly as the roomFrom one does. A slab
			// overlapping two rooms with only `roomFrom=` supplied is an INTERNAL doorway; defaulting it to
			// limbo would silently turn it into an exit portal and count it into numExitPortals.
			P.To = OverTo >= 0 ? OverTo : (Touch.Num() == 2 ? (Touch[0] == P.From ? Touch[1] : Touch[0]) : 0);
		}
		else if (Touch.Num() == 2)
		{
			// no ordering rule exists in the game (roomFrom < roomTo in only 843/3,466 - law 9), so RUDE
			// chooses the lower index as `from`; deterministic, and the comparator checks it
			P.From = Touch[0];
			P.To = Touch[1];
		}
		else if (Touch.Num() == 1)
		{
			P.From = Touch[0];
			P.To = 0;   // an exit portal: roomTo is limbo in 2,116/3,466
		}
		else
		{
			++PortalsWithoutTwoRooms;
			Refuse(FString::Printf(TEXT("portal '%s' touches %d room volumes - it needs exactly one (an exit to limbo) or two, or a roomFrom=/roomTo= override"),
				*V->GetActorLabel(), Touch.Num()));
			continue;
		}
		if (P.From < 0 || P.To < 0 || P.From == P.To || P.From == 0)
		{
			++PortalsWithoutTwoRooms;
			Refuse(FString::Printf(TEXT("portal '%s' resolved to roomFrom %d roomTo %d - roomFrom is never limbo (0/3,466) and the two are never equal (3,466/3,466)"),
				*V->GetActorLabel(), P.From, P.To));
			continue;
		}

		// the plane: the box's THINNEST axis; its mid-plane quad is coplanar by construction (3,466/3,466)
		const FVector E = V->Box.Get()->GetUnscaledBoxExtent();
		const FTransform Rel = V->Box.Get()->GetComponentTransform().GetRelativeTransform(RootXf);
		const FVector Sc = Rel.GetScale3D().GetAbs();
		const FVector Se(E.X * Sc.X, E.Y * Sc.Y, E.Z * Sc.Z);
		int32 ThinAxis = 0;
		if (Se.Y < Se[ThinAxis]) { ThinAxis = 1; }
		if (Se.Z < Se[ThinAxis]) { ThinAxis = 2; }
		{
			double Second = TNumericLimits<double>::Max();
			for (int32 k = 0; k < 3; ++k) { if (k != ThinAxis) { Second = FMath::Min(Second, Se[k]); } }
			if (Second <= Se[ThinAxis] * 1.01)
			{
				P.bAmbiguousPlane = true;
				++PortalsAmbiguousPlane;
			}
		}
		const int32 AxA = (ThinAxis + 1) % 3;
		const int32 AxB = (ThinAxis + 2) % 3;
		auto UnitAxis = [](int32 I) { return FVector(I == 0 ? 1.0 : 0.0, I == 1 ? 1.0 : 0.0, I == 2 ? 1.0 : 0.0); };
		const FVector WorldA = Rel.TransformVector(UnitAxis(AxA)).GetSafeNormal();
		const FVector WorldB = Rel.TransformVector(UnitAxis(AxB)).GetSafeNormal();
		const int32 UpAxis = FMath::Abs(WorldB.Z) > FMath::Abs(WorldA.Z) ? AxB : AxA;
		const int32 AcrossAxis = (UpAxis == AxA) ? AxB : AxA;
		auto PlaneCorner = [&Rel, &E, ThinAxis, UpAxis, AcrossAxis](double SAcross, double SUp)
		{
			FVector L(0.0, 0.0, 0.0);
			L[AcrossAxis] = SAcross * E[AcrossAxis];
			L[UpAxis] = SUp * E[UpAxis];
			L[ThinAxis] = 0.0;
			return Rel.TransformPosition(L);
		};
		// L H H L along the vertical axis - the plurality corner order (1,762/3,206 vertical portals, law 9)
		FVector Q[4];
		Q[0] = RudeAuthToRage(PlaneCorner(-1.0, -1.0));
		Q[1] = RudeAuthToRage(PlaneCorner(-1.0, +1.0));
		Q[2] = RudeAuthToRage(PlaneCorner(+1.0, +1.0));
		Q[3] = RudeAuthToRage(PlaneCorner(+1.0, -1.0));
		// wind the quad so its Newell normal points from roomFrom towards roomTo. In the game's own files
		// that is the MINORITY spelling: 1,609/3,466 quads point towards roomTo, 1,855 point towards
		// roomFrom and 2 are degenerate (law 9) - a near coin flip with no rule behind it. RUDE picks the
		// towards-roomTo half so two exports of one level agree and the comparator can check the choice; it
		// is RUDE's convention, not the game's, and flipping the test below to `> 0.0` would emit the other.
		// Reversing an L H H L quad leaves it L H H L, so the two conventions never fight.
		FVector Nrm(0.0, 0.0, 0.0);
		for (int32 k = 0; k < 4; ++k)
		{
			const FVector& Ca = Q[k];
			const FVector& Cb = Q[(k + 1) % 4];
			Nrm.X += (Ca.Y - Cb.Y) * (Ca.Z + Cb.Z);
			Nrm.Y += (Ca.Z - Cb.Z) * (Ca.X + Cb.X);
			Nrm.Z += (Ca.X - Cb.X) * (Ca.Y + Cb.Y);
		}
		FVector FromMin, FromMax, ToMin, ToMax;
		RudeAuthBoxToRage(Rooms[P.From].LocalMin, Rooms[P.From].LocalMax, FromMin, FromMax);
		RudeAuthBoxToRage(Rooms[P.To].LocalMin, Rooms[P.To].LocalMax, ToMin, ToMax);
		const FVector Towards = ((ToMin + ToMax) * 0.5) - ((FromMin + FromMax) * 0.5);
		if (FVector::DotProduct(Nrm, Towards) < 0.0)
		{
			const FVector T0 = Q[0], T1 = Q[1];
			Q[0] = Q[3]; Q[3] = T0;
			Q[1] = Q[2]; Q[2] = T1;
		}
		for (int32 k = 0; k < 4; ++k) { P.Corners[k] = Q[k]; }
		Portals.Add(P);
	}
	for (const FRudeAuthPortal& P : Portals)
	{
		if (Rooms.IsValidIndex(P.From)) { ++Rooms[P.From].PortalCount; }
		if (Rooms.IsValidIndex(P.To)) { ++Rooms[P.To].PortalCount; }
	}

	// ---- 3) entities: any static-mesh actor of this interior, or standing inside one of its room boxes
	TArray<FRudeAuthEntity> Ents;
	int32 Unmapped = 0, NonUniformXY = 0, HiddenCount = 0, AlsoYmapEntity = 0;
	FString UnmappedNames;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A == Root) { continue; }
		if (A->IsA<ARudeMloRoomVolume>() || A->IsA<ARudeMloPortalVolume>()) { continue; }
		const UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>();
		if (!SMC || !SMC->GetStaticMesh()) { continue; }
		const FString Tagged = RudeAuthTagValue(A, TEXT("RUDE_MLO:"));
		const FVector WorldPos = A->GetActorLocation();
		bool bMine = Tagged.Equals(Name, ESearchCase::IgnoreCase);
		if (!bMine && Tagged.IsEmpty())
		{
			// untagged: it belongs if it stands inside one of THIS interior's non-limbo room boxes
			const FVector LocalPos = RootXf.InverseTransformPosition(WorldPos);
			for (int32 r = 1; r < Rooms.Num(); ++r)
			{
				if (RudeAuthAabbContains(Rooms[r].LocalMin, Rooms[r].LocalMax, LocalPos)) { bMine = true; break; }
			}
		}
		if (!bMine) { continue; }
		FRudeAuthEntity Ent;
		Ent.Actor = A;
		Ent.World = WorldPos;
		Ent.Local = A->GetActorTransform().GetRelativeTransform(RootXf);
		Ent.Archetype = RudeAuthArchetypeOf(A, Ent.How);
		Ent.bHidden = A->IsHidden() || A->IsTemporarilyHiddenInEditor();
		if (Ent.bHidden) { ++HiddenCount; }
		// an actor carrying the YMAP component is also exported by ExportLevelYmaps: the same prop would ship
		// twice, once in a ymap and once inside the interior. Counted loudly - it is right only when meant.
		if (A->FindComponentByClass<URudeMloEntityComponent>() == nullptr
			&& A->FindComponentByClass<URudeEntityComponent>() != nullptr) { ++AlsoYmapEntity; }
		if (Ent.Archetype.IsEmpty() || !RudeAuthIsCleanText(Ent.Archetype))
		{
			++Unmapped;
			if (UnmappedNames.Len() < 200) { UnmappedNames += FString::Printf(TEXT("%s%s"), UnmappedNames.IsEmpty() ? TEXT("") : TEXT(", "), *A->GetActorLabel()); }
			continue;
		}
		const FVector S = Ent.Local.GetScale3D();
		if (FMath::Abs(S.X - S.Y) > 1e-4) { ++NonUniformXY; }
		// the SMALLEST containing room wins (nested volumes are how an author says "the alcove, not the hall");
		// nothing containing it = limbo, which is exactly what room 0 is for (law 6)
		const FVector LocalPos = Ent.Local.GetLocation();
		double Best = TNumericLimits<double>::Max();
		Ent.Room = 0;
		for (int32 r = 1; r < Rooms.Num(); ++r)
		{
			if (!RudeAuthAabbContains(Rooms[r].LocalMin, Rooms[r].LocalMax, LocalPos)) { continue; }
			const FVector Size = Rooms[r].LocalMax - Rooms[r].LocalMin;
			const double Vol = Size.X * Size.Y * Size.Z;
			if (Vol < Best) { Best = Vol; Ent.Room = r; }
		}
		Ents.Add(Ent);
	}
	if (Unmapped > 0)
	{
		Refuse(FString::Printf(TEXT("%d actor(s) inside the interior have no archetype (%s) - place them with AddMloProp, or give the actor a URudeMloEntityComponent"), Unmapped, *UnmappedNames));
	}
	// deterministic ordinals: room, then archetype, then actor name (the order ExportMloYtyp appends in)
	Ents.Sort([](const FRudeAuthEntity& A, const FRudeAuthEntity& B)
	{
		if (A.Room != B.Room) { return A.Room < B.Room; }
		if (A.Archetype != B.Archetype) { return A.Archetype < B.Archetype; }
		return A.Actor->GetName() < B.Actor->GetName();
	});
	for (int32 i = 0; i < Ents.Num(); ++i) { Rooms[Ents[i].Room].Attached.Add(i); }

	// ---- 4) archetype existence in the corpus: a LOWER BOUND, reported, never fatal. A drawable can live in
	//         a ydd or a yft the ledger names differently, so "not found" does not prove "not there".
	int32 ArchInCorpus = 0, ArchNotFound = 0;
	if (!CorpusRoot.TrimStartAndEnd().IsEmpty())
	{
		FString CorpusErr;
		const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
		if (!Corpus.IsValid()) { return Fail(CorpusErr); }
		TSet<FString> Distinct;
		for (const FRudeAuthEntity& E : Ents) { Distinct.Add(E.Archetype); }
		for (const FString& Arch : Distinct)
		{
			const bool bFound = Corpus->Effective(TEXT("ydr"), Arch) != nullptr
				|| Corpus->Effective(TEXT("yft"), Arch) != nullptr
				|| Corpus->Effective(TEXT("ydd"), Arch) != nullptr;
			if (bFound) { ++ArchInCorpus; } else { ++ArchNotFound; }
		}
	}

	// ---- 4b) THE VERDICT IS DECIDED HERE, BEFORE A SINGLE BYTE IS WRITTEN -----------------------------
	// Every refusal this lane has now been evaluated: the room set (§1), the portal graph (§2) and the entity
	// resolution (§3). So `ok` is known before §6 spells the text and §8 opens a file, and §8 writes only
	// when it is true. A refused interior must not leave a broken stream/<name>.ytyp on disk beside an
	// fxmanifest.lua - that is a resource the game will try to load, and it would make the verdict the agent
	// reads disagree with what the file system actually holds. An earlier GOOD export is left alone: a
	// refusal writes nothing, it does not delete (NOTES.md 1.17).
	int32 RoomsWithoutPortals = 0;
	for (int32 r = 1; r < Rooms.Num(); ++r) { if (Rooms[r].PortalCount == 0) { ++RoomsWithoutPortals; } }
	const bool bOk = Refusals == 0 && Rooms.Num() >= 2 && Unmapped == 0 && PortalsWithoutTwoRooms == 0;

	// ---- 5) bounds. ZERO in 541/541 corpus archetypes (law 2), so nothing can be learned - RUDE writes the
	//         union of the rooms AND the props, which contains everything this interior declares.
	FVector BbMinL = Rooms[0].LocalMin, BbMaxL = Rooms[0].LocalMax;
	for (const FRudeAuthRoom& R : Rooms)
	{
		BbMinL = BbMinL.ComponentMin(R.LocalMin);
		BbMaxL = BbMaxL.ComponentMax(R.LocalMax);
	}
	for (const FRudeAuthEntity& E : Ents)
	{
		BbMinL = BbMinL.ComponentMin(E.Local.GetLocation());
		BbMaxL = BbMaxL.ComponentMax(E.Local.GetLocation());
	}
	FVector BbMin, BbMax;
	RudeAuthBoxToRage(BbMinL, BbMaxL, BbMin, BbMax);
	const FVector BsCentre = (BbMin + BbMax) * 0.5;
	const double BsRadius = (BbMax - BbMin).Size() * 0.5;

	// ---- 6) the ytyp ------------------------------------------------------------------------------
	auto Num = [](double Value) { return RudeNumText(Value); };
	auto Vec3 = [&Num](const TCHAR* Tag, const FVector& Vec, const TCHAR* Indent)
	{
		return FString::Printf(TEXT("%s<%s x=\"%s\" y=\"%s\" z=\"%s\" />\n"), Indent, Tag, *Num(Vec.X), *Num(Vec.Y), *Num(Vec.Z));
	};
	FString EntBlock;
	for (int32 i = 0; i < Ents.Num(); ++i)
	{
		const FRudeAuthEntity& E = Ents[i];
		const FVector Pr = RudeAuthToRage(E.Local.GetLocation());
		const FQuat Q = E.Local.GetRotation().GetNormalized();
		const FVector S = E.Local.GetScale3D();
		uint32 Guid = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%d:%s:%f:%f:%f"), *Name, i, *E.Archetype, Pr.X, Pr.Y, Pr.Z));
		if (Guid == 0) { Guid = 1; }   // non-zero in 67,440/67,440
		EntBlock += TEXT("    <Item type=\"CEntityDef\">\n");
		EntBlock += FString::Printf(TEXT("     <archetypeName>%s</archetypeName>\n"), *E.Archetype);
		EntBlock += TEXT("     <flags value=\"1572864\" />\n");                     // modal 22,677/67,440
		EntBlock += FString::Printf(TEXT("     <guid value=\"%u\" />\n"), Guid);
		EntBlock += FString::Printf(TEXT("     <position x=\"%s\" y=\"%s\" z=\"%s\" />\n"), *Num(Pr.X), *Num(Pr.Y), *Num(Pr.Z));
		EntBlock += FString::Printf(TEXT("     <rotation x=\"%s\" y=\"%s\" z=\"%s\" w=\"%s\" />\n"), *Num(Q.X), *Num(-Q.Y), *Num(Q.Z), *Num(Q.W));
		EntBlock += FString::Printf(TEXT("     <scaleXY value=\"%s\" />\n"), *Num(S.X));
		EntBlock += FString::Printf(TEXT("     <scaleZ value=\"%s\" />\n"), *Num(S.Z));
		EntBlock += TEXT("     <parentIndex value=\"-1\" />\n");                    // 67,440/67,440
		EntBlock += TEXT("     <lodDist value=\"-1\" />\n");                        // modal 43,428/67,440
		EntBlock += TEXT("     <childLodDist value=\"0\" />\n");                    // 67,440/67,440
		EntBlock += TEXT("     <lodLevel>LODTYPES_DEPTH_ORPHANHD</lodLevel>\n");    // 67,440/67,440
		EntBlock += TEXT("     <numChildren value=\"0\" />\n");                     // 67,440/67,440
		EntBlock += TEXT("     <priorityLevel>PRI_REQUIRED</priorityLevel>\n");     // 65,547/67,440
		EntBlock += TEXT("     <extensions />\n");                                  // 66,562/67,440
		EntBlock += TEXT("     <ambientOcclusionMultiplier value=\"255\" />\n");    // 65,286/67,440
		EntBlock += TEXT("     <artificialAmbientOcclusion value=\"255\" />\n");    // 67,222/67,440
		EntBlock += TEXT("     <tintValue value=\"0\" />\n");                       // 66,852/67,440
		EntBlock += TEXT("    </Item>\n");
	}
	FString RoomBlock;
	for (const FRudeAuthRoom& R : Rooms)
	{
		FVector RMin, RMax;
		RudeAuthBoxToRage(R.LocalMin, R.LocalMax, RMin, RMax);
		RoomBlock += TEXT("    <Item>\n");
		RoomBlock += FString::Printf(TEXT("     <name>%s</name>\n"), *R.Name);
		RoomBlock += Vec3(TEXT("bbMin"), RMin, TEXT("     "));
		RoomBlock += Vec3(TEXT("bbMax"), RMax, TEXT("     "));
		RoomBlock += FString::Printf(TEXT("     <blend value=\"%s\" />\n"), *Num(R.Blend));
		RoomBlock += R.Timecycle.IsEmpty() ? FString(TEXT("     <timecycleName />\n"))
			: FString::Printf(TEXT("     <timecycleName>%s</timecycleName>\n"), *R.Timecycle);
		RoomBlock += R.Timecycle2.IsEmpty() ? FString(TEXT("     <secondaryTimecycleName />\n"))
			: FString::Printf(TEXT("     <secondaryTimecycleName>%s</secondaryTimecycleName>\n"), *R.Timecycle2);
		RoomBlock += FString::Printf(TEXT("     <flags value=\"%d\" />\n"), R.Flags);
		RoomBlock += FString::Printf(TEXT("     <portalCount value=\"%d\" />\n"), R.PortalCount);   // DERIVED (law 4)
		RoomBlock += FString::Printf(TEXT("     <floorId value=\"%d\" />\n"), R.FloorId);
		RoomBlock += FString::Printf(TEXT("     <exteriorVisibiltyDepth value=\"%d\" />\n"), R.Depth);
		RoomBlock += RudeMloIntList(TEXT("     "), TEXT("attachedObjects"), R.Attached);
		RoomBlock += TEXT("    </Item>\n");
	}
	FString PortalBlock;
	for (const FRudeAuthPortal& P : Portals)
	{
		PortalBlock += TEXT("    <Item>\n");
		PortalBlock += FString::Printf(TEXT("     <roomFrom value=\"%d\" />\n"), P.From);
		PortalBlock += FString::Printf(TEXT("     <roomTo value=\"%d\" />\n"), P.To);
		PortalBlock += FString::Printf(TEXT("     <flags value=\"%d\" />\n"), P.Volume->Flags);
		PortalBlock += FString::Printf(TEXT("     <mirrorPriority value=\"%d\" />\n"), P.Volume->MirrorPriority);
		PortalBlock += FString::Printf(TEXT("     <opacity value=\"%d\" />\n"), P.Volume->Opacity);
		PortalBlock += FString::Printf(TEXT("     <audioOcclusion value=\"%u\" />\n"), (uint32)P.Volume->AudioOcclusion);
		PortalBlock += TEXT("     <corners>\n");
		for (int32 k = 0; k < 4; ++k)
		{
			// "x, y, z, NaN" - 13,864/13,864 corners carry the trailing NaN (law 8)
			PortalBlock += FString::Printf(TEXT("      <Item>%s, %s, %s, NaN</Item>\n"),
				*Num(P.Corners[k].X), *Num(P.Corners[k].Y), *Num(P.Corners[k].Z));
		}
		PortalBlock += TEXT("     </corners>\n");
		PortalBlock += TEXT("     <attachedObjects />\n");   // doors are v2; empty in 2,621/3,466 anyway
		PortalBlock += TEXT("    </Item>\n");
	}
	FString Ytyp;
	Ytyp += TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapTypes>\n <extensions />\n <archetypes>\n");
	Ytyp += TEXT("  <Item type=\"CMloArchetypeDef\">\n");
	Ytyp += TEXT("   <lodDist value=\"30\" />\n");            // modal 92/541; not derivable (law 1)
	Ytyp += TEXT("   <flags value=\"0\" />\n");               // 541/541
	Ytyp += TEXT("   <specialAttribute value=\"0\" />\n");    // 541/541
	Ytyp += Vec3(TEXT("bbMin"), BbMin, TEXT("   "));
	Ytyp += Vec3(TEXT("bbMax"), BbMax, TEXT("   "));
	Ytyp += Vec3(TEXT("bsCentre"), BsCentre, TEXT("   "));
	Ytyp += FString::Printf(TEXT("   <bsRadius value=\"%s\" />\n"), *Num(BsRadius));
	Ytyp += TEXT("   <hdTextureDist value=\"5\" />\n");       // 541/541
	Ytyp += FString::Printf(TEXT("   <name>%s</name>\n"), *Name);
	Ytyp += TEXT("   <textureDictionary />\n   <clipDictionary />\n   <drawableDictionary />\n");   // 541/541 each
	Ytyp += FString::Printf(TEXT("   <physicsDictionary>%s</physicsDictionary>\n"), *Name);         // ==name in 378/541
	Ytyp += TEXT("   <assetType>ASSET_TYPE_ASSETLESS</assetType>\n");                               // 541/541
	Ytyp += FString::Printf(TEXT("   <assetName>%s</assetName>\n"), *Name);                         // ==name in 541/541
	Ytyp += TEXT("   <extensions />\n");                                                            // 541/541
	Ytyp += TEXT("   <mloFlags value=\"0\" />\n");                                                  // modal 325/541
	Ytyp += EntBlock.IsEmpty() ? FString(TEXT("   <entities />\n"))
		: (TEXT("   <entities>\n") + EntBlock + TEXT("   </entities>\n"));
	Ytyp += TEXT("   <rooms itemType=\"CMloRoomDef\">\n") + RoomBlock + TEXT("   </rooms>\n");
	Ytyp += PortalBlock.IsEmpty() ? FString(TEXT("   <portals itemType=\"CMloPortalDef\" />\n"))
		: (TEXT("   <portals itemType=\"CMloPortalDef\">\n") + PortalBlock + TEXT("   </portals>\n"));
	Ytyp += TEXT("   <entitySets itemType=\"CMloEntitySet\" />\n");                     // 0 sets in 409/541
	Ytyp += TEXT("   <timeCycleModifiers itemType=\"CMloTimeCycleModifier\" />\n");     // 0 in 342/541
	Ytyp += TEXT("  </Item>\n </archetypes>\n");
	Ytyp += FString::Printf(TEXT(" <name>%s</name>\n"), *Name);   // == the file stem in 541/541
	Ytyp += TEXT(" <dependencies />\n <compositeEntityTypes itemType=\"CCompositeEntityType\" />\n</CMapTypes>\n");

	// ---- 7) the ymap: ONE CMloInstanceDef at the interior root's transform (law 10) -----------------
	const FVector RootPos = RudeAuthToRage(RootXf.GetLocation());
	const FQuat RootQ = RootXf.GetRotation().GetNormalized();
	int32 NumExitPortals = 0;
	for (const FRudeAuthPortal& P : Portals) { if (P.To == 0 || P.From == 0) { ++NumExitPortals; } }
	uint32 InstGuid = FCrc::StrCrc32(*FString::Printf(TEXT("%s:instance:%f:%f:%f"), *Name, RootPos.X, RootPos.Y, RootPos.Z));
	if (InstGuid == 0) { InstGuid = 1; }
	FString Inst;
	Inst += TEXT("  <Item type=\"CMloInstanceDef\">\n");
	Inst += FString::Printf(TEXT("   <archetypeName>%s</archetypeName>\n"), *Name);
	Inst += TEXT("   <flags value=\"1572864\" />\n");                          // 759/1,745 (1572872 is 961)
	Inst += FString::Printf(TEXT("   <guid value=\"%u\" />\n"), InstGuid);     // non-zero 1,745/1,745
	Inst += FString::Printf(TEXT("   <position x=\"%s\" y=\"%s\" z=\"%s\" />\n"), *Num(RootPos.X), *Num(RootPos.Y), *Num(RootPos.Z));
	Inst += FString::Printf(TEXT("   <rotation x=\"%s\" y=\"%s\" z=\"%s\" w=\"%s\" />\n"), *Num(RootQ.X), *Num(-RootQ.Y), *Num(RootQ.Z), *Num(RootQ.W));
	Inst += TEXT("   <scaleXY value=\"1\" />\n   <scaleZ value=\"1\" />\n");   // 1,745/1,745
	Inst += TEXT("   <parentIndex value=\"-1\" />\n");                         // no LOD parent -> ORPHANHD (764/1,745 agree)
	Inst += TEXT("   <lodDist value=\"30\" />\n");                             // modal 379/1,745
	Inst += TEXT("   <childLodDist value=\"0\" />\n");                         // 1,745/1,745
	Inst += TEXT("   <lodLevel>LODTYPES_DEPTH_ORPHANHD</lodLevel>\n");
	Inst += TEXT("   <numChildren value=\"0\" />\n");                          // 1,745/1,745
	Inst += TEXT("   <priorityLevel>PRI_REQUIRED</priorityLevel>\n");          // 1,745/1,745
	Inst += TEXT("   <extensions />\n");                                       // 1,679/1,745
	Inst += TEXT("   <ambientOcclusionMultiplier value=\"255\" />\n");         // 1,745/1,745
	Inst += TEXT("   <artificialAmbientOcclusion value=\"255\" />\n");         // 1,745/1,745
	Inst += TEXT("   <tintValue value=\"0\" />\n");                            // 1,745/1,745
	Inst += TEXT("   <groupId value=\"0\" />\n");                              // 1,162/1,745
	Inst += TEXT("   <floorId value=\"0\" />\n");                              // 1,745/1,745
	Inst += TEXT("   <defaultEntitySets />\n");                                // 1,739/1,745
	Inst += FString::Printf(TEXT("   <numExitPortals value=\"%d\" />\n"), NumExitPortals);   // = limbo's portals
	Inst += TEXT("   <MLOInstflags value=\"0\" />\n");                         // 1,509/1,745
	Inst += TEXT("  </Item>\n");

	// extents: the WORLD box of every room volume and every prop, converted whole. The streaming pad is 30 m,
	// the figure the one measured MLO ymap uses (prologue06_int: streamingExtents == position +/- 30 exactly) -
	// n=1, flagged in NOTES.md.
	FVector WMin = Rooms[0].WorldMin, WMax = Rooms[0].WorldMax;
	for (const FRudeAuthRoom& R : Rooms)
	{
		WMin = WMin.ComponentMin(R.WorldMin);
		WMax = WMax.ComponentMax(R.WorldMax);
	}
	for (const FRudeAuthEntity& E : Ents)
	{
		WMin = WMin.ComponentMin(E.World);
		WMax = WMax.ComponentMax(E.World);
	}
	FVector EntMin, EntMax;
	RudeAuthBoxToRage(WMin, WMax, EntMin, EntMax);
	const FVector StrMin = EntMin - FVector(30.0);
	const FVector StrMax = EntMax + FVector(30.0);
	FString Ymap;
	Ymap += TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapData>\n");
	Ymap += FString::Printf(TEXT(" <name>%s</name>\n <parent />\n"), *Name);   // no LOD parent: the 163/1,745 form
	Ymap += TEXT(" <flags value=\"0\" />\n");                                  // 1,506/1,745
	Ymap += TEXT(" <contentFlags value=\"9\" />\n");                           // 0x40 clear <-> empty physicsDictionaries (1,745/1,745)
	Ymap += Vec3(TEXT("streamingExtentsMin"), StrMin, TEXT(" "));
	Ymap += Vec3(TEXT("streamingExtentsMax"), StrMax, TEXT(" "));
	Ymap += Vec3(TEXT("entitiesExtentsMin"), EntMin, TEXT(" "));
	Ymap += Vec3(TEXT("entitiesExtentsMax"), EntMax, TEXT(" "));
	Ymap += TEXT(" <entities>\n") + Inst + TEXT(" </entities>\n");
	Ymap += TEXT(" <containerLods itemType=\"rage__fwContainerLodDef\" />\n <boxOccluders itemType=\"BoxOccluder\" />\n");
	Ymap += TEXT(" <occludeModels itemType=\"OccludeModel\" />\n <physicsDictionaries />\n <instancedData>\n");
	Ymap += TEXT("  <ImapLink />\n  <PropInstanceList itemType=\"rage__fwPropInstanceListDef\" />\n");
	Ymap += TEXT("  <GrassInstanceList itemType=\"rage__fwGrassInstanceListDef\" />\n </instancedData>\n");
	Ymap += TEXT(" <timeCycleModifiers itemType=\"CTimeCycleModifier\" />\n <carGenerators itemType=\"CCarGen\" />\n");
	Ymap += TEXT(" <LODLightsSOA>\n  <direction itemType=\"FloatXYZ\" />\n  <falloff />\n  <falloffExponent />\n");
	Ymap += TEXT("  <timeAndStateFlags />\n  <hash />\n  <coneInnerAngle />\n  <coneOuterAngleOrCapExt />\n");
	Ymap += TEXT("  <coronaIntensity />\n </LODLightsSOA>\n <DistantLODLightsSOA>\n");
	Ymap += TEXT("  <position itemType=\"FloatXYZ\" />\n  <RGBI />\n  <numStreetLights value=\"0\" />\n");
	Ymap += TEXT("  <category value=\"0\" />\n </DistantLODLightsSOA>\n <block>\n  <version value=\"0\" />\n");
	Ymap += FString::Printf(TEXT("  <flags value=\"0\" />\n  <name>%s</name>\n  <exportedBy>RUDE</exportedBy>\n"), *Name);
	Ymap += TEXT("  <owner></owner>\n  <time></time>\n </block>\n</CMapData>\n");

	// ---- 8) write - ONLY when §4b said the graph is valid. UTF-8 without a BOM, LF: 0/2,765 corpus ytyps
	//         and 0/19,387 ymaps carry a BOM at all, and every file carrying an MLO is LF - 0/424 ytyps and
	//         0/1,745 ymaps are CRLF (the 6 CRLF ytyps and 4 CRLF ymaps in the corpus carry none). Law 12,
	//         measured by this lane's own encoding pass.
	const FString StreamDir = OutDir / TEXT("stream");
	const FString YtypPath = StreamDir / (Name + TEXT(".ytyp"));
	const FString YmapPath = StreamDir / (Name + TEXT(".ymap"));
	bool bWritten = false;
	if (bOk)
	{
		IFileManager::Get().MakeDirectory(*StreamDir, true);
		if (!FFileHelper::SaveStringToFile(Ytyp, *YtypPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return Fail(TEXT("cannot write ") + YtypPath); }
		if (!FFileHelper::SaveStringToFile(Ymap, *YmapPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			// never leave HALF a resource behind: the ytyp already landed, and a ytyp without its ymap is an
			// interior the game defines and never places - the failure mode this tool exists to prevent.
			IFileManager::Get().Delete(*YtypPath, false, true, true);
			return Fail(TEXT("cannot write ") + YmapPath + TEXT(" - the partly written resource was removed"));
		}
		FFileHelper::SaveStringToFile(
			TEXT("fx_version 'cerulean'\ngame 'gta5'\n\nauthor 'RUDE - RAGE <-> Unreal Development Environment'\n")
			TEXT("description 'RUDE-authored interior'\n\n-- Required for streamed ymaps to take effect (reloads map storage on load).\n")
			TEXT("this_is_a_map 'yes'\n"),
			*(OutDir / TEXT("fxmanifest.lua")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		bWritten = true;
	}

	// ok is COMPUTED (§4b): an invalid graph is a refusal, never a warning next to ok:true - and a refusal
	// writes nothing. `ytyp` / `ymap` are the paths the interior WOULD occupy; `written` says whether this
	// call touched them.
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"written\":%s,\"interior\":\"%s\",\"ytyp\":\"%s\",\"ymap\":\"%s\",\"rooms\":%d,\"portals\":%d,\"entities\":%d,"
		"\"entitiesUnmapped\":%d,\"portalsWithoutTwoRooms\":%d,\"portalsAmbiguousPlane\":%d,"
		"\"bounds\":[%s,%s,%s,%s,%s,%s],\"bsRadius\":%s,\"limboSynthesized\":%s,\"limboFieldsForced\":%d,"
		"\"limboAttached\":%d,\"attachedTotal\":%d,\"numExitPortals\":%d,\"roomsWithoutPortals\":%d,"
		"\"adoptedUntagged\":%d,\"nonUniformScaleXY\":%d,\"hiddenIncluded\":%d,\"alsoYmapEntity\":%d,"
		"\"orphanVolumes\":%d,\"archetypesInCorpus\":%d,\"archetypesNotInCorpus\":%d,"
		"\"refused\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), bWritten ? TEXT("true") : TEXT("false"),
		*RudeJsonEscape(Name), *RudeJsonEscape(YtypPath), *RudeJsonEscape(YmapPath),
		Rooms.Num(), Portals.Num(), Ents.Num(), Unmapped, PortalsWithoutTwoRooms, PortalsAmbiguousPlane,
		*Num(BbMin.X), *Num(BbMin.Y), *Num(BbMin.Z), *Num(BbMax.X), *Num(BbMax.Y), *Num(BbMax.Z), *Num(BsRadius),
		bLimboSynth ? TEXT("true") : TEXT("false"), LimboFieldsForced, Rooms[0].Attached.Num(), Ents.Num(), NumExitPortals,
		RoomsWithoutPortals, Adopted, NonUniformXY, HiddenCount, AlsoYmapEntity, OrphanVolumes, ArchInCorpus, ArchNotFound, *Refused);
}
