// RUDE - RAGE <-> Unreal Development Environment
//
// THE CHAOS TEST-DRIVE, editor side (GDD Tier 2, WP11). Runtime classes: RudeCore/RudeDriveablePawn.h.
// Design + the handling.meta -> Chaos table with its honesty tags: scratchpad/wp11/drive/DESIGN.md.
//
// From a VehicleComposite_<name> actor (ImportVehicleComposite) and its URudeVehicle asset:
//   1. the yft skeleton, re-read from the asset's SourceYft (the DataAsset keeps tags, not frames): every bone with
//      its LOCAL TRS through the vehicle lane's proven GtaToUe map; the ROOT gets a +90 deg yaw folded in so the
//      car's nose is the pawn's +X (Chaos drives along +X; RUDE space has the nose at -Y)
//   2. a USkeletalMesh: the composite's `Body` static mesh (LOD0), every vertex weighted 1.0 to `chassis`; every
//      placed `Wheel_<i>_<bone>` component's mesh (LOD0) at its placed frame - the other side's mirror folded into
//      the vertices, winding flipped - weighted 1.0 to that wheel bone; materials borrowed per slot name
//   3. a UPhysicsAsset with ONE body on the root bone: the body mesh's convex collision when it has any, else the
//      body's bounds box (counted either way)
//   4. an ARudeDriveablePawn beside the composite: one FChaosWheelSetup per wheel_* bone (front axle = the
//      URudeDriveWheelFront class, steers; the rest URudeDriveWheelRear, handbrake), the per-wheel numbers, and the
//      handling row mapped onto Chaos. Every mapped line is written to the component's MappingNotes with its tag.
// Verdict: counts for every step, the fields the handling row lacked (defaults used, named), the wheel radii read
// off the meshes, the chassis shape, ground clearance, and ok computed from body + wheels + physics + pawn.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeVehicleAsset.h"
#include "RudeDriveablePawn.h"

#include "Animation/Skeleton.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "SkinWeightsAttributesRef.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "XmlFile.h"

namespace RudeDrive
{
	static FString Fail(const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); }

	static FString Attr(const FXmlNode* N, const TCHAR* Key, const TCHAR* Def)
	{
		if (!N) { return Def; }
		const FString V = N->GetAttribute(Key);
		return V.IsEmpty() ? FString(Def) : V;
	}
	static FVector Vec3(const FXmlNode* N, const FVector& Def)
	{
		if (!N) { return Def; }
		return FVector(FCString::Atod(*Attr(N, TEXT("x"), TEXT("0"))), FCString::Atod(*Attr(N, TEXT("y"), TEXT("0"))), FCString::Atod(*Attr(N, TEXT("z"), TEXT("0"))));
	}
	static FString JsonStrings(const TArray<FString>& In, int32 Cap = 0)
	{
		FString O;
		for (int32 i = 0; i < In.Num() && (Cap == 0 || i < Cap); ++i) { O += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *RudeJsonEscape(In[i])); }
		return O;
	}
	static FString JsonFloats(const TArray<float>& In)
	{
		FString O;
		for (int32 i = 0; i < In.Num(); ++i) { O += FString::Printf(TEXT("%s%.2f"), i ? TEXT(",") : TEXT(""), In[i]); }
		return O;
	}

	struct FBone
	{
		FString Name;
		int32 Tag = -1;
		int32 Parent = -1;
		FTransform LocalGta = FTransform::Identity;
	};

	// GTA metres -> UE centimetres with the pinned Y mirror: the vehicle lane's measured bone map (RudeVehicle.cpp
	// GtaToUe - forward orientation, plain mirror). The vertices went through the same (x*100, -y*100, z*100).
	static FTransform GtaToUe(const FTransform& G)
	{
		const FQuat Q = G.GetRotation();
		const FVector T = G.GetTranslation();
		return FTransform(FQuat(-Q.X, Q.Y, -Q.Z, Q.W).GetNormalized(), FVector(T.X * 100.0, -T.Y * 100.0, T.Z * 100.0), G.GetScale3D());
	}

	// <Fragment><Drawable><Skeleton><Bones>: the vehicle lane's ParseSkeleton, with its refusals (degenerate scale,
	// a parent that does not precede its child - both hold on every measured vehicle; a violation is a broken file).
	static bool ReadSkeleton(const FXmlNode* Drawable, TArray<FBone>& Out, FString& Err)
	{
		Out.Reset();
		const FXmlNode* Skel = Drawable ? Drawable->FindChildNode(TEXT("Skeleton")) : nullptr;
		const FXmlNode* List = Skel ? Skel->FindChildNode(TEXT("Bones")) : nullptr;
		if (!List) { Err = TEXT("the fragment has no <Drawable><Skeleton><Bones>"); return false; }
		for (const FXmlNode* It : List->GetChildrenNodes())
		{
			FBone B;
			if (const FXmlNode* N = It->FindChildNode(TEXT("Name"))) { B.Name = N->GetContent().TrimStartAndEnd(); }
			B.Tag = FCString::Atoi(*Attr(It->FindChildNode(TEXT("Tag")), TEXT("value"), TEXT("-1")));
			B.Parent = FCString::Atoi(*Attr(It->FindChildNode(TEXT("ParentIndex")), TEXT("value"), TEXT("-1")));
			const FXmlNode* R = It->FindChildNode(TEXT("Rotation"));
			const FQuat Q(FCString::Atod(*Attr(R, TEXT("x"), TEXT("0"))), FCString::Atod(*Attr(R, TEXT("y"), TEXT("0"))),
			              FCString::Atod(*Attr(R, TEXT("z"), TEXT("0"))), FCString::Atod(*Attr(R, TEXT("w"), TEXT("1"))));
			const FVector S = Vec3(It->FindChildNode(TEXT("Scale")), FVector::OneVector);
			if (S.GetAbsMin() < UE_KINDA_SMALL_NUMBER) { Err = FString::Printf(TEXT("bone '%s' has a degenerate <Scale> %s"), *B.Name, *S.ToString()); return false; }
			if (B.Parent >= Out.Num()) { Err = FString::Printf(TEXT("bone '%s' (index %d) names parent %d, which does not precede it"), *B.Name, Out.Num(), B.Parent); return false; }
			B.LocalGta = FTransform(Q.GetNormalized(), Vec3(It->FindChildNode(TEXT("Translation")), FVector::ZeroVector), S);
			Out.Add(MoveTemp(B));
		}
		if (Out.Num() == 0) { Err = TEXT("the skeleton has no bones"); return false; }
		return true;
	}

	// "wheel_lf" -> ('l', "f"); "wheelmesh_lf" is not a wheel (the vehicle lane's rule).
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

	static AActor* FindCompositeActor(UWorld* World, const FString& Label)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(FName(TEXT("RUDE_VEHICLE_ROOT"))) && It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)) { return *It; }
		}
		return nullptr;
	}

	// Copy one static mesh's LOD0 into the skeletal mesh description: positions through Xf (the component's placed
	// frame, mirror scale included) then Drive (the +90 yaw), every vertex weighted 1.0 to Bone, normals through the
	// same rotation with the mirror's sign flip, winding reversed when the placed frame is a mirror (negative
	// determinant), polygon groups keyed by SlotPrefix + the source slot name, materials borrowed by slot name.
	static bool AppendStaticMesh(FMeshDescription& MD, FSkeletalMeshAttributes& A, const UStaticMesh* SM, const FTransform& Xf, const FTransform& Drive,
	                             uint16 Bone, const FString& SlotPrefix, TArray<FSkeletalMaterial>& Mats, TMap<FName, FPolygonGroupID>& Groups,
	                             int32& OutVerts, int32& OutTris, FBox& OutBox, FString& Why)
	{
		const FMeshDescription* SD = SM ? SM->GetMeshDescription(0) : nullptr;
		if (!SD) { Why = FString::Printf(TEXT("%s has no LOD0 mesh description"), *GetNameSafe(SM)); return false; }
		FStaticMeshConstAttributes SA(*SD);
		TVertexAttributesConstRef<FVector3f> SPos = SA.GetVertexPositions();
		TVertexInstanceAttributesConstRef<FVector3f> SNrm = SA.GetVertexInstanceNormals();
		TVertexInstanceAttributesConstRef<FVector2f> SUV = SA.GetVertexInstanceUVs();
		TVertexInstanceAttributesConstRef<FVector4f> SCol = SA.GetVertexInstanceColors();
		TPolygonGroupAttributesConstRef<FName> SSlots = SA.GetPolygonGroupMaterialSlotNames();
		const int32 UVChannels = FMath::Min(SUV.IsValid() ? SUV.GetNumChannels() : 0, 2);
		TVertexAttributesRef<FVector3f> DPos = A.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> DNrm = A.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector2f> DUV = A.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector4f> DCol = A.GetVertexInstanceColors();
		TPolygonGroupAttributesRef<FName> DSlots = A.GetPolygonGroupMaterialSlotNames();
		FSkinWeightsVertexAttributesRef Skin = A.GetVertexSkinWeights();
		const FTransform Full = Xf * Drive;                     // Xf first, then Drive (Drive is a pure rotation: exact)
		const FVector Sign = Xf.GetScale3D().GetSignVector();   // the mirror, applied to normals in the mesh's own frame
		const bool bFlip = Xf.GetDeterminant() < 0.0;
		const FQuat NrmRot = Full.GetRotation();
		const TArray<FStaticMaterial>& SMats = SM->GetStaticMaterials();
		const float One = 1.f;
		auto GroupFor = [&](FPolygonGroupID SG) -> FPolygonGroupID
		{
			const FName SrcSlot = SSlots.IsValid() ? SSlots[SG] : NAME_None;
			const FName Slot(*(SlotPrefix + SrcSlot.ToString()));
			if (const FPolygonGroupID* Found = Groups.Find(Slot)) { return *Found; }
			UMaterialInterface* MI = nullptr;
			for (const FStaticMaterial& M : SMats) { if (M.MaterialSlotName == SrcSlot || M.ImportedMaterialSlotName == SrcSlot) { MI = M.MaterialInterface; break; } }
			if (!MI && SMats.IsValidIndex(SG.GetValue())) { MI = SMats[SG.GetValue()].MaterialInterface; }
			Mats.Add(FSkeletalMaterial(MI, true, false, Slot, Slot));
			const FPolygonGroupID G = MD.CreatePolygonGroup();
			DSlots[G] = Slot;
			Groups.Add(Slot, G);
			return G;
		};
		TMap<FVertexID, FVertexID> VMap;
		VMap.Reserve(SD->Vertices().Num());
		for (const FVertexID V : SD->Vertices().GetElementIDs())
		{
			const FVector P = Full.TransformPosition(FVector(SPos[V]));
			const FVertexID NV = MD.CreateVertex();
			DPos[NV] = FVector3f(P);
			Skin.Set(NV, UE::AnimationCore::FBoneWeights::Create(&Bone, &One, 1));
			VMap.Add(V, NV);
			OutBox += P;
			++OutVerts;
		}
		for (const FTriangleID T : SD->Triangles().GetElementIDs())
		{
			TArrayView<const FVertexInstanceID> Src = SD->GetTriangleVertexInstances(T);
			if (Src.Num() != 3) { continue; }
			const FPolygonGroupID G = GroupFor(SD->GetTrianglePolygonGroup(T));
			TArray<FVertexInstanceID> Inst;
			Inst.Reserve(3);
			for (int32 c = 0; c < 3; ++c)
			{
				const FVertexInstanceID SI = Src[bFlip ? 2 - c : c];
				const FVertexInstanceID DI = MD.CreateVertexInstance(VMap[SD->GetVertexInstanceVertex(SI)]);
				const FVector N = NrmRot.RotateVector(FVector(SNrm[SI]) * Sign).GetSafeNormal();
				DNrm[DI] = FVector3f(N);
				for (int32 ch = 0; ch < 2; ++ch) { DUV.Set(DI, ch, ch < UVChannels ? SUV.Get(SI, ch) : FVector2f::ZeroVector); }
				DCol[DI] = SCol.IsValid() ? SCol[SI] : FVector4f(1.f, 1.f, 1.f, 1.f);
				Inst.Add(DI);
			}
			MD.CreateTriangle(G, Inst);
			++OutTris;
		}
		return true;
	}
}

FString URudeToolset::BuildDriveable(const FString& ActorLabel, const FString& LocationCm)
{
	using namespace RudeDrive;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString Label = ActorLabel.TrimStartAndEnd();
	if (Label.IsEmpty()) { return Fail(TEXT("give the composite actor's label, e.g. VehicleComposite_blista")); }
	AActor* Actor = FindCompositeActor(World, Label);
	if (!Actor) { return Fail(FString::Printf(TEXT("no vehicle actor labelled '%s' (ImportVehicleComposite labels them VehicleComposite_<name>)"), *Label)); }
	FString AssetPkg;
	for (const FName& T : Actor->Tags) { const FString S = T.ToString(); if (S.StartsWith(TEXT("RUDE_VEHICLE_ASSET:"))) { AssetPkg = S.Mid(19); } }
	if (AssetPkg.IsEmpty()) { return Fail(TEXT("that actor carries no RUDE_VEHICLE_ASSET tag - it was placed by ImportVehicle (v1), not ImportVehicleComposite; re-import with the composite tool")); }
	URudeVehicle* VA = LoadObject<URudeVehicle>(nullptr, *(AssetPkg + TEXT(".") + FPackageName::GetShortName(AssetPkg)));
	if (!VA) { return Fail(FString::Printf(TEXT("the vehicle asset %s did not load"), *AssetPkg)); }
	const FString Name = VA->VehicleName.IsEmpty() ? FPackageName::GetShortName(AssetPkg).Replace(TEXT("_vehicle"), TEXT("")) : VA->VehicleName;
	const FString VehicleFolder = FPackageName::GetLongPackagePath(AssetPkg);
	TArray<FString> Problems, MissingFields, Notes;

	// ---- 1) the skeleton, from the yft the asset names ----------------------------------------------------------
	if (VA->SourceYft.IsEmpty() || !FPaths::FileExists(VA->SourceYft))
	{
		return Fail(FString::Printf(TEXT("the asset's SourceYft is not on disk (%s) - the DataAsset keeps bone tags, not bone frames; the yft is needed to build the skeleton"), *VA->SourceYft));
	}
	TArray<FBone> Bones;
	{
		FXmlFile Yft(VA->SourceYft);
		if (!Yft.IsValid()) { return Fail(FString::Printf(TEXT("yft XML load failed: %s"), *Yft.GetLastError())); }
		const FXmlNode* Root = Yft.GetRootNode();
		if (!Root || Root->GetTag() != TEXT("Fragment")) { return Fail(TEXT("yft root is not <Fragment>")); }
		FString Err;
		if (!ReadSkeleton(Root->FindChildNode(TEXT("Drawable")), Bones, Err)) { return Fail(Err); }
	}
	// UE local frames; the root carries the drive rotation (nose -Y -> +X, right +X -> +Y; Z untouched)
	const FTransform Drive(FRotator(0.f, 90.f, 0.f));
	TArray<FTransform> LocalUe, CompUe;
	LocalUe.SetNum(Bones.Num());
	CompUe.SetNum(Bones.Num());
	int32 Roots = 0;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		LocalUe[i] = GtaToUe(Bones[i].LocalGta);
		if (Bones[i].Parent < 0) { LocalUe[i] = LocalUe[i] * Drive; ++Roots; }
		CompUe[i] = Bones[i].Parent < 0 ? LocalUe[i] : LocalUe[i] * CompUe[Bones[i].Parent];
	}
	if (Roots != 1) { Problems.Add(FString::Printf(TEXT("%d root bones (expected 1): every root got the drive rotation"), Roots)); }
	int32 ChassisIdx = INDEX_NONE;
	for (int32 i = 0; i < Bones.Num(); ++i) { if (Bones[i].Name.Equals(TEXT("chassis"), ESearchCase::IgnoreCase)) { ChassisIdx = i; break; } }
	if (ChassisIdx == INDEX_NONE)
	{
		return Fail(FString::Printf(TEXT("the skeleton (%d bones, root '%s') has no bone named chassis; the body needs one to hang on (every measured vehicle has it)"), Bones.Num(), *Bones[0].Name));
	}
	struct FWheelBone { int32 Bone = -1; TCHAR Side = 0; FString Axle; bool bFront = false; };
	TArray<FWheelBone> WheelBones;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		FWheelBone W;
		if (!WheelSuffix(Bones[i].Name, W.Side, W.Axle)) { continue; }
		W.Bone = i;
		W.bFront = W.Axle.StartsWith(TEXT("f"));
		WheelBones.Add(W);
	}
	if (WheelBones.Num() == 0) { return Fail(TEXT("the skeleton has no wheel_* bones - nothing to drive on")); }
	WheelBones.StableSort([](const FWheelBone& A, const FWheelBone& B) { return A.bFront && !B.bFront; });   // front axle first, skeleton order within

	// ---- 2) the placed components: Body + Wheel_<i>_<bone> ------------------------------------------------------
	TArray<UStaticMeshComponent*> Comps;
	Actor->GetComponents<UStaticMeshComponent>(Comps);
	UStaticMeshComponent* BodyComp = nullptr;
	TMap<int32, UStaticMeshComponent*> WheelComp;   // bone index -> placed wheel mesh
	int32 WheelCompsUnmatched = 0;
	for (UStaticMeshComponent* C : Comps)
	{
		const FString CN = C->GetName();
		if (CN.Equals(TEXT("Body"), ESearchCase::IgnoreCase)) { BodyComp = C; continue; }
		if (!CN.StartsWith(TEXT("Wheel_"))) { continue; }
		int32 Us = INDEX_NONE;
		const FString Rest = CN.Mid(6);
		if (!Rest.FindChar(TEXT('_'), Us)) { ++WheelCompsUnmatched; continue; }
		const FString BoneName = Rest.Mid(Us + 1);
		int32 Bi = INDEX_NONE;
		for (int32 i = 0; i < Bones.Num(); ++i) { if (Bones[i].Name.Equals(BoneName, ESearchCase::IgnoreCase)) { Bi = i; break; } }
		if (Bi == INDEX_NONE || !C->GetStaticMesh()) { ++WheelCompsUnmatched; continue; }
		WheelComp.Add(Bi, C);
	}
	UStaticMesh* BodyMesh = BodyComp ? BodyComp->GetStaticMesh().Get() : VA->BodyMesh.LoadSynchronous();
	const FTransform BodyXf = BodyComp ? BodyComp->GetRelativeTransform() : FTransform::Identity;
	if (!BodyMesh) { return Fail(TEXT("no body mesh: the composite has no 'Body' component and the asset's BodyMesh did not load")); }
	if (!BodyComp) { Problems.Add(TEXT("no 'Body' component on the composite - the asset's BodyMesh was used at identity")); }

	// ---- 3) the skeletal mesh -----------------------------------------------------------------------------------
	FReferenceSkeleton RefSkel;
	{
		FReferenceSkeletonModifier Mod(RefSkel, nullptr);
		TSet<FName> Seen;
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			const FName BN(*Bones[i].Name);
			if (Seen.Contains(BN)) { return Fail(FString::Printf(TEXT("duplicate bone name '%s' (index %d) - UE bones are keyed by name"), *Bones[i].Name, i)); }
			Seen.Add(BN);
			Mod.Add(FMeshBoneInfo(BN, Bones[i].Name, Bones[i].Parent), LocalUe[i]);
		}
	}
	const FString SkelName = TEXT("SKEL_") + Name + TEXT("_drive");
	const FString MeshName = Name + TEXT("_drive");
	const FString PhysName = TEXT("PHYS_") + Name + TEXT("_drive");
	UPackage* SkelPkg = CreatePackage(*(VehicleFolder / SkelName));
	SkelPkg->FullyLoad();
	USkeleton* Skeleton = FindObject<USkeleton>(SkelPkg, *SkelName);
	const bool bNewSkeleton = Skeleton == nullptr;
	if (!Skeleton) { Skeleton = NewObject<USkeleton>(SkelPkg, FName(*SkelName), RF_Public | RF_Standalone); }
	if (!Skeleton) { return Fail(TEXT("NewObject<USkeleton> failed")); }
	UPackage* MeshPkg = CreatePackage(*(VehicleFolder / MeshName));
	MeshPkg->FullyLoad();
	USkeletalMesh* SK = FindObject<USkeletalMesh>(MeshPkg, *MeshName);
	const bool bNewMesh = SK == nullptr;
	if (!SK) { SK = NewObject<USkeletalMesh>(MeshPkg, FName(*MeshName), RF_Public | RF_Standalone); }
	if (!SK) { return Fail(TEXT("NewObject<USkeletalMesh> failed")); }
	SK->PreEditChange(nullptr);
	SK->SetNumSourceModels(0);
	SK->GetMaterials().Empty();
	SK->SetRefSkeleton(RefSkel);
	FSkeletalMeshLODInfo& Lod = SK->AddLODInfo();
	SK->GetImportedModel()->LODModels.Empty();
	SK->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel());   // the ped lane's measured requirement before Commit
	Lod.BuildSettings.bRecomputeNormals = false;
	Lod.BuildSettings.bRecomputeTangents = true;
	Lod.BuildSettings.bUseMikkTSpace = true;
	Lod.LODHysteresis = 0.02f;
	FMeshDescription* MD = SK->CreateMeshDescription(0);
	if (!MD) { return Fail(TEXT("CreateMeshDescription failed")); }
	FSkeletalMeshAttributes A(*MD);
	A.Register();
	A.GetVertexInstanceUVs().SetNumChannels(2);
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		const FBoneID BID = A.CreateBone();
		A.GetBoneNames()[BID] = FName(*Bones[i].Name);
		A.GetBoneParentIndices()[BID] = Bones[i].Parent;
		A.GetBonePoses()[BID] = LocalUe[i];
	}
	TArray<FSkeletalMaterial> Mats;
	TMap<FName, FPolygonGroupID> Groups;
	int32 BodyVerts = 0, BodyTris = 0, WheelVerts = 0, WheelTris = 0, WheelsBound = 0, WheelsMirrored = 0;
	FBox BodyBox(ForceInit), WheelBox(ForceInit);
	{
		FString Why;
		if (!AppendStaticMesh(*MD, A, BodyMesh, BodyXf, Drive, (uint16)ChassisIdx, TEXT(""), Mats, Groups, BodyVerts, BodyTris, BodyBox, Why)) { return Fail(TEXT("body: ") + Why); }
	}
	TArray<float> WheelRadii, WheelWidths;
	TArray<FString> WheelBoneNames;
	TArray<bool> WheelMirrored;
	for (const FWheelBone& W : WheelBones)
	{
		WheelBoneNames.Add(Bones[W.Bone].Name);
		UStaticMeshComponent* const* C = WheelComp.Find(W.Bone);
		if (!C)
		{
			Problems.Add(FString::Printf(TEXT("bone %s has no placed Wheel_* component on the composite - no wheel mesh bound; radius taken from the others"), *Bones[W.Bone].Name));
			WheelRadii.Add(-1.f); WheelWidths.Add(-1.f); WheelMirrored.Add(false);
			continue;
		}
		const FTransform Xf = (*C)->GetRelativeTransform();
		const bool bMirror = Xf.GetDeterminant() < 0.0;
		FString Why;
		FBox Box(ForceInit);
		if (!AppendStaticMesh(*MD, A, (*C)->GetStaticMesh(), Xf, Drive, (uint16)W.Bone, TEXT("wheel__"), Mats, Groups, WheelVerts, WheelTris, Box, Why))
		{
			Problems.Add(FString::Printf(TEXT("wheel %s: %s"), *Bones[W.Bone].Name, *Why));
			WheelRadii.Add(-1.f); WheelWidths.Add(-1.f); WheelMirrored.Add(bMirror);
			continue;
		}
		WheelBox += Box;
		++WheelsBound;
		if (bMirror) { ++WheelsMirrored; }
		// radius / width off the prototype's own extents (mesh-local: the axle is local X)
		const FVector Ext = (*C)->GetStaticMesh()->GetBounds().BoxExtent;
		WheelRadii.Add((float)FMath::Max(Ext.Y, Ext.Z));
		WheelWidths.Add((float)(Ext.X * 2.0));
		WheelMirrored.Add(bMirror);
	}
	{
		float Sum = 0.f; int32 Cnt = 0;
		for (float R : WheelRadii) { if (R > 0.f) { Sum += R; ++Cnt; } }
		const float Mean = Cnt > 0 ? Sum / Cnt : 32.f;
		if (Cnt == 0) { Problems.Add(TEXT("no wheel mesh was bound at all - wheel radius defaulted to 32 cm (Chaos's own)")); }
		for (int32 i = 0; i < WheelRadii.Num(); ++i) { if (WheelRadii[i] <= 0.f) { WheelRadii[i] = Mean; WheelWidths[i] = 20.f; } }
	}
	SK->SetMaterials(Mats);
	SK->CommitMeshDescription(0);
	SK->CalculateInvRefMatrices();
	SK->SetSkeleton(Skeleton);
	if (!Skeleton->MergeAllBonesToBoneTree(SK, false)) { Problems.Add(TEXT("the skeleton refused to merge this mesh's bones (a stale SKEL_*_drive from another bone set? delete it and re-run)")); }
	SK->InvalidateDeriveDataCacheGUID();
	SK->Build();
	SK->PostEditChange();
	SK->MarkPackageDirty();
	Skeleton->SetPreviewMesh(SK, true);
	Skeleton->MarkPackageDirty();
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		if (bNewSkeleton) { ARM.Get().AssetCreated(Skeleton); }
		if (bNewMesh) { ARM.Get().AssetCreated(SK); }
	}

	// ---- 4) the physics asset: one chassis body on the root bone -----------------------------------------------
	UPackage* PhysPkg = CreatePackage(*(VehicleFolder / PhysName));
	PhysPkg->FullyLoad();
	UPhysicsAsset* PA = FindObject<UPhysicsAsset>(PhysPkg, *PhysName);
	const bool bNewPhys = PA == nullptr;
	if (!PA) { PA = NewObject<UPhysicsAsset>(PhysPkg, FName(*PhysName), RF_Public | RF_Standalone); }
	if (!PA) { return Fail(TEXT("NewObject<UPhysicsAsset> failed")); }
	PA->SkeletalBodySetups.Empty();
	USkeletalBodySetup* BS = NewObject<USkeletalBodySetup>(PA, NAME_None, RF_Transactional);
	BS->BoneName = FName(*Bones[0].Name);
	BS->PhysicsType = EPhysicsType::PhysType_Default;
	BS->bGenerateMirroredCollision = false;
	FString ChassisShape;
	int32 ConvexCopied = 0, BoxesCopied = 0, ElemsSkipped = 0;
	if (const UBodySetup* Src = BodyMesh->GetBodySetup())
	{
		const FTransform Full = BodyXf * Drive;
		for (const FKConvexElem& E : Src->AggGeom.ConvexElems)
		{
			FKConvexElem C = E;
			C.SetTransform(E.GetTransform() * Full);
			BS->AggGeom.ConvexElems.Add(C);
			++ConvexCopied;
		}
		for (const FKBoxElem& E : Src->AggGeom.BoxElems)
		{
			FKBoxElem B = E;
			B.Center = Full.TransformPosition(E.Center);
			B.Rotation = (Full.GetRotation() * FQuat(E.Rotation)).Rotator();
			BS->AggGeom.BoxElems.Add(B);
			++BoxesCopied;
		}
		ElemsSkipped = Src->AggGeom.GetElementCount() - Src->AggGeom.ConvexElems.Num() - Src->AggGeom.BoxElems.Num();
	}
	if (ConvexCopied + BoxesCopied > 0) { ChassisShape = FString::Printf(TEXT("copied:%d convex,%d box"), ConvexCopied, BoxesCopied); }
	else
	{
		// no simple collision on the body (the drawable lane's complex-as-simple does not simulate): its bounds box
		const FVector Ext = BodyBox.GetExtent();
		FKBoxElem Box((float)(Ext.X * 2.0), (float)(Ext.Y * 2.0), (float)(Ext.Z * 2.0));
		Box.Center = BodyBox.GetCenter();
		BS->AggGeom.BoxElems.Add(Box);
		ChassisShape = TEXT("box-from-body-bounds");
	}
	BS->InvalidatePhysicsData();
	BS->CreatePhysicsMeshes();
	PA->SkeletalBodySetups.Add(BS);
	PA->UpdateBodySetupIndexMap();
	PA->UpdateBoundsBodiesArray();
	PA->SetPreviewMesh(SK);
	PA->MarkPackageDirty();
	SK->SetPhysicsAsset(PA);
	SK->MarkPackageDirty();
	if (bNewPhys)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().AssetCreated(PA);
	}
	FAssetCompilingManager::Get().FinishAllCompilation();

	// ---- 5) handling.meta -> Chaos (DESIGN.md's table; the tag on each line is the honesty of the mapping) -------
	auto HF = [&](const TCHAR* Field, float Def) -> float
	{
		const FString* V = VA->Handling.Find(Field);
		if (!V || V->IsEmpty()) { MissingFields.Add(Field); return Def; }
		return FCString::Atof(**V);
	};
	auto HVec = [&](const TCHAR* Field, const FVector& Def) -> FVector
	{
		const FString* V = VA->Handling.Find(Field);
		if (!V || V->IsEmpty()) { MissingFields.Add(Field); return Def; }
		float X = (float)Def.X, Y = (float)Def.Y, Z = (float)Def.Z;
		FParse::Value(**V, TEXT("x="), X); FParse::Value(**V, TEXT("y="), Y); FParse::Value(**V, TEXT("z="), Z);
		return FVector(X, Y, Z);
	};
	const float Grav = 9.81f;
	const int32 NumWheels = WheelBones.Num();
	const float Mass = FMath::Max(1.f, HF(TEXT("fMass"), 1500.f));
	const float DragCoeff = HF(TEXT("fInitialDragCoeff"), 7.5f);
	const float DriveBiasFront = FMath::Clamp(HF(TEXT("fDriveBiasFront"), 0.f), 0.f, 1.f);
	const int32 Gears = FMath::Clamp(FMath::RoundToInt(HF(TEXT("nInitialDriveGears"), 4.f)), 1, 8);
	const float DriveForce = FMath::Max(0.01f, HF(TEXT("fInitialDriveForce"), 0.2f));
	const float MaxFlatVelKmh = FMath::Max(10.f, HF(TEXT("fInitialDriveMaxFlatVel"), 140.f));
	const float BrakeForce = FMath::Max(0.01f, HF(TEXT("fBrakeForce"), 0.6f));
	const float BrakeBiasFront = FMath::Clamp(HF(TEXT("fBrakeBiasFront"), 0.5f), 0.f, 1.f);
	const float HandBrakeForce = FMath::Max(0.f, HF(TEXT("fHandBrakeForce"), 0.5f));
	const float SteeringLock = FMath::Clamp(HF(TEXT("fSteeringLock"), 35.f), 1.f, 89.f);
	const float TractionMax = FMath::Max(0.1f, HF(TEXT("fTractionCurveMax"), 2.f));
	const float SuspForce = FMath::Max(0.01f, HF(TEXT("fSuspensionForce"), 2.f));
	const float SuspComp = HF(TEXT("fSuspensionCompDamp"), 1.5f);
	const float SuspReb = HF(TEXT("fSuspensionReboundDamp"), 2.f);
	const float SuspUpper = HF(TEXT("fSuspensionUpperLimit"), 0.1f);
	const float SuspLower = HF(TEXT("fSuspensionLowerLimit"), -0.1f);
	const float SuspBiasFront = FMath::Clamp(HF(TEXT("fSuspensionBiasFront"), 0.5f), 0.f, 1.f);
	const FVector CoM = HVec(TEXT("vecCentreOfMassOffset"), FVector::ZeroVector);
	const FVector Inertia = HVec(TEXT("vecInertiaMultiplier"), FVector::OneVector);
	float MeanRadiusCm = 0.f;
	for (float R : WheelRadii) { MeanRadiusCm += R; }
	MeanRadiusCm = WheelRadii.Num() ? MeanRadiusCm / WheelRadii.Num() : 32.f;
	const float RadiusM = MeanRadiusCm / 100.f;
	const float Travel = FMath::Max(0.02f, SuspUpper - SuspLower);                           // metres, the meta's own unit
	const float SpringNm = SuspForce * (Mass / NumWheels) * Grav / Travel;                    // N/m per wheel
	const float SpringRate = SpringNm / 100.f;                                                // the wheel class's unit (MToCm inside)
	const float DampingRatio = FMath::Clamp(((SuspComp + SuspReb) * 0.5f) / 3.5f, 0.2f, 0.9f);
	const float MaxRPM = 6000.f, IdleRPM = 900.f, Efficiency = 0.9f;
	const float TopSpeedMs = MaxFlatVelKmh / 3.6f;
	const float FinalRatio = FMath::Clamp(MaxRPM * 2.f * PI * RadiusM / (60.f * TopSpeedMs), 1.f, 12.f);   // top gear = 1.0 hits MaxRPM at the meta's top speed
	const float MaxTorque = DriveForce * Mass * Grav * RadiusM / (FinalRatio * Efficiency);      // the meta's force at the wheels, in top gear
	const float BrakeTotalN = BrakeForce * Mass * Grav;
	const float HandbrakeTotalN = HandBrakeForce * Mass * Grav;
	const float ChaosDrag = DragCoeff / 25.f;
	const FVector CoMUe(CoM.Y * 100.0, CoM.X * 100.0, CoM.Z * 100.0);
	const FVector InertiaUe(Inertia.Y, Inertia.X, Inertia.Z);
	EVehicleDifferential Diff = EVehicleDifferential::AllWheelDrive;
	if (DriveBiasFront >= 0.99f) { Diff = EVehicleDifferential::FrontWheelDrive; }
	else if (DriveBiasFront <= 0.01f) { Diff = EVehicleDifferential::RearWheelDrive; }
	const float FrontRearSplit = 1.f - DriveBiasFront;
	Notes.Add(FString::Printf(TEXT("fMass %.0f -> Mass %.0f kg (as read: the meta's unit is kg)"), Mass, Mass));
	Notes.Add(FString::Printf(TEXT("wheel radius %.1f cm, width %.1f cm (measured off the wheel meshes' extents; the axle is mesh-local X)"), MeanRadiusCm, WheelWidths.Num() ? WheelWidths[0] : 0.f));
	Notes.Add(FString::Printf(TEXT("fInitialDriveMaxFlatVel %.0f (read as km/h: 135 -> 37.5 m/s fits a hatchback; m/s or mph do not) -> FinalRatio %.2f so top gear 1.0 at %.0f rpm = that speed [kinematics, given MaxRPM]"), MaxFlatVelKmh, FinalRatio, MaxRPM));
	Notes.Add(FString::Printf(TEXT("fInitialDriveForce %.3f -> MaxTorque %.0f Nm = force*mass*g*r/(final*%.2f), matched in TOP gear (inferred: GTA applies the force in every gear; Chaos multiplies by the ratio, so low gears pull harder here)"), DriveForce, MaxTorque, Efficiency));
	Notes.Add(FString::Printf(TEXT("nInitialDriveGears %d -> %d forward ratios, geometric 3.2 .. 1.0, reverse 2.9 (inferred: the meta has no ratios); MaxRPM %.0f / idle %.0f / up %.0f / down %.0f (inferred: the meta has no rpm)"), Gears, Gears, MaxRPM, IdleRPM, MaxRPM * 0.9f, MaxRPM * 0.35f));
	Notes.Add(FString::Printf(TEXT("fBrakeForce %.2f, fBrakeBiasFront %.2f -> per-wheel MaxBrakeTorque = force*mass*g*r/%d * 2*bias|2*(1-bias) (inferred: brake force read as a g multiplier); fHandBrakeForce %.2f -> rear handbrake torque = force*mass*g*r/2"), BrakeForce, BrakeBiasFront, NumWheels, HandBrakeForce));
	Notes.Add(FString::Printf(TEXT("fSteeringLock %.1f -> front MaxSteerAngle %.1f deg (as read: degrees; verify the lock in PIE)"), SteeringLock, SteeringLock));
	Notes.Add(FString::Printf(TEXT("fSuspensionForce %.2f over travel %.2f m -> SpringRate %.0f N/m = force*(mass/%d)*g/travel (inferred), x2*fSuspensionBiasFront %.2f front / x2*(1-bias) rear; Chaos unit %.0f"), SuspForce, Travel, SpringNm, NumWheels, SuspBiasFront, SpringRate));
	Notes.Add(FString::Printf(TEXT("fSuspensionCompDamp %.2f / fSuspensionReboundDamp %.2f -> DampingRatio %.2f = mean/3.5 clamped 0.2..0.9 (PLACEHOLDER anchored so blista's 1.3/2.2 lands on Chaos's default 0.5; the meta's damping unit is unknown)"), SuspComp, SuspReb, DampingRatio));
	Notes.Add(FString::Printf(TEXT("fSuspensionUpperLimit %.3f / LowerLimit %.3f -> MaxRaise %.1f cm / MaxDrop %.1f cm (as read: metres)"), SuspUpper, SuspLower, SuspUpper * 100.f, -SuspLower * 100.f));
	Notes.Add(FString::Printf(TEXT("fDriveBiasFront %.2f -> %s%s (front 1.0 / rear 0.0 exact; between = AllWheelDrive with FrontRearSplit = 1-bias per the header's '<0.5 = more front' - inferred)"), DriveBiasFront,
		Diff == EVehicleDifferential::FrontWheelDrive ? TEXT("FrontWheelDrive") : Diff == EVehicleDifferential::RearWheelDrive ? TEXT("RearWheelDrive") : TEXT("AllWheelDrive"),
		Diff == EVehicleDifferential::AllWheelDrive ? *FString::Printf(TEXT(" split %.2f"), FrontRearSplit) : TEXT("")));
	Notes.Add(FString::Printf(TEXT("fInitialDragCoeff %.2f -> DragCoefficient %.3f = /25 (PLACEHOLDER: blista's 8 lands near Chaos's default 0.3)"), DragCoeff, ChaosDrag));
	Notes.Add(FString::Printf(TEXT("fTractionCurveMax %.2f -> FrictionForceMultiplier %.2f (inferred: peak grip in g vs Chaos's 2.0 default, same order)"), TractionMax, TractionMax));
	Notes.Add(FString::Printf(TEXT("vecCentreOfMassOffset (%.2f %.2f %.2f) m -> CenterOfMassOverride (%.0f %.0f %.0f) cm (axis map: GTA x right,y fwd -> pawn y right,x fwd)"), CoM.X, CoM.Y, CoM.Z, CoMUe.X, CoMUe.Y, CoMUe.Z));
	Notes.Add(FString::Printf(TEXT("vecInertiaMultiplier (%.2f %.2f %.2f) -> InertiaTensorScale (%.2f %.2f %.2f) (inferred: both unit-free per-axis multipliers; axes swapped like the mesh)"), Inertia.X, Inertia.Y, Inertia.Z, InertiaUe.X, InertiaUe.Y, InertiaUe.Z));
	Notes.Add(FString::Printf(TEXT("ChassisWidth %.0f / ChassisHeight %.0f cm (measured off the body bounds)"), BodyBox.GetSize().Y, BodyBox.GetSize().Z));

	// ---- 6) the pawn ---------------------------------------------------------------------------------------------
	const FName IdTag(*(TEXT("RUDE_DRIVEABLE:") + Name));
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It) { if (It->Tags.Contains(IdTag)) { Stale.Add(*It); } }
		for (AActor* S : Stale) { World->DestroyActor(S); }
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FTransform CT = Actor->GetActorTransform();
	// beside the composite (+6 m on its X), nose the same way: the composite's nose is its -Y, the pawn's is +X.
	// LocationCm "x,y,z" (UE cm) puts the car there instead - a World Partition level only has ground around the
	// player, so a car left at the origin 600 m from the sandbox spawn would fall through unloaded cells in Play.
	FVector SpawnLoc = CT.TransformPosition(FVector(600.f, 0.f, 50.f));
	{
		TArray<FString> P;
		LocationCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
		if (P.Num() == 3) { SpawnLoc = FVector(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2])); }
		else if (P.Num() != 0) { return Fail(TEXT("LocationCm must be \"x,y,z\" in UE centimetres, or empty (= beside the composite)")); }
	}
	const FRotator SpawnRot(0.f, CT.Rotator().Yaw - 90.f, 0.f);
	ARudeDriveablePawn* Pawn = World->SpawnActor<ARudeDriveablePawn>(ARudeDriveablePawn::StaticClass(), SpawnLoc, SpawnRot, SP);
	if (!Pawn) { return Fail(TEXT("ARudeDriveablePawn spawn failed")); }
	URudeDriveMovement* Mv = Pawn->GetDriveMovement();
	if (!Mv) { World->DestroyActor(Pawn); return Fail(TEXT("the pawn's movement component is not a URudeDriveMovement - the SetDefaultSubobjectClass in its constructor did not take")); }
	Pawn->GetMesh()->SetSkeletalMeshAsset(SK);
	Pawn->VehicleName = Name;
	Pawn->VehicleAssetPath = AssetPkg;
	Pawn->SetActorLabel(TEXT("Driveable_") + Name);
	Pawn->SetFolderPath(FName(TEXT("RUDE_VEHICLES")));
	Pawn->Tags.Add(IdTag);
	Pawn->Tags.Add(FName(TEXT("RUDE_DRIVEABLE_ROOT")));
	Pawn->Tags.Add(FName(*(TEXT("RUDE_VEHICLE_ASSET:") + AssetPkg)));
	Mv->Mass = Mass;
	Mv->ChassisWidth = (float)BodyBox.GetSize().Y;
	Mv->ChassisHeight = (float)BodyBox.GetSize().Z;
	Mv->DragCoefficient = ChaosDrag;
	Mv->bEnableCenterOfMassOverride = true;
	Mv->CenterOfMassOverride = CoMUe;
	Mv->InertiaTensorScale = InertiaUe;
	Mv->bReverseAsBrake = true;
	Mv->bMechanicalSimEnabled = true;
	Mv->EngineSetup.MaxTorque = MaxTorque;
	Mv->EngineSetup.MaxRPM = MaxRPM;
	Mv->EngineSetup.EngineIdleRPM = IdleRPM;
	Mv->EngineSetup.EngineBrakeEffect = 0.05f;
	{
		FRichCurve* Curve = Mv->EngineSetup.TorqueCurve.GetRichCurve();
		Curve->Reset();
		Curve->AddKey(0.f, 0.55f);
		Curve->AddKey(IdleRPM, 0.7f);
		Curve->AddKey(MaxRPM * 0.5f, 1.f);
		Curve->AddKey(MaxRPM, 0.8f);
	}
	Mv->TransmissionSetup.bUseAutomaticGears = true;
	Mv->TransmissionSetup.bUseAutoReverse = true;
	Mv->TransmissionSetup.FinalRatio = FinalRatio;
	Mv->TransmissionSetup.ForwardGearRatios.Reset();
	for (int32 g = 0; g < Gears; ++g)
	{
		const float T = Gears > 1 ? (float)(Gears - 1 - g) / (float)(Gears - 1) : 0.f;   // 1 .. 0
		Mv->TransmissionSetup.ForwardGearRatios.Add(FMath::Pow(3.2f, T));
	}
	Mv->TransmissionSetup.ReverseGearRatios.Reset();
	Mv->TransmissionSetup.ReverseGearRatios.Add(2.9f);
	Mv->TransmissionSetup.ChangeUpRPM = MaxRPM * 0.9f;
	Mv->TransmissionSetup.ChangeDownRPM = MaxRPM * 0.35f;
	Mv->TransmissionSetup.GearChangeTime = 0.3f;
	Mv->TransmissionSetup.TransmissionEfficiency = Efficiency;
	Mv->DifferentialSetup.DifferentialType = Diff;
	Mv->DifferentialSetup.FrontRearSplit = FrontRearSplit;
	Mv->SteeringSetup.SteeringType = ESteeringType::Ackermann;
	Mv->WheelSetups.Reset();
	Mv->WheelParams.Reset();
	int32 NumFront = 0;
	for (const FWheelBone& W : WheelBones) { if (W.bFront) { ++NumFront; } }
	int32 FrontWheels = 0;
	for (int32 i = 0; i < WheelBones.Num(); ++i)
	{
		const FWheelBone& W = WheelBones[i];
		FChaosWheelSetup WS;
		WS.WheelClass = W.bFront ? TSubclassOf<UChaosVehicleWheel>(URudeDriveWheelFront::StaticClass()) : TSubclassOf<UChaosVehicleWheel>(URudeDriveWheelRear::StaticClass());
		WS.BoneName = FName(*Bones[W.Bone].Name);
		WS.AdditionalOffset = FVector::ZeroVector;
		Mv->WheelSetups.Add(WS);
		FRudeDriveWheelParams P;
		P.BoneName = WS.BoneName;
		P.bFront = W.bFront;
		P.bMirrored = WheelMirrored.IsValidIndex(i) ? WheelMirrored[i] : false;
		P.RadiusCm = WheelRadii[i];
		P.WidthCm = WheelWidths[i];
		P.MassKg = Mass * 0.02f;
		P.MaxSteerDeg = W.bFront ? SteeringLock : 0.f;
		const float RM = WheelRadii[i] / 100.f;
		P.BrakeTorqueNm = BrakeTotalN * RM / NumWheels * (W.bFront ? 2.f * BrakeBiasFront : 2.f * (1.f - BrakeBiasFront));
		P.HandbrakeTorqueNm = W.bFront ? 0.f : HandbrakeTotalN * RM / FMath::Max(1, NumWheels - NumFront);
		P.FrictionMultiplier = TractionMax;
		P.SpringRate = SpringRate * (W.bFront ? 2.f * SuspBiasFront : 2.f * (1.f - SuspBiasFront));
		P.SpringPreload = 50.f;
		P.DampingRatio = DampingRatio;
		P.MaxRaiseCm = FMath::Max(1.f, SuspUpper * 100.f);
		P.MaxDropCm = FMath::Max(1.f, -SuspLower * 100.f);
		Mv->WheelParams.Add(P);
		if (W.bFront) { ++FrontWheels; }
	}
	Mv->MappingNotes = Notes;
	Mv->MarkPackageDirty();
	Pawn->MarkPackageDirty();

	// ---- 7) verdict ---------------------------------------------------------------------------------------------
	float WheelBottomZ = 0.f;
	for (int32 i = 0; i < WheelBones.Num(); ++i)
	{
		const float Z = (float)CompUe[WheelBones[i].Bone].GetTranslation().Z - WheelRadii[i];
		WheelBottomZ = i == 0 ? Z : FMath::Min(WheelBottomZ, Z);
	}
	const float ChassisMinZ = (float)BodyBox.Min.Z;
	TArray<FString> WheelSlots;
	for (const FSkeletalMaterial& M : Mats) { if (M.MaterialSlotName.ToString().StartsWith(TEXT("wheel__"))) { WheelSlots.Add(M.MaterialSlotName.ToString()); } }
	const bool bOk = BodyVerts > 0 && BodyTris > 0 && WheelsBound == WheelBones.Num() && WheelCompsUnmatched == 0 && Mats.Num() > 0 && FrontWheels > 0 && Roots == 1;
	const int32 Cap = 20;
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"vehicle\":\"%s\",\"composite\":\"%s\",\"actor\":\"%s\",\"asset\":\"%s\","
		"\"skeletalMesh\":\"%s\",\"skeleton\":\"%s\",\"physicsAsset\":\"%s\","
		"\"bones\":%d,\"rootBone\":\"%s\",\"chassisBone\":\"%s\",\"chassisBoneIndex\":%d,\"driveYawDeg\":90,"
		"\"bodyVertices\":%d,\"bodyTriangles\":%d,\"wheelBones\":%d,\"wheelBoneNames\":[%s],\"frontWheels\":%d,\"wheelsBound\":%d,\"wheelsMirrored\":%d,"
		"\"wheelComponentsUnmatched\":%d,\"wheelVertices\":%d,\"wheelTriangles\":%d,\"wheelRadiiCm\":[%s],\"wheelWidthsCm\":[%s],"
		"\"materials\":%d,\"wheelMaterialSlots\":%d,\"chassisShape\":\"%s\",\"collisionElemsSkipped\":%d,"
		"\"chassisMinZcm\":%.1f,\"wheelBottomZcm\":%.1f,\"groundClearanceCm\":%.1f,"
		"\"bodyBoundsCm\":[%.0f,%.0f,%.0f],\"mass\":%.0f,\"maxTorqueNm\":%.0f,\"maxRpm\":%.0f,\"finalRatio\":%.2f,\"gears\":%d,"
		"\"differential\":\"%s\",\"steerLockDeg\":%.1f,\"springRate\":%.0f,\"dampingRatio\":%.2f,\"maxRaiseCm\":%.1f,\"maxDropCm\":%.1f,\"dragCoefficient\":%.3f,"
		"\"handlingFields\":%d,\"handlingFieldsMissing\":[%s],\"mappingNotes\":[%s],"
		"\"problemCount\":%d,\"problems\":[%s],"
		"\"next\":\"SandboxSetup, Play, then: Rude.Native EnterVehicle %s\"}"),
		bOk ? TEXT("true") : TEXT("false"),
		*RudeJsonEscape(Name), *RudeJsonEscape(Actor->GetActorLabel()), *RudeJsonEscape(Pawn->GetActorLabel()), *RudeJsonEscape(AssetPkg),
		*RudeJsonEscape(VehicleFolder / MeshName), *RudeJsonEscape(VehicleFolder / SkelName), *RudeJsonEscape(VehicleFolder / PhysName),
		Bones.Num(), *RudeJsonEscape(Bones[0].Name), *RudeJsonEscape(Bones[ChassisIdx].Name), ChassisIdx,
		BodyVerts, BodyTris, WheelBones.Num(), *JsonStrings(WheelBoneNames), FrontWheels, WheelsBound, WheelsMirrored,
		WheelCompsUnmatched, WheelVerts, WheelTris, *JsonFloats(WheelRadii), *JsonFloats(WheelWidths),
		Mats.Num(), WheelSlots.Num(), *RudeJsonEscape(ChassisShape), ElemsSkipped,
		ChassisMinZ, WheelBottomZ, ChassisMinZ - WheelBottomZ,
		BodyBox.GetSize().X, BodyBox.GetSize().Y, BodyBox.GetSize().Z, Mass, MaxTorque, MaxRPM, FinalRatio, Gears,
		Diff == EVehicleDifferential::FrontWheelDrive ? TEXT("FrontWheelDrive") : Diff == EVehicleDifferential::RearWheelDrive ? TEXT("RearWheelDrive") : TEXT("AllWheelDrive"),
		SteeringLock, SpringRate, DampingRatio, FMath::Max(1.f, SuspUpper * 100.f), FMath::Max(1.f, -SuspLower * 100.f), ChaosDrag,
		VA->Handling.Num(), *JsonStrings(MissingFields), *JsonStrings(Notes),
		Problems.Num(), *JsonStrings(Problems, Cap),
		*RudeJsonEscape(Name));
}
