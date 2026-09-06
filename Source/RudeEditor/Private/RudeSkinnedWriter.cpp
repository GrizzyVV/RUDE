// RUDE - RAGE <-> Unreal Development Environment
//
// THE SKINNED DRAWABLE DICTIONARY WRITER (WP11 draft, 2026-09-06): ExportYddBinary + ProbeYddBinary.
// A FiveM ped's clothing is a .ydd (pgDictionary<gtaDrawable>) of SKINNED drawables that bind to the ped's
// .yft skeleton by rig index. This lane writes that container from USkeletalMesh assets (ImportPed's outputs,
// or anything skinned to the same USkeleton) and parses it back.
//
// EVERYTHING BYTE-LEVEL HERE WAS MEASURED (scratchpad/wp11/ydd_writer/LAWS.md, denominators there):
//   container   3/3 game binaries + ROUT's 400/400: RSC7 v165, 0x40 header, ascending hash array, 8-byte entry
//               pointers, 0xD0 records; entry +0x08 (blockmap) / +0x18 (skeleton) / +0xC8 (bound) raw NULL.
//   skinned     52/52 XML models + 7/7 binary: grmModel +0x28 = rig bone count, +0x29 = 1, +0x2D = 1;
//               grmGeometry +0x68 -> identity u16 bone-id table (56/56, ROUT 3,960/3,960), +0x72 = count;
//               fvf mask 0x7F / stride 48 / 7 channels with the GTAV1 nibble constant (the game's own Med/Low
//               layout and 16/28 High geometries); VB +0x0A = 0 in the game (the static writer's 0x59 is a
//               CW-oracle residue - both load); BlendWeights sum to 255 on 27,808/27,808 + 17,223/17,223.
//   shader      the `ped` template, identical in 12/12 XML + 3/3 binary shaders; +0x14 = 16*(13+8) = 336,
//               +0x16 = 432 (allocation law roundup16(16*(npar+nvec)+4*npar+32), fitted 11/11), +0x24 = 5<<24.
// NOT verified: an in-game load of a RUDE ydd (Matt's test). Unknowns are counted in the verdict, never
// silently defaulted (the toolset rule).
//
// Shared machinery is LIFTED through RudeBinaryShared.h (RSC7 load, fvf decode, page plan, the single-
// ownership self-check now callable per entry with a dictionary-wide in-degree map) - not duplicated.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeBinaryShared.h"

#include "Animation/Skeleton.h"
#include "BoneWeights.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "SkeletalMeshAttributes.h"
#include "SkinWeightsAttributesRef.h"

namespace RudeYdd
{
	// ---- byte helpers (the ybn/ydr writers' one-liners; file-local there) ----
	static void PU32(TArray<uint8>& B, int32 O, uint32 V) { B[O] = V & 0xFF; B[O + 1] = (V >> 8) & 0xFF; B[O + 2] = (V >> 16) & 0xFF; B[O + 3] = (V >> 24) & 0xFF; }
	static void PU16(TArray<uint8>& B, int32 O, uint16 V) { B[O] = V & 0xFF; B[O + 1] = (V >> 8) & 0xFF; }
	static void PF32(TArray<uint8>& B, int32 O, float V) { uint32 U; FMemory::Memcpy(&U, &V, 4); PU32(B, O, U); }
	// 8-byte tagged fixup into the system segment (0x50000000 | offset); NEVER for a null - a null slot stays raw 0.
	static void PPTR(TArray<uint8>& B, int32 O, int32 Target) { PU32(B, O, 0x50000000u | (uint32)Target); PU32(B, O + 4, 0); }
	static void PVEC3(TArray<uint8>& B, int32 O, const FVector3f& V) { PF32(B, O, V.X); PF32(B, O + 4, V.Y); PF32(B, O + 8, V.Z); }
	static uint32 RU32(const TArray<uint8>& B, int32 O) { uint32 V = 0; if (O >= 0 && O + 4 <= B.Num()) { FMemory::Memcpy(&V, B.GetData() + O, 4); } return V; }
	static uint16 RU16(const TArray<uint8>& B, int32 O) { uint16 V = 0; if (O >= 0 && O + 2 <= B.Num()) { FMemory::Memcpy(&V, B.GetData() + O, 2); } return V; }
	static uint8 RU8(const TArray<uint8>& B, int32 O) { return (O >= 0 && O < B.Num()) ? B[O] : 0; }
	static float RF32(const TArray<uint8>& B, int32 O) { float V = 0.f; if (O >= 0 && O + 4 <= B.Num()) { FMemory::Memcpy(&V, B.GetData() + O, 4); } return V; }
	static uint64 RU64(const TArray<uint8>& B, int32 O) { uint64 V = 0; if (O >= 0 && O + 8 <= B.Num()) { FMemory::Memcpy(&V, B.GetData() + O, 8); } return V; }
	// system-segment pointer -> offset, validating the whole [off, off+Need) span
	static bool Deref(const TArray<uint8>& Sys, uint32 Tagged, int32 Need, int32& Out)
	{
		if ((Tagged >> 28) != 5) { return false; }
		Out = (int32)(Tagged & 0x0FFFFFFFu);
		return Out >= 0 && Need >= 0 && Out + Need <= Sys.Num();
	}
	static FString CStr(const TArray<uint8>& Sys, uint32 Tagged, int32 Max = 96)
	{
		FString S; int32 O = 0;
		if (!Deref(Sys, Tagged, 1, O)) { return S; }
		while (O < Sys.Num() && Sys[O] != 0 && S.Len() < Max) { S.AppendChar((TCHAR)Sys[O++]); }
		return S;
	}

	// ---- the `ped` shader template (LAWS.md section 5): name, register byte, texture?, vec4 value ----
	struct FPedParam { const TCHAR* Name; uint8 Reg; bool bTexture; float V[4]; };
	static const FPedParam kPed[13] =
	{
		{ TEXT("DiffuseSampler"),        0,   true,  { 0.f, 0.f, 0.f, 0.f } },
		{ TEXT("TextureSamplerDiffPal"), 2,   true,  { 0.f, 0.f, 0.f, 0.f } },   // unbound in 12/12 + 3/3
		{ TEXT("VolumeSampler"),         3,   true,  { 0.f, 0.f, 0.f, 0.f } },   // "givemechecker" 12/12 + 3/3
		{ TEXT("BumpSampler"),           4,   true,  { 0.f, 0.f, 0.f, 0.f } },
		{ TEXT("SpecSampler"),           5,   true,  { 0.f, 0.f, 0.f, 0.f } },
		{ TEXT("umGlobalParams"),        187, false, { 0.0025f, 0.0025f, 7.f, 7.f } },
		{ TEXT("envEffFatThickness"),    186, false, { 25.f, 25.f, 0.f, 0.f } },
		{ TEXT("specularIntensityMult"), 185, false, { 1.f, 0.f, 0.f, 0.f } },
		{ TEXT("specularFalloffMult"),   184, false, { 250.f, 0.f, 0.f, 0.f } },
		{ TEXT("specularFresnel"),       183, false, { 0.94f, 0.f, 0.f, 0.f } },
		{ TEXT("bumpiness"),             182, false, { 1.f, 0.f, 0.f, 0.f } },
		{ TEXT("detailSettings"),        181, false, { 0.1f, 0.5f, 60.f, 0.f } },
		{ TEXT("StubbleControl"),        180, false, { 2.f, 0.6f, 0.f, 0.f } },
	};
	static const int32 kPedNPar = 13, kPedNVec = 8, kPedNTex = 5;
	static const int32 kPedHashOfs = 16 * (kPedNPar + kPedNVec);                              // 336, measured 3/3
	static const int32 kPedAlloc = (16 * (kPedNPar + kPedNVec) + 4 * kPedNPar + 32 + 15) & ~15; // 432, measured 3/3
	// skinned GTAV1 layout: Position(0) BlendWeights(1) BlendIndices(2) Normal(3) Colour0(4) Colour1(5) TexCoord0(6)
	static const uint32 kSkinMask = 0x7F; static const int32 kSkinStride = 48; static const uint8 kSkinChans = 7;
	// measured vfts of dictionary objects (3/3 binaries) - build residue, kept for likeness, not load-bearing
	static const uint32 VFT_DICT = 0x40571578u, VFT_DRAWABLE = 0x40571168u, VFT_SG = 0x406117F0u, VFT_STUB = 0x406187F8u,
	                    VFT_MODEL = 0x4060EA98u, VFT_GEO = 0x40616798u, VFT_VB = 0x4061B3F8u, VFT_IB = 0x4061B158u;

	struct FSkin { uint8 W[4]; uint8 I[4]; };
	struct FYddVert { FVector3f P; FVector3f N; FVector2f UV; uint8 C[4]; FSkin S; };
	struct FGeo
	{
		FString Slot, Diffuse, Normal, Spec;
		TArray<FYddVert> V; TArray<int32> Idx;
		FVector3f Mn = FVector3f(FLT_MAX), Mx = FVector3f(-FLT_MAX);
	};
	struct FDrawable
	{
		FString Name, Asset; uint32 Hash = 0;
		TArray<FGeo> Geos;
		FVector3f Mn = FVector3f(FLT_MAX), Mx = FVector3f(-FLT_MAX);
		int32 SrcVerts = 0, InflUnmapped = 0, InflTruncated = 0, Rebound = 0, BonesUnmapped = 0, Uv1Dropped = 0, TexMissing = 0, MaxRig = -1;
	};

	static FString Fail(const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); }
	static uint8 Byte01(float F) { return (uint8)FMath::Clamp(FMath::RoundToInt(F * 255.f), 0, 255); }
}

// ---- ExportYddBinary -------------------------------------------------------------------------
FString URudeToolset::ExportYddBinary(const FString& SkeletalMeshAssetPaths, const FString& DrawableNames,
                                      const FString& OutYddPath, const FString& Options)
{
	using namespace RudeYdd;
#if WITH_EDITORONLY_DATA
	TArray<FString> Paths, Names;
	SkeletalMeshAssetPaths.ParseIntoArray(Paths, TEXT(","), true);
	DrawableNames.ParseIntoArray(Names, TEXT(","), true);
	if (Paths.Num() == 0) { return Fail(TEXT("give at least one USkeletalMesh content path (comma list)")); }
	if (OutYddPath.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("OutYddPath is empty")); }
	FString RigPath;
	{
		TArray<FString> Toks; Options.ParseIntoArray(Toks, TEXT(";"), true);
		for (const FString& T : Toks)
		{
			const FString U = T.TrimStartAndEnd();
			if (U.StartsWith(TEXT("SKELETON="), ESearchCase::IgnoreCase)) { RigPath = U.Mid(9).TrimStartAndEnd(); }
		}
	}

	// ---- 1) the meshes and the rig (bone ORDER = the ped's yft order, as ImportPed built the USkeleton) ----
	TArray<USkeletalMesh*> Meshes;
	for (const FString& P : Paths)
	{
		USkeletalMesh* M = LoadObject<USkeletalMesh>(nullptr, *P.TrimStartAndEnd());
		if (!M) { return Fail(FString::Printf(TEXT("SkeletalMesh not found: %s"), *P.TrimStartAndEnd())); }
		Meshes.Add(M);
	}
	USkeleton* RigSkel = RigPath.IsEmpty() ? Meshes[0]->GetSkeleton() : LoadObject<USkeleton>(nullptr, *RigPath);
	if (!RigPath.IsEmpty() && !RigSkel) { return Fail(FString::Printf(TEXT("SKELETON not found: %s"), *RigPath)); }
	const FReferenceSkeleton* Rig = RigSkel ? &RigSkel->GetReferenceSkeleton() : &Meshes[0]->GetRefSkeleton();
	const FString RigName = RigSkel ? RigSkel->GetPathName() : (Meshes[0]->GetPathName() + TEXT(" (own reference skeleton; no USkeleton assigned)"));
	const int32 NumRig = Rig->GetNum();
	if (NumRig <= 0) { return Fail(TEXT("the rig has no bones")); }
	// grmModel+0x28 is a byte and BlendIndices are bytes: the measured rigs are 98..106 bones
	if (NumRig > 255) { return Fail(FString::Printf(TEXT("the rig has %d bones; the format carries the bone count in a byte (grmModel+0x28) and blend indices as bytes"), NumRig)); }
	TMap<FName, int32> RigIndex;
	for (int32 i = 0; i < NumRig; ++i) { RigIndex.Add(Rig->GetBoneName(i), i); }

	// ---- 2) gather: one drawable per mesh, one geometry per polygon group, skin per vertex ----
	TArray<FDrawable> Ds;
	for (int32 mi = 0; mi < Meshes.Num(); ++mi)
	{
		USkeletalMesh* SK = Meshes[mi];
		FDrawable D;
		D.Asset = SK->GetPathName();
		D.Name = Names.IsValidIndex(mi) && !Names[mi].TrimStartAndEnd().IsEmpty() ? Names[mi].TrimStartAndEnd().ToLower() : SK->GetName().ToLower();
		D.Hash = RudeJoaat(D.Name);
		const FMeshDescription* MD = SK->GetMeshDescription(0);
		if (!MD) { return Fail(FString::Printf(TEXT("%s: no MeshDescription on LOD0 (no source geometry to export)"), *D.Asset)); }
		FSkeletalMeshConstAttributes A(*MD);
		TVertexAttributesConstRef<FVector3f> Positions = A.GetVertexPositions();
		TVertexInstanceAttributesConstRef<FVector3f> InstNormals = A.GetVertexInstanceNormals();
		TVertexInstanceAttributesConstRef<FVector2f> InstUVs = A.GetVertexInstanceUVs();
		TVertexInstanceAttributesConstRef<FVector4f> InstColors = A.GetVertexInstanceColors();
		TPolygonGroupAttributesConstRef<FName> GroupSlots = A.GetPolygonGroupMaterialSlotNames();
		FSkinWeightsVertexAttributesConstRef SkinWeights = A.GetVertexSkinWeights();
		const int32 NumUV = InstUVs.GetNumChannels();
		D.SrcVerts = MD->Vertices().Num();
		// mesh-description bone -> rig bone, by NAME
		const int32 NB = A.GetNumBones();
		if (NB == 0) { return Fail(FString::Printf(TEXT("%s: the mesh description carries no bones - not a skinned mesh"), *D.Asset)); }
		FSkeletalMeshAttributesShared::FBoneNameAttributesConstRef BoneNames = A.GetBoneNames();
		TArray<int32> BoneToRig;
		for (int32 b = 0; b < NB; ++b)
		{
			const int32* R = RigIndex.Find(BoneNames[FBoneID(b)]);
			BoneToRig.Add(R ? *R : -1);
			if (!R) { ++D.BonesUnmapped; }
		}
		// skin bytes per vertex (LAWS: 4 slots, bytes summing to 255, largest-remainder rounding)
		TArray<FSkin> Skins; Skins.SetNumZeroed(MD->Vertices().GetArraySize());
		TBitArray<> SkinDone(false, Skins.Num());
		auto SkinOf = [&](const FVertexID VID) -> const FSkin&
		{
			const int32 k = VID.GetValue();
			if (!SkinDone[k])
			{
				TArray<TPair<int32, uint32>> Infl;
				for (UE::AnimationCore::FBoneWeight BW : SkinWeights.Get(VID))
				{
					const int32 MB = (int32)BW.GetBoneIndex();
					const uint32 Raw = BW.GetRawWeight();
					if (Raw == 0) { continue; }
					const int32 RB = BoneToRig.IsValidIndex(MB) ? BoneToRig[MB] : -1;
					if (RB < 0) { ++D.InflUnmapped; continue; }
					Infl.Add(TPair<int32, uint32>(RB, Raw));
				}
				Infl.Sort([](const TPair<int32, uint32>& X, const TPair<int32, uint32>& Y) { return X.Value > Y.Value; });
				if (Infl.Num() > 4) { D.InflTruncated += Infl.Num() - 4; Infl.SetNum(4); }
				FSkin S; FMemory::Memzero(S);
				uint64 Sum = 0;
				for (const auto& X : Infl) { Sum += X.Value; }
				if (Sum == 0) { S.W[0] = 255; S.I[0] = 0; ++D.Rebound; D.MaxRig = FMath::Max(D.MaxRig, 0); }
				else
				{
					int32 Base[4] = { 0, 0, 0, 0 }; double Frac[4] = { 0, 0, 0, 0 }; int32 Tot = 0;
					for (int32 q = 0; q < Infl.Num(); ++q)
					{
						const double F = (double)Infl[q].Value * 255.0 / (double)Sum;
						Base[q] = (int32)F; Frac[q] = F - Base[q]; Tot += Base[q];
					}
					for (int32 r = 255 - Tot; r > 0; --r)
					{
						int32 Best = 0;
						for (int32 q = 1; q < Infl.Num(); ++q) { if (Frac[q] > Frac[Best]) { Best = q; } }
						Base[Best] += 1; Frac[Best] = -1.0;
					}
					for (int32 q = 0; q < Infl.Num(); ++q)
					{
						S.W[q] = (uint8)FMath::Clamp(Base[q], 0, 255); S.I[q] = (uint8)Infl[q].Key;
						D.MaxRig = FMath::Max(D.MaxRig, Infl[q].Key);
					}
				}
				Skins[k] = S; SkinDone[k] = true;
			}
			return Skins[k];
		};
		// per polygon group: weld by (vertex, normal, uv, colour) - the skin rides with the vertex
		for (const FPolygonGroupID GroupID : MD->PolygonGroups().GetElementIDs())
		{
			FGeo G;
			G.Slot = GroupSlots[GroupID].ToString();
			const TArray<FSkeletalMaterial>& Mats = SK->GetMaterials();
			int32 SlotIdx = INDEX_NONE;
			for (int32 i = 0; i < Mats.Num(); ++i) { if (Mats[i].MaterialSlotName == GroupSlots[GroupID]) { SlotIdx = i; break; } }
			if (SlotIdx == INDEX_NONE && Mats.IsValidIndex(GroupID.GetValue())) { SlotIdx = GroupID.GetValue(); }
			if (Mats.IsValidIndex(SlotIdx))
			{
				if (const UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Mats[SlotIdx].MaterialInterface))
				{
					auto TexName = [&](const TCHAR* Param) -> FString
					{
						UTexture* T = nullptr;
						if (!MIC->GetTextureParameterValue(FMaterialParameterInfo(Param), T) || !T) { return FString(); }
						if (T->GetPathName().StartsWith(TEXT("/Engine/"))) { return FString(); }   // a master's unused slot
						return T->GetName().ToLower();
					};
					G.Diffuse = TexName(TEXT("Diffuse")); G.Normal = TexName(TEXT("Normal")); G.Spec = TexName(TEXT("Specular"));
				}
			}
			TMap<FString, int32> Weld;
			for (const FPolygonID PolyID : MD->GetPolygonGroupPolygonIDs(GroupID))
			{
				for (const FTriangleID TriID : MD->GetPolygonTriangles(PolyID))
				{
					for (const FVertexInstanceID Inst : MD->GetTriangleVertexInstances(TriID))
					{
						const FVertexID VID = MD->GetVertexInstanceVertex(Inst);
						const FVector3f P = Positions[VID];
						const FVector3f N = InstNormals[Inst];
						const FVector2f UV = InstUVs.Get(Inst, 0);
						const FVector4f C = InstColors[Inst];
						if (NumUV > 1 && !InstUVs.Get(Inst, 1).IsZero()) { ++D.Uv1Dropped; }
						const FString Key = FString::Printf(TEXT("%d|%.3f,%.3f,%.3f|%.4f,%.4f|%.3f,%.3f,%.3f,%.3f"),
							VID.GetValue(), N.X, N.Y, N.Z, UV.X, UV.Y, C.X, C.Y, C.Z, C.W);
						int32 Index;
						if (const int32* Found = Weld.Find(Key)) { Index = *Found; }
						else
						{
							FYddVert V;
							// the ped importer's map, inverted: UE cm -> GTA metres with the Y mirror; normals mirrored
							V.P = FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f);
							V.N = FVector3f(N.X, -N.Y, N.Z);
							V.UV = UV;
							V.C[0] = Byte01(C.X); V.C[1] = Byte01(C.Y); V.C[2] = Byte01(C.Z); V.C[3] = Byte01(C.W);
							V.S = SkinOf(VID);
							Index = G.V.Num();
							G.V.Add(V);
							Weld.Add(Key, Index);
							G.Mn = G.Mn.ComponentMin(V.P); G.Mx = G.Mx.ComponentMax(V.P);
						}
						G.Idx.Add(Index);
					}
				}
			}
			if (G.V.Num() > 0 && G.Idx.Num() >= 3)
			{
				if (G.V.Num() > 65535) { return Fail(FString::Printf(TEXT("%s/%s: geometry exceeds 65535 vertices (u16 indices) - split the mesh"), *D.Asset, *G.Slot)); }
				D.Mn = D.Mn.ComponentMin(G.Mn); D.Mx = D.Mx.ComponentMax(G.Mx);
				D.Geos.Add(MoveTemp(G));
			}
		}
		if (D.Geos.Num() == 0) { return Fail(FString::Printf(TEXT("%s: no polygon group with triangles"), *D.Asset)); }
		Ds.Add(MoveTemp(D));
	}
	// entries in ASCENDING hash order (2/2 dictionaries, ROUT 400/400); a duplicate hash is a refusal
	Ds.Sort([](const FDrawable& X, const FDrawable& Y) { return X.Hash < Y.Hash; });
	for (int32 i = 1; i < Ds.Num(); ++i)
	{
		if (Ds[i].Hash == Ds[i - 1].Hash) { return Fail(FString::Printf(TEXT("two entries hash to the same value: '%s' and '%s'"), *Ds[i - 1].Name, *Ds[i].Name)); }
	}
	const int32 N = Ds.Num();

	// ---- 3) the page plan: page = pow2 >= the largest single block (no block may span a page) ----
	uint32 Largest = 0x2000;
	for (const FDrawable& D : Ds)
	{
		for (const FGeo& G : D.Geos)
		{
			Largest = FMath::Max(Largest, (uint32)G.V.Num() * (uint32)kSkinStride);
			Largest = FMath::Max(Largest, (uint32)G.Idx.Num() * 2u);
		}
		const uint32 NG = (uint32)D.Geos.Num();
		Largest = FMath::Max(Largest, (NG + 1) * 0x20u);       // geoBounds
		Largest = FMath::Max(Largest, NG * 8u);                // geometry / shader pointer arrays
		Largest = FMath::Max(Largest, (uint32)kPedAlloc);      // one shader's parameter block
	}
	Largest = FMath::Max(Largest, (uint32)N * 0xD0u);          // the contiguous record block
	Largest = FMath::Max(Largest, (uint32)N * 8u);
	Largest = FMath::Max(Largest, (uint32)NumRig * 2u);
	const int32 PAGE = (int32)FMath::RoundUpToPowerOfTwo(Largest);

	TArray<uint8> Seg; Seg.AddZeroed(0x40);                    // the dictionary header, filled last
	bool bPageOverflow = false;
	auto Emit = [&Seg, PAGE, &bPageOverflow](const TArray<uint8>& Dt, int32 Align = 16) -> int32
	{
		if (Seg.Num() % Align) { Seg.AddZeroed(Align - (Seg.Num() % Align)); }
		if (Dt.Num() <= PAGE && (Seg.Num() % PAGE) + Dt.Num() > PAGE) { Seg.AddZeroed(PAGE - (Seg.Num() % PAGE)); }
		else if (Dt.Num() > PAGE) { bPageOverflow = true; }
		const int32 O = Seg.Num(); Seg.Append(Dt); return O;
	};
	auto EmitStr = [&](const FString& S) -> int32
	{
		TArray<uint8> B; B.SetNumZeroed(S.Len() + 1);
		for (int32 i = 0; i < S.Len(); ++i) { B[i] = (uint8)S[i]; }
		return Emit(B);
	};

	// ---- 4) the records (0xD0 each, contiguous, in hash order), then every block each one owns ----
	TArray<uint8> Recs; Recs.AddZeroed(N * 0xD0);
	const int32 ORecs = Emit(Recs);
	TArray<int32> RecBase;
	for (int32 i = 0; i < N; ++i) { RecBase.Add(ORecs + i * 0xD0); }
	TArray<int32> GeoBoneIdSlots;   // geometry+0x68 offsets, for the dictionary-level ownership audit
	int32 TotalVerts = 0, TotalTris = 0, TotalGeos = 0;
	for (int32 di = 0; di < N; ++di)
	{
		const FDrawable& D = Ds[di];
		const int32 Base = RecBase[di];
		const int32 NG = D.Geos.Num();
		const bool bUnion = NG > 1;
		TArray<uint8> GeoBounds; GeoBounds.AddZeroed((bUnion ? NG + 1 : 1) * 0x20);
		TArray<uint8> ShaderMap; ShaderMap.AddZeroed(FMath::Max(NG * 2, 8));
		TArray<int32> OGeo;
		for (int32 gi = 0; gi < NG; ++gi)
		{
			const FGeo& G = D.Geos[gi];
			// vertex data: stride 48, channels in ascending bit order (LAWS section 4)
			TArray<uint8> VD; VD.SetNumZeroed(G.V.Num() * kSkinStride);
			for (int32 v = 0; v < G.V.Num(); ++v)
			{
				const FYddVert& X = G.V[v]; const int32 o = v * kSkinStride;
				PVEC3(VD, o + 0, X.P);
				for (int32 k = 0; k < 4; ++k) { VD[o + 12 + k] = X.S.W[k]; VD[o + 16 + k] = X.S.I[k]; }
				PVEC3(VD, o + 20, X.N);
				for (int32 k = 0; k < 4; ++k) { VD[o + 32 + k] = X.C[k]; VD[o + 36 + k] = 0; }   // Colour1 = 0 (17/18 measured)
				PF32(VD, o + 40, X.UV.X); PF32(VD, o + 44, X.UV.Y);
			}
			const int32 OV = Emit(VD);
			TArray<uint8> ID; ID.SetNumZeroed(G.Idx.Num() * 2);
			for (int32 i = 0; i < G.Idx.Num(); ++i) { PU16(ID, i * 2, (uint16)G.Idx[i]); }
			const int32 OI = Emit(ID);
			// the bone-id table: identity over the rig (56/56 + 7/7 measured); OWN copy per geometry
			TArray<uint8> Bid; Bid.SetNumZeroed(NumRig * 2);
			for (int32 k = 0; k < NumRig; ++k) { PU16(Bid, k * 2, (uint16)k); }
			const int32 OBid = Emit(Bid);
			TArray<uint8> Fvf; Fvf.AddZeroed(0x10);                              // own fvf per geometry (crash #6)
			PU32(Fvf, 0x00, kSkinMask); PU16(Fvf, 0x04, (uint16)kSkinStride); Fvf[0x07] = kSkinChans;
			PU32(Fvf, 0x08, 0x55996996u); PU32(Fvf, 0x0c, 0x77555555u);
			const int32 OFvf = Emit(Fvf);
			TArray<uint8> Vb; Vb.AddZeroed(0x80);
			PU32(Vb, 0x00, VFT_VB); PU32(Vb, 0x04, 1);
			PU16(Vb, 0x08, (uint16)kSkinStride); PU16(Vb, 0x0a, 0);              // +0x0A = 0 in the game (7/7)
			PPTR(Vb, 0x10, OV); PU32(Vb, 0x18, (uint32)G.V.Num()); PPTR(Vb, 0x20, OV); PPTR(Vb, 0x30, OFvf);
			const int32 OVb = Emit(Vb);
			TArray<uint8> Ib; Ib.AddZeroed(0x60);
			PU32(Ib, 0x00, VFT_IB); PU32(Ib, 0x04, 1); PU32(Ib, 0x08, (uint32)G.Idx.Num()); PPTR(Ib, 0x10, OI);
			const int32 OIb = Emit(Ib);
			TArray<uint8> Ge; Ge.AddZeroed(0xa0);
			PU32(Ge, 0x00, VFT_GEO); PU32(Ge, 0x04, 1);
			PPTR(Ge, 0x18, OVb); PPTR(Ge, 0x38, OIb);
			PU32(Ge, 0x58, (uint32)G.Idx.Num()); PU32(Ge, 0x5c, (uint32)(G.Idx.Num() / 3));
			PU16(Ge, 0x60, (uint16)G.V.Num()); PU16(Ge, 0x62, 3);
			PPTR(Ge, 0x68, OBid);                                                // the skinned delta
			PU16(Ge, 0x70, (uint16)kSkinStride); PU16(Ge, 0x72, (uint16)NumRig);  // stride is a u16; +0x72 = bone-id count
			PPTR(Ge, 0x78, OV);
			const int32 OGe = Emit(Ge);
			OGeo.Add(OGe);
			GeoBoneIdSlots.Add(OGe + 0x68);
			const int32 Pair = (bUnion ? gi + 1 : gi) * 0x20;
			PVEC3(GeoBounds, Pair, G.Mn); PVEC3(GeoBounds, Pair + 0x10, G.Mx);
			PU16(ShaderMap, gi * 2, (uint16)gi);
			TotalVerts += G.V.Num(); TotalTris += G.Idx.Num() / 3; ++TotalGeos;
		}
		if (bUnion) { PVEC3(GeoBounds, 0x00, D.Mn); PVEC3(GeoBounds, 0x10, D.Mx); }
		const int32 OGb = Emit(GeoBounds);
		const int32 OSm = Emit(ShaderMap);
		TArray<uint8> GeoArr; GeoArr.AddZeroed(NG * 8);
		for (int32 i = 0; i < NG; ++i) { PPTR(GeoArr, i * 8, OGeo[i]); }
		const int32 OGa = Emit(GeoArr);
		TArray<uint8> Model; Model.AddZeroed(0x30);
		PU32(Model, 0x00, VFT_MODEL); PU32(Model, 0x04, 1);
		PPTR(Model, 0x08, OGa); PU16(Model, 0x10, (uint16)NG); PU16(Model, 0x12, (uint16)NG);
		PPTR(Model, 0x18, OGb); PPTR(Model, 0x20, OSm);
		Model[0x28] = (uint8)NumRig;                                             // rig bone count (52/52, 7/7)
		PU16(Model, 0x29, 1);                                                    // skinned (52/52, 7/7)
		Model[0x2b] = 0; Model[0x2c] = 0xff; Model[0x2d] = 1;                    // BoneIndex, RenderMask, skinned flag
		PU16(Model, 0x2e, (uint16)NG);
		const int32 OM = Emit(Model);
		TArray<uint8> ModelArr; ModelArr.AddZeroed(8); PPTR(ModelArr, 0, OM);
		const int32 OMa = Emit(ModelArr);
		TArray<uint8> ModelsHdr; ModelsHdr.AddZeroed(0x10); PPTR(ModelsHdr, 0x00, OMa); PU16(ModelsHdr, 0x08, 1); PU16(ModelsHdr, 0x0a, 1);
		const int32 OMh = Emit(ModelsHdr);

		// shaders: one `ped` shader per geometry (shader i <-> geometry i, the static writer's pairing)
		TArray<int32> OSh;
		for (int32 gi = 0; gi < NG; ++gi)
		{
			const FGeo& G = D.Geos[gi];
			auto Stub = [&](const FString& Name) -> int32
			{
				const int32 ON = EmitStr(Name.ToLower());                      // own string per stub (single ownership)
				TArray<uint8> St; St.AddZeroed(0x50);
				PU32(St, 0x00, VFT_STUB); PU32(St, 0x04, 1); PPTR(St, 0x28, ON); PU32(St, 0x30, 0x00020001u);
				return Emit(St);
			};
			int32 StubOfs[13]; for (int32 k = 0; k < 13; ++k) { StubOfs[k] = -1; }
			if (G.Diffuse.IsEmpty()) { ++Ds[di].TexMissing; }
			StubOfs[0] = Stub(G.Diffuse.IsEmpty() ? FString(TEXT("none")) : G.Diffuse);
			StubOfs[2] = Stub(TEXT("givemechecker"));
			if (!G.Normal.IsEmpty()) { StubOfs[3] = Stub(G.Normal); }            // unbound sampler = raw NULL (measured legal)
			if (!G.Spec.IsEmpty()) { StubOfs[4] = Stub(G.Spec); }
			TArray<uint8> Zero; Zero.AddZeroed(kPedAlloc);
			const int32 OTbl = Emit(Zero);
			int32 VecIdx = 0;
			for (int32 pi = 0; pi < kPedNPar; ++pi)
			{
				const FPedParam& PP = kPed[pi];
				PU32(Seg, OTbl + pi * 16, (PP.bTexture ? 0u : 1u) | ((uint32)PP.Reg << 8));   // class | register<<8
				if (PP.bTexture) { if (StubOfs[pi] >= 0) { PPTR(Seg, OTbl + pi * 16 + 8, StubOfs[pi]); } }
				else
				{
					const int32 VOfs = OTbl + kPedNPar * 16 + VecIdx * 16;
					for (int32 c = 0; c < 4; ++c) { PF32(Seg, VOfs + c * 4, PP.V[c]); }
					PPTR(Seg, OTbl + pi * 16 + 8, VOfs);
					++VecIdx;
				}
				PU32(Seg, OTbl + kPedHashOfs + pi * 4, RudeJoaat(PP.Name));
			}
			TArray<uint8> Blk; Blk.AddZeroed(0x30);
			PPTR(Blk, 0x00, OTbl);
			PU32(Blk, 0x08, RudeJoaat(TEXT("ped")));
			PU32(Blk, 0x10, 0x80000000u | (uint32)kPedNPar);                    // npar | bucket 0 <<8 | 0x8000<<16
			PU32(Blk, 0x14, ((uint32)kPedAlloc << 16) | (uint32)kPedHashOfs);
			PU32(Blk, 0x18, RudeJoaat(TEXT("ped.sps")));
			PU32(Blk, 0x20, 0x0000ff01u);
			PU32(Blk, 0x24, (uint32)kPedNTex << 24);
			OSh.Add(Emit(Blk));
		}
		TArray<uint8> ShArr; ShArr.AddZeroed(OSh.Num() * 8);
		for (int32 i = 0; i < OSh.Num(); ++i) { PPTR(ShArr, i * 8, OSh[i]); }
		const int32 OShArr = Emit(ShArr);
		TArray<uint8> SG; SG.AddZeroed(0x40);
		PU32(SG, 0x00, VFT_SG); PU32(SG, 0x04, 1);
		PPTR(SG, 0x10, OShArr); PU16(SG, 0x18, (uint16)OSh.Num()); PU16(SG, 0x1a, (uint16)OSh.Num());
		PU32(SG, 0x30, 8);                                                       // the 1-shader value (2/2); not load-bearing
		const int32 OSG = Emit(SG);
		const int32 OName = EmitStr(D.Name + TEXT(".#dd"));

		// the record: blockmap (+0x08), skeleton (+0x18), lights, bound (+0xC8) stay RAW ZERO
		PU32(Seg, Base + 0x00, VFT_DRAWABLE); PU32(Seg, Base + 0x04, 1);
		PPTR(Seg, Base + 0x10, OSG);
		{
			const FVector3f C = (D.Mn + D.Mx) * 0.5f;
			PVEC3(Seg, Base + 0x20, C); PF32(Seg, Base + 0x2c, (D.Mx - C).Size());
			PVEC3(Seg, Base + 0x30, D.Mn); PU32(Seg, Base + 0x3c, 0x7f800001u);
			PVEC3(Seg, Base + 0x40, D.Mx); PU32(Seg, Base + 0x4c, 0x7f800001u);
		}
		PPTR(Seg, Base + 0x50, OMh);
		for (int32 k = 0; k < 4; ++k) { PF32(Seg, Base + 0x70 + k * 4, 9998.f); }
		PU32(Seg, Base + 0x80, 0x0000ff01u);                                     // High present, bucket 0
		PU32(Seg, Base + 0x98, 0x00120000u);                                     // the static writer's constant (meaning unknown, not load-bearing)
		PPTR(Seg, Base + 0xa0, OMh);
		PPTR(Seg, Base + 0xa8, OName);
	}

	// ---- 5) the dictionary arrays, the block map, the header ----
	TArray<uint8> HashArr; HashArr.AddZeroed(N * 4);
	TArray<uint8> PtrArr; PtrArr.AddZeroed(N * 8);
	for (int32 i = 0; i < N; ++i) { PU32(HashArr, i * 4, Ds[i].Hash); PPTR(PtrArr, i * 8, RecBase[i]); }
	const int32 OHash = Emit(HashArr);
	const int32 OPtr = Emit(PtrArr);
	// block map = 16 + 8 * pages (ROUT 360/360); sized from a provisional page count with a margin
	const int32 ProvisionalPages = Seg.Num() / PAGE + 2;
	TArray<uint8> Bm; Bm.AddZeroed(FMath::Max(0x40, (16 + 8 * ProvisionalPages + 15) & ~15));
	const int32 OBm = Emit(Bm);
	uint32 Padded = 0, NPages = 0;
	const uint32 LowFlags = RudeBinSysPageFlagsUniform((uint32)Seg.Num(), (uint32)PAGE, Padded, NPages);
	if (LowFlags == 0xFFFFFFFFu) { return Fail(TEXT("unencodable page plan - resource too large")); }
	if (16 + 8 * (int32)NPages > Bm.Num()) { return Fail(TEXT("internal: block map smaller than the page count it declares")); }
	if (bPageOverflow) { return Fail(TEXT("internal: a block exceeded the page size and would span a page boundary (no-span law)")); }
	const uint32 SysFlag = 0xa0000000u | LowFlags;
	const uint32 GfxFlag = 0x50000000u;                                          // all-in-system, no graphics segment
	Seg.SetNumZeroed((int32)Padded);
	Seg[OBm + 0x08] = (uint8)NPages; Seg[OBm + 0x09] = 0;
	PU32(Seg, 0x00, VFT_DICT); PU32(Seg, 0x04, 1);
	PPTR(Seg, 0x08, OBm);
	PU32(Seg, 0x18, 1);
	PPTR(Seg, 0x20, OHash); PU32(Seg, 0x28, (uint32)N | ((uint32)N << 16));
	PPTR(Seg, 0x30, OPtr);  PU32(Seg, 0x38, (uint32)N | ((uint32)N << 16));

	// ---- 6) self-check before writing: single ownership across the WHOLE dictionary + the drawable laws ----
	{
		TMap<int32, int32> InDeg; int32 Shared = 0, DeclBad = 0, BoundsBad = 0; FString First;
		RudeBinOwn(InDeg, RU32(Seg, 0x08), TEXT("dict+0x08 blockmap"), Shared, First);
		RudeBinOwn(InDeg, RU32(Seg, 0x20), TEXT("dict+0x20 hashes"), Shared, First);
		RudeBinOwn(InDeg, RU32(Seg, 0x30), TEXT("dict+0x30 entries"), Shared, First);
		for (int32 i = 0; i < N; ++i) { RudeBinOwn(InDeg, RU32(Seg, OPtr + i * 8), FString::Printf(TEXT("entry[%d]"), i), Shared, First); }
		for (int32 i = 0; i < GeoBoneIdSlots.Num(); ++i) { RudeBinOwn(InDeg, RU32(Seg, GeoBoneIdSlots[i]), FString::Printf(TEXT("geo bone-ids #%d"), i), Shared, First); }
		for (int32 i = 0; i < N; ++i) { RudeBinVerifyDrawable(Seg, RecBase[i], InDeg, Shared, DeclBad, BoundsBad, First); }
		if (Shared > 0 || DeclBad > 0 || BoundsBad > 0)
		{
			return Fail(FString::Printf(TEXT("SELF-CHECK FAILED, refusing to write: %d shared block(s), %d bad declaration(s), %d bounds/count problem(s). First: %s"), Shared, DeclBad, BoundsBad, *First));
		}
	}

	// ---- 7) RSC7 v165 ----
	int32 ZSize = FCompression::CompressMemoryBound(NAME_Zlib, Seg.Num());
	TArray<uint8> Z; Z.SetNumUninitialized(ZSize);
	if (!FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZSize, Seg.GetData(), Seg.Num())) { return Fail(TEXT("zlib compress failed")); }
	if (ZSize < 7 || Z[0] != 0x78) { return Fail(TEXT("unexpected zlib stream")); }
	TArray<uint8> Out;
	auto AddU32 = [&](uint32 V) { Out.Add(V & 0xFF); Out.Add((V >> 8) & 0xFF); Out.Add((V >> 16) & 0xFF); Out.Add((V >> 24) & 0xFF); };
	Out.Add('R'); Out.Add('S'); Out.Add('C'); Out.Add('7');
	AddU32(165); AddU32(SysFlag); AddU32(GfxFlag);
	Out.Append(Z.GetData() + 2, ZSize - 6);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutYddPath), true);
	if (!FFileHelper::SaveArrayToFile(Out, *OutYddPath)) { return Fail(FString::Printf(TEXT("write failed: %s"), *OutYddPath)); }

	FString DJson;
	for (const FDrawable& D : Ds)
	{
		int32 DV = 0, DT = 0; FString Tex;
		for (const FGeo& G : D.Geos)
		{
			DV += G.V.Num(); DT += G.Idx.Num() / 3;
			Tex += FString::Printf(TEXT("%s{\"slot\":\"%s\",\"diffuse\":\"%s\",\"normal\":\"%s\",\"spec\":\"%s\"}"), Tex.IsEmpty() ? TEXT("") : TEXT(","),
				*RudeJsonEscape(G.Slot), *RudeJsonEscape(G.Diffuse), *RudeJsonEscape(G.Normal), *RudeJsonEscape(G.Spec));
		}
		DJson += FString::Printf(
			TEXT("%s{\"name\":\"%s\",\"hash\":\"0x%08x\",\"asset\":\"%s\",\"geometries\":%d,\"vertices\":%d,\"sourceVertices\":%d,\"triangles\":%d,")
			TEXT("\"bonesReferenced\":%d,\"meshBonesUnmapped\":%d,\"influencesUnmapped\":%d,\"influencesTruncated\":%d,\"verticesRebound\":%d,")
			TEXT("\"uv1Dropped\":%d,\"texturesMissing\":%d,\"textures\":[%s]}"),
			DJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(D.Name), D.Hash, *RudeJsonEscape(D.Asset), D.Geos.Num(), DV, D.SrcVerts, DT,
			D.MaxRig + 1, D.BonesUnmapped, D.InflUnmapped, D.InflTruncated, D.Rebound, D.Uv1Dropped, D.TexMissing, *Tex);
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"yddPath\":\"%s\",\"entries\":%d,\"drawables\":[%s],\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,")
		TEXT("\"rigBones\":%d,\"rig\":\"%s\",\"layout\":\"mask 0x7f stride 48 (Position BlendWeights BlendIndices Normal Colour0 Colour1 TexCoord0)\",")
		TEXT("\"shader\":\"ped (13 params, alloc %d, hashOfs %d)\",\"bytes\":%d,\"segSize\":%d,\"page\":%d,\"pages\":%u,\"sysFlags\":\"0x%08x\",")
		TEXT("\"selfCheck\":\"passed (dictionary-wide single ownership + geoBounds/count + declarations)\",")
		TEXT("\"note\":\"the game also needs the matching .ytd (ExportYtdBinary / ExportMeshTextures) and a ped variation (ymt) row for the drawable index; in-game load unverified\"}"),
		*RudeJsonEscape(OutYddPath), N, *DJson, TotalGeos, TotalVerts, TotalTris, NumRig, *RudeJsonEscape(RigName), kPedAlloc, kPedHashOfs,
		Out.Num(), Seg.Num(), PAGE, NPages, SysFlag);
#else
	return Fail(TEXT("editor-only"));
#endif
}

// ---- ProbeYddBinary ------------------------------------------------------------------------
FString URudeToolset::ProbeYddBinary(const FString& BinPath)
{
	using namespace RudeYdd;
	TArray<uint8> Sys, Gfx; uint32 Version = 0; FString Err;
	if (!RudeBinLoadRsc7(BinPath, Sys, Gfx, Version, Err)) { return Fail(Err); }
	if (Sys.Num() < 0x40) { return Fail(TEXT("system segment shorter than a dictionary header")); }
	const int32 N = (int32)(RU32(Sys, 0x28) & 0xFFFF);
	const int32 N2 = (int32)(RU32(Sys, 0x38) & 0xFFFF);
	int32 OHash = 0, OPtr = 0;
	if (N <= 0 || !Deref(Sys, RU32(Sys, 0x20), N * 4, OHash) || !Deref(Sys, RU32(Sys, 0x30), N * 8, OPtr))
	{
		return Fail(FString::Printf(TEXT("dictionary hash/entry arrays do not resolve (count %d / %d)"), N, N2));
	}
	int32 OBm = 0; int32 SysPages = -1, GfxPages = -1;
	if (Deref(Sys, RU32(Sys, 0x08), 16, OBm)) { SysPages = RU8(Sys, OBm + 8); GfxPages = RU8(Sys, OBm + 9); }
	bool bAscending = true;
	TArray<uint32> Hashes;
	for (int32 i = 0; i < N; ++i) { Hashes.Add(RU32(Sys, OHash + i * 4)); if (i > 0 && Hashes[i] < Hashes[i - 1]) { bAscending = false; } }

	TMap<int32, int32> InDeg; int32 Shared = 0, DeclBad = 0, BoundsBad = 0; FString First;
	RudeBinOwn(InDeg, RU32(Sys, 0x08), TEXT("dict+0x08 blockmap"), Shared, First);
	RudeBinOwn(InDeg, RU32(Sys, 0x20), TEXT("dict+0x20 hashes"), Shared, First);
	RudeBinOwn(InDeg, RU32(Sys, 0x30), TEXT("dict+0x30 entries"), Shared, First);

	static const int32 LodSlot[4] = { 0x50, 0x58, 0x60, 0x68 };
	static const TCHAR* LodName[4] = { TEXT("high"), TEXT("med"), TEXT("low"), TEXT("vlow") };
	FString EJson;
	int32 TotalGeos = 0, TotalVerts = 0, TotalTris = 0, TotalSum255 = 0, TotalSkinned = 0, TotalBadIdx = 0, EntriesUnresolved = 0, Skeletons = 0, Bounds = 0;
	int32 TotalNameMismatch = 0;
	for (int32 i = 0; i < N; ++i)
	{
		const uint32 PtrE = RU32(Sys, OPtr + i * 8);
		int32 Base = 0;
		if (!Deref(Sys, PtrE, 0xD0, Base)) { ++EntriesUnresolved; continue; }
		RudeBinOwn(InDeg, PtrE, FString::Printf(TEXT("entry[%d]"), i), Shared, First);
		const FString RawName = CStr(Sys, RU32(Sys, Base + 0xa8));
		FString Name = RawName; Name.RemoveFromEnd(TEXT(".#dd"));
		const bool bNameHash = !Name.IsEmpty() && RudeJoaat(Name) == Hashes[i];
		if (!Name.IsEmpty() && !bNameHash) { ++TotalNameMismatch; }
		const bool bSkel = RU32(Sys, Base + 0x18) != 0, bBound = RU32(Sys, Base + 0xc8) != 0;
		if (bSkel) { ++Skeletons; }
		if (bBound) { ++Bounds; }
		// shaders
		FString ShJson; int32 NSh = 0;
		int32 SG = 0;
		if (Deref(Sys, RU32(Sys, Base + 0x10), 0x40, SG))
		{
			NSh = RU16(Sys, SG + 0x18);
			int32 Arr = 0;
			if (NSh > 0 && NSh <= 4096 && Deref(Sys, RU32(Sys, SG + 0x10), NSh * 8, Arr))
			{
				for (int32 si = 0; si < NSh; ++si)
				{
					int32 Blk = 0;
					if (!Deref(Sys, RU32(Sys, Arr + si * 8), 0x30, Blk)) { continue; }
					const int32 NPar = (int32)(RU32(Sys, Blk + 0x10) & 0xFF);
					const int32 Bucket = (int32)((RU32(Sys, Blk + 0x10) >> 8) & 0xFF);
					int32 Tbl = 0; FString Texs; int32 Unbound = 0;
					if (NPar > 0 && NPar <= 96 && Deref(Sys, RU32(Sys, Blk + 0x00), NPar * 16, Tbl))
					{
						for (int32 pi = 0; pi < NPar; ++pi)
						{
							if (RU8(Sys, Tbl + pi * 16) != 0) { continue; }           // a vector
							const uint32 SP = RU32(Sys, Tbl + pi * 16 + 8);
							int32 Stub = 0;
							if (SP == 0) { ++Unbound; continue; }
							if (!Deref(Sys, SP, 0x34, Stub)) { continue; }
							const FString TN = CStr(Sys, RU32(Sys, Stub + 0x28));
							if (!TN.IsEmpty()) { Texs += FString::Printf(TEXT("%s\"%s\""), Texs.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(TN)); }
						}
					}
					ShJson += FString::Printf(TEXT("%s{\"hash\":\"0x%08x\",\"params\":%d,\"bucket\":%d,\"dsize\":%u,\"alloc\":%u,\"unboundSamplers\":%d,\"textures\":[%s]}"),
						si ? TEXT(",") : TEXT(""), RU32(Sys, Blk + 0x08), NPar, Bucket, RU16(Sys, Blk + 0x14), RU16(Sys, Blk + 0x16), Unbound, *Texs);
				}
			}
		}
		// models / geometries / skin
		FString MJson;
		int32 EGeos = 0, EVerts = 0, ETris = 0, ESum255 = 0, ESkinned = 0, EBadIdx = 0, EMaxBlend = -1, EBoneIds = -1, EIdentity = 0, ENonIdentity = 0, EBoneCount28 = -1, EFlag2d = -1, EU29 = -1;
		TSet<FString> Decls;
		for (int32 lod = 0; lod < 4; ++lod)
		{
			int32 MH = 0;
			const uint32 PL = RU32(Sys, Base + LodSlot[lod]);
			if (PL == 0 || !Deref(Sys, PL, 0x10, MH)) { continue; }
			const int32 NMod = RU16(Sys, MH + 8);
			int32 MArr = 0;
			if (NMod <= 0 || !Deref(Sys, RU32(Sys, MH + 0), NMod * 8, MArr)) { continue; }
			for (int32 mi = 0; mi < NMod; ++mi)
			{
				int32 M = 0;
				if (!Deref(Sys, RU32(Sys, MArr + mi * 8), 0x30, M)) { continue; }
				const int32 NGeo = RU16(Sys, M + 0x10);
				if (lod == 0 && mi == 0) { EBoneCount28 = RU8(Sys, M + 0x28); EU29 = RU16(Sys, M + 0x29); EFlag2d = RU8(Sys, M + 0x2d); }
				int32 GArr = 0;
				if (NGeo <= 0 || !Deref(Sys, RU32(Sys, M + 0x08), NGeo * 8, GArr)) { continue; }
				FString GJson;
				for (int32 gi = 0; gi < NGeo; ++gi)
				{
					int32 G = 0;
					if (!Deref(Sys, RU32(Sys, GArr + gi * 8), 0xa0, G)) { continue; }
					const int32 VCnt = RU16(Sys, G + 0x60), Stride = RU16(Sys, G + 0x70), NBid = RU16(Sys, G + 0x72);
					const uint32 IdxCount = RU32(Sys, G + 0x58);
					// bone-id table
					int32 BidO = 0; bool bIdent = false; int32 MaxBid = -1;
					const uint32 PBid = RU32(Sys, G + 0x68);
					if (NBid > 0 && Deref(Sys, PBid, NBid * 2, BidO))
					{
						RudeBinOwn(InDeg, PBid, FString::Printf(TEXT("entry[%d] geo bone-ids"), i), Shared, First);
						bIdent = true;
						for (int32 k = 0; k < NBid; ++k) { const int32 B = RU16(Sys, BidO + k * 2); MaxBid = FMath::Max(MaxBid, B); if (B != k) { bIdent = false; } }
						if (bIdent) { ++EIdentity; } else { ++ENonIdentity; }
						EBoneIds = FMath::Max(EBoneIds, NBid);
					}
					// declaration + vertices
					int32 VB = 0, Fvf = 0, VData = 0; uint32 Mask = 0; uint64 Nib = 0; int32 Ofs[16]; FString DE;
					bool bDecl = false, bSkin = false; int32 Sum255 = 0, BadIdx = 0, MaxBlend = -1;
					if (Deref(Sys, RU32(Sys, G + 0x18), 0x40, VB) && Deref(Sys, RU32(Sys, VB + 0x30), 0x10, Fvf))
					{
						Mask = RU32(Sys, Fvf); Nib = RU64(Sys, Fvf + 8);
						bDecl = RudeBinBuildDecl(Mask, Nib, Stride, Ofs, DE);
						if (!bDecl) { ++DeclBad; if (First.IsEmpty()) { First = DE; } }
						bSkin = bDecl && Ofs[1] >= 0 && Ofs[2] >= 0;
						if (bSkin && Deref(Sys, RU32(Sys, VB + 0x10), VCnt * Stride, VData))
						{
							for (int32 v = 0; v < VCnt; ++v)
							{
								const int32 o = VData + v * Stride;
								int32 S = 0;
								for (int32 k = 0; k < 4; ++k)
								{
									const int32 W = RU8(Sys, o + Ofs[1] + k), I = RU8(Sys, o + Ofs[2] + k);
									S += W;
									if (W > 0) { MaxBlend = FMath::Max(MaxBlend, I); if (NBid > 0 && I >= NBid) { ++BadIdx; } }
								}
								if (S == 255) { ++Sum255; }
							}
						}
					}
					Decls.Add(FString::Printf(TEXT("mask=0x%x,stride=%d"), Mask, Stride));
					GJson += FString::Printf(TEXT("%s{\"geo\":%d,\"verts\":%d,\"tris\":%u,\"stride\":%d,\"mask\":\"0x%x\",\"skinned\":%s,\"weightSum255\":%d,\"maxBlendIndex\":%d,\"blendIndicesOutsideTable\":%d,\"boneIds\":%d,\"boneIdsIdentity\":%s,\"boneIdsMax\":%d}"),
						gi ? TEXT(",") : TEXT(""), gi, VCnt, IdxCount / 3, Stride, Mask, bSkin ? TEXT("true") : TEXT("false"), Sum255, MaxBlend, BadIdx, NBid, bIdent ? TEXT("true") : TEXT("false"), MaxBid);
					++EGeos; EVerts += VCnt; ETris += (int32)(IdxCount / 3); ESum255 += Sum255; EBadIdx += BadIdx;
					if (bSkin) { ESkinned += VCnt; }
					EMaxBlend = FMath::Max(EMaxBlend, MaxBlend);
				}
				MJson += FString::Printf(TEXT("%s{\"lod\":\"%s\",\"model\":%d,\"geoCount\":%d,\"boneCountAt0x28\":%d,\"u16At0x29\":%d,\"flagAt0x2d\":%d,\"renderMask\":%d,\"geos\":[%s]}"),
					MJson.IsEmpty() ? TEXT("") : TEXT(","), LodName[lod], mi, NGeo, RU8(Sys, M + 0x28), RU16(Sys, M + 0x29), RU8(Sys, M + 0x2d), RU8(Sys, M + 0x2c), *GJson);
			}
		}
		RudeBinVerifyDrawable(Sys, Base, InDeg, Shared, DeclBad, BoundsBad, First);
		FString DeclJson;
		for (const FString& Dd : Decls) { DeclJson += FString::Printf(TEXT("%s\"%s\""), DeclJson.IsEmpty() ? TEXT("") : TEXT(","), *Dd); }
		EJson += FString::Printf(
			TEXT("%s{\"index\":%d,\"name\":\"%s\",\"hash\":\"0x%08x\",\"nameHashesToEntry\":%s,\"hasSkeleton\":%s,\"hasEmbeddedBound\":%s,\"shaderCount\":%d,\"shaders\":[%s],")
			TEXT("\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"skinnedVertices\":%d,\"weightSum255\":%d,\"maxBlendIndex\":%d,\"blendIndicesOutsideTable\":%d,")
			TEXT("\"boneIdTable\":%d,\"boneIdTablesIdentity\":%d,\"boneIdTablesNonIdentity\":%d,\"boneCountAt0x28\":%d,\"u16At0x29\":%d,\"flagAt0x2d\":%d,\"lodDistHigh\":%g,\"flagsHigh\":\"0x%08x\",")
			TEXT("\"declarations\":[%s],\"models\":[%s]}"),
			EJson.IsEmpty() ? TEXT("") : TEXT(","), i, *RudeJsonEscape(Name), Hashes[i], bNameHash ? TEXT("true") : TEXT("false"), bSkel ? TEXT("true") : TEXT("false"), bBound ? TEXT("true") : TEXT("false"),
			NSh, *ShJson, EGeos, EVerts, ETris, ESkinned, ESum255, EMaxBlend, EBadIdx, EBoneIds, EIdentity, ENonIdentity, EBoneCount28, EU29, EFlag2d,
			RF32(Sys, Base + 0x70), RU32(Sys, Base + 0x80), *DeclJson, *MJson);
		TotalGeos += EGeos; TotalVerts += EVerts; TotalTris += ETris; TotalSum255 += ESum255; TotalSkinned += ESkinned; TotalBadIdx += EBadIdx;
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"path\":\"%s\",\"version\":%u,\"sysSize\":%d,\"gfxSize\":%d,\"blockmapSysPages\":%d,\"blockmapGfxPages\":%d,")
		TEXT("\"entries\":%d,\"entriesAt0x38\":%d,\"entriesUnresolved\":%d,\"hashesAscending\":%s,\"namesNotHashingToEntry\":%d,\"entriesWithSkeleton\":%d,\"entriesWithBound\":%d,")
		TEXT("\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"skinnedVertices\":%d,\"weightSum255\":%d,\"blendIndicesOutsideTable\":%d,")
		TEXT("\"sharedBlocks\":%d,\"declsRejected\":%d,\"boundsProblems\":%d,\"firstProblem\":\"%s\",\"detail\":[%s]}"),
		*RudeJsonEscape(BinPath), Version, Sys.Num(), Gfx.Num(), SysPages, GfxPages,
		N, N2, EntriesUnresolved, bAscending ? TEXT("true") : TEXT("false"), TotalNameMismatch, Skeletons, Bounds,
		TotalGeos, TotalVerts, TotalTris, TotalSkinned, TotalSum255, TotalBadIdx,
		Shared, DeclBad, BoundsBad, *RudeJsonEscape(First), *EJson);
}
