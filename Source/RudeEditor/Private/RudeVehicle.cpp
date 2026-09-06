// RUDE - RAGE <-> Unreal Development Environment
//
// THE VEHICLE SHOWROOM (v1). A GTA V vehicle is a FRAGMENT, and a fragment is not one drawable:
// the body is the fragment's main drawable, but the WHEEL is a separate CHILD drawable that the
// game instances once per wheel BONE. Import a vehicle through the ordinary fragment lane and
// you get a car sitting on the ground with no wheels at all - which is exactly what RUDE did
// before this file existed.
//
// This is the consumer for QUARRY's yft2xml `--extras` output (quarry/yft2xml.py, 2026-07-28):
//   <Fragment><Drawable><Skeleton><Bones>   every bone, with its LOCAL T/R/S and its Tag
//   <Fragment><Physics><LOD1><Groups>       group NAMES (the group<->bone join)
//   <Fragment><Physics><LOD1><Children>     GroupIndex + BoneTag, and a <Drawable> on the
//                                           children that actually carry geometry
//   <yft>/<vehicle>/<groupName>.ydr.xml     one standalone, directly importable sidecar per
//                                           geometry-bearing child (the wheel mesh)
// The sidecar exists because a child drawable carries NO ShaderGroup of its own - its
// <ShaderIndex> values index the FRAGMENT's shader group - so QUARRY splices the fragment's
// shader group in and hands us a document ImportYdr already knows how to read. That is the whole
// reason this file imports meshes by calling ImportYdr rather than growing a second mesh builder.
//
// MEASURED FACTS THIS LANE RESTS ON (quarry-side probes, 2026-07-28, vs the reference corpus):
//   * A car carries ONE wheel drawable, authored on bone wheel_lf, instanced at every wheel bone
//     (23 of 24 sampled vehicles); buses/trucks carry TWO (wheel_lf + wheel_lr, 4 of 24);
//     helicopters and boats carry none (they still have wheel bones - a heli's skids do not).
//   * Wheel bones are named wheel_<side><axle>: side l|r, axle f|r|m1|m2|m3. 270 wheel bones
//     over 1,500 sampled fragments; 4 wheels is the mode, 6 and 10 occur.
//   * Wheel bone SCALE is unit in 270/270. Wheel bone ROTATION is identity in 228/270 - the
//     remaining ~16% are real (motorbike fork rake, e.g. faggio2's wheel_lf at -0.1736 X).
//   * A wheel bone's parent chain is NOT always trivial: 224/270 hang straight off `chassis` at
//     identity, but 46 sit two or three bones deep. So the transform MUST be composed up the
//     chain - reading the local translation alone is wrong on one wheel in six.
#include "RudeToolset.h"
#include "RudeCorpus.h"
#include "RudeToolsetInternal.h"
#include "RudeVehicleAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/Function.h"
#include "XmlFile.h"

namespace RudeVehicle
{
	// GTA metres -> UE centimetres with the pinned Y mirror. This is the SAME convention
	// ImportDrawableNode applies to every vertex (RudeToolset.cpp: Pos.X*100, -Pos.Y*100,
	// Pos.Z*100), so a bone frame converted here lands in the same space as the mesh built there.
	//
	// The ROTATION mapping deserves its own note, because two different-looking formulas live in
	// this plugin and both are right. Mirroring about the XZ plane is M = diag(1,-1,1); a rotation
	// R becomes M R M, whose axis is -(M a) with the angle unchanged, i.e. the quaternion
	// (x,y,z,w) -> (-x, y, -z, w). That is exactly ExportYdr's `ToGtaQuat` involution. ImportMapArea
	// instead writes (x,-y,z,w) for a ymap entity - the conjugate - because a ymap stores the
	// entity's INVERSE rotation; mirror(conj(stored)) == (x,-y,z,w). A skeleton bone is not a ymap
	// entity and carries no such inversion, so the plain mirror is what applies here.
	//
	// ✅ MEASURED 2026-08-04 - this used to be flagged 🧠 INFERRED and the flag is now retired.
	// WHAT WAS WRONG WITH THE OLD NOTE: it named a falsifiable probe - "faggio2 (fork rake -0.1736
	// on X), imported and looked at: the wrong convention tilts the front wheel BACKWARD" - and
	// that probe CANNOT discriminate. WHAT IT WOULD HAVE COST: nothing has shipped wrong, but an
	// agent running that probe would have "confirmed" the convention on an observation that is
	// IDENTICAL under both hypotheses. HOW IT WAS MEASURED (offline, whole corpus, no editor):
	//   * 1,763 vehicle yft / 7,056 wheel bones. 186 (2.64%) carry a non-identity own rotation and
	//     182 a non-identity ancestor - and on every one of them the wheel's own rotation exactly
	//     CANCELS its fork/gear parent's, so the wheel's MODEL-SPACE orientation composes to
	//     0.00 deg under BOTH hypotheses (faggio2, bati, sanchez, enduro, akula, lazer: 0.00 deg
	//     either way). The wheel never tilts. What the two conventions disagree about is the
	//     wheel's POSITION: faggio2 0.509 m, sanchez 0.591 m, hexer 0.964 m, lazer 1.743 m.
	//   * The real discriminator is physical and lives inside the file: every wheel of a vehicle
	//     rests on ONE ground plane. Composing the chain FORWARD (what Resolve() below does) vs
	//     INVERSE-stored (ymap-style conj at every level), then taking each wheel's lowest point
	//     through its own sidecar bbox: over the 126 vehicles where every wheel bone has its own
	//     axle sidecar (exact radius, no prototype substitution) FORWARD wins 110-16, and over the
	//     44 where the two answers differ by more than 5 cm FORWARD wins 44-0. Median ground
	//     spread FORWARD 1.1 mm, INVERSE 36.7 mm. The 16 INVERSE-leaning cases are all under
	//     3.2 cm - inside front/rear tyre-radius noise - and not one of them is decisive.
	//   * The oracle is an exact transliteration, not an approximation: UE composes
	//     Q(AxB) = Q(B)Q(A), T(AxB) = Q(B)(S(B)T(A))Q(B)^-1 + T(B) (TransformNonVectorized.h:
	//     1316-1330) and FQuat's A*B applies B then A (Quat.h:29-31) - the same Hamilton order.
	// CONCLUSION: a bone <Rotation> is a FORWARD orientation, the composition below is right, and
	// the plain mirror is the correct GTA->UE map for it. ⛔ DO NOT "fix" this to (x,-y,z,w) - the
	// inversion is a ymap CEntityDef property and was proven for that record only.
	// THE PROBE THAT WOULD ACTUALLY FALSIFY THIS (it replaces the fork-rake one): import `sanchez`
	// or `hexer` and read the FRONT wheel's Y OFFSET from the actor origin, never its tilt.
	// Correct: sanchez wheel_lf lands at UE (0, -84.6, -16.6) cm. The inverse convention would put
	// it at UE (0, -27.0, -29.9) cm - tucked under the engine. Oracle:
	// scratchpad/rude_owner/bone_convention.py (ground-plane spread over the discriminating set).
	static FTransform GtaToUe(const FTransform& G)
	{
		const FQuat Q = G.GetRotation();
		const FVector T = G.GetTranslation();
		return FTransform(FQuat(-Q.X, Q.Y, -Q.Z, Q.W).GetNormalized(),
			FVector(T.X * 100.0, -T.Y * 100.0, T.Z * 100.0),
			G.GetScale3D());
	}

	struct FBone
	{
		FString Name;
		int32 Tag = -1;
		int32 Parent = -1;
		FTransform LocalGta = FTransform::Identity;
	};

	// A fragment child that actually has geometry: the wheel prototype and its sidecar identity.
	struct FGeoChild
	{
		FString GroupName;      // = the bone it was authored on, e.g. "wheel_lf"
		int32 BoneTag = -1;
		TCHAR Side = 0;         // 'l' / 'r', 0 when the group is not a wheel
		FString Axle;           // "f" / "r" / "m1" ...
		FString SidecarPath;
		UStaticMesh* Mesh = nullptr;
		FString AssetPath;
	};

	static FString Attr(const FXmlNode* N, const TCHAR* Key, const TCHAR* Def)
	{
		if (!N) { return Def; }
		const FString V = N->GetAttribute(Key);
		return V.IsEmpty() ? FString(Def) : V;
	}

	static FVector Vec3(const FXmlNode* N, const FVector& Def)
	{
		if (!N) { return Def; }
		return FVector(FCString::Atod(*Attr(N, TEXT("x"), TEXT("0"))),
			FCString::Atod(*Attr(N, TEXT("y"), TEXT("0"))),
			FCString::Atod(*Attr(N, TEXT("z"), TEXT("0"))));
	}

	// "wheel_lf" -> ('l', "f") | "wheel_rm1" -> ('r', "m1"). "wheelmesh_lf" is REJECTED: it does
	// not start with "wheel_", and it is a render-LOD helper bone, not a wheel.
	static bool WheelSuffix(const FString& BoneName, TCHAR& OutSide, FString& OutAxle)
	{
		if (!BoneName.StartsWith(TEXT("wheel_"), ESearchCase::IgnoreCase)) { return false; }
		const FString Suffix = BoneName.Mid(6).ToLower();
		if (Suffix.Len() < 2) { return false; }
		const TCHAR S = Suffix[0];
		if (S != TEXT('l') && S != TEXT('r')) { return false; }
		OutSide = S;
		OutAxle = Suffix.Mid(1);
		return true;
	}

	// ImportYdr's verdict is JSON; read fields out of it rather than string-scraping.
	static TSharedPtr<FJsonObject> ParseVerdict(const FString& Verdict)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Verdict);
		if (!FJsonSerializer::Deserialize(Reader, Obj)) { return nullptr; }
		return Obj;
	}

	static int32 JsonInt(const FString& Verdict, const TCHAR* Field, int32 Fallback)
	{
		const TSharedPtr<FJsonObject> Obj = ParseVerdict(Verdict);
		double D = 0;
		return (Obj.IsValid() && Obj->TryGetNumberField(Field, D)) ? (int32)D : Fallback;
	}

	static FString JsonEscape(const FString& In)
	{
		return In.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
	}

	// Import one *.ydr.xml / *.yft.xml into DestFolder unless its package is already there, then
	// hand back the loaded mesh. Same skip-if-exists idempotence model as ImportIndexedDrawable.
	// The expected asset name is the file stem minus its type extension - what ImportYdr picks -
	// but when we DO import we take the assetPath out of its own verdict instead of trusting the
	// guess, because ImportYdr may name a plain drawable after its inner <Name>.
	static UStaticMesh* ImportOrLoad(const FString& XmlPath, const FString& DestFolder,
		FString& OutAssetPath, FString& OutVerdict)
	{
		FString Stem = FPaths::GetBaseFilename(XmlPath);
		Stem.RemoveFromEnd(TEXT(".ydr"));
		Stem.RemoveFromEnd(TEXT(".yft"));
		OutAssetPath = DestFolder / Stem;
		if (!FPackageName::DoesPackageExist(OutAssetPath))
		{
			OutVerdict = URudeToolset::ImportYdr(XmlPath, DestFolder);
			if (!OutVerdict.Contains(TEXT("\"ok\":true")))
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject> Obj = ParseVerdict(OutVerdict);
			FString Reported;
			if (Obj.IsValid() && Obj->TryGetStringField(TEXT("assetPath"), Reported)
				&& !Reported.IsEmpty())
			{
				OutAssetPath = Reported;
			}
		}
		return LoadObject<UStaticMesh>(nullptr, *OutAssetPath);
	}

	// The fragment's skeleton: every bone with its LOCAL frame, tag and parent. A degenerate <Scale>
	// refuses (bone scale is unit in 270/270 measured wheel bones; a zero component is a lost
	// attribute, and a collapsed frame would place a part as an invisible sliver). No <Skeleton>
	// at all is not a parse error - Bones stays empty and the caller decides.
	// Shared by ImportVehicle and ImportVehicleComposite (lifted 2026-09-06; behaviour unchanged).
	static bool ParseSkeleton(const FXmlNode* DrawableNode, TArray<FBone>& Bones, FString& Why)
	{
		Bones.Reset();
		const FXmlNode* Skel = DrawableNode ? DrawableNode->FindChildNode(TEXT("Skeleton")) : nullptr;
		const FXmlNode* BoneList = Skel ? Skel->FindChildNode(TEXT("Bones")) : nullptr;
		if (!BoneList) { return true; }
		for (const FXmlNode* It : BoneList->GetChildrenNodes())
		{
			FBone B;
			if (const FXmlNode* N = It->FindChildNode(TEXT("Name")))
			{
				B.Name = N->GetContent().TrimStartAndEnd();
			}
			B.Tag = FCString::Atoi(*Attr(It->FindChildNode(TEXT("Tag")), TEXT("value"), TEXT("-1")));
			B.Parent = FCString::Atoi(*Attr(It->FindChildNode(TEXT("ParentIndex")),
				TEXT("value"), TEXT("-1")));
			const FXmlNode* R = It->FindChildNode(TEXT("Rotation"));
			const FQuat Q(FCString::Atod(*Attr(R, TEXT("x"), TEXT("0"))),
				FCString::Atod(*Attr(R, TEXT("y"), TEXT("0"))),
				FCString::Atod(*Attr(R, TEXT("z"), TEXT("0"))),
				FCString::Atod(*Attr(R, TEXT("w"), TEXT("1"))));
			const FVector S = Vec3(It->FindChildNode(TEXT("Scale")), FVector::OneVector);
			if (S.GetAbsMin() < UE_KINDA_SMALL_NUMBER)
			{
				Why = FString::Printf(TEXT("bone '%s' has a degenerate <Scale> (%s) - refusing rather "
					"than placing parts on a collapsed frame"), *B.Name, *S.ToString());
				return false;
			}
			B.LocalGta = FTransform(Q.GetNormalized(),
				Vec3(It->FindChildNode(TEXT("Translation")), FVector::ZeroVector), S);
			Bones.Add(MoveTemp(B));
		}
		return true;
	}

	// Compose each bone's MODEL-space frame by walking its parent chain. Memoised, with an
	// in-progress marker so a malformed parent cycle refuses instead of recursing forever.
	// UE's FTransform composition is child-then-parent: Child * Parent (see GtaToUe's note for
	// why this FORWARD composition is the measured-correct one).
	static bool ResolveBoneWorld(const TArray<FBone>& Bones, TArray<FTransform>& WorldGta, FString& Why)
	{
		TArray<uint8> State;                       // 0 = untouched, 1 = resolving, 2 = done
		WorldGta.SetNum(Bones.Num());
		State.SetNumZeroed(Bones.Num());
		TFunction<bool(int32)> Resolve = [&](int32 i) -> bool
		{
			if (!Bones.IsValidIndex(i)) { return false; }
			if (State[i] == 2) { return true; }
			if (State[i] == 1) { return false; }   // cycle
			State[i] = 1;
			const int32 P = Bones[i].Parent;
			if (P < 0 || !Bones.IsValidIndex(P))
			{
				WorldGta[i] = Bones[i].LocalGta;
			}
			else
			{
				if (!Resolve(P)) { return false; }
				WorldGta[i] = Bones[i].LocalGta * WorldGta[P];
			}
			State[i] = 2;
			return true;
		};
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			if (!Resolve(i))
			{
				Why = FString::Printf(TEXT("bone %d (%s) sits in a parent CYCLE - refusing rather than "
					"placing parts off a half-composed frame"), i, *Bones[i].Name);
				return false;
			}
		}
		return true;
	}
}

FString URudeToolset::ImportVehicle(const FString& CorpusRoot, const FString& VehicleName,
                                    const FString& DestFolder)
{
	using namespace RudeVehicle;

	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *JsonEscape(Why));
	};

	// ---- 0) resolve the fragment XML ------------------------------------------------------
	FString Name = VehicleName.TrimStartAndEnd().ToLower();
	Name.RemoveFromEnd(TEXT(".xml"));
	Name.RemoveFromEnd(TEXT(".yft"));
	if (Name.IsEmpty()) { return Fail(TEXT("give a vehicle name, e.g. adder")); }
	// A corpus root resolves the fragment through the ledger; any other folder is taken as an
	// ad-hoc "<folder>/<name>.yft.xml" drop so the tool stays drivable on a single export.
	FString XmlPath = CorpusRoot / (Name + TEXT(".yft.xml"));
	if (FRudeCorpus::LooksLikeCorpus(CorpusRoot))
	{
		FString CorpusErr;
		const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
		if (!Corpus.IsValid()) { return Fail(CorpusErr); }
		if (const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("yft"), Name)) { XmlPath = Corpus->PathOf(*Row); }
	}
	if (!FPaths::FileExists(XmlPath))
	{
		return Fail(FString::Printf(TEXT("no fragment XML at %s - is the name right, and has "
			"QUARRY converted this vehicle?"), *XmlPath));
	}
	// One folder per vehicle. The body mesh and every wheel mesh land together, which is also
	// what keeps wheels from colliding: EVERY car's wheel sidecar is called wheel_lf, so a flat
	// DestFolder would give them all one asset and the second car would silently reuse the
	// first car's wheel. (Agent's call, 2026-07-28 - the collision is real, the fix is a folder.)
	const FString VehicleFolder = DestFolder / Name;
	if (!FPackageName::IsValidLongPackageName(VehicleFolder / Name))
	{
		return Fail(FString::Printf(TEXT("bad content path: %s"), *VehicleFolder));
	}

	FXmlFile Xml(XmlPath);
	if (!Xml.IsValid())
	{
		return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError()));
	}
	const FXmlNode* Root = Xml.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("Fragment"))
	{
		return Fail(TEXT("root is not <Fragment> - this is not a fragment XML"));
	}
	const FXmlNode* DrawableNode = Root->FindChildNode(TEXT("Drawable"));
	if (!DrawableNode) { return Fail(TEXT("<Fragment> has no <Drawable>")); }

	// ---- 1) the skeleton -------------------------------------------------------------------
	// Parse + compose live in RudeVehicle::ParseSkeleton / ResolveBoneWorld (shared with the
	// composite lane since 2026-09-06). The messages are the same ones this block used to emit.
	TArray<FBone> Bones;
	TArray<FTransform> WorldGta;
	{
		FString Why;
		if (!ParseSkeleton(DrawableNode, Bones, Why)) { return Fail(Why); }
		if (Bones.Num() == 0)
		{
			// LOUD, and it names the fix: the extras are opt-in on the QUARRY side, so an old corpus
			// file simply has no <Skeleton> and there is nothing here to place wheels on.
			return Fail(FString::Printf(TEXT("%s has no <Drawable><Skeleton><Bones> - regenerate it "
				"with QUARRY's yft2xml --extras (python quarry/yft2xml.py <%s.yft> --extras --out "
				"<corpus>/yft), then re-run"), *XmlPath, *Name));
		}
		if (!ResolveBoneWorld(Bones, WorldGta, Why)) { return Fail(Why); }
	}

	// ---- 2) the physics children: which drawable belongs to which bone ---------------------
	TArray<FString> GroupNames;
	TArray<FGeoChild> GeoChildren;
	TArray<FString> Missing;
	if (const FXmlNode* Phys = Root->FindChildNode(TEXT("Physics")))
	{
		if (const FXmlNode* Lod1 = Phys->FindChildNode(TEXT("LOD1")))
		{
			if (const FXmlNode* Groups = Lod1->FindChildNode(TEXT("Groups")))
			{
				for (const FXmlNode* It : Groups->GetChildrenNodes())
				{
					const FXmlNode* N = It->FindChildNode(TEXT("Name"));
					GroupNames.Add(N ? N->GetContent().TrimStartAndEnd() : FString());
				}
			}
			if (const FXmlNode* Children = Lod1->FindChildNode(TEXT("Children")))
			{
				for (const FXmlNode* It : Children->GetChildrenNodes())
				{
					// Only a child that CARRIES geometry has a mesh to place; the rest are
					// collision-only stubs (17 of the adder's 18 children). ⚠ Measured 2026-09-06 on
					// ROUT's corpus (blista/taxi/burrito): EVERY child carries a <Drawable> HEADER
					// (name, bounds, lod distances) and only wheel_lf's carries <DrawableModelsHigh>.
					// QUARRY emitted <Drawable> on geometry children only, so the old test read 21
					// sidecars into "missing" here. Geometry = models present, not a header.
					const FXmlNode* ChildDrawable = It->FindChildNode(TEXT("Drawable"));
					if (!ChildDrawable || !ChildDrawable->FindChildNode(TEXT("DrawableModelsHigh"))) { continue; }
					FGeoChild C;
					const int32 Gi = FCString::Atoi(*Attr(It->FindChildNode(TEXT("GroupIndex")),
						TEXT("value"), TEXT("-1")));
					C.GroupName = GroupNames.IsValidIndex(Gi) ? GroupNames[Gi] : FString();
					C.BoneTag = FCString::Atoi(*Attr(It->FindChildNode(TEXT("BoneTag")),
						TEXT("value"), TEXT("-1")));
					if (C.GroupName.IsEmpty())
					{
						Missing.Add(FString::Printf(TEXT("child with GroupIndex %d has no group "
							"name - no sidecar can be found for it"), Gi));
						continue;
					}
					WheelSuffix(C.GroupName, C.Side, C.Axle);
					C.SidecarPath = FPaths::GetPath(XmlPath) / Name /
						(C.GroupName + TEXT(".ydr.xml"));
					GeoChildren.Add(MoveTemp(C));
				}
			}
		}
	}

	// ---- 3) import the body, then each child (wheel) mesh -----------------------------------
	FString BodyAssetPath, BodyVerdict;
	UStaticMesh* BodyMesh = ImportOrLoad(XmlPath, VehicleFolder, BodyAssetPath, BodyVerdict);
	if (!BodyMesh)
	{
		return Fail(FString::Printf(TEXT("body import failed: %s"),
			*(BodyVerdict.IsEmpty() ? FString(TEXT("asset did not load")) : BodyVerdict)));
	}
	const int32 BodyGeos = JsonInt(BodyVerdict, TEXT("geometries"), -1);

	for (FGeoChild& C : GeoChildren)
	{
		if (!FPaths::FileExists(C.SidecarPath))
		{
			Missing.Add(FString::Printf(TEXT("sidecar %s not found (regenerate this vehicle with "
				"yft2xml --extras)"), *C.SidecarPath));
			continue;
		}
		FString Verdict;
		C.Mesh = ImportOrLoad(C.SidecarPath, VehicleFolder, C.AssetPath, Verdict);
		if (!C.Mesh)
		{
			Missing.Add(FString::Printf(TEXT("child mesh %s failed to import"), *C.GroupName));
		}
	}

	// ---- 4) choose a prototype per wheel bone -----------------------------------------------
	// One authored wheel serves many bones. Match on the AXLE first (a bus's rear wheel is a
	// different mesh from its front), then fall back: a mid axle rides the REAR wheel, anything
	// unmatched rides the front, and a single-prototype car uses that one for everything.
	// (Agent's rule, 2026-07-28. The axle fallback for mid axles is 🧠 inferred from truck/coach
	// wheel sizing, not measured against a rendered frame.)
	TMap<FString, const FGeoChild*> ByAxle;
	const FGeoChild* AnyWheel = nullptr;
	for (const FGeoChild& C : GeoChildren)
	{
		if (C.Side == 0 || !C.Mesh) { continue; }
		ByAxle.Add(C.Axle, &C);
		if (!AnyWheel) { AnyWheel = &C; }
	}
	auto PickPrototype = [&](const FString& Axle) -> const FGeoChild*
	{
		if (const FGeoChild** Exact = ByAxle.Find(Axle)) { return *Exact; }
		if (Axle.StartsWith(TEXT("m")))
		{
			if (const FGeoChild** Rear = ByAxle.Find(TEXT("r"))) { return *Rear; }
		}
		if (const FGeoChild** Front = ByAxle.Find(TEXT("f"))) { return *Front; }
		return AnyWheel;
	};

	// ---- 5) spawn --------------------------------------------------------------------------
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }

	// Idempotent re-run is clear-by-TAG, per vehicle - the same contract ImportMlo uses, and for
	// the same reason: OFPA can rewrite outliner folder paths, so a folder clear is not reliable
	// and would also kill other vehicles.
	const FName IdTag(*(TEXT("RUDE_VEHICLE:") + Name));
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(IdTag)) { Stale.Add(*It); }
		}
		for (AActor* A : Stale) { World->DestroyActor(A); }
	}

	AActor* Actor = World->SpawnActor<AActor>();
	if (!Actor) { return Fail(TEXT("actor spawn failed")); }
	USceneComponent* RootComp = NewObject<USceneComponent>(Actor, TEXT("Root"));
	Actor->SetRootComponent(RootComp);
	RootComp->SetMobility(EComponentMobility::Static);
	RootComp->RegisterComponent();
	Actor->AddInstanceComponent(RootComp);
	Actor->SetActorLabel(TEXT("Vehicle_") + Name);
	Actor->SetFolderPath(FName(TEXT("RUDE_VEHICLES")));
	Actor->Tags.Add(IdTag);
	Actor->Tags.Add(FName(TEXT("RUDE_VEHICLE_ROOT")));

	auto AddPart = [&](UStaticMesh* Mesh, const FName& CompName, const FTransform& Xf)
	{
		UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(Actor, CompName);
		SMC->SetStaticMesh(Mesh);
		SMC->SetMobility(EComponentMobility::Static);
		SMC->SetupAttachment(RootComp);
		SMC->SetRelativeTransform(Xf);
		SMC->RegisterComponent();
		Actor->AddInstanceComponent(SMC);
		return SMC;
	};
	// The body drawable's vertices are already in vehicle-local space, so it sits at identity -
	// the vehicle's own origin is the actor's origin, which is what makes the wheel bone frames
	// (also vehicle-local) drop straight in as relative transforms.
	AddPart(BodyMesh, TEXT("Body"), FTransform::Identity);

	// Individual UStaticMeshComponents, NOT the InstancedStaticMeshComponent pattern the map
	// tools use. ISM is right for thousands of map instances; a showroom has four wheels and the
	// author wants to click one. (Agent's call.)
	int32 WheelsPlaced = 0, WheelsMirrored = 0, WheelBones = 0;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		TCHAR Side = 0;
		FString Axle;
		if (!WheelSuffix(Bones[i].Name, Side, Axle)) { continue; }
		++WheelBones;
		const FGeoChild* Proto = PickPrototype(Axle);
		if (!Proto || !Proto->Mesh)
		{
			Missing.Add(FString::Printf(TEXT("bone %s has no wheel mesh to place"),
				*Bones[i].Name));
			continue;
		}
		FTransform Xf = GtaToUe(WorldGta[i]);
		if (Side != Proto->Side)
		{
			// The wheel is authored for ONE side (wheel_lf on 23 of 24 sampled cars). Placed
			// unmirrored on the other side, its rim faces into the arch. Reflect it in the
			// bone's own frame across the axle axis - and the axle IS local X: the adder's wheel
			// bbox is +-0.16 on X against +-0.34 on Y and Z, i.e. a disc lying in the YZ plane,
			// and the GTA->UE map leaves X alone. UE reverses culling for a negative-determinant
			// transform, so the mirrored copy renders solid rather than inside-out.
			Xf = FTransform(FQuat::Identity, FVector::ZeroVector, FVector(-1.f, 1.f, 1.f)) * Xf;
			++WheelsMirrored;
		}
		// The component name carries the bone INDEX as well as its name: NewObject with an
		// explicit name that already exists under the same outer is a hard error, and nothing
		// guarantees a fragment's bone names are unique.
		AddPart(Proto->Mesh, FName(*FString::Printf(TEXT("Wheel_%d_%s"), i, *Bones[i].Name)), Xf);
		++WheelsPlaced;
	}
	if (WheelBones == 0)
	{
		// Not an error: helicopters, boats and planes are fragments with skeletons and no wheels.
		Missing.Add(TEXT("this fragment has no wheel_* bones - nothing to place wheels on"));
	}

	// ⛔ THE LIST IS CAPPED; THE COUNT IS NOT. WHAT WAS WRONG: this loop stopped at 20 entries and
	// the verdict emitted only "missing":[...] - so the ONE field carrying every failure this tool
	// can have (four sources: :359 nameless group, :386 absent sidecar, :394 failed child import,
	// :481 bone with no prototype, plus the no-wheel-bones note at :506) silently capped, and a bus with
	// 30 sidecar failures read as a bounded 20. WHAT IT COST: nothing measured yet - the only
	// in-engine run to date is `adder` (2026-08-02) with missing[] empty - but it is the vehicle
	// lane's only failure channel. Every other RUDE tool that caps a list reports the total beside
	// it (ImportYdrBatch caps failedFiles at 30 and still reports `failed`); this now matches.
	FString MissingJson, WheelMeshesJson;
	const int32 MissingListCap = 20;
	for (int32 i = 0; i < Missing.Num() && i < MissingListCap; ++i)
	{
		MissingJson += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""),
			*JsonEscape(Missing[i]));
	}
	const int32 MissingTruncated = FMath::Max(0, Missing.Num() - MissingListCap);
	int32 Nw = 0, NonWheelChildMeshes = 0;
	for (const FGeoChild& C : GeoChildren)
	{
		if (!C.Mesh) { continue; }
		if (C.Side == 0)
		{
			// A geometry child on a non-wheel group (rare, and not a wheel by any reading of its
			// name). Its mesh IS imported - the author may want it - but nothing places it, and
			// calling it a wheel in the verdict would be a lie.
			++NonWheelChildMeshes;
			continue;
		}
		WheelMeshesJson += FString::Printf(TEXT("%s\"%s\""), Nw++ ? TEXT(",") : TEXT(""),
			*JsonEscape(C.AssetPath));
	}
	// wheelMesh is the PRIMARY wheel (the one authored bone, wheel_lf on 23 of 24 sampled cars);
	// wheelMeshes lists them all, because a bus really does carry two.
	const FString WheelMeshField = AnyWheel
		? FString::Printf(TEXT("\"%s\""), *JsonEscape(AnyWheel->AssetPath))
		: FString(TEXT("null"));
	const FString ActorLabel = Actor->GetActorLabel();

	// ⛔ `ok` IS COMPUTED, NOT ASSERTED. WHAT WAS WRONG: this verdict opened with a literal
	// "ok":true, so a fragment that HAS wheel bones and placed ZERO wheels - literally the failure
	// this file was written to prevent (see the header: "a car sitting on the ground with no
	// wheels at all") - returned success to all three observation surfaces, every one of which
	// decides purely by searching for "ok":false (RudeCommandlet.cpp:115, RudeToolPanel.cpp:73
	// and :317 via FRudeInvoke::ReportedFailure). WHAT IT COST: nothing observed - the single
	// in-engine run, `adder` 2026-08-02, reported wheelsPlaced 4 / wheelBones 4 and is unaffected
	// - but an old corpus with no `--extras` sidecars produces exactly this shape and used to read
	// green. HOW IT IS MEASURED: wheelBones is what the SKELETON declares, wheelsPlaced is what
	// actually received a mesh, so wheelBones > 0 && wheelsPlaced == 0 is a wheel-less vehicle by
	// the fragment's own account. wheelBones == 0 stays ok:true on purpose: helicopters, boats and
	// planes are fragments with skeletons and no wheels, and that is a correct import.
	const bool bWheelsIntact = (WheelBones == 0) || (WheelsPlaced > 0);
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"wheelsIntact\":%s,\"vehicle\":\"%s\",\"bodyAsset\":\"%s\",\"bodyGeos\":%d,"
		"\"wheelMesh\":%s,\"wheelMeshes\":[%s],\"wheelsPlaced\":%d,\"wheelsMirrored\":%d,"
		"\"wheelBones\":%d,\"bonesRead\":%d,\"geoChildren\":%d,\"nonWheelChildMeshes\":%d,"
		"\"actor\":\"%s\",\"missingCount\":%d,\"missingTruncated\":%d,\"missing\":[%s]}"),
		bWheelsIntact ? TEXT("true") : TEXT("false"),
		bWheelsIntact ? TEXT("true") : TEXT("false"),
		*Name, *JsonEscape(BodyAssetPath), BodyGeos, *WheelMeshField,
		*WheelMeshesJson, WheelsPlaced, WheelsMirrored, WheelBones, Bones.Num(),
		GeoChildren.Num(), NonWheelChildMeshes, *JsonEscape(ActorLabel),
		Missing.Num(), MissingTruncated, *MissingJson);
}

// ---- ImportVehicleComposite / SetVehicleLivery (WP10 vehicles lane, 2026-09-06) ------------------
// MEASURED over blista / taxi / burrito, base + _hi (scratchpad/wp10/vehicles/LAWS.md):
//   * <Physics><LOD1><Children> = one child per group (21 / 26 / 28). EVERY child carries a <Drawable>
//     header; ONLY wheel_lf's carries geometry. Doors, bonnet, boot, bumpers, wings and windows are
//     NOT separate drawables - they are vertices of the main drawable (HasSkin 1, BlendIndices on
//     every geometry) weighted to their bones. So a child is a BONE FRAME + a BOUND: the composite
//     gives each one a named component at its bone's model-space frame (the door PIVOT the author
//     rotates), and the wheel child's mesh rides every wheel_* bone by ImportVehicle's rule.
//   * The _hi fragment is the same skeleton and children with ONE lod group (blista High 24 geos /
//     36,947 vertex rows vs base High 15 / 12,101); the base carries High/Medium/Low/VeryLow. The
//     body mesh is therefore _hi High as LOD0 and the base groups as LOD1..4 - the "detail toggle".
//   * The main drawable has NO <Bounds>; collision is <Physics><LOD1><Archetype><Bounds> (Composite:
//     Geometry per body part, Disc per wheel, one Box). It is grafted onto the LOD0 drawable so
//     ImportDrawableNode's embedded-bounds path builds the collision mesh the way it does for props.
//   * A child drawable has no ShaderGroup; its ShaderIndex indexes the FRAGMENT's. Every drawable
//     here is therefore re-spelled into a self-contained <Drawable> buffer (header + the fragment's
//     ShaderGroup + models under <DrawableModelsHigh> [+ Bounds]) and handed to ImportDrawableNode -
//     the one mesh + material path every lane uses. No second mesh builder.
//   * handling joins through vehicles.meta's handlingId (taxi -> TAXI2), never the model name; the
//     base game's carvariations is a PSO (carvariations.ymt.pso.xml) whose tags are joaat hashes of
//     the meta's spellings (liveries = A6648434, colors = 08676A67, indices = E9BF9F2D, kits = 6DDF749B).
//   * Liveries are the <veh>_sign_<n> textures (burrito: 4 in the ytd, 4 hi-res in +hi.ytd), bound
//     by one fragment shader (burrito: vehicle_paint3 DiffuseSampler2; taxi's taxi_signs_2 is a fixed
//     sign - all 30 carvariations livery flags false). Index n-1 is INFERRED, carried as data.
// ⚠ This corpus (2026-09-04 export) carries NO pixel sidecars for vehicles.rpf ytds, so textures do
//   not bind and SetVehicleLivery refuses with textureMissing until ROUT exports them.
namespace RudeVehicle
{
	// A self-contained <Drawable> document for ImportDrawableNode: Header's own fields (everything
	// but ShaderGroup / Skeleton / Joints / DrawableModels* / Bounds), ShaderGroup, Models' items
	// under <DrawableModelsHigh> (a base Medium/Low/VeryLow group is re-tagged High so the shared
	// importer reads it), and optionally a <Bounds> subtree.
	// The archetype composite for the body's collision, minus the child types ParseBound's catalogue
	// does not carry. MEASURED IN-ENGINE 2026-09-06 (scratchpad/wp10/vehicles/cli_probe.log): blista's
	// four wheel bounds are type="Disc", ParseBound refuses "unknownType:Disc", and its gate
	// (collisionBoundsMalformed) turned the WHOLE body import ok:false while 16 Geometry + 1 Box had
	// imported (378 collision triangles). A Disc is a wheel's cylinder - no faithful UE target, the
	// same class as Cylinder (unmapped) - so it is left out of the graft and COUNTED in the verdict
	// (boundsSkippedDisc); the wheel bounds still ride the DataAsset's child table (BoundType + box as
	// spelled). Every composite child carries its own transform (ParseBound reads it per item), so
	// dropping one cannot desync another. Any OTHER unseen type still reaches ParseBound and gates.
	static void SpellBoundsFiltered(const FXmlNode* Bounds, FString& O)
	{
		O += TEXT(" <Bounds");
		for (const FXmlAttribute& A : Bounds->GetAttributes())
		{
			O += TEXT(" "); O += A.GetTag(); O += TEXT("=\"");
			RudeXmlEscapeInto(O, A.GetValue());
			O += TEXT("\"");
		}
		O += TEXT(">\n");
		for (const FXmlNode* C : Bounds->GetChildrenNodes())
		{
			if (C->GetTag() != TEXT("Children")) { RudeXmlNodeToString(C, O, 2); continue; }
			O += TEXT("  <Children>\n");
			for (const FXmlNode* Item : C->GetChildrenNodes())
			{
				if (Item->GetAttribute(TEXT("type")) == TEXT("Disc")) { continue; }
				RudeXmlNodeToString(Item, O, 3);
			}
			O += TEXT("  </Children>\n");
		}
		O += TEXT(" </Bounds>\n");
	}

	static FString SpellDrawableBuffer(const FXmlNode* Header, const FXmlNode* ShaderGroup,
		const FXmlNode* Models, const FXmlNode* Bounds)
	{
		FString O;
		O += TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Drawable>\n");
		if (Header)
		{
			for (const FXmlNode* C : Header->GetChildrenNodes())
			{
				const FString& T = C->GetTag();
				if (T == TEXT("ShaderGroup") || T == TEXT("Skeleton") || T == TEXT("Joints")
					|| T == TEXT("Bounds") || T.StartsWith(TEXT("DrawableModels"))) { continue; }
				RudeXmlNodeToString(C, O, 1);
			}
		}
		if (ShaderGroup) { RudeXmlNodeToString(ShaderGroup, O, 1); }
		O += TEXT(" <DrawableModelsHigh>\n");
		if (Models) { for (const FXmlNode* M : Models->GetChildrenNodes()) { RudeXmlNodeToString(M, O, 2); } }
		O += TEXT(" </DrawableModelsHigh>\n");
		if (Bounds) { SpellBoundsFiltered(Bounds, O); }
		O += TEXT("</Drawable>\n");
		return O;
	}

	// Import a re-spelled drawable buffer as <DestFolder>/<MeshName> unless the package exists (the
	// same skip-if-exists idempotence as ImportOrLoad), then hand back the loaded mesh.
	static UStaticMesh* ImportBuffer(const FString& Buffer, const FString& MeshName, const FString& DestFolder,
		FString& OutAssetPath, FString& OutVerdict)
	{
		OutAssetPath = DestFolder / MeshName;
		if (!FPackageName::DoesPackageExist(OutAssetPath))
		{
			FXmlFile Doc(Buffer, EConstructMethod::ConstructFromBuffer);
			if (!Doc.IsValid() || !Doc.GetRootNode())
			{
				OutVerdict = FString::Printf(TEXT("{\"ok\":false,\"error\":\"re-spelled drawable did not parse: %s\"}"),
					*JsonEscape(Doc.GetLastError()));
				return nullptr;
			}
			OutVerdict = ImportDrawableNode(Doc.GetRootNode(), MeshName, DestFolder, nullptr);
			// A mesh the importer BUILT (its verdict names an assetPath) is kept even when that verdict
			// is not ok, so the composite finishes and REPORTS the gate instead of dying on it (rule
			// added on the live tree by another session, 2026-09-06, kept here; the composite folds a
			// not-ok body verdict into its own ok and missing[] - see bodyVerdictOk - so nothing is
			// swallowed). Note for the record: missing textures never gate ImportDrawableNode's ok; the
			// blista refusal that motivated the rule was the wheel Disc bounds, now filtered out.
			const TSharedPtr<FJsonObject> Obj = ParseVerdict(OutVerdict);
			FString Reported;
			if (Obj.IsValid() && Obj->TryGetStringField(TEXT("assetPath"), Reported) && !Reported.IsEmpty())
			{
				OutAssetPath = Reported;
			}
			else if (!OutVerdict.Contains(TEXT("\"ok\":true"))) { return nullptr; }
		}
		return LoadObject<UStaticMesh>(nullptr, *OutAssetPath);
	}

	// Copy Src's LOD0 geometry in as Body's LOD<LodIndex>: its material slots are appended to the body
	// as lod<N>__<slot> (a fresh name per LOD - the base and _hi shader lists differ), the polygon
	// groups are re-pointed at those names, and the section map pins each section to its material
	// index so the build cannot fall back to "polygon group ordinal = material index".
	static bool AppendLod(UStaticMesh* Body, UStaticMesh* Src, int32 LodIndex, FString& Why)
	{
		const FMeshDescription* SrcDesc = Src ? Src->GetMeshDescription(0) : nullptr;
		if (!SrcDesc) { Why = TEXT("LOD source has no mesh description"); return false; }
		TArray<FStaticMaterial> Mats = Body->GetStaticMaterials();
		TMap<FName, FName> Rename;
		for (const FStaticMaterial& M : Src->GetStaticMaterials())
		{
			const FName NewSlot(*FString::Printf(TEXT("lod%d__%s"), LodIndex, *M.MaterialSlotName.ToString()));
			Rename.Add(M.MaterialSlotName, NewSlot);
			Mats.Add(FStaticMaterial(M.MaterialInterface, NewSlot, NewSlot));
		}
		Body->SetStaticMaterials(Mats);
		FMeshDescription Copy = *SrcDesc;
		{
			FStaticMeshAttributes A(Copy);
			TPolygonGroupAttributesRef<FName> Slots = A.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID G : Copy.PolygonGroups().GetElementIDs())
			{
				if (const FName* N = Rename.Find(Slots[G])) { Slots[G] = *N; }
			}
		}
		while (Body->GetNumSourceModels() <= LodIndex) { Body->AddSourceModel(); }
		FMeshDescription* Dst = Body->CreateMeshDescription(LodIndex, MoveTemp(Copy));
		if (!Dst) { Why = TEXT("CreateMeshDescription failed"); return false; }
		Body->CommitMeshDescription(LodIndex);
		FStaticMeshSourceModel& SM = Body->GetSourceModel(LodIndex);
		SM.ReductionSettings.PercentTriangles = 1.f;
		SM.ReductionSettings.PercentVertices = 1.f;
		SM.BuildSettings.bRecomputeNormals = false;
		SM.BuildSettings.bRecomputeTangents = false;
		{
			FStaticMeshConstAttributes A(*Dst);
			TPolygonGroupAttributesConstRef<FName> Slots = A.GetPolygonGroupMaterialSlotNames();
			int32 Section = 0;
			for (const FPolygonGroupID G : Dst->PolygonGroups().GetElementIDs())
			{
				int32 MatIdx = INDEX_NONE;
				for (int32 i = 0; i < Mats.Num(); ++i) { if (Mats[i].MaterialSlotName == Slots[G]) { MatIdx = i; break; } }
				if (MatIdx != INDEX_NONE) { Body->GetSectionInfoMap().Set(LodIndex, Section, FMeshSectionInfo(MatIdx)); }
				++Section;
			}
		}
		return true;
	}

	// PSO-form tags (hash_XXXXXXXX) back to the meta's spelling when the joaat of a known name matches;
	// otherwise the hash tag as spelled - never invented. Measured on carvariations.ymt.pso.xml.
	static FString ResolveTag(const FString& Tag)
	{
		static const TCHAR* Known[] = { TEXT("colors"), TEXT("indices"), TEXT("liveries"), TEXT("kits"),
			TEXT("windowsWithExposedEdges"), TEXT("plateProbabilities"), TEXT("lightSettings"), TEXT("sirenSettings"),
			TEXT("modelName"), TEXT("Name"), TEXT("Value"), TEXT("Probabilities"), TEXT("variationData") };
		if (!Tag.StartsWith(TEXT("hash_")) || Tag.Len() != 13) { return Tag; }
		const uint32 H = FParse::HexNumber(*Tag.Mid(5));
		for (const TCHAR* K : Known) { if (RudeJoaat(K) == H) { return K; } }
		return Tag;
	}

	// Every field of a meta item BY NAME as spelled: the "value" attribute, else the attributes joined
	// ("x=1 y=2 z=3"), else the text. Subtrees flatten with '/', repeated tags and list items index [n].
	static void FlattenItem(const FXmlNode* N, const FString& Prefix, TMap<FString, FString>& Out, int32 Depth)
	{
		if (!N || Depth > 6) { return; }
		TMap<FString, int32> Total, Seen;
		for (const FXmlNode* C : N->GetChildrenNodes()) { ++Total.FindOrAdd(ResolveTag(C->GetTag())); }
		for (const FXmlNode* C : N->GetChildrenNodes())
		{
			const FString Tag = ResolveTag(C->GetTag());
			int32& Nth = Seen.FindOrAdd(Tag);
			const bool bIndexed = Tag == TEXT("Item") || Total[Tag] > 1;
			const FString Key = Prefix + (bIndexed ? FString::Printf(TEXT("%s[%d]"), *Tag, Nth) : Tag);
			++Nth;
			if (C->GetChildrenNodes().Num() == 0)
			{
				FString V = C->GetAttribute(TEXT("value"));
				if (V.IsEmpty() && C->GetAttributes().Num() > 0)
				{
					for (const FXmlAttribute& A : C->GetAttributes())
					{
						V += (V.IsEmpty() ? TEXT("") : TEXT(" ")) + A.GetTag() + TEXT("=") + A.GetValue();
					}
				}
				if (V.IsEmpty()) { V = C->GetContent().TrimStartAndEnd(); }
				Out.Add(Key, V);
			}
			else
			{
				const FString Type = C->GetAttribute(TEXT("type"));
				if (!Type.IsEmpty()) { Out.Add(Key + TEXT("@type"), Type); }
				FlattenItem(C, Key + TEXT("/"), Out, Depth + 1);
			}
		}
	}

	struct FMetaHit
	{
		bool bFound = false;
		int32 SlotRank = -1;
		FString Path, Slot, Xml;
		TMap<FString, FString> Fields;
	};

	// The <Item> whose <Field> text equals Value (case-insensitive), searched over EVERY copy of
	// (Type, Name) in the corpus lowest slot first, so the copy the game loads last wins: a base
	// vehicle's row lives in update.rpf's copy, a DLC vehicle's in its own pack's. A file is parsed
	// only when its text contains Value at all.
	static FMetaHit FindMetaItem(const FRudeCorpus& Corpus, const TCHAR* Type, const TCHAR* Name,
		const TCHAR* Field, const FString& Value, int32& Searched)
	{
		FMetaHit Hit;
		if (Value.IsEmpty()) { return Hit; }
		for (const FRudeCorpusEntry* E : Corpus.History(Type, Name))
		{
			const FString Path = Corpus.PathOf(*E);
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path)) { continue; }
			++Searched;
			if (!Text.Contains(Value, ESearchCase::IgnoreCase)) { continue; }
			FXmlFile Doc(Text, EConstructMethod::ConstructFromBuffer);
			if (!Doc.IsValid()) { continue; }
			TFunction<const FXmlNode*(const FXmlNode*)> Find = [&](const FXmlNode* N) -> const FXmlNode*
			{
				if (!N) { return nullptr; }
				if (N->GetTag() == TEXT("Item"))
				{
					if (const FXmlNode* F = N->FindChildNode(Field))
					{
						if (F->GetContent().TrimStartAndEnd().Equals(Value, ESearchCase::IgnoreCase)) { return N; }
					}
				}
				for (const FXmlNode* C : N->GetChildrenNodes()) { if (const FXmlNode* R = Find(C)) { return R; } }
				return nullptr;
			};
			if (const FXmlNode* Item = Find(Doc.GetRootNode()))
			{
				Hit.bFound = true;
				Hit.SlotRank = E->SlotRank;
				Hit.Path = Path;
				Hit.Slot = E->Slot;
				Hit.Xml.Reset();
				RudeXmlNodeToString(Item, Hit.Xml, 0);
				Hit.Fields.Reset();
				FlattenItem(Item, TEXT(""), Hit.Fields, 0);
			}
		}
		return Hit;
	}

	static AActor* FindVehicleActor(UWorld* World, const FString& Label)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(FName(TEXT("RUDE_VEHICLE_ROOT"))) && It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)) { return *It; }
		}
		return nullptr;
	}

	static FString JsonStrings(const TArray<FString>& In, int32 Cap = 0)
	{
		FString O;
		for (int32 i = 0; i < In.Num() && (Cap == 0 || i < Cap); ++i)
		{
			O += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *JsonEscape(In[i]));
		}
		return O;
	}
}

FString URudeToolset::ImportVehicleComposite(const FString& CorpusRoot, const FString& VehicleName,
                                             const FString& DestFolder)
{
	using namespace RudeVehicle;
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *JsonEscape(Why));
	};

	// ---- 0) the base + _hi fragments and the two texture dictionaries, through the ledger ----------
	FString Name = VehicleName.TrimStartAndEnd().ToLower();
	Name.RemoveFromEnd(TEXT(".xml"));
	Name.RemoveFromEnd(TEXT(".yft"));
	if (Name.IsEmpty()) { return Fail(TEXT("give a vehicle name, e.g. blista")); }
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::LooksLikeCorpus(CorpusRoot)
		? FRudeCorpus::Open(CorpusRoot, CorpusErr) : nullptr;
	if (!Corpus.IsValid())
	{
		return Fail(FString::Printf(TEXT("CorpusRoot must be a ROUT filebase (the folder holding _FILEBASE.json"
			" and _PROVENANCE.jsonl) - the meta join needs the ledger: %s"), *CorpusErr));
	}
	auto Effective = [&](const TCHAR* Type, const FString& N, FString& OutSlot) -> FString
	{
		const FRudeCorpusEntry* Row = Corpus->Effective(Type, N);
		if (!Row) { return FString(); }
		OutSlot = Row->Slot;
		return Corpus->PathOf(*Row);
	};
	FString BaseSlot, HiSlot, YtdSlot, HiYtdSlot;
	const FString BasePath = Effective(TEXT("yft"), Name, BaseSlot);
	const FString HiPath = Effective(TEXT("yft"), Name + TEXT("_hi"), HiSlot);
	const FString YtdPath = Effective(TEXT("ytd"), Name, YtdSlot);
	const FString HiYtdPath = Effective(TEXT("ytd"), Name + TEXT("+hi"), HiYtdSlot);
	if (BasePath.IsEmpty() || !FPaths::FileExists(BasePath))
	{
		return Fail(FString::Printf(TEXT("the corpus has no yft named '%s'"), *Name));
	}
	const FString VehicleFolder = DestFolder / Name;
	if (!FPackageName::IsValidLongPackageName(VehicleFolder / Name))
	{
		return Fail(FString::Printf(TEXT("bad content path: %s"), *VehicleFolder));
	}

	FXmlFile BaseXml(BasePath);
	if (!BaseXml.IsValid()) { return Fail(FString::Printf(TEXT("XML load failed: %s"), *BaseXml.GetLastError())); }
	const FXmlNode* BaseRoot = BaseXml.GetRootNode();
	if (!BaseRoot || BaseRoot->GetTag() != TEXT("Fragment")) { return Fail(TEXT("root is not <Fragment> - this is not a fragment XML")); }
	const FXmlNode* BaseDrawable = BaseRoot->FindChildNode(TEXT("Drawable"));
	if (!BaseDrawable) { return Fail(TEXT("<Fragment> has no <Drawable>")); }
	const FXmlNode* BaseShaders = BaseDrawable->FindChildNode(TEXT("ShaderGroup"));

	TUniquePtr<FXmlFile> HiXml;
	const FXmlNode* HiRoot = nullptr;
	const FXmlNode* HiDrawable = nullptr;
	if (!HiPath.IsEmpty() && FPaths::FileExists(HiPath))
	{
		HiXml = MakeUnique<FXmlFile>(HiPath);
		HiRoot = HiXml->IsValid() ? HiXml->GetRootNode() : nullptr;
		HiDrawable = (HiRoot && HiRoot->GetTag() == TEXT("Fragment")) ? HiRoot->FindChildNode(TEXT("Drawable")) : nullptr;
		if (!HiDrawable)
		{
			return Fail(FString::Printf(TEXT("%s exists but is not a <Fragment> with a <Drawable> (%s)"),
				*HiPath, *HiXml->GetLastError()));
		}
	}

	// ---- 1) the skeleton (the base file's; the _hi's is the same one: 68/68 and 74/74 measured) --
	TArray<FBone> Bones;
	TArray<FTransform> WorldGta;
	{
		FString Why;
		if (!ParseSkeleton(BaseDrawable, Bones, Why)) { return Fail(Why); }
		if (Bones.Num() == 0) { return Fail(TEXT("the fragment has no <Drawable><Skeleton><Bones> - nothing to attach children to")); }
		if (!ResolveBoneWorld(Bones, WorldGta, Why)) { return Fail(Why); }
	}
	TMap<int32, int32> BoneByTag;
	int32 WheelBones = 0;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		if (!BoneByTag.Contains(Bones[i].Tag)) { BoneByTag.Add(Bones[i].Tag, i); }
		TCHAR Side = 0; FString Axle;
		if (WheelSuffix(Bones[i].Name, Side, Axle)) { ++WheelBones; }
	}

	// ---- 2) the physics children: group, bone, geometry, bound (bounds pair by ordinal) -----------
	struct FChildRow
	{
		int32 Index = -1, GroupIndex = -1, ParentGroup = 255, BoneTag = -1, BoneIndex = -1;
		FString GroupName, BoundType, BoxMin, BoxMax, PristineMass, DamagedMass, AssetPath, ComponentName;
		const FXmlNode* Drawable = nullptr;      // base file
		const FXmlNode* HiDrawableNode = nullptr; // _hi file, same ordinal
		bool bGeometry = false;
		UStaticMesh* Mesh = nullptr;
	};
	TArray<FChildRow> Children;
	TArray<FString> Missing;
	const FXmlNode* Phys = BaseRoot->FindChildNode(TEXT("Physics"));
	const FXmlNode* Lod1 = Phys ? Phys->FindChildNode(TEXT("LOD1")) : nullptr;
	if (!Lod1) { return Fail(TEXT("<Physics><LOD1> is missing - not a vehicle-shaped fragment")); }
	TArray<FString> GroupNames;
	TArray<int32> GroupParents;
	if (const FXmlNode* Groups = Lod1->FindChildNode(TEXT("Groups")))
	{
		for (const FXmlNode* It : Groups->GetChildrenNodes())
		{
			const FXmlNode* N = It->FindChildNode(TEXT("Name"));
			GroupNames.Add(N ? N->GetContent().TrimStartAndEnd() : FString());
			GroupParents.Add(FCString::Atoi(*Attr(It->FindChildNode(TEXT("ParentIndex")), TEXT("value"), TEXT("255"))));
		}
	}
	const FXmlNode* ArchBounds = nullptr;
	if (const FXmlNode* Arch = Lod1->FindChildNode(TEXT("Archetype"))) { ArchBounds = Arch->FindChildNode(TEXT("Bounds")); }
	TArray<const FXmlNode*> BoundChildren;
	if (ArchBounds)
	{
		if (const FXmlNode* BC = ArchBounds->FindChildNode(TEXT("Children")))
		{
			for (const FXmlNode* B : BC->GetChildrenNodes()) { BoundChildren.Add(B); }
		}
	}
	TArray<const FXmlNode*> HiChildren;
	if (HiRoot)
	{
		const FXmlNode* HP = HiRoot->FindChildNode(TEXT("Physics"));
		const FXmlNode* HL = HP ? HP->FindChildNode(TEXT("LOD1")) : nullptr;
		const FXmlNode* HC = HL ? HL->FindChildNode(TEXT("Children")) : nullptr;
		if (HC) { for (const FXmlNode* It : HC->GetChildrenNodes()) { HiChildren.Add(It); } }
	}
	TMap<FString, int32> BoundTypeCounts;
	int32 BoundsSkippedDisc = 0;
	if (const FXmlNode* Ch = Lod1->FindChildNode(TEXT("Children")))
	{
		for (const FXmlNode* It : Ch->GetChildrenNodes())
		{
			FChildRow C;
			C.Index = Children.Num();
			C.GroupIndex = FCString::Atoi(*Attr(It->FindChildNode(TEXT("GroupIndex")), TEXT("value"), TEXT("-1")));
			C.GroupName = GroupNames.IsValidIndex(C.GroupIndex) ? GroupNames[C.GroupIndex] : FString::Printf(TEXT("group%d"), C.GroupIndex);
			C.ParentGroup = GroupParents.IsValidIndex(C.GroupIndex) ? GroupParents[C.GroupIndex] : 255;
			C.BoneTag = FCString::Atoi(*Attr(It->FindChildNode(TEXT("BoneTag")), TEXT("value"), TEXT("-1")));
			if (const int32* Bi = BoneByTag.Find(C.BoneTag)) { C.BoneIndex = *Bi; }
			C.PristineMass = Attr(It->FindChildNode(TEXT("PristineMass")), TEXT("value"), TEXT(""));
			C.DamagedMass = Attr(It->FindChildNode(TEXT("DamagedMass")), TEXT("value"), TEXT(""));
			C.Drawable = It->FindChildNode(TEXT("Drawable"));
			C.bGeometry = C.Drawable && C.Drawable->FindChildNode(TEXT("DrawableModelsHigh"));
			if (HiChildren.IsValidIndex(C.Index))
			{
				const FXmlNode* HD = HiChildren[C.Index]->FindChildNode(TEXT("Drawable"));
				if (HD && HD->FindChildNode(TEXT("DrawableModelsHigh"))) { C.HiDrawableNode = HD; }
			}
			if (BoundChildren.IsValidIndex(C.Index))
			{
				const FXmlNode* B = BoundChildren[C.Index];
				C.BoundType = B->GetAttribute(TEXT("type"));
				++BoundTypeCounts.FindOrAdd(C.BoundType);
				if (C.BoundType == TEXT("Disc")) { ++BoundsSkippedDisc; }   // left out of the collision graft (see SpellBoundsFiltered)
				auto Spell = [&](const TCHAR* Tag) -> FString
				{
					const FXmlNode* V = B->FindChildNode(Tag);
					return V ? FString::Printf(TEXT("%s %s %s"), *V->GetAttribute(TEXT("x")), *V->GetAttribute(TEXT("y")), *V->GetAttribute(TEXT("z"))) : FString();
				};
				C.BoxMin = Spell(TEXT("BoxMin"));
				C.BoxMax = Spell(TEXT("BoxMax"));
			}
			if (C.BoneIndex < 0)
			{
				Missing.Add(FString::Printf(TEXT("child %d (%s) names bone tag %d, which the skeleton does not have - placed at the origin"), C.Index, *C.GroupName, C.BoneTag));
			}
			Children.Add(MoveTemp(C));
		}
	}
	if (Children.Num() == 0) { return Fail(TEXT("<Physics><LOD1><Children> is empty")); }
	if (BoundChildren.Num() != Children.Num())
	{
		Missing.Add(FString::Printf(TEXT("bound children (%d) != physics children (%d): the ordinal pairing measured on 3 vehicles does not hold here; bound rows are as paired"), BoundChildren.Num(), Children.Num()));
	}

	// ---- 3) the body: LOD0 = _hi High (or base High), LOD1.. = base High/Medium/Low/VeryLow ------
	const FString BodyName = Name + TEXT("_body");
	const FXmlNode* Lod0Drawable = HiDrawable ? HiDrawable : BaseDrawable;
	const FXmlNode* Lod0Shaders = Lod0Drawable->FindChildNode(TEXT("ShaderGroup"));
	const FXmlNode* Lod0Models = Lod0Drawable->FindChildNode(TEXT("DrawableModelsHigh"));
	if (!Lod0Models) { return Fail(TEXT("the body drawable has no <DrawableModelsHigh>")); }
	FString BodyAssetPath, BodyVerdict;
	UStaticMesh* Body = ImportBuffer(SpellDrawableBuffer(Lod0Drawable, Lod0Shaders, Lod0Models, ArchBounds),
		BodyName, VehicleFolder, BodyAssetPath, BodyVerdict);
	if (!Body)
	{
		return Fail(FString::Printf(TEXT("body import failed: %s"), *(BodyVerdict.IsEmpty() ? FString(TEXT("asset did not load")) : BodyVerdict)));
	}
	const int32 BodyGeos = JsonInt(BodyVerdict, TEXT("geometries"), -1);
	const int32 BodyMissingTex = JsonInt(BodyVerdict, TEXT("missingTextures"), -1);
	// An existing package skips the import (empty verdict = nothing to judge); a fresh import that the
	// drawable importer gated (slotsWithoutMaterial / collisionBoundsMalformed) is kept as built but
	// NAMED here and folded into this verdict's ok - the gate stays visible.
	const bool bBodyVerdictOk = BodyVerdict.IsEmpty() || BodyVerdict.Contains(TEXT("\"ok\":true"));
	if (!bBodyVerdictOk)
	{
		FString Reasons;
		const TSharedPtr<FJsonObject> BodyObj = ParseVerdict(BodyVerdict);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (BodyObj.IsValid() && BodyObj->TryGetArrayField(TEXT("collisionReasons"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr) { Reasons += (Reasons.IsEmpty() ? TEXT("") : TEXT(", ")) + V->AsString(); }
		}
		Missing.Add(FString::Printf(TEXT("body drawable verdict is ok:false (mesh kept as built): slotsWithoutMaterial %d, collisionBoundsMalformed %d%s%s"),
			JsonInt(BodyVerdict, TEXT("slotsWithoutMaterial"), -1), JsonInt(BodyVerdict, TEXT("collisionBoundsMalformed"), -1),
			Reasons.IsEmpty() ? TEXT("") : TEXT(" - "), *Reasons));
	}
	TArray<FString> LodSources, LodAssets;
	LodSources.Add((HiDrawable ? Name + TEXT("_hi") : Name) + TEXT(":DrawableModelsHigh"));
	int32 LodFailed = 0;
	{
		static const TCHAR* GroupTags[] = { TEXT("DrawableModelsHigh"), TEXT("DrawableModelsMedium"), TEXT("DrawableModelsLow"), TEXT("DrawableModelsVeryLow") };
		TArray<TPair<FString, const FXmlNode*>> Sources;
		for (const TCHAR* G : GroupTags)
		{
			if (const FXmlNode* M = BaseDrawable->FindChildNode(G)) { Sources.Emplace(FString(G), M); }
		}
		if (!HiDrawable && Sources.Num() > 0) { Sources.RemoveAt(0); }   // base High IS LOD0 then
		// Rebuild the LOD stack every run (idempotent): LOD0 stays, LOD1.. and their slots are re-made.
		Body->SetNumSourceModels(1);
		{
			TArray<FStaticMaterial> Mats = Body->GetStaticMaterials();
			Mats.RemoveAll([](const FStaticMaterial& M) { return M.MaterialSlotName.ToString().StartsWith(TEXT("lod")); });
			Body->SetStaticMaterials(Mats);
		}
		for (int32 si = 0; si < Sources.Num(); ++si)
		{
			const int32 LodIndex = si + 1;
			const FString LodName = FString::Printf(TEXT("%s_body_lod%d"), *Name, LodIndex);
			FString LodPath, LodVerdict, Why;
			UStaticMesh* LodMesh = ImportBuffer(SpellDrawableBuffer(BaseDrawable, BaseShaders, Sources[si].Value, nullptr),
				LodName, VehicleFolder, LodPath, LodVerdict);
			if (!LodMesh || !AppendLod(Body, LodMesh, LodIndex, Why))
			{
				++LodFailed;
				Missing.Add(FString::Printf(TEXT("LOD%d (%s) failed: %s"), LodIndex, *Sources[si].Key, LodMesh ? *Why : *LodVerdict));
				continue;
			}
			LodSources.Add(Name + TEXT(":") + Sources[si].Key);
			LodAssets.Add(LodPath);
		}
		Body->Build(true);
		Body->PostEditChange();
		Body->MarkPackageDirty();
	}
	const int32 LodCount = Body->GetNumSourceModels();

	// ---- 4) child meshes: the geometry-bearing children (wheel_lf on all three measured) ---------
	// LOD0 from the _hi child of the same ordinal when it carries models, base as LOD1.
	TMap<FString, FChildRow*> ByAxle;
	FChildRow* AnyWheel = nullptr;
	int32 ChildrenWithGeometry = 0;
	for (FChildRow& C : Children)
	{
		if (!C.bGeometry) { continue; }
		++ChildrenWithGeometry;
		const FXmlNode* L0 = C.HiDrawableNode ? C.HiDrawableNode : C.Drawable;
		const FXmlNode* L0Shaders = C.HiDrawableNode ? Lod0Shaders : BaseShaders;
		FString Verdict;
		C.Mesh = ImportBuffer(SpellDrawableBuffer(L0, L0Shaders, L0->FindChildNode(TEXT("DrawableModelsHigh")), nullptr),
			Name + TEXT("_") + C.GroupName, VehicleFolder, C.AssetPath, Verdict);
		if (!C.Mesh)
		{
			Missing.Add(FString::Printf(TEXT("child %s mesh failed: %s"), *C.GroupName, *Verdict));
			continue;
		}
		if (C.HiDrawableNode)
		{
			FString LodPath, LodVerdict, Why;
			UStaticMesh* LodMesh = ImportBuffer(SpellDrawableBuffer(C.Drawable, BaseShaders, C.Drawable->FindChildNode(TEXT("DrawableModelsHigh")), nullptr),
				Name + TEXT("_") + C.GroupName + TEXT("_lod1"), VehicleFolder, LodPath, LodVerdict);
			C.Mesh->SetNumSourceModels(1);
			{
				TArray<FStaticMaterial> Mats = C.Mesh->GetStaticMaterials();
				Mats.RemoveAll([](const FStaticMaterial& M) { return M.MaterialSlotName.ToString().StartsWith(TEXT("lod")); });
				C.Mesh->SetStaticMaterials(Mats);
			}
			if (!LodMesh || !AppendLod(C.Mesh, LodMesh, 1, Why))
			{
				Missing.Add(FString::Printf(TEXT("child %s LOD1 failed: %s"), *C.GroupName, LodMesh ? *Why : *LodVerdict));
			}
			C.Mesh->Build(true);
			C.Mesh->PostEditChange();
			C.Mesh->MarkPackageDirty();
		}
		TCHAR Side = 0; FString Axle;
		if (WheelSuffix(C.GroupName, Side, Axle))
		{
			ByAxle.Add(Axle, &C);
			if (!AnyWheel) { AnyWheel = &C; }
		}
	}
	auto PickPrototype = [&](const FString& Axle) -> const FChildRow*
	{
		if (FChildRow* const* Exact = ByAxle.Find(Axle)) { return *Exact; }
		if (Axle.StartsWith(TEXT("m"))) { if (FChildRow* const* Rear = ByAxle.Find(TEXT("r"))) { return *Rear; } }
		if (FChildRow* const* Front = ByAxle.Find(TEXT("f"))) { return *Front; }
		return AnyWheel;
	};

	// ---- 5) spawn: one actor, Body at identity, a component per child at its bone frame -----------
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FName IdTag(*(TEXT("RUDE_VEHICLE_COMPOSITE:") + Name));
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It) { if (It->Tags.Contains(IdTag)) { Stale.Add(*It); } }
		for (AActor* A : Stale) { World->DestroyActor(A); }
	}
	AActor* Actor = World->SpawnActor<AActor>();
	if (!Actor) { return Fail(TEXT("actor spawn failed")); }
	USceneComponent* RootComp = NewObject<USceneComponent>(Actor, TEXT("Root"));
	Actor->SetRootComponent(RootComp);
	RootComp->SetMobility(EComponentMobility::Static);
	RootComp->RegisterComponent();
	Actor->AddInstanceComponent(RootComp);
	Actor->SetActorLabel(TEXT("VehicleComposite_") + Name);
	Actor->SetFolderPath(FName(TEXT("RUDE_VEHICLES")));
	Actor->Tags.Add(IdTag);
	Actor->Tags.Add(FName(TEXT("RUDE_VEHICLE_ROOT")));
	const FString AssetName = Name + TEXT("_vehicle");
	const FString AssetPkgName = VehicleFolder / AssetName;
	Actor->Tags.Add(FName(*(TEXT("RUDE_VEHICLE_ASSET:") + AssetPkgName)));

	auto AddMesh = [&](UStaticMesh* Mesh, const FName& CompName, const FTransform& Xf) -> USceneComponent*
	{
		UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(Actor, CompName);
		SMC->SetStaticMesh(Mesh);
		SMC->SetMobility(EComponentMobility::Static);
		SMC->SetupAttachment(RootComp);
		SMC->SetRelativeTransform(Xf);
		SMC->RegisterComponent();
		Actor->AddInstanceComponent(SMC);
		return SMC;
	};
	auto AddFrame = [&](const FName& CompName, const FTransform& Xf) -> USceneComponent*
	{
		USceneComponent* SC = NewObject<USceneComponent>(Actor, CompName);
		SC->SetMobility(EComponentMobility::Static);
		SC->SetupAttachment(RootComp);
		SC->SetRelativeTransform(Xf);
		SC->RegisterComponent();
		Actor->AddInstanceComponent(SC);
		return SC;
	};
	AddMesh(Body, TEXT("Body"), FTransform::Identity);

	int32 ChildComponents = 0;
	for (FChildRow& C : Children)
	{
		const FTransform Xf = C.BoneIndex >= 0 ? GtaToUe(WorldGta[C.BoneIndex]) : FTransform::Identity;
		// Index + name: NewObject with a name already used under the same outer is a hard error, and
		// burrito has 28 children over 22 groups (a group name repeats).
		const FName CompName(*FString::Printf(TEXT("Child_%02d_%s"), C.Index, *C.GroupName));
		TCHAR Side = 0; FString Axle;
		const bool bWheel = WheelSuffix(C.GroupName, Side, Axle);
		// A non-wheel child with geometry rides its bone as a mesh; a wheel child is a frame here (its
		// mesh is instanced at every wheel bone below); a geometry-less child is the bone frame + its
		// bound row in the DataAsset - the pivot a door swings on.
		USceneComponent* Comp = (C.Mesh && !bWheel) ? AddMesh(C.Mesh, CompName, Xf) : AddFrame(CompName, Xf);
		if (Comp) { ++ChildComponents; C.ComponentName = CompName.ToString(); }
	}

	// wheels: ImportVehicle's rule - the one authored wheel at every wheel_* bone, mirrored across
	// local X for the other side (the axle IS local X; UE reverses culling for a negative determinant).
	int32 WheelsPlaced = 0, WheelsMirrored = 0;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		TCHAR Side = 0; FString Axle;
		if (!WheelSuffix(Bones[i].Name, Side, Axle)) { continue; }
		const FChildRow* Proto = PickPrototype(Axle);
		if (!Proto || !Proto->Mesh)
		{
			Missing.Add(FString::Printf(TEXT("bone %s has no wheel mesh to place"), *Bones[i].Name));
			continue;
		}
		FTransform Xf = GtaToUe(WorldGta[i]);
		TCHAR ProtoSide = 0; FString ProtoAxle;
		WheelSuffix(Proto->GroupName, ProtoSide, ProtoAxle);
		if (Side != ProtoSide)
		{
			Xf = FTransform(FQuat::Identity, FVector::ZeroVector, FVector(-1.f, 1.f, 1.f)) * Xf;
			++WheelsMirrored;
		}
		AddMesh(Proto->Mesh, FName(*FString::Printf(TEXT("Wheel_%d_%s"), i, *Bones[i].Name)), Xf);
		++WheelsPlaced;
	}

	// ---- 6) liveries: <veh>_sign_<n> in the ytds; the shader that binds one; carvariations flags --
	auto YtdNames = [&](const FString& Path, TArray<FString>& Out)
	{
		if (Path.IsEmpty() || !FPaths::FileExists(Path)) { return; }
		FXmlFile Doc(Path);
		const FXmlNode* R = Doc.IsValid() ? Doc.GetRootNode() : nullptr;
		if (!R) { return; }
		for (const FXmlNode* It : R->GetChildrenNodes())
		{
			if (const FXmlNode* N = It->FindChildNode(TEXT("Name"))) { Out.Add(N->GetContent().TrimStartAndEnd()); }
		}
	};
	TArray<FString> YtdTex, HiYtdTex;
	YtdNames(YtdPath, YtdTex);
	YtdNames(HiYtdPath, HiYtdTex);
	const FString SignPrefix = Name + TEXT("_sign_");
	TMap<int32, FRudeVehicleLivery> LiveryByIndex;
	auto Collect = [&](const TArray<FString>& Names, bool bHi)
	{
		for (const FString& T : Names)
		{
			if (!T.StartsWith(SignPrefix, ESearchCase::IgnoreCase)) { continue; }
			const int32 K = FCString::Atoi(*T.Mid(SignPrefix.Len()));
			if (K <= 0) { continue; }
			FRudeVehicleLivery& L = LiveryByIndex.FindOrAdd(K - 1);
			L.Index = K - 1;
			if (bHi) { L.HiTextureName = T; } else { L.TextureName = T; }
		}
	};
	Collect(YtdTex, false);
	Collect(HiYtdTex, true);
	int32 LiveryShaderIndex = -1;
	FString LiveryPreset, LiverySampler;
	if (const FXmlNode* SG = Lod0Shaders)
	{
		if (const FXmlNode* Sh = SG->FindChildNode(TEXT("Shaders")))
		{
			int32 si = 0;
			for (const FXmlNode* Item : Sh->GetChildrenNodes())
			{
				if (const FXmlNode* Params = Item->FindChildNode(TEXT("Parameters")))
				{
					for (const FXmlNode* P : Params->GetChildrenNodes())
					{
						if (P->GetAttribute(TEXT("type")) != TEXT("Texture")) { continue; }
						const FXmlNode* TN = P->FindChildNode(TEXT("Name"));
						if (TN && TN->GetContent().TrimStartAndEnd().StartsWith(SignPrefix, ESearchCase::IgnoreCase) && LiveryShaderIndex < 0)
						{
							LiveryShaderIndex = si;
							LiverySampler = P->GetAttribute(TEXT("name"));
							const FXmlNode* SN = Item->FindChildNode(TEXT("Name"));
							LiveryPreset = SN ? SN->GetContent().TrimStartAndEnd() : FString();
						}
					}
				}
				++si;
			}
		}
	}
	// the body material slots (<preset>__<geo>, LOD0 geometry ordinal) that use the livery shader
	TArray<FString> LiverySlots;
	if (LiveryShaderIndex >= 0)
	{
		int32 Geo = 0;
		for (const FXmlNode* ModelItem : Lod0Models->GetChildrenNodes())
		{
			if (const FXmlNode* Geometries = ModelItem->FindChildNode(TEXT("Geometries")))
			{
				for (const FXmlNode* G : Geometries->GetChildrenNodes())
				{
					if (FCString::Atoi(*Attr(G->FindChildNode(TEXT("ShaderIndex")), TEXT("value"), TEXT("-1"))) == LiveryShaderIndex)
					{
						LiverySlots.Add(FString::Printf(TEXT("%s__%d"), *LiveryPreset, Geo));
					}
					++Geo;
				}
			}
		}
	}
	// the RUDE master parameter that sampler lands on (the same table ImportDrawableNode binds by)
	FString LiveryParam;
	if (LiverySampler == TEXT("DiffuseSampler")) { LiveryParam = TEXT("Diffuse"); }
	else if (LiverySampler == TEXT("BumpSampler")) { LiveryParam = TEXT("Normal"); }
	else if (LiverySampler == TEXT("SpecSampler")) { LiveryParam = TEXT("Specular"); }
	else if (LiverySampler == TEXT("DetailSampler")) { LiveryParam = TEXT("Detail"); }
	// Added on the live tree by another session (2026-09-06, "the L masters"): a second diffuse for the
	// vehicle_paint3-class liveries. Kept as authored; SetVehicleLivery still checks the slot's master
	// actually exposes the parameter before writing it, so a master without Diffuse2 refuses by name.
	else if (LiverySampler == TEXT("DiffuseSampler2")) { LiveryParam = TEXT("Diffuse2"); }   // the L masters (2026-09-06)

	// ---- 7) the three metadata rows, joined the way the game joins them ----------------------------
	int32 MetaFilesSearched = 0;
	FMetaHit VehHit = FindMetaItem(*Corpus, TEXT("meta"), TEXT("vehicles"), TEXT("modelName"), Name, MetaFilesSearched);
	const FString HandlingId = VehHit.bFound ? VehHit.Fields.FindRef(TEXT("handlingId")) : FString();
	FMetaHit HandHit = FindMetaItem(*Corpus, TEXT("meta"), TEXT("handling"), TEXT("handlingName"), HandlingId, MetaFilesSearched);
	FMetaHit CarHit = FindMetaItem(*Corpus, TEXT("meta"), TEXT("carvariations"), TEXT("modelName"), Name, MetaFilesSearched);
	{
		FMetaHit Pso = FindMetaItem(*Corpus, TEXT("ymt"), TEXT("carvariations"), TEXT("modelName"), Name, MetaFilesSearched);
		if (Pso.bFound && (!CarHit.bFound || Pso.SlotRank >= CarHit.SlotRank)) { CarHit = Pso; }
	}
	if (!VehHit.bFound) { Missing.Add(FString::Printf(TEXT("no vehicles.meta row with modelName %s in any copy (%d files searched)"), *Name, MetaFilesSearched)); }
	if (VehHit.bFound && !HandHit.bFound) { Missing.Add(FString::Printf(TEXT("no handling.meta row with handlingName %s"), *HandlingId)); }
	if (!CarHit.bFound) { Missing.Add(FString::Printf(TEXT("no carvariations row with modelName %s (meta or PSO)"), *Name)); }
	// livery flags: colors/Item[n]/liveries as one text list (PSO) or liveries/Item[m] (meta)
	TSet<int32> Enabled;
	for (const TPair<FString, FString>& KV : CarHit.Fields)
	{
		if (!KV.Key.Contains(TEXT("/liveries"))) { continue; }
		if (KV.Key.EndsWith(TEXT("/liveries")))
		{
			TArray<FString> Bools;
			KV.Value.ParseIntoArrayWS(Bools);
			for (int32 i = 0; i < Bools.Num(); ++i) { if (Bools[i].Equals(TEXT("true"), ESearchCase::IgnoreCase)) { Enabled.Add(i); } }
		}
		else
		{
			int32 Lb = INDEX_NONE;
			if (KV.Key.FindLastChar(TEXT('['), Lb) && KV.Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
			{
				Enabled.Add(FCString::Atoi(*KV.Key.Mid(Lb + 1)));
			}
		}
	}
	for (int32 E : Enabled)
	{
		FRudeVehicleLivery& L = LiveryByIndex.FindOrAdd(E);
		L.Index = E;
		L.bEnabledByCarVariations = true;
	}
	TArray<int32> LiveryKeys;
	LiveryByIndex.GetKeys(LiveryKeys);
	LiveryKeys.Sort();

	// ---- 8) the DataAsset ---------------------------------------------------------------------------
	UPackage* APkg = CreatePackage(*AssetPkgName);
	if (!APkg) { return Fail(TEXT("CreatePackage failed for the vehicle asset")); }
	APkg->FullyLoad();
	URudeVehicle* VA = FindObject<URudeVehicle>(APkg, *AssetName);
	const bool bNewAsset = VA == nullptr;
	if (!VA) { VA = NewObject<URudeVehicle>(APkg, FName(*AssetName), RF_Public | RF_Standalone); }
	if (!VA) { return Fail(TEXT("NewObject<URudeVehicle> failed")); }
	VA->VehicleName = Name;
	VA->BodyMesh = Body;
	VA->LodCount = LodCount;
	VA->LodSources = LodSources;
	VA->LodAssets = LodAssets;
	VA->BoneTags.Reset();
	for (const FBone& B : Bones) { VA->BoneTags.Add(FName(*B.Name), B.Tag); }
	VA->BoneCount = Bones.Num();
	VA->WheelBoneCount = WheelBones;
	VA->Children.Reset();
	for (const FChildRow& C : Children)
	{
		FRudeVehicleChild R;
		R.ChildIndex = C.Index; R.GroupIndex = C.GroupIndex; R.GroupName = C.GroupName; R.ParentGroupIndex = C.ParentGroup;
		R.BoneTag = C.BoneTag; R.BoneIndex = C.BoneIndex; R.BoneName = C.BoneIndex >= 0 ? Bones[C.BoneIndex].Name : FString();
		R.bHasGeometry = C.bGeometry; R.Mesh = C.Mesh; R.ComponentName = C.ComponentName;
		R.BoundType = C.BoundType; R.BoundBoxMin = C.BoxMin; R.BoundBoxMax = C.BoxMax;
		R.PristineMass = C.PristineMass; R.DamagedMass = C.DamagedMass;
		VA->Children.Add(MoveTemp(R));
	}
	VA->BoundTypeCounts = BoundTypeCounts;
	VA->Liveries.Reset();
	for (int32 K : LiveryKeys) { VA->Liveries.Add(LiveryByIndex[K]); }
	VA->LiveryShaderIndex = LiveryShaderIndex;
	VA->LiveryShaderPreset = LiveryPreset;
	VA->LiverySampler = LiverySampler;
	VA->LiveryMaterialParameter = LiveryParam;
	VA->LiveryMaterialSlots = LiverySlots;
	VA->bHasLiveryFlag = VehHit.bFound && VehHit.Fields.FindRef(TEXT("flags")).Contains(TEXT("FLAG_HAS_LIVERY"));
	VA->VehiclesMeta = VehHit.Fields;
	VA->HandlingId = HandlingId;
	VA->Handling = HandHit.Fields;
	VA->CarVariations = CarHit.Fields;
	VA->SourceYft = BasePath; VA->SourceHiYft = HiPath; VA->SourceYtd = YtdPath; VA->SourceHiYtd = HiYtdPath;
	VA->SourceVehiclesMeta = VehHit.Path; VA->SourceHandling = HandHit.Path; VA->SourceCarVariations = CarHit.Path;
	VA->VehiclesMetaXml = VehHit.Xml; VA->HandlingXml = HandHit.Xml; VA->CarVariationsXml = CarHit.Xml;
	VA->MarkPackageDirty();
	if (bNewAsset)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(VA);
	}

	// ---- 9) verdict: ok is COMPUTED - body + every child placed + wheels where the skeleton has them
	FString BoundJson;
	for (const TPair<FString, int32>& KV : BoundTypeCounts)
	{
		BoundJson += FString::Printf(TEXT("%s\"%s\":%d"), BoundJson.IsEmpty() ? TEXT("") : TEXT(","), *JsonEscape(KV.Key), KV.Value);
	}
	TArray<FString> LiveryNames;
	for (int32 K : LiveryKeys)
	{
		const FRudeVehicleLivery& L = LiveryByIndex[K];
		LiveryNames.Add(L.TextureName.IsEmpty() ? L.HiTextureName : L.TextureName);
	}
	const int32 MissingListCap = 20;
	const bool bWheelsIntact = (WheelBones == 0) || (WheelsPlaced > 0);
	const bool bOk = bBodyVerdictOk && bWheelsIntact && ChildComponents == Children.Num() && LodFailed == 0;
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"vehicle\":\"%s\",\"actor\":\"%s\",\"asset\":\"%s\",\"bodyAsset\":\"%s\",\"bodyGeos\":%d,"
		"\"bodyMissingTextures\":%d,\"bodyVerdictOk\":%s,\"hiFragment\":%s,\"lodCount\":%d,\"lodSources\":[%s],\"lodFailed\":%d,"
		"\"bonesRead\":%d,\"wheelBones\":%d,\"wheelsPlaced\":%d,\"wheelsMirrored\":%d,"
		"\"children\":%d,\"childrenWithGeometry\":%d,\"childComponents\":%d,\"boundChildren\":%d,\"boundTypes\":{%s},\"boundsSkippedDisc\":%d,"
		"\"liveries\":%d,\"liveryTextures\":[%s],\"liveryShaderIndex\":%d,\"liveryShader\":\"%s\",\"liverySampler\":\"%s\","
		"\"liveryParameter\":\"%s\",\"liverySlots\":%d,\"hasLiveryFlag\":%s,"
		"\"handlingId\":\"%s\",\"handlingFields\":%d,\"vehiclesMetaFields\":%d,\"carVariationsFields\":%d,"
		"\"metaFilesSearched\":%d,\"sourceHandling\":\"%s\",\"sourceCarVariations\":\"%s\","
		"\"missingCount\":%d,\"missingTruncated\":%d,\"missing\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"),
		*Name, *JsonEscape(Actor->GetActorLabel()), *JsonEscape(AssetPkgName), *JsonEscape(BodyAssetPath), BodyGeos,
		BodyMissingTex, bBodyVerdictOk ? TEXT("true") : TEXT("false"), HiDrawable ? TEXT("true") : TEXT("false"), LodCount, *JsonStrings(LodSources), LodFailed,
		Bones.Num(), WheelBones, WheelsPlaced, WheelsMirrored,
		Children.Num(), ChildrenWithGeometry, ChildComponents, BoundChildren.Num(), *BoundJson, BoundsSkippedDisc,
		LiveryKeys.Num(), *JsonStrings(LiveryNames), LiveryShaderIndex, *JsonEscape(LiveryPreset), *JsonEscape(LiverySampler),
		*JsonEscape(LiveryParam), LiverySlots.Num(), VA->bHasLiveryFlag ? TEXT("true") : TEXT("false"),
		*JsonEscape(HandlingId), HandHit.Fields.Num(), VehHit.Fields.Num(), CarHit.Fields.Num(),
		MetaFilesSearched, *JsonEscape(HandHit.Path), *JsonEscape(CarHit.Path),
		Missing.Num(), FMath::Max(0, Missing.Num() - MissingListCap), *JsonStrings(Missing, MissingListCap));
}

FString URudeToolset::SetVehicleLivery(const FString& ActorLabel, const FString& LiveryIndex)
{
	using namespace RudeVehicle;
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *JsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	AActor* Actor = FindVehicleActor(World, ActorLabel.TrimStartAndEnd());
	if (!Actor) { return Fail(FString::Printf(TEXT("no vehicle actor labelled '%s' (ImportVehicleComposite labels them VehicleComposite_<name>)"), *ActorLabel)); }
	FString AssetPath;
	for (const FName& T : Actor->Tags)
	{
		const FString S = T.ToString();
		if (S.StartsWith(TEXT("RUDE_VEHICLE_ASSET:"))) { AssetPath = S.Mid(19); }
	}
	URudeVehicle* VA = AssetPath.IsEmpty() ? nullptr : LoadObject<URudeVehicle>(nullptr, *AssetPath);
	if (!VA) { return Fail(TEXT("that actor carries no RUDE vehicle asset (was it built by ImportVehicleComposite?)")); }
	if (VA->Liveries.Num() == 0) { return Fail(FString::Printf(TEXT("%s has no liveries: no <veh>_sign_<n> texture in its ytds and no carvariations livery flag"), *VA->VehicleName)); }
	const int32 Idx = FCString::Atoi(*LiveryIndex.TrimStartAndEnd());
	const FRudeVehicleLivery* L = VA->Liveries.FindByPredicate([&](const FRudeVehicleLivery& X) { return X.Index == Idx; });
	if (!L) { return Fail(FString::Printf(TEXT("livery %d is not one of this vehicle's %d (indices are the DataAsset's Liveries[].Index)"), Idx, VA->Liveries.Num())); }
	if (VA->LiveryShaderIndex < 0) { return Fail(TEXT("no fragment shader binds a _sign_ texture, so there is no material slot to swap")); }
	if (VA->LiveryMaterialParameter.IsEmpty())
	{
		return Fail(FString::Printf(TEXT("the livery rides sampler %s on %s, which the RUDE masters expose no parameter for (the sampler table maps Diffuse/Bump/Spec/DetailSampler) - a %s master with a second diffuse is needed first"),
			*VA->LiverySampler, *VA->LiveryShaderPreset, *VA->LiveryShaderPreset));
	}
	const FString WantName = L->HiTextureName.IsEmpty() ? L->TextureName : L->HiTextureName;
	if (WantName.IsEmpty()) { return Fail(FString::Printf(TEXT("livery %d is flagged in carvariations but no %s_sign_%d texture exists in the ytds"), Idx, *VA->VehicleName, Idx + 1)); }

	// the texture asset by name under /Game/RUDE/Textures, preferring the vehicle's +hi then base dictionary
	UTexture2D* Tex = nullptr;
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> Found;
		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Game/RUDE/Textures"));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
		ARM.Get().GetAssets(Filter, Found);
		const FAssetData* Best = nullptr;
		int32 BestScore = -1;
		for (const FAssetData& AD : Found)
		{
			for (const FString& Candidate : { L->HiTextureName, L->TextureName })
			{
				if (Candidate.IsEmpty() || !AD.AssetName.ToString().Equals(Candidate, ESearchCase::IgnoreCase)) { continue; }
				const FString Pkg = AD.PackagePath.ToString();
				const int32 Score = Pkg.EndsWith(TEXT("/") + VA->VehicleName + TEXT("+hi")) ? 3 : Pkg.EndsWith(TEXT("/") + VA->VehicleName) ? 2 : 1;
				if (Score > BestScore) { BestScore = Score; Best = &AD; }
			}
		}
		if (Best) { Tex = Cast<UTexture2D>(Best->GetAsset()); }
	}
	if (!Tex)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"textureMissing\":\"%s\",\"error\":\"livery texture %s is not imported under /Game/RUDE/Textures - ImportYtd %s.ytd / %s+hi.ytd first (the 2026-09-04 corpus carries no pixel sidecars for vehicles.rpf ytds, measured 2026-09-06)\"}"),
			*JsonEscape(WantName), *JsonEscape(WantName), *VA->VehicleName, *VA->VehicleName);
	}

	// every body slot that uses the livery shader: LOD0 by the recorded slot names, LOD1.. by preset
	UStaticMeshComponent* BodyComp = nullptr;
	for (UActorComponent* AC : Actor->GetComponents())
	{
		if (AC && AC->GetName() == TEXT("Body")) { BodyComp = Cast<UStaticMeshComponent>(AC); }
	}
	UStaticMesh* Mesh = BodyComp ? BodyComp->GetStaticMesh() : nullptr;
	if (!Mesh) { return Fail(TEXT("the actor has no Body static mesh component")); }
	int32 SlotsUpdated = 0, SlotsWithoutInstance = 0, SlotsUnsupported = 0;
	const FName Param(*VA->LiveryMaterialParameter);
	for (const FStaticMaterial& M : Mesh->GetStaticMaterials())
	{
		FString Slot = M.MaterialSlotName.ToString();
		bool bLivery = VA->LiveryMaterialSlots.Contains(Slot);
		if (!bLivery && Slot.StartsWith(TEXT("lod")))
		{
			int32 Sep = INDEX_NONE;
			if (Slot.FindChar(TEXT('_'), Sep)) { Slot = Slot.Mid(Sep + 2); }
			bLivery = Slot.StartsWith(VA->LiveryShaderPreset + TEXT("__"));
		}
		if (!bLivery) { continue; }
		UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(M.MaterialInterface);
		if (!MIC) { ++SlotsWithoutInstance; continue; }
		TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Ids;
		MIC->GetAllTextureParameterInfo(Infos, Ids);
		bool bHas = false;
		for (const FMaterialParameterInfo& I : Infos) { if (I.Name == Param) { bHas = true; break; } }
		if (!bHas) { ++SlotsUnsupported; continue; }
		MIC->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Param), Tex);
		MIC->PostEditChange();
		MIC->MarkPackageDirty();
		++SlotsUpdated;
	}
	VA->CurrentLivery = Idx;
	for (FRudeVehicleLivery& X : VA->Liveries) { if (X.Index == Idx) { X.Texture = Tex; } }
	VA->MarkPackageDirty();
	return FString::Printf(TEXT("{\"ok\":%s,\"vehicle\":\"%s\",\"livery\":%d,\"texture\":\"%s\",\"textureAsset\":\"%s\",\"parameter\":\"%s\","
		"\"slotsUpdated\":%d,\"slotsWithoutInstance\":%d,\"slotsUnsupported\":%d,\"note\":\"material instances are per mesh, so every placed copy of this body shows the livery\"}"),
		SlotsUpdated > 0 ? TEXT("true") : TEXT("false"), *VA->VehicleName, Idx, *JsonEscape(WantName), *JsonEscape(Tex->GetPathName()),
		*JsonEscape(VA->LiveryMaterialParameter), SlotsUpdated, SlotsWithoutInstance, SlotsUnsupported);
}
