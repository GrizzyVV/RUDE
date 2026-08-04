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
	const FString XmlPath = CorpusRoot / TEXT("yft") / (Name + TEXT(".yft.xml"));
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
	TArray<FBone> Bones;
	if (const FXmlNode* Skel = DrawableNode->FindChildNode(TEXT("Skeleton")))
	{
		if (const FXmlNode* BoneList = Skel->FindChildNode(TEXT("Bones")))
		{
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
				if (S.GetAbsMin() < UE_KINDA_SMALL_NUMBER)   // the macro without UE_ is deprecated
				{
					// Bone scale is unit in 270/270 measured wheel bones. A zero component means
					// the XML lost an attribute, and a collapsed bone frame would place a wheel
					// as an invisible sliver - fail where it is diagnosable instead.
					return Fail(FString::Printf(TEXT("bone '%s' has a degenerate <Scale> "
						"(%s) - refusing rather than placing parts on a collapsed frame"),
						*B.Name, *S.ToString()));
				}
				B.LocalGta = FTransform(Q.GetNormalized(),
					Vec3(It->FindChildNode(TEXT("Translation")), FVector::ZeroVector), S);
				Bones.Add(MoveTemp(B));
			}
		}
	}
	if (Bones.Num() == 0)
	{
		// LOUD, and it names the fix: the extras are opt-in on the QUARRY side, so an old corpus
		// file simply has no <Skeleton> and there is nothing here to place wheels on.
		return Fail(FString::Printf(TEXT("%s has no <Drawable><Skeleton><Bones> - regenerate it "
			"with QUARRY's yft2xml --extras (python quarry/yft2xml.py <%s.yft> --extras --out "
			"<corpus>/yft), then re-run"), *XmlPath, *Name));
	}

	// Compose each bone's MODEL-space frame by walking its parent chain. Memoised, with an
	// in-progress marker so a malformed parent cycle refuses instead of recursing forever.
	TArray<FTransform> WorldGta;
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
			// UE's FTransform composition is child-then-parent: Child * Parent.
			WorldGta[i] = Bones[i].LocalGta * WorldGta[P];
		}
		State[i] = 2;
		return true;
	};
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		if (!Resolve(i))
		{
			return Fail(FString::Printf(TEXT("bone %d (%s) sits in a parent CYCLE - refusing "
				"rather than placing wheels off a half-composed frame"), i, *Bones[i].Name));
		}
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
					// collision-only stubs (17 of the adder's 18 children).
					if (!It->FindChildNode(TEXT("Drawable"))) { continue; }
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
					C.SidecarPath = CorpusRoot / TEXT("yft") / Name /
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
