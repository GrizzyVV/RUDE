// RUDE - RAGE <-> Unreal Development Environment
// THE SCENARIO LANE, READ HALF. A CScenarioPointRegion (.ymt) - GTA's ambient-life data:
// where a ped smokes, where a coyote wanders, which routes get walked and driven - becomes
// MOVABLE actors in the level the operator already has open.
//
// Why this file exists at all (UX_AND_FEATURE_NOTES section 8/8b, Matt's operator knowledge):
// scenario points are SPATIAL data that FiveM authors today by typing coordinates, and a bad
// coordinate does not fail at load - it time-bombs until the region streams in. Authoring them
// in context, on the imported city, is the workflow RUDE uniquely has; seeing them is step one.
//
// Formats: QUARRY's meta2xml emits <CScenarioPointRegion> XML (LOG "SCENARIO REGION EMITTER":
// 204/204 binaries convert, 5,534,470 leaf comparisons, FMT_BUG = 0). Nothing in RUDE consumed
// it until now.
#include "RudeToolset.h"

#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "XmlFile.h"

namespace RudeScenario
{
	// Marker sizes in UE cm-scale multipliers of the 100-unit engine primitives. 🧠 AGENT CALL,
	// UNCALIBRATED - the same status as ImportMlo's candela scale: nothing measurable pins how
	// big an authoring gizmo should be, so it is ONE named constant for Matt's eyes to tune,
	// never a number sprinkled through the spawn loop.
	static const float PointMarkerScale = 0.5f;    // 50 cm cone - a ped-ish footprint at map scale
	static const float NodeMarkerScale = 0.25f;    // 25 cm sphere - graph anchors read as smaller

	// "<Tag value="N" />" is the emitter's rendering for EVERY scalar field in this schema
	// (SCEN_POINT_INTS, edge indices, the AccelGrid cells). Default is only reached when the
	// tag is absent, which the emitter never does - it has no silent defaults (LOG) - so a
	// default that shows up in the verdict means corrupt input, not a normal case.
	static double Val(const FXmlNode* Parent, const TCHAR* Tag, double Default)
	{
		const FXmlNode* N = Parent ? Parent->FindChildNode(Tag) : nullptr;
		return N ? FCString::Atod(*N->GetAttribute(TEXT("value"))) : Default;
	}

	static int32 IVal(const FXmlNode* Parent, const TCHAR* Tag, int32 Default)
	{
		return (int32)Val(Parent, Tag, (double)Default);
	}

	// Booleans render as value="true"/"false" (lowercase, LOG "EXTENSIONS DECODED"), so an
	// Atoi-style read would silently yield 0 for every true.
	static bool BoolVal(const FXmlNode* Parent, const TCHAR* Tag)
	{
		const FXmlNode* N = Parent ? Parent->FindChildNode(Tag) : nullptr;
		return N && N->GetAttribute(TEXT("value")).Equals(TEXT("true"), ESearchCase::IgnoreCase);
	}

	// Element TEXT, not an attribute: ScenarioType / Flags / EntityType / the LookUps <Item>s.
	static FString Txt(const FXmlNode* Parent, const TCHAR* Tag)
	{
		const FXmlNode* N = Parent ? Parent->FindChildNode(Tag) : nullptr;
		return N ? N->GetContent().TrimStartAndEnd() : FString();
	}

	// THE IMPORT-LANE TRANSFORM, reused not reinvented: RUDE's pinned RAGE->UE convention is
	// metres->cm and a Y mirror, exactly as ImportMapArea's manifest and ImportMlo's entity
	// placement do it (RudeToolset.cpp: FVector(Px * 100.0, -Py * 100.0, Pz * 100.0)). A
	// scenario point sitting one convention off from the buildings it belongs to is worthless.
	static FVector RageToUe(const FXmlNode* N, const TCHAR* XAttr, const TCHAR* YAttr, const TCHAR* ZAttr)
	{
		return FVector(FCString::Atod(*N->GetAttribute(XAttr)) * 100.0,
		               -FCString::Atod(*N->GetAttribute(YAttr)) * 100.0,
		               FCString::Atod(*N->GetAttribute(ZAttr)) * 100.0);
	}

	// ⭐ HEADING CONVENTION - DERIVED BY MEASUREMENT (agent, 2026-07-28), not assumed and not
	// read off any third-party tool (clean-room). vPositionAndDirection.w is a heading in
	// RADIANS measured from RAGE +Y, counter-clockwise: forward_rage = (-sin w, cos w, 0).
	// HOW IT WAS PROVEN, from the user's own data: a chained scenario point should face the
	// next node in its chain. Over all 204 regions, 58,082 chain edges whose FROM node has a
	// coincident point were tested against four candidate conventions; only
	// w == atan2(-dx, dy) correlates - median angular error 0.198 rad (11 deg), 65.1% of points
	// within 0.35 rad of their successor. The other three candidates all sit at a median of
	// ~1.571 rad (90 deg) = pure noise. Measured range of w corpus-wide: [-3.141451, 3.141593].
	// Under the Y mirror a rotation about Z negates, and RAGE-from-+Y becomes UE-from-+X by
	// adding 90 deg, so: UE yaw = -(w_deg + 90). Sanity-checked algebraically - the resulting
	// UE forward (-sin w, -cos w) equals the mirrored successor delta (dx, -dy) exactly.
	static float HeadingToUeYawDegrees(double W)
	{
		return (float)(-(FMath::RadiansToDegrees(W) + 90.0));
	}

	// The verdict is hand-built JSON like every other RUDE tool, so anything corpus- or
	// path-derived must be escaped: Windows paths carry backslashes and a raw one produces a
	// verdict that silently fails to parse for the caller.
	static FString Esc(const FString& S)
	{
		return S.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
	}

	static FString JsonStrArray(const TArray<FString>& Items)
	{
		FString Out;
		for (int32 i = 0; i < Items.Num(); ++i)
		{
			Out += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *Esc(Items[i]));
		}
		return Out;
	}

	// Read a LookUps child list (<TypeNames><Item>name</Item>...</TypeNames>). Empty lists are
	// self-closing WITHOUT an itemType in this schema (measured, meta2xml scenario_xml), so an
	// absent child list is normal and yields an empty array - not an error.
	static void ReadNameList(const FXmlNode* LookUps, const TCHAR* Tag, TArray<FString>& Out)
	{
		const FXmlNode* N = LookUps ? LookUps->FindChildNode(Tag) : nullptr;
		if (!N) { return; }
		for (const FXmlNode* I : N->GetChildrenNodes())
		{
			Out.Add(I->GetContent().TrimStartAndEnd());
		}
	}

	// One parsed CScenarioPoint. The eleven ints are carried VERBATIM in emitter field order
	// because they become the actor's round-trip tag (see the RUDE_SCEN_Rec note below).
	struct FScenPoint
	{
		FVector UeLoc = FVector::ZeroVector;
		float Yaw = 0.f;
		FString TypeName;
		FString ModelSetName;
		FString Flags;
		int32 Rec[11] = { 0 };      // iType, ModelSetId, iInterior, iRequiredIMapId, iProbability,
		                            // uAvailableInMpSp, iTimeStartOverride, iTimeEndOverride,
		                            // iRadius, iTimeTillPedLeaves, iScenarioGroup
		// ⛔ A malformed record keeps its SLOT and is marked invalid instead of being dropped:
		// the point's array index IS its identity (the RUDE_SCEN_Point:<i> tag promises the
		// MyPoints index, and an exporter will address it that way). Compacting the array
		// renumbers everything after it - a corruption that no in-range check can catch,
		// because the shifted indices are all still in range. (Review finding, 2026-07-28.)
		bool bValid = true;
	};

	struct FScenNode
	{
		FVector UeLoc = FVector::ZeroVector;
		FString TypeName;
		FString AttachProp;         // hash_9B1D60AB: empty for hash 0, else an attachment prop name
		bool bIn = false;
		bool bOut = false;
		// ⛔ Same slot-preserving rule as FScenPoint, and it matters MORE here: edges address
		// nodes BY INDEX, so dropping one silently repoints every edge that followed it.
		bool bValid = true;
	};

	struct FScenEdge
	{
		int32 From = -1;
		int32 To = -1;
		int32 Action = 0;
		int32 NavMode = 0;
		int32 NavSpeed = 0;
		int32 Chain = -1;           // owning chain index; -1 = referenced by no chain
	};

	// Region-level CExtensionDefSpawnPoint (the <LoadSavePoints> container). ⚠ MEASURED, and
	// counter-intuitive: at REGION level `offsetPosition` holds ABSOLUTE world coordinates, not
	// an offset - the only two in the whole game (nw_countryside) land on real map features
	// (-2566.87, 2313.82, 32.22 in the NW countryside; 450.54, 5572.13, 795.52 = the Mount
	// Chiliad summit). Inside an EntityOverride the same struct IS prop-relative, which is why
	// v1 spawns these two and summarises those.
	struct FScenLoadSave
	{
		FVector UeLoc = FVector::ZeroVector;
		FQuat Rot = FQuat::Identity;
		FString SpawnType;
	};

	// A filter token: exact type name, or a trailing '*' prefix. Type vocabulary is per-region
	// (each file carries its own LookUps/TypeNames), so "world_vehicle*" beats making a human
	// type the 40 vehicle spellings his region happens to use.
	struct FScenTypeFilter
	{
		TArray<FString> Exact;
		TArray<FString> Prefix;

		bool IsEmpty() const { return Exact.Num() == 0 && Prefix.Num() == 0; }

		bool Passes(const FString& TypeLower) const
		{
			if (IsEmpty()) { return true; }
			for (const FString& E : Exact) { if (TypeLower == E) { return true; } }
			for (const FString& P : Prefix) { if (TypeLower.StartsWith(P, ESearchCase::CaseSensitive)) { return true; } }
			return false;
		}
	};

	// Build one marker actor: scene root + a single unlit-weight primitive. MOVABLE on purpose -
	// these exist to be dragged, and Static mobility would advertise them to the lighting build
	// as world geometry they are not. Collision off so they never block a click or a trace on the
	// real map underneath; shadows off so a field of 3,000 markers cannot darken the scene.
	static AActor* SpawnMarker(UWorld* World, UStaticMesh* Mesh, const FString& Label,
	                           const FVector& Loc, const FQuat& Rot, float Scale,
	                           bool bFaceAlongX, AActor* AttachTo)
	{
		AActor* A = World->SpawnActor<AActor>();
		if (!A) { return nullptr; }
		USceneComponent* R = NewObject<USceneComponent>(A, TEXT("Root"));
		A->SetRootComponent(R);
		R->SetMobility(EComponentMobility::Movable);
		R->RegisterComponent();
		A->AddInstanceComponent(R);

		UStaticMeshComponent* SM = NewObject<UStaticMeshComponent>(A, TEXT("Marker"));
		SM->SetStaticMesh(Mesh);
		SM->SetMobility(EComponentMobility::Movable);
		SM->SetRelativeScale3D(FVector(Scale));
		if (bFaceAlongX)
		{
			// The engine cone points +Z; pitching it -90 turns its axis into the actor's +X, so
			// the actor's yaw IS the ped's facing and a human can read heading straight off the
			// viewport instead of trusting a number.
			SM->SetRelativeRotation(FRotator(-90.f, 0.f, 0.f));
		}
		SM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SM->SetCastShadow(false);
		SM->SetupAttachment(R);
		SM->RegisterComponent();
		A->AddInstanceComponent(SM);

		A->SetActorLocationAndRotation(Loc, Rot);
		A->SetActorLabel(Label);
		A->SetFolderPath(FName(TEXT("RUDE_SCENARIOS")));
		if (AttachTo) { A->AttachToActor(AttachTo, FAttachmentTransformRules::KeepWorldTransform); }
		return A;
	}
}

FString URudeToolset::ImportScenarioRegion(const FString& CorpusRoot, const FString& RegionName,
                                           const FString& Filter)
{
	using namespace RudeScenario;

	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Esc(Why));
	};

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }

	// ---- 1) LOCATE THE REGION FILE -------------------------------------------------------
	// WHERE the emitter's output lands: meta2xml.convert() returns kind "ymt" and quarry's meta
	// pass writes "<stem>.<kind>.xml" beside the binary, which `resolve` then flattens into
	// `_resolved/<kind>/`. So the corpus convention is <CorpusRoot>/ymt/<region>.ymt.xml -
	// character-for-character the ytyp/ymap shape ImportMapArea already globs.
	// ⚠ MEASURED 2026-07-28, and the reason for the tolerance below: quarry.py's cmd_meta still
	// loops `for kind in ('ytyp','ymap')` and no extraction has ever passed --types ymt, so a
	// filebase built today has NO ymt folder at all (LOG "SCENARIO DATA RECON" filebase note) -
	// the emitter is wired, the PIPELINE STAGE is not. Two tolerances keep the tool drivable
	// across that gap and cost nothing once it closes: CorpusRoot may point straight AT a folder
	// of *.ymt.xml, and RegionName may be a path to one file.
	FString Dir = CorpusRoot / TEXT("ymt");
	if (!FPaths::DirectoryExists(Dir)) { Dir = CorpusRoot; }
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(Dir / TEXT("*.ymt.xml")), true, false);
	Found.Sort();

	auto StemOf = [](const FString& FileName) -> FString
	{
		FString S = FPaths::GetCleanFilename(FileName);
		if (S.EndsWith(TEXT(".ymt.xml"), ESearchCase::IgnoreCase)) { S = S.LeftChop(8); }
		return S;
	};

	const FString Region = RegionName.TrimStartAndEnd();
	if (Region.IsEmpty())
	{
		// No name given: answer with the menu instead of an error - the ImportArea discovery
		// path ("what can I type here?"), same shape, so the panel behaves consistently.
		TArray<FString> Stems;
		for (const FString& F : Found) { Stems.Add(StemOf(F)); }
		return FString::Printf(TEXT("{\"ok\":true,\"folder\":\"%s\",\"regions\":[%s]}"),
			*Esc(Dir), *JsonStrArray(Stems));
	}

	FString RegionFile;
	if (Region.EndsWith(TEXT(".xml"), ESearchCase::IgnoreCase) ||
	    Region.Contains(TEXT("/")) || Region.Contains(TEXT("\\")))
	{
		RegionFile = Region;
	}
	else
	{
		for (const FString& F : Found)
		{
			if (StemOf(F).Equals(Region, ESearchCase::IgnoreCase))
			{
				RegionFile = Dir / F;
				break;
			}
		}
	}
	if (RegionFile.IsEmpty() || !FPaths::FileExists(RegionFile))
	{
		TArray<FString> Sample;
		for (const FString& F : Found)
		{
			if (Sample.Num() >= 20) { break; }
			Sample.Add(StemOf(F));
		}
		return Fail(FString::Printf(
			TEXT("scenario region '%s' not found among %d *.ymt.xml under %s%s - run with an empty RegionName to list them. Leading names: %s"),
			*Region, Found.Num(), *Dir,
			Found.Num() == 0 ? TEXT(" (the corpus has no scenario XML yet: quarry's meta pass does not convert ymt)") : TEXT(""),
			*FString::Join(Sample, TEXT(", "))));
	}
	const FString RegionStem = StemOf(RegionFile);

	// ---- 2) PARSE ------------------------------------------------------------------------
	FXmlFile Xml(RegionFile);
	if (!Xml.IsValid())
	{
		return Fail(FString::Printf(TEXT("XML load failed (%s): %s"),
			*FPaths::GetCleanFilename(RegionFile), *Xml.GetLastError()));
	}
	const FXmlNode* Root = Xml.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("CScenarioPointRegion"))
	{
		// The emitter derives kind from the ROOT STRUCT for exactly this reason, so a
		// mislabelled file cannot be read as the wrong schema. Refuse rather than half-parse.
		return Fail(FString::Printf(TEXT("root element is <%s>, not <CScenarioPointRegion> - %s is not a scenario region"),
			Root ? *Root->GetTag() : TEXT("(none)"), *FPaths::GetCleanFilename(RegionFile)));
	}

	const FXmlNode* LookUps = Root->FindChildNode(TEXT("LookUps"));
	TArray<FString> TypeNames, PedSetNames, VehSetNames;
	ReadNameList(LookUps, TEXT("TypeNames"), TypeNames);
	ReadNameList(LookUps, TEXT("PedModelSetNames"), PedSetNames);
	ReadNameList(LookUps, TEXT("VehicleModelSetNames"), VehSetNames);

	// unknownTypes = everything this importer could not put a NAME to. Two honest sources, both
	// reported the same way because they mean the same thing to a human: an iType index outside
	// the file's own TypeNames (0 occurrences in 159,449 measured points - so a non-zero count
	// here means corrupt or hand-edited input), and a TypeNames entry the converter could only
	// spell as hash_XXXXXXXX because the joaat preimage is not in the user's own data.
	TSet<FString> UnknownTypes;
	int32 ModelSetOutOfRange = 0;

	auto ResolveType = [&](int32 Idx) -> FString
	{
		if (!TypeNames.IsValidIndex(Idx))
		{
			UnknownTypes.Add(FString::Printf(TEXT("iType=%d(TypeNames has %d)"), Idx, TypeNames.Num()));
			return FString::Printf(TEXT("type%d"), Idx);
		}
		if (TypeNames[Idx].StartsWith(TEXT("hash_"))) { UnknownTypes.Add(TypeNames[Idx]); }
		return TypeNames[Idx];
	};

	// 🧠 MEASURED-CONSISTENT, SEMANTICS INFERRED: a point's ModelSetId indexes
	// VehicleModelSetNames for vehicle scenario types and PedModelSetNames for everything else.
	// Evidence over 204 regions / 159,449 points: 372 vehicle-typed points carry an id valid
	// ONLY in the vehicle list, and 24,893 ped-typed points carry one valid ONLY in the ped
	// list - the two lists are demonstrably addressed by two different populations - and under
	// this rule EVERY point resolves (0 out of range). What stays inferred is the discriminator
	// itself (the type-name prefix), which is why a miss is COUNTED and the name simply omitted,
	// never quietly resolved out of the other list.
	auto ResolveModelSet = [&](const FString& TypeName, int32 ModelSetId) -> FString
	{
		const bool bVehicle = TypeName.StartsWith(TEXT("world_vehicle"), ESearchCase::IgnoreCase) ||
		                      TypeName.Equals(TEXT("drive"), ESearchCase::IgnoreCase);
		const TArray<FString>& List = bVehicle ? VehSetNames : PedSetNames;
		if (!List.IsValidIndex(ModelSetId))
		{
			++ModelSetOutOfRange;
			return FString();
		}
		return List[ModelSetId];
	};

	// Points: the TOP-LEVEL container only. FindChildNode is non-recursive by design here -
	// <MyPoints> also appears inside every <Clusters> item, and a recursive search would silently
	// merge clustered points into the flat set (v1 summarises clusters, it does not spawn them).
	const FXmlNode* PointsN = Root->FindChildNode(TEXT("Points"));
	const FXmlNode* MyPointsN = PointsN ? PointsN->FindChildNode(TEXT("MyPoints")) : nullptr;
	TArray<FScenPoint> Points;
	int32 PointsMissingTransform = 0;
	int32 NodesMissingTransform = 0;
	if (MyPointsN)
	{
		static const TCHAR* RecTags[11] = {
			TEXT("iType"), TEXT("ModelSetId"), TEXT("iInterior"), TEXT("iRequiredIMapId"),
			TEXT("iProbability"), TEXT("uAvailableInMpSp"), TEXT("iTimeStartOverride"),
			TEXT("iTimeEndOverride"), TEXT("iRadius"), TEXT("iTimeTillPedLeaves"),
			TEXT("iScenarioGroup") };
		for (const FXmlNode* It : MyPointsN->GetChildrenNodes())
		{
			const FXmlNode* V = It->FindChildNode(TEXT("vPositionAndDirection"));
			FScenPoint P;
			if (!V)
			{
				// The emitter require()s this field, so its absence is corrupt input: count it
				// loudly and do not SPAWN it (never invent an origin coordinate - that is the
				// "quiet wrong coordinate" class that ships a latent crash on a live server,
				// UX section 8b) - but KEEP ITS SLOT so later points keep their true indices.
				++PointsMissingTransform;
				P.bValid = false;
				Points.Add(MoveTemp(P));
				continue;
			}
			P.UeLoc = RageToUe(V, TEXT("x"), TEXT("y"), TEXT("z"));
			P.Yaw = HeadingToUeYawDegrees(FCString::Atod(*V->GetAttribute(TEXT("w"))));
			for (int32 i = 0; i < 11; ++i) { P.Rec[i] = IVal(It, RecTags[i], 0); }
			P.TypeName = ResolveType(P.Rec[0]);
			P.ModelSetName = ResolveModelSet(P.TypeName, P.Rec[1]);
			P.Flags = Txt(It, TEXT("Flags"));
			Points.Add(MoveTemp(P));
		}
	}

	// Region-level spawn points (see FScenLoadSave: absolute coordinates, measured).
	const FXmlNode* LoadSaveN = PointsN ? PointsN->FindChildNode(TEXT("LoadSavePoints")) : nullptr;
	TArray<FScenLoadSave> LoadSaves;
	if (LoadSaveN)
	{
		for (const FXmlNode* It : LoadSaveN->GetChildrenNodes())
		{
			const FXmlNode* O = It->FindChildNode(TEXT("offsetPosition"));
			if (!O) { continue; }
			FScenLoadSave L;
			L.UeLoc = RageToUe(O, TEXT("x"), TEXT("y"), TEXT("z"));
			if (const FXmlNode* R = It->FindChildNode(TEXT("offsetRotation")))
			{
				// Same quaternion mirror the import lane uses everywhere: (x, -y, z, w).
				L.Rot = FQuat(FCString::Atod(*R->GetAttribute(TEXT("x"))),
				              -FCString::Atod(*R->GetAttribute(TEXT("y"))),
				              FCString::Atod(*R->GetAttribute(TEXT("z"))),
				              FCString::Atod(*R->GetAttribute(TEXT("w"))));
				L.Rot.Normalize();
			}
			L.SpawnType = Txt(It, TEXT("spawnType"));
			LoadSaves.Add(MoveTemp(L));
		}
	}

	// Chaining graph. Node ScenarioType is a real NAME string here, not a LookUps index
	// (measured: facility_main nodes read <ScenarioType>world_human_clipboard_facility</...>),
	// so the type filter compares against it directly.
	const FXmlNode* CG = Root->FindChildNode(TEXT("ChainingGraph"));
	TArray<FScenNode> Nodes;
	TArray<FScenEdge> Edges;
	int32 NumChains = 0, BadEdgeRefs = 0, BadChainEdgeIds = 0, EdgesWithoutChain = 0;
	if (CG)
	{
		if (const FXmlNode* NodesN = CG->FindChildNode(TEXT("Nodes")))
		{
			for (const FXmlNode* It : NodesN->GetChildrenNodes())
			{
				const FXmlNode* P = It->FindChildNode(TEXT("Position"));
				FScenNode N;
				if (!P)
				{
					// Slot preserved deliberately - see FScenNode::bValid. Dropping a node here
					// would renumber every later node and silently repoint the edges that
					// reference them, and badEdgeRefs could never see it.
					++NodesMissingTransform;
					N.bValid = false;
					Nodes.Add(MoveTemp(N));
					continue;
				}
				N.UeLoc = RageToUe(P, TEXT("x"), TEXT("y"), TEXT("z"));
				N.TypeName = Txt(It, TEXT("ScenarioType"));
				N.AttachProp = Txt(It, TEXT("hash_9B1D60AB"));
				// Booleans spell lowercase true/false in this emitter (LOG "EXTENSIONS
				// DECODED": bool is an int subclass, so the bool branch precedes int and
				// str() never ships "True") - compare the string, do not Atoi it.
				N.bIn = BoolVal(It, TEXT("HasIncomingEdges"));
				N.bOut = BoolVal(It, TEXT("HasOutgoingEdges"));
				if (N.TypeName.StartsWith(TEXT("hash_"))) { UnknownTypes.Add(N.TypeName); }
				Nodes.Add(MoveTemp(N));
			}
		}
		if (const FXmlNode* EdgesN = CG->FindChildNode(TEXT("Edges")))
		{
			for (const FXmlNode* It : EdgesN->GetChildrenNodes())
			{
				FScenEdge E;
				E.From = IVal(It, TEXT("NodeIndexFrom"), -1);
				E.To = IVal(It, TEXT("NodeIndexTo"), -1);
				E.Action = IVal(It, TEXT("Action"), 0);
				E.NavMode = IVal(It, TEXT("NavMode"), 0);
				E.NavSpeed = IVal(It, TEXT("NavSpeed"), 0);
				if (!Nodes.IsValidIndex(E.From) || !Nodes.IsValidIndex(E.To)) { ++BadEdgeRefs; }
				Edges.Add(E);
			}
		}
		// Chains own edges: measured across all 204 regions, chain EdgeIds PARTITION the edge
		// set exactly (58,312 references over 58,312 edges, zero duplicates, zero out of range).
		// The chain index is therefore a clean colour key for the drawn routes.
		if (const FXmlNode* ChainsN = CG->FindChildNode(TEXT("Chains")))
		{
			for (const FXmlNode* It : ChainsN->GetChildrenNodes())
			{
				const int32 ChainIdx = NumChains++;
				const FXmlNode* IdsN = It->FindChildNode(TEXT("EdgeIds"));
				if (!IdsN) { continue; }
				// EdgeIds renders in the scalar_list law (<=10 inline, then ten per line), so the
				// whole content is whitespace-parsed - NEVER split on newlines.
				TArray<FString> Toks;
				IdsN->GetContent().ParseIntoArrayWS(Toks);
				for (const FString& S : Toks)
				{
					const int32 EIdx = FCString::Atoi(*S);
					if (Edges.IsValidIndex(EIdx)) { Edges[EIdx].Chain = ChainIdx; }
					else { ++BadChainEdgeIds; }
				}
			}
		}
	}
	for (const FScenEdge& E : Edges) { if (E.Chain < 0) { ++EdgesWithoutChain; } }

	// Clusters + entity overrides: SUMMARISED, not spawned (v1 scope). A cluster is a sphere of
	// points the game treats as one spawn group, and an override re-points scenarios at a
	// specific prop instance - both need their own authoring affordance, and spawning their
	// points into the flat set would misrepresent what the region actually contains.
	int32 NumClusters = 0, ClusterPoints = 0, NumOverrides = 0, OverridePoints = 0;
	if (const FXmlNode* ClustersN = Root->FindChildNode(TEXT("Clusters")))
	{
		for (const FXmlNode* It : ClustersN->GetChildrenNodes())
		{
			++NumClusters;
			const FXmlNode* CP = It->FindChildNode(TEXT("Points"));
			const FXmlNode* CMy = CP ? CP->FindChildNode(TEXT("MyPoints")) : nullptr;
			if (CMy) { ClusterPoints += CMy->GetChildrenNodes().Num(); }
		}
	}
	if (const FXmlNode* OverridesN = Root->FindChildNode(TEXT("EntityOverrides")))
	{
		for (const FXmlNode* It : OverridesN->GetChildrenNodes())
		{
			++NumOverrides;
			const FXmlNode* SP = It->FindChildNode(TEXT("ScenarioPoints"));
			if (SP) { OverridePoints += SP->GetChildrenNodes().Num(); }
		}
	}

	// ---- 3) FILTER, VALIDATED BEFORE ANYTHING IS DESTROYED --------------------------------
	// Order matters: a typo'd filter must NOT clear the operator's previous import and then
	// fail. Everything below this point is already parsed, so validation is free here and
	// catastrophic later.
	FScenTypeFilter TypeFilter;
	{
		const FString F = Filter.TrimStartAndEnd();
		if (!F.IsEmpty() && !F.Equals(TEXT("ALL"), ESearchCase::IgnoreCase))
		{
			// The region's OWN type spellings - every place a type name can appear, so a filter
			// that is valid for the data cannot be rejected: point types come from LookUps,
			// chaining nodes and region-level spawn points carry theirs as literal strings.
			TSet<FString> Vocabulary;
			for (const FString& T : TypeNames) { Vocabulary.Add(T.ToLower()); }
			for (const FScenNode& N : Nodes) { Vocabulary.Add(N.TypeName.ToLower()); }
			for (const FScenLoadSave& L : LoadSaves) { Vocabulary.Add(L.SpawnType.ToLower()); }

			TArray<FString> Toks;
			F.ParseIntoArray(Toks, TEXT(","), true);
			for (FString T : Toks)
			{
				T.TrimStartAndEndInline();
				if (T.IsEmpty()) { continue; }
				const bool bPrefix = T.EndsWith(TEXT("*"));
				const FString Key = (bPrefix ? T.LeftChop(1) : T).ToLower();
				bool bMatches = false;
				for (const FString& V : Vocabulary)
				{
					if (bPrefix ? V.StartsWith(Key, ESearchCase::CaseSensitive) : V == Key)
					{
						bMatches = true;
						break;
					}
				}
				if (!bMatches)
				{
					TArray<FString> Sample;
					for (const FString& T2 : TypeNames)
					{
						if (Sample.Num() >= 40) { break; }
						Sample.Add(T2);
					}
					return Fail(FString::Printf(
						TEXT("filter '%s' matches no scenario type in region %s - types here (%d): %s"),
						*T, *RegionStem, TypeNames.Num(), *FString::Join(Sample, TEXT(", "))));
				}
				if (bPrefix) { TypeFilter.Prefix.Add(Key); } else { TypeFilter.Exact.Add(Key); }
			}
		}
	}

	// The two engine primitives this tool draws with. Loud failure, never a silent skip: a
	// scenario import that spawned nothing because a mesh would not load must not read as
	// "this region is empty".
	UStaticMesh* ConeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!ConeMesh || !SphereMesh)
	{
		return Fail(TEXT("engine marker meshes missing (/Engine/BasicShapes/Cone|Sphere) - cannot draw scenario points"));
	}

	// ---- 4) CLEAR-BY-TAG, THEN SPAWN ------------------------------------------------------
	// The idempotency pattern is ImportMlo's, unchanged and for its reasons: OFPA can rewrite
	// folder paths, so clearing a FOLDER is unreliable and would also kill other imported
	// regions; every actor of this region carries IdTag, so root, points, nodes and the chain
	// display all die here even though DestroyActor does not cascade to attached children.
	const FName IdTag(*(TEXT("RUDE_SCEN:") + RegionStem));
	int32 Cleared = 0;
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(IdTag)) { Stale.Add(*It); }
		}
		for (AActor* A : Stale) { World->DestroyActor(A); ++Cleared; }
	}

	AActor* RootActor = World->SpawnActor<AActor>();
	if (!RootActor) { return Fail(TEXT("root actor spawn failed")); }
	{
		USceneComponent* RootComp = NewObject<USceneComponent>(RootActor, TEXT("Root"));
		RootActor->SetRootComponent(RootComp);
		RootComp->SetMobility(EComponentMobility::Movable);
		RootComp->RegisterComponent();
		RootActor->AddInstanceComponent(RootComp);
		RootActor->SetActorLabel(TEXT("SCEN_") + RegionStem);
		RootActor->SetFolderPath(FName(TEXT("RUDE_SCENARIOS")));
		RootActor->Tags.Add(IdTag);
		RootActor->Tags.Add(FName(TEXT("RUDE_SCEN_ROOT")));
	}

	// Points. One actor each, because the entire point of this lane is that a human can select
	// ONE and drag it (UX section 8: authoring ambient life in context). Measured worst case in
	// the shipped game is 3,254 points in a single region (chumash_hills), so this stays inside
	// what the editor handles - but on a World Partition level it is also 3,254 external actor
	// packages on save, which is a real cost the operator should know about, not a surprise.
	int32 Spawned = 0, Skipped = 0;
	for (int32 i = 0; i < Points.Num(); ++i)
	{
		const FScenPoint& P = Points[i];
		// an invalid slot exists only to hold its index - never spawn it
		if (!P.bValid) { ++Skipped; continue; }
		if (!TypeFilter.Passes(P.TypeName.ToLower())) { ++Skipped; continue; }
		AActor* A = SpawnMarker(World, ConeMesh,
			FString::Printf(TEXT("SP_%s_%d"), *P.TypeName, i),
			P.UeLoc, FRotator(0.f, P.Yaw, 0.f).Quaternion(), PointMarkerScale, true, RootActor);
		if (!A) { continue; }
		A->Tags.Add(IdTag);
		// Identity for a future exporter: the INDEX into this region's MyPoints array is the
		// only stable handle the format itself provides (points have no name or guid).
		A->Tags.Add(FName(*FString::Printf(TEXT("RUDE_SCEN_Point:%d"), i)));
		A->Tags.Add(FName(*(TEXT("RUDE_SCEN_Type:") + P.TypeName)));
		if (!P.ModelSetName.IsEmpty() && P.ModelSetName != TEXT("none"))
		{
			A->Tags.Add(FName(*(TEXT("RUDE_SCEN_ModelSet:") + P.ModelSetName)));
		}
		// ⭐ THE ROUND-TRIP PAYLOAD. v1 makes POSITION and HEADING authorable in the viewport;
		// the other eleven fields have no UE representation yet. Carrying them verbatim, in the
		// emitter's own field order, makes each actor SELF-DESCRIBING - the exporter rebuilds a
		// full CScenarioPoint from the actor alone and never has to re-open (or trust the
		// stability of) the source region file. Ugly in the details panel and deliberately
		// temporary: a UScenarioPointComponent with real UPROPERTYs supersedes it the moment the
		// authoring lane lands, and THEN these fields become editable too.
		A->Tags.Add(FName(*FString::Printf(TEXT("RUDE_SCEN_Rec:%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d"),
			P.Rec[0], P.Rec[1], P.Rec[2], P.Rec[3], P.Rec[4], P.Rec[5],
			P.Rec[6], P.Rec[7], P.Rec[8], P.Rec[9], P.Rec[10])));
		A->Tags.Add(FName(*(TEXT("RUDE_SCEN_Flags:") + P.Flags)));
		++Spawned;
		if (Spawned % 500 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ImportScenarioRegion %s: %d/%d points spawned"),
				*RegionStem, Spawned, Points.Num());
		}
	}

	// Region-level spawn points: two exist in the whole shipped game, and they are spawned
	// rather than counted because the container is part of Points and dropping it silently
	// would be a hole in the round trip.
	int32 LoadSaveSpawned = 0;
	for (int32 i = 0; i < LoadSaves.Num(); ++i)
	{
		const FScenLoadSave& L = LoadSaves[i];
		if (!TypeFilter.Passes(L.SpawnType.ToLower())) { continue; }
		AActor* A = SpawnMarker(World, ConeMesh,
			FString::Printf(TEXT("LSP_%s_%d"), *L.SpawnType, i),
			L.UeLoc, L.Rot, PointMarkerScale, true, RootActor);
		if (!A) { continue; }
		A->Tags.Add(IdTag);
		A->Tags.Add(FName(*FString::Printf(TEXT("RUDE_SCEN_LoadSave:%d"), i)));
		A->Tags.Add(FName(*(TEXT("RUDE_SCEN_Type:") + L.SpawnType)));
		++LoadSaveSpawned;
	}

	// Chaining nodes. Spawned as their OWN actors even though 64,709 of the game's 64,940 nodes
	// sit at exactly the same coordinates as a scenario point (measured, string-exact match on
	// all three components): they are separate records carrying their own ScenarioType, their
	// own attachment-prop hash and their own in/out edge flags, and 231 of them have NO
	// coincident point at all - so folding them into the point actors would lose data that the
	// export half has to write back. A sphere, not a cone: the record has no heading field, and
	// a directional marker would invent one.
	int32 NodesSpawned = 0;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const FScenNode& N = Nodes[i];
		if (!N.bValid) { continue; }
		if (!TypeFilter.Passes(N.TypeName.ToLower())) { continue; }
		AActor* A = SpawnMarker(World, SphereMesh,
			FString::Printf(TEXT("SN_%s_%d"), *N.TypeName, i),
			N.UeLoc, FQuat::Identity, NodeMarkerScale, false, RootActor);
		if (!A) { continue; }
		A->Tags.Add(IdTag);
		A->Tags.Add(FName(*FString::Printf(TEXT("RUDE_SCEN_Node:%d"), i)));
		A->Tags.Add(FName(*(TEXT("RUDE_SCEN_Type:") + N.TypeName)));
		if (!N.AttachProp.IsEmpty())
		{
			A->Tags.Add(FName(*(TEXT("RUDE_SCEN_NodeProp:") + N.AttachProp)));
		}
		A->Tags.Add(FName(*FString::Printf(TEXT("RUDE_SCEN_NodeEdges:%d,%d"),
			N.bIn ? 1 : 0, N.bOut ? 1 : 0)));
		++NodesSpawned;
	}

	// ---- 5) THE CHAINING GRAPH, DRAWN -----------------------------------------------------
	// WHY SPLINE COMPONENTS AND NOT DrawDebugLine (the choice this lane asked me to justify):
	// debug lines are frame-scoped or lifetime-scoped, vanish on level reload, cannot be
	// selected, and are never saved - useless for something an author opens tomorrow. A
	// USplineComponent with bDrawDebug renders persistently in the editor viewport whenever the
	// Splines show flag is on (verified in engine source: FSplinePDISceneProxy::GetViewRelevance
	// gates on exactly `bDrawDebug && IsShown(View) && ShowFlags.Splines`), saves with the level,
	// and is selectable - so the routes are real editor content rather than a transient overlay.
	//
	// WHY ONE SPLINE PER EDGE AND NOT PER CHAIN: a chain is NOT a path. Measured over all 204
	// regions: of 6,992 chains, 212 branch (node in/out degree up to 4), so laying a chain's
	// edges end-to-end as one polyline would DRAW A LIE in 3% of chains. One spline per edge is
	// 1:1 with the record that has to be written back, carries that edge's own
	// Action/NavMode/NavSpeed in its tag, and still shows chain grouping through colour.
	// They live on ONE actor as many components, not many actors: an edge is not something a
	// human drags (you move its NODES), and the measured worst case is 686 edges in a region -
	// 686 extra actors would bury the outliner for no authoring gain.
	AActor* ChainsActor = nullptr;
	int32 ChainSplines = 0;
	if (Edges.Num() > 0)
	{
		ChainsActor = World->SpawnActor<AActor>();
		if (!ChainsActor) { return Fail(TEXT("chain display actor spawn failed")); }
		USceneComponent* CR = NewObject<USceneComponent>(ChainsActor, TEXT("Root"));
		ChainsActor->SetRootComponent(CR);
		CR->SetMobility(EComponentMobility::Movable);
		CR->RegisterComponent();
		ChainsActor->AddInstanceComponent(CR);
		// Kept at the world ORIGIN with an identity transform, which is what lets every spline
		// point below be written in Local space while holding UE WORLD coordinates - no
		// world/local conversion to get wrong, and no dependency on component registration order.
		ChainsActor->SetActorTransform(FTransform::Identity);
		ChainsActor->SetActorLabel(RegionStem + TEXT("_Chains"));
		ChainsActor->SetFolderPath(FName(TEXT("RUDE_SCENARIOS")));
		ChainsActor->Tags.Add(IdTag);
		ChainsActor->Tags.Add(FName(TEXT("RUDE_SCEN_Chains")));
		ChainsActor->AttachToActor(RootActor, FAttachmentTransformRules::KeepWorldTransform);

		for (int32 i = 0; i < Edges.Num(); ++i)
		{
			const FScenEdge& E = Edges[i];
			if (!Nodes.IsValidIndex(E.From) || !Nodes.IsValidIndex(E.To)) { continue; }
			// An edge is drawn only when both its endpoints survive the filter, so a filtered
			// view never shows a route into points that are not there.
			if (!TypeFilter.Passes(Nodes[E.From].TypeName.ToLower()) ||
			    !TypeFilter.Passes(Nodes[E.To].TypeName.ToLower()))
			{
				continue;
			}
			const FVector A = Nodes[E.From].UeLoc;
			const FVector B = Nodes[E.To].UeLoc;

			USplineComponent* Sp = NewObject<USplineComponent>(ChainsActor,
				FName(*FString::Printf(TEXT("Edge_%d"), i)));
			Sp->SetMobility(EComponentMobility::Movable);
			Sp->bDrawDebug = true;
			Sp->SetClosedLoop(false, false);
			Sp->ClearSplinePoints(false);
			Sp->AddSplinePoint(A, ESplineCoordinateSpace::Local, false);
			Sp->AddSplinePoint(B, ESplineCoordinateSpace::Local, false);
			// Chain edges are DIRECTED (NodeIndexFrom -> NodeIndexTo) and one-way walk/drive
			// routes are the normal case, so the polyline gets a third point folded back from B
			// at 25 degrees: an arrowhead barb that costs zero extra components. Length is
			// clamped so long highway edges do not grow absurd barbs and short interior edges
			// do not get invisible ones.
			const FVector Delta = B - A;
			const double Len = Delta.Size();
			if (Len > 1.0)
			{
				const FVector U = Delta / Len;
				const double BarbLen = FMath::Min(Len * 0.15, 150.0);
				Sp->AddSplinePoint(B + (-U).RotateAngleAxis(25.f, FVector::UpVector) * BarbLen,
					ESplineCoordinateSpace::Local, false);
			}
			for (int32 Pt = 0; Pt < Sp->GetNumberOfSplinePoints(); ++Pt)
			{
				// Linear, not Curve: an edge is a straight segment between two nodes, and a
				// curved interpolation would draw a route the data does not describe.
				Sp->SetSplinePointType(Pt, ESplinePointType::Linear, false);
			}
			Sp->UpdateSpline();
#if WITH_EDITORONLY_DATA
			// Colour by owning chain so the route groups read at a glance. Golden-ish hue step
			// keeps adjacent chain indices visually distinct; an edge owned by no chain (never
			// seen in the shipped corpus - the partition is exact - but possible in hand-authored
			// data) draws white so it stands out as the anomaly it is.
			Sp->EditorUnselectedSplineSegmentColor = (E.Chain >= 0)
				? FLinearColor::MakeFromHSV8((uint8)((E.Chain * 47) % 256), 255, 255)
				: FLinearColor::White;
#endif
			Sp->SetupAttachment(CR);
			Sp->RegisterComponent();
			ChainsActor->AddInstanceComponent(Sp);
			++ChainSplines;
		}
		// Edge records the exporter needs, kept on the display actor rather than on 686 actors:
		// tags are FName-cheap and searchable, and this keeps the graph self-describing without
		// re-reading the region file. Written for EVERY edge, including ones the filter did not
		// draw - a filtered VIEW must never become a lossy import, or exporting after browsing
		// one scenario type would delete every route the operator was not looking at.
		for (int32 i = 0; i < Edges.Num(); ++i)
		{
			const FScenEdge& E = Edges[i];
			ChainsActor->Tags.Add(FName(*FString::Printf(TEXT("RUDE_SCEN_Edge:%d,%d,%d,%d,%d,%d,%d"),
				i, E.From, E.To, E.Action, E.NavMode, E.NavSpeed, E.Chain)));
		}
	}

	World->MarkPackageDirty();

	TArray<FString> UnknownList = UnknownTypes.Array();
	UnknownList.Sort();

	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ImportScenarioRegion %s: %d/%d points, %d nodes, %d edges (%d splines), %d chains"),
		*RegionStem, Spawned, Points.Num(), NodesSpawned, Edges.Num(), ChainSplines, NumChains);

	return FString::Printf(TEXT(
		"{\"ok\":true,\"region\":\"%s\",\"regionFile\":\"%s\",\"points\":%d,\"chains\":%d,"
		"\"nodes\":%d,\"edges\":%d,\"spawned\":%d,\"skipped\":%d,\"unknownTypes\":[%s],"
		"\"nodesSpawned\":%d,\"chainSplines\":%d,\"edgesWithoutChain\":%d,"
		"\"loadSavePoints\":%d,\"loadSaveSpawned\":%d,\"clusters\":%d,\"clusterPoints\":%d,"
		"\"entityOverrides\":%d,\"overridePoints\":%d,\"typeNames\":%d,"
		"\"modelSetOutOfRange\":%d,\"pointsMissingTransform\":%d,"
		"\"nodesMissingTransform\":%d,\"badEdgeRefs\":%d,"
		"\"badChainEdgeIds\":%d,\"clearedActors\":%d,\"filter\":\"%s\"}"),
		*Esc(RegionStem), *Esc(FPaths::GetCleanFilename(RegionFile)),
		Points.Num(), NumChains, Nodes.Num(), Edges.Num(), Spawned, Skipped,
		*JsonStrArray(UnknownList),
		NodesSpawned, ChainSplines, EdgesWithoutChain,
		LoadSaves.Num(), LoadSaveSpawned, NumClusters, ClusterPoints,
		NumOverrides, OverridePoints, TypeNames.Num(),
		ModelSetOutOfRange, PointsMissingTransform, NodesMissingTransform, BadEdgeRefs,
		BadChainEdgeIds, Cleared, *Esc(Filter.TrimStartAndEnd()));
}
