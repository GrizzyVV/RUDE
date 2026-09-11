// RUDE - RAGE <-> Unreal Development Environment
//
// THE SKINNED DRAWABLE DICTIONARY WRITER (WP11 draft, 2026-09-06): ExportYddBinary + ProbeYddBinary.
// A FiveM ped's clothing is a .ydd (pgDictionary<gtaDrawable>) of SKINNED drawables that bind to the ped's
// .yft skeleton by rig index. This lane writes that container from USkeletalMesh assets (ImportPed's outputs,
// or anything skinned to the same USkeleton) and parses it back.
//
// EVERYTHING BYTE-LEVEL HERE WAS MEASURED (maintainer lane `ydd_writer` (`LAWS.md`), denominators there):
//   container   3/3 game binaries + ROUT's 400/400: RSC7 v165, 0x40 header, ascending hash array, 8-byte entry
//               pointers, 0xD0 records; entry +0x08 (blockmap) / +0x18 (skeleton) / +0xC8 (bound) raw NULL.
//   skinned     52/52 XML models + 7/7 binary: grmModel +0x28 = rig bone count, +0x29 = 1, +0x2D = 1;
//               grmGeometry +0x68 -> identity u16 bone-id table (56/56, ROUT 3,960/3,960), +0x72 = count;
//               fvf mask 0x7F / stride 48 / 7 channels with the GTAV1 nibble constant (the game's own Med/Low
//               layout and 16/28 High geometries); VB +0x0A = 0 in the game (the static writer's 0x59 is a
//               reference-oracle residue - both load); BlendWeights sum to 255 on 27,808/27,808 + 17,223/17,223.
//   shader      the `ped` template, identical in 12/12 XML + 3/3 binary shaders; +0x14 = 16*(13+8) = 336,
//               +0x16 = 432 (allocation law roundup16(16*(npar+nvec)+4*npar+32), fitted 11/11), +0x24 = 5<<24.
//   LOD groups  (WP12, RUDE_PEDLOD, maintainer lane `ped_lods` (`LAWS.md`)): every LOD the mesh carries is
//               written, High/Medium/Low; an absent group leaves its pointer AND its flag word raw zero
//               (172/172 absent Medium, 555/555 absent Low measured), and VeryLow is never written (0/2,125).
//               A Medium/Low geometry is the SAME skinned form as High (mask 0x7F stride 48, 1,980/1,984 and
//               1,573/1,579), so the LOD groups add call sites, not byte-level code. A ONE-LOD mesh emits
//               exactly the bytes the single-group writer emitted. The one word that follows the group count
//               is +0x98, written from the measured per-group-count mode and reported (`u98`).
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
#include "Engine/StaticMesh.h"   // RUDE_PEDPROPS: rigid entries from static meshes
#include "StaticMeshAttributes.h"

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
	// RUDE_PEDUV1: the two RICHER skinned layouts the game itself ships. Census over 12 real component-ped
	// dictionaries (componentpeds_a_m_m), 251 <Layout> blocks: 157 (62.5%) plain (the mask above),
	// 74 (29.5%) + TexCoord1 + Tangent, 20 (8.0%) + Tangent alone - so 94/251 (37.5%) of real ped geometry
	// carries more than mask 0x7F, and writing everything plain DROPS a second UV set and the tangent.
	// 0/251 carry TexCoord1 WITHOUT Tangent, so UV1 implies Tangent here and the writer enforces that.
	// Channel sizes come from the SAME GTAV1 nibble word already proven on the rigid prop path
	// (bit 7 = nibble 5 = float2, bit 14 = nibble 7 = float4); the declaration self-check re-derives the
	// stride from the mask before the file is written, so a wrong constant here cannot ship.
	//   0x40FF: +0 pos f3 |+12 bw |+16 bi |+20 nrm f3 |+32 c0 |+36 c1 |+40 uv0 f2 |+48 uv1 f2 |+56 tan f4 = 72
	//   0x407F: +0 pos f3 |+12 bw |+16 bi |+20 nrm f3 |+32 c0 |+36 c1 |+40 uv0 f2 |+48 tan f4             = 64
	static const uint32 kSkinUv1TanMask = 0x40FF; static const int32 kSkinUv1TanStride = 72;
	static const uint32 kSkinTanMask    = 0x407F; static const int32 kSkinTanStride    = 64;
	// the fvf +0x07 byte is the channel COUNT - popcount of the mask (7 on BOTH proven layouts, 0x7F and 0x40F9)
	static constexpr uint8 RudeChanCount(uint32 Mask)
	{
		uint8 N = 0;
		for (int32 b = 0; b < 16; ++b) { if ((Mask >> b) & 1u) { ++N; } }
		return N;
	}
	// RUDE_PEDPROPS_BEGIN templates
	// ---- ped props (WP11, maintainer lane `pedprops` (`LAWS.md`)): the RIGID entry and the `ped_alpha` template ----
	// The game's prop layout: Position(0) Normal(3) Colour0(4) Colour1(5) TexCoord0(6) TexCoord1(7) Tangent(14) ->
	// mask 0x40F9, stride 64, 7 channels (2,674/2,677 prop geometries over 709 peds). The fvf nibble word is the same
	// GTAV1 constant (channel 14 = type 7 = float4). Bytes: +0 pos | +12 nrm | +24 c0 | +28 c1 | +32 uv0 | +40 uv1 | +48 tan.
	static const uint32 kRigidMask = 0x40F9; static const int32 kRigidStride = 64; static const uint8 kRigidChans = 7;
	// the popcount rule is not asserted by hand - it is CHECKED against the two layouts that have already
	// shipped and loaded in the game, at compile time. If either constant is ever edited wrongly, this fails.
	static_assert(RudeChanCount(kSkinMask) == kSkinChans, "skinned channel count must be the mask popcount");
	static_assert(RudeChanCount(kRigidMask) == kRigidChans, "rigid channel count must be the mask popcount");
	// `ped_alpha` (bucket 1; the lens of every 2-geometry prop, 932/2,672 prop shaders; FileName ped_alpha.sps 8/8):
	// 12 params, 4 samplers - no VolumeSampler, Bump/Spec on registers 5/6 (registers verbatim from the game's files),
	// the same 8 vec4s on 187..180. Vector VALUES vary per file (a_m_m_business_01 p_eyes_000 shown): per-shader
	// constants, copied, never computed.
	static const FPedParam kPedAlpha[12] =
	{
		{ TEXT("DiffuseSampler"),        0,   true,  { 0.f, 0.f, 0.f, 0.f } },
		{ TEXT("TextureSamplerDiffPal"), 2,   true,  { 0.f, 0.f, 0.f, 0.f } },
		{ TEXT("BumpSampler"),           5,   true,  { 0.f, 0.f, 0.f, 0.f } },
		{ TEXT("SpecSampler"),           6,   true,  { 0.f, 0.f, 0.f, 0.f } },
		{ TEXT("umGlobalParams"),        187, false, { 0.0025f, 0.0025f, 7.f, 7.f } },
		{ TEXT("envEffFatThickness"),    186, false, { 25.f, 25.f, 0.f, 0.f } },
		{ TEXT("specularIntensityMult"), 185, false, { 0.9f, 0.f, 0.f, 0.f } },
		{ TEXT("specularFalloffMult"),   184, false, { 400.f, 0.f, 0.f, 0.f } },
		{ TEXT("specularFresnel"),       183, false, { 0.8f, 0.f, 0.f, 0.f } },
		{ TEXT("bumpiness"),             182, false, { 0.65f, 0.f, 0.f, 0.f } },
		{ TEXT("detailSettings"),        181, false, { 0.1f, 0.75f, 40.f, 0.f } },
		{ TEXT("StubbleControl"),        180, false, { 2.f, 0.6f, 0.f, 0.f } },
	};
	struct FShaderTemplate { const TCHAR* Name; const FPedParam* Params; int32 NPar; int32 NVec; int32 NTex; int32 Bucket; };
	static const FShaderTemplate kTplPed = { TEXT("ped"), kPed, kPedNPar, kPedNVec, kPedNTex, 0 };
	static const FShaderTemplate kTplPedAlpha = { TEXT("ped_alpha"), kPedAlpha, 12, 8, 4, 1 };
	static int32 TplHashOfs(const FShaderTemplate& T) { return 16 * (T.NPar + T.NVec); }                                    // the measured +0x14 (336 ped / 320 ped_alpha)
	static int32 TplAlloc(const FShaderTemplate& T) { return (16 * (T.NPar + T.NVec) + 4 * T.NPar + 32 + 15) & ~15; }   // the fitted +0x16 (432 / 400)
	static const int32 kPedAlphaAlloc = (16 * (12 + 8) + 4 * 12 + 32 + 15) & ~15;                                        // 400
	// RUDE_PEDPROPS_END templates
	// measured vfts of dictionary objects (3/3 binaries) - build residue, kept for likeness, not load-bearing
	static const uint32 VFT_DICT = 0x40571578u, VFT_DRAWABLE = 0x40571168u, VFT_SG = 0x406117F0u, VFT_STUB = 0x406187F8u,
	                    VFT_MODEL = 0x4060EA98u, VFT_GEO = 0x40616798u, VFT_VB = 0x4061B3F8u, VFT_IB = 0x4061B158u;

	struct FSkin { uint8 W[4]; uint8 I[4]; };
	struct FYddVert { FVector3f P; FVector3f N; FVector2f UV; uint8 C[4]; FSkin S; FVector2f UV1 = FVector2f::ZeroVector; FVector4f T = FVector4f(1.f, 0.f, 0.f, 1.f); };   // RUDE_PEDPROPS: UV1 + tangent ride rigid entries
	struct FGeo
	{
		FString Slot, Diffuse, Normal, Spec, Mat;   // Mat = the material the slot resolved to (measurement)
		FString Preset; bool bRigid = false;        // RUDE_PEDPROPS: the slot's shader preset (ped / ped_alpha); rigid = no skin
		// RUDE_PEDUV1: which SKINNED layout this geometry writes. Decided from the SOURCE, not from a preference:
		// bUv1 = the mesh description actually has a second UV channel; bTan = that, or the slot is normal-mapped
		// (a tangent is the thing a normal map needs). Both false = the proven mask 0x7F / stride 48.
		bool bUv1 = false, bTan = false;
		TArray<FYddVert> V; TArray<int32> Idx;
		FVector3f Mn = FVector3f(FLT_MAX), Mx = FVector3f(-FLT_MAX);
	};
	// RUDE_PEDUV1: ONE place decides a geometry's vertex format. The page plan and the packer MUST agree, and
	// on the first run of the widening gate they did not - sizing kept the old skinned stride while the packer
	// wrote the wider one, and the no-span law refused the file. Lift, never duplicate: this is the exact shape
	// that rule exists to prevent, and it cost a gate run to prove the rule still earns its keep.
	static uint32 RudeGeoMask(const FGeo& G)
	{
		return G.bRigid ? kRigidMask : G.bUv1 ? kSkinUv1TanMask : G.bTan ? kSkinTanMask : kSkinMask;
	}
	static int32 RudeGeoStride(const FGeo& G)
	{
		return G.bRigid ? kRigidStride : G.bUv1 ? kSkinUv1TanStride : G.bTan ? kSkinTanStride : kSkinStride;
	}
	struct FDrawable
	{
		FString Name, Asset; uint32 Hash = 0;
		TArray<FGeo> Geos;
		// RUDE_PEDLOD: the Medium / Low groups (index 0 = Medium, 1 = Low) and the entry's four lodDist floats.
		// An EMPTY array = the group is absent, and its record slot (+0x58 / +0x60) and flag word (+0x84 / +0x88)
		// stay raw zero - a shape the game itself ships (172/172 absent Medium and 555/555 absent Low words read
		// 0x0). VeryLow is never written (0/2,125 game entries carry one). maintainer lane `ped_lods` (`LAWS.md`).
		TArray<FGeo> LodGeos[2];
		float LodDist[4] = { 9998.f, 9998.f, 9998.f, 9998.f };   // measured constant on 2,119/2,125 game entries
		bool bLodDistCarried = false;                            // the four floats came from the caller (LODDIST=), not this default
		int32 LodShaderSubstituted = 0;                          // the game names the LOD shader `ped_default` (1,022/1,022)
		int32 LodsSkipped = 0;                                   // a LOD the mesh declares but cannot be gathered (no description / no bones)
		uint32 U98 = 0x00120000u;                                // +0x98, chosen by GROUP COUNT (law 8) and reported in the verdict
		FVector3f Mn = FVector3f(FLT_MAX), Mx = FVector3f(-FLT_MAX);
		int32 SrcVerts = 0, InflUnmapped = 0, InflTruncated = 0, Rebound = 0, BonesUnmapped = 0, Uv1Dropped = 0, TexMissing = 0, MaxRig = -1;
		// RUDE_PEDUV1: the conservation counters for the richer layouts - geometries written in each of the three
		// skinned forms, and how many second-UV vertices actually reached the file instead of being dropped.
		int32 GeosSkinPlain = 0, GeosSkinTan = 0, GeosSkinUv1Tan = 0, Uv1Carried = 0, TangentsWritten = 0;
		int32 TangentsSynthesised = 0, TangentsCarried = 0;
		bool bRigid = false; int32 ShaderSubstituted = 0;   // RUDE_PEDPROPS
	};

	static FString Fail(const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); }
	static uint8 Byte01(float F) { return (uint8)FMath::Clamp(FMath::RoundToInt(F * 255.f), 0, 255); }
	// RUDE_PEDPROPS_BEGIN rigid gather
	// The RIGID entry's geometry from a UStaticMesh's LOD0 description (a ped prop: ImportPed's <ped>/props/ meshes):
	// one geometry per polygon group, welded by (vertex, normal, uv0, uv1, colour, tangent), positions / normals /
	// tangents back through the importer's Y mirror (tangent handedness flips under a reflection). The shader preset
	// rides on the slot name (<preset>__<geo>, ImportDrawableNode's spelling) so a lens keeps ped_alpha.
	static bool GatherRigid(UStaticMesh* SM, FDrawable& D, FString& Err)
	{
		const FMeshDescription* MD = SM->GetMeshDescription(0);
		if (!MD) { Err = FString::Printf(TEXT("%s: no MeshDescription on LOD0 (no source geometry to export)"), *D.Asset); return false; }
		FStaticMeshConstAttributes A(*MD);
		TVertexAttributesConstRef<FVector3f> Positions = A.GetVertexPositions();
		TVertexInstanceAttributesConstRef<FVector3f> InstNormals = A.GetVertexInstanceNormals();
		TVertexInstanceAttributesConstRef<FVector3f> InstTangents = A.GetVertexInstanceTangents();
		TVertexInstanceAttributesConstRef<float> InstSigns = A.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesConstRef<FVector2f> InstUVs = A.GetVertexInstanceUVs();
		TVertexInstanceAttributesConstRef<FVector4f> InstColors = A.GetVertexInstanceColors();
		TPolygonGroupAttributesConstRef<FName> GroupSlots = A.GetPolygonGroupMaterialSlotNames();
		const int32 NumUV = InstUVs.GetNumChannels();
		D.SrcVerts = MD->Vertices().Num();
		const TArray<FStaticMaterial>& Mats = SM->GetStaticMaterials();
		for (const FPolygonGroupID GroupID : MD->PolygonGroups().GetElementIDs())
		{
			FGeo G;
			G.bRigid = true;
			G.Slot = GroupSlots[GroupID].ToString();
			{
				FString L, R;
				G.Preset = G.Slot.Split(TEXT("__"), &L, &R, ESearchCase::CaseSensitive, ESearchDir::FromEnd) ? L.ToLower() : G.Slot.ToLower();
			}
			int32 SlotIdx = INDEX_NONE;
			for (int32 i = 0; i < Mats.Num(); ++i) { if (Mats[i].MaterialSlotName == GroupSlots[GroupID]) { SlotIdx = i; break; } }
			if (SlotIdx == INDEX_NONE && Mats.IsValidIndex(GroupID.GetValue())) { SlotIdx = GroupID.GetValue(); }
			if (Mats.IsValidIndex(SlotIdx))
			{
				G.Mat = Mats[SlotIdx].MaterialInterface ? Mats[SlotIdx].MaterialInterface->GetPathName() : FString(TEXT("null"));
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
						const FVector3f Tn = InstTangents[Inst];
						const float Sg = InstSigns[Inst];
						const FVector2f UV = InstUVs.Get(Inst, 0);
						const FVector2f UV1 = NumUV > 1 ? InstUVs.Get(Inst, 1) : FVector2f::ZeroVector;
						const FVector4f C = InstColors[Inst];
						const FString Key = FString::Printf(TEXT("%d|%.3f,%.3f,%.3f|%.4f,%.4f|%.4f,%.4f|%.3f,%.3f,%.3f,%.3f|%.3f,%.3f,%.3f,%.1f"),
							VID.GetValue(), N.X, N.Y, N.Z, UV.X, UV.Y, UV1.X, UV1.Y, C.X, C.Y, C.Z, C.W, Tn.X, Tn.Y, Tn.Z, Sg);
						int32 Index;
						if (const int32* Found = Weld.Find(Key)) { Index = *Found; }
						else
						{
							FYddVert V;
							V.P = FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f);   // UE cm -> GTA metres, Y mirror (the importer inverted)
							V.N = FVector3f(N.X, -N.Y, N.Z);
							V.UV = UV; V.UV1 = UV1;
							// mirrored tangent; the bitangent sign flips under the reflection (B = w N x T, det -1): a UE +1 is a RAGE -1
							V.T = FVector4f(Tn.X, -Tn.Y, Tn.Z, Sg >= 0.f ? -1.f : 1.f);
							V.C[0] = Byte01(C.X); V.C[1] = Byte01(C.Y); V.C[2] = Byte01(C.Z); V.C[3] = Byte01(C.W);
							FMemory::Memzero(V.S);
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
				if (G.V.Num() > 65535) { Err = FString::Printf(TEXT("%s/%s: geometry exceeds 65535 vertices (u16 indices) - split the mesh"), *D.Asset, *G.Slot); return false; }
				D.Mn = D.Mn.ComponentMin(G.Mn); D.Mx = D.Mx.ComponentMax(G.Mx);
				D.Geos.Add(MoveTemp(G));
			}
		}
		if (D.Geos.Num() == 0) { Err = FString::Printf(TEXT("%s: no polygon group with triangles"), *D.Asset); return false; }
		return true;
	}
	// RUDE_PEDPROPS_END rigid gather
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
	// RUDE_PEDLOD: `LODDIST=a/b/c/d,a/b/c/d,...` - the four +0x70..0x7C floats per ENTRY, in the same order as the
	// asset list, an empty group meaning "this entry has none, use the default". ExportPedReplace fills it from the
	// outfit ImportPed wrote, which is how an entry re-emits the floats the game spelled (maintainer lane `ped_lods`
	// (`LAWS.md`) law 2.3). Empty entries are KEPT by the split so entry i keeps pairing with asset i.
	TArray<FString> LodDistSpecs;
	int32 LodDistCarried = 0, LodDistMalformed = 0;
	{
		TArray<FString> Toks; Options.ParseIntoArray(Toks, TEXT(";"), true);
		for (const FString& T : Toks)
		{
			const FString U = T.TrimStartAndEnd();
			if (U.StartsWith(TEXT("SKELETON="), ESearchCase::IgnoreCase)) { RigPath = U.Mid(9).TrimStartAndEnd(); }
			else if (U.StartsWith(TEXT("LODDIST="), ESearchCase::IgnoreCase)) { U.Mid(8).ParseIntoArray(LodDistSpecs, TEXT(","), false); }
		}
	}

	// ---- 1) the meshes and the rig (bone ORDER = the ped's yft order, as ImportPed built the USkeleton) ----
	// RUDE_PEDPROPS: an entry is a USkeletalMesh (skinned - clothing) or a UStaticMesh (RIGID - a ped prop: hats /
	// glasses / earpieces / watches; 977/977 game prop models are unskinned, maintainer lane `pedprops` (`LAWS.md`)). The
	// rig is needed only when a skinned mesh is present.
	TArray<USkeletalMesh*> Meshes;
	TArray<UStaticMesh*> Statics;   // parallel to Meshes: exactly one of the two is non-null per entry
	for (const FString& P : Paths)
	{
		UObject* O = LoadObject<UObject>(nullptr, *P.TrimStartAndEnd());
		USkeletalMesh* SK = Cast<USkeletalMesh>(O);
		UStaticMesh* SM = Cast<UStaticMesh>(O);
		if (!SK && !SM) { return Fail(FString::Printf(TEXT("neither a SkeletalMesh nor a StaticMesh: %s"), *P.TrimStartAndEnd())); }
		Meshes.Add(SK); Statics.Add(SM);
	}
	USkeletalMesh* FirstSkinned = nullptr;
	for (USkeletalMesh* M : Meshes) { if (M) { FirstSkinned = M; break; } }
	USkeleton* RigSkel = RigPath.IsEmpty() ? (FirstSkinned ? FirstSkinned->GetSkeleton() : nullptr) : LoadObject<USkeleton>(nullptr, *RigPath);
	if (!RigPath.IsEmpty() && !RigSkel) { return Fail(FString::Printf(TEXT("SKELETON not found: %s"), *RigPath)); }
	const FReferenceSkeleton* Rig = RigSkel ? &RigSkel->GetReferenceSkeleton() : (FirstSkinned ? &FirstSkinned->GetRefSkeleton() : nullptr);
	const FString RigName = RigSkel ? RigSkel->GetPathName() : (FirstSkinned ? FirstSkinned->GetPathName() + TEXT(" (own reference skeleton; no USkeleton assigned)") : FString(TEXT("none (every entry rigid)")));
	const int32 NumRig = Rig ? Rig->GetNum() : 0;
	if (FirstSkinned && NumRig <= 0) { return Fail(TEXT("the rig has no bones")); }
	// grmModel+0x28 is a byte and BlendIndices are bytes: the measured rigs are 98..106 bones
	if (NumRig > 255) { return Fail(FString::Printf(TEXT("the rig has %d bones; the format carries the bone count in a byte (grmModel+0x28) and blend indices as bytes"), NumRig)); }
	TMap<FName, int32> RigIndex;
	for (int32 i = 0; i < NumRig; ++i) { RigIndex.Add(Rig->GetBoneName(i), i); }

	// ---- 2) gather: one drawable per mesh, one geometry per polygon group, skin per vertex ----
	TArray<FDrawable> Ds;
	for (int32 mi = 0; mi < Meshes.Num(); ++mi)
	{
		FDrawable D;
		// RUDE_PEDLOD: the entry's own four lodDist floats when the caller supplied them; anything that is not four
		// numbers is COUNTED and the measured modal 9998 x4 stands (no silent default, the toolset rule). This runs
		// before the rigid / skinned split so a prop entry can carry them too - the game gives every prop the modal
		// value, so in practice nothing changes for the rigid path.
		if (LodDistSpecs.IsValidIndex(mi) && !LodDistSpecs[mi].TrimStartAndEnd().IsEmpty())
		{
			TArray<FString> Fl; LodDistSpecs[mi].ParseIntoArray(Fl, TEXT("/"), true);
			if (Fl.Num() == 4)
			{
				for (int32 k = 0; k < 4; ++k) { D.LodDist[k] = FCString::Atof(*Fl[k]); }
				D.bLodDistCarried = true; ++LodDistCarried;
			}
			else { ++LodDistMalformed; }
		}
		if (Statics[mi])
		{
			// RUDE_PEDPROPS: the RIGID entry - a static mesh gathered without skin (GatherRigid)
			UStaticMesh* SM = Statics[mi];
			D.Asset = SM->GetPathName();
			D.bRigid = true;
			D.Name = Names.IsValidIndex(mi) && !Names[mi].TrimStartAndEnd().IsEmpty() ? Names[mi].TrimStartAndEnd().ToLower() : SM->GetName().ToLower();
			D.Hash = RudeJoaat(D.Name);
			FString GErr;
			if (!GatherRigid(SM, D, GErr)) { return Fail(GErr); }
			Ds.Add(MoveTemp(D));
			continue;
		}
		USkeletalMesh* SK = Meshes[mi];
		D.Asset = SK->GetPathName();
		D.Name = Names.IsValidIndex(mi) && !Names[mi].TrimStartAndEnd().IsEmpty() ? Names[mi].TrimStartAndEnd().ToLower() : SK->GetName().ToLower();
		D.Hash = RudeJoaat(D.Name);
		// RUDE_PEDLOD: EVERY LOD the mesh carries, up to three groups (High / Medium / Low). The vertex layout, the
		// skin bytes, the identity bone-id table and the skinned model record are IDENTICAL in a Medium / Low group
		// (maintainer lane `ped_lods` (`LAWS.md`) laws 5.1-5.5: mask 0x7F stride 48 on 1,980/1,984 Medium and
		// 1,573/1,579 Low geometries; model words (1,0,255,1) on 1,953/1,953 and 1,570/1,571; bone-id tables identity
		// 1,984/1,984 and 1,578/1,579; weight sums 255 on 14,058/14,058 and 2,459/2,459 vertices) - so this is the
		// SAME gather, run once per LOD. A mesh with ONE LOD walks it once and produces exactly the bytes the
		// single-group writer produced. VeryLow is never gathered (0/2,125 game entries carry one, law 1.1).
		const int32 NumLods = FMath::Clamp(SK->GetLODNum(), 1, 3);
		for (int32 Lod = 0; Lod < NumLods; ++Lod)
		{
			const FMeshDescription* MD = SK->GetMeshDescription(Lod);
			if (!MD) { if (Lod == 0) { return Fail(FString::Printf(TEXT("%s: no MeshDescription on LOD0 (no source geometry to export)"), *D.Asset)); } ++D.LodsSkipped; break; }
			FSkeletalMeshConstAttributes A(*MD);
			TVertexAttributesConstRef<FVector3f> Positions = A.GetVertexPositions();
			TVertexInstanceAttributesConstRef<FVector3f> InstNormals = A.GetVertexInstanceNormals();
			TVertexInstanceAttributesConstRef<FVector2f> InstUVs = A.GetVertexInstanceUVs();
			TVertexInstanceAttributesConstRef<FVector4f> InstColors = A.GetVertexInstanceColors();
			// RUDE_PEDUV1: a skeletal mesh description carries tangents like a static one (FSkeletalMeshConstAttributes
			// derives from the static set) - they were simply never read on this path.
			TVertexInstanceAttributesConstRef<FVector3f> InstTangents = A.GetVertexInstanceTangents();
			TVertexInstanceAttributesConstRef<float> InstSigns = A.GetVertexInstanceBinormalSigns();
			TPolygonGroupAttributesConstRef<FName> GroupSlots = A.GetPolygonGroupMaterialSlotNames();
			FSkinWeightsVertexAttributesConstRef SkinWeights = A.GetVertexSkinWeights();
			const int32 NumUV = InstUVs.GetNumChannels();
			if (Lod == 0) { D.SrcVerts = MD->Vertices().Num(); }
			// mesh-description bone -> rig bone, by NAME
			const int32 NB = A.GetNumBones();
			// RUDE_PEDLOD: LOD0 without bones is still a hard refusal (it is not a skinned mesh). A LOWER LOD without
			// them is one bad LOD, not a bad dictionary: it is SKIPPED and COUNTED (`lodsSkipped`), the same way an
			// absent mesh description is handled one line above - so one defective LOD can never cost every entry
			// in the file its export.
			if (NB == 0) { if (Lod == 0) { return Fail(FString::Printf(TEXT("%s: the mesh description carries no bones - not a skinned mesh"), *D.Asset)); } ++D.LodsSkipped; break; }
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
					G.Mat = Mats[SlotIdx].MaterialInterface ? Mats[SlotIdx].MaterialInterface->GetPathName() : FString(TEXT("null"));
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
				// RUDE_PEDUV1: pick this geometry's layout from what the SOURCE carries, BEFORE welding - the weld key must
				// include every channel that will reach the file, or two instances differing only in UV1 or in tangent
				// handedness collapse into one and the extra channel is silently averaged away (the law-56 shape again).
				// Choose this geometry's layout from what the SOURCE actually HOLDS - not from what its material could use.
				// An all-zero second UV channel is not a second UV set (UE hands one out freely), and a normal-mapped slot
				// whose description carries no tangent basis is not a tangent: declaring either would cost bytes a vertex to
				// store nothing. This is also what keeps LODs on the game's own shape - the measured ped law is mask 0x7f on
				// 1,980/1,984 Medium and 1,573/1,579 Low geometries, and those LOD blocks ship no tangent for us to carry.
				// The one derivation allowed is below: UV1 present but no basis, because 0/251 game blocks carry UV1 alone.
				bool bSrcUv1 = false, bSrcTan = false;
				for (const FPolygonID PID : MD->GetPolygonGroupPolygonIDs(GroupID))
				{
					for (const FTriangleID TID : MD->GetPolygonTriangles(PID))
					{
						for (const FVertexInstanceID VI : MD->GetTriangleVertexInstances(TID))
						{
							if (NumUV > 1 && !bSrcUv1 && !InstUVs.Get(VI, 1).IsZero()) { bSrcUv1 = true; }
							if (!bSrcTan && !InstTangents[VI].IsNearlyZero()) { bSrcTan = true; }
							if (bSrcUv1 && bSrcTan) { break; }
						}
						if (bSrcUv1 && bSrcTan) { break; }
					}
					if (bSrcUv1 && bSrcTan) { break; }
				}
				G.bUv1 = bSrcUv1;
				G.bTan = bSrcUv1 || bSrcTan;
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
							const FVector2f UV1 = G.bUv1 ? InstUVs.Get(Inst, 1) : FVector2f::ZeroVector;
							const FVector3f Tn = InstTangents[Inst];
							const float Sg = InstSigns[Inst];
							// only a SECOND UV set the chosen layout cannot hold is a drop; carried ones are counted at pack time
							if (NumUV > 1 && !G.bUv1 && !InstUVs.Get(Inst, 1).IsZero()) { ++D.Uv1Dropped; }
							const FString Key = FString::Printf(TEXT("%d|%.3f,%.3f,%.3f|%.4f,%.4f|%.3f,%.3f,%.3f,%.3f|%.4f,%.4f|%.3f,%.3f,%.3f,%.1f"),
								VID.GetValue(), N.X, N.Y, N.Z, UV.X, UV.Y, C.X, C.Y, C.Z, C.W,
								G.bUv1 ? UV1.X : 0.f, G.bUv1 ? UV1.Y : 0.f,
								G.bTan ? Tn.X : 0.f, G.bTan ? Tn.Y : 0.f, G.bTan ? Tn.Z : 0.f, G.bTan ? Sg : 0.f);
							int32 Index;
							if (const int32* Found = Weld.Find(Key)) { Index = *Found; }
							else
							{
								FYddVert V;
								// the ped importer's map, inverted: UE cm -> GTA metres with the Y mirror; normals mirrored
								V.P = FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f);
								V.N = FVector3f(N.X, -N.Y, N.Z);
								V.UV = UV;
								// RUDE_PEDUV1: the richer channels, under the SAME mirror the rigid path proved - the bitangent sign flips
								// under the reflection (B = w N x T, det -1), so a UE +1 is a RAGE -1. Zero when the layout has no room.
								V.UV1 = G.bUv1 ? UV1 : FVector2f::ZeroVector;
								V.T = G.bTan ? FVector4f(Tn.X, -Tn.Y, Tn.Z, Sg >= 0.f ? -1.f : 1.f) : FVector4f(1.f, 0.f, 0.f, 1.f);
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
					// RUDE_PEDUV1: a DECLARED tangent channel must be FILLED. The game's own ped files carry a tangent wherever
					// they carry a second UV set (74/251 both, 20/251 tangent only, 0/251 UV1 without one), so a geometry that
					// reaches here with UV1 but no basis - a user mesh whose description never had one - gets the standard
					// per-triangle derivation from positions and UV0 rather than a channel full of zeros. It is COUNTED as
					// synthesised, because a derived basis is not the file's own and the distinction has to survive the verdict.
					if (G.bTan)
					{
						bool bHaveBasis = false;
						for (const FYddVert& X : G.V) { if (!FVector3f(X.T.X, X.T.Y, X.T.Z).IsNearlyZero()) { bHaveBasis = true; break; } }
						if (bHaveBasis) { D.TangentsCarried += G.V.Num(); }
						else
						{
							TArray<FVector3f> Tan, Bit;
							Tan.SetNumZeroed(G.V.Num()); Bit.SetNumZeroed(G.V.Num());
							for (int32 t = 0; t + 2 < G.Idx.Num(); t += 3)
							{
								const int32 a0 = G.Idx[t], a1 = G.Idx[t + 1], a2 = G.Idx[t + 2];
								const FVector3f e1 = G.V[a1].P - G.V[a0].P, e2 = G.V[a2].P - G.V[a0].P;
								const FVector2f d1 = G.V[a1].UV - G.V[a0].UV, d2 = G.V[a2].UV - G.V[a0].UV;
								const float Det = d1.X * d2.Y - d2.X * d1.Y;
								if (FMath::IsNearlyZero(Det)) { continue; }   // a degenerate UV triangle contributes nothing
								const float r = 1.f / Det;
								const FVector3f Tt = (e1 * d2.Y - e2 * d1.Y) * r;
								const FVector3f Bt = (e2 * d1.X - e1 * d2.X) * r;
								Tan[a0] += Tt; Tan[a1] += Tt; Tan[a2] += Tt;
								Bit[a0] += Bt; Bit[a1] += Bt; Bit[a2] += Bt;
							}
							for (int32 v = 0; v < G.V.Num(); ++v)
							{
								const FVector3f N3 = G.V[v].N.GetSafeNormal();
								FVector3f T3 = (Tan[v] - N3 * FVector3f::DotProduct(N3, Tan[v])).GetSafeNormal();
								if (T3.IsNearlyZero()) { T3 = FMath::Abs(N3.Z) < 0.9f ? FVector3f(0, 0, 1) : FVector3f(1, 0, 0); T3 = (T3 - N3 * FVector3f::DotProduct(N3, T3)).GetSafeNormal(); }
								const float W = FVector3f::DotProduct(FVector3f::CrossProduct(N3, T3), Bit[v]) < 0.f ? -1.f : 1.f;
								G.V[v].T = FVector4f(T3.X, T3.Y, T3.Z, W);
							}
							D.TangentsSynthesised += G.V.Num();
						}
					}
					// RUDE_PEDUV1: the conservation counters for the three skinned forms - which layout this geometry got, and
					// how many vertices actually carried a second UV / a tangent into the file instead of losing them.
					if (G.bUv1) { ++D.GeosSkinUv1Tan; D.Uv1Carried += G.V.Num(); }
					else if (G.bTan) { ++D.GeosSkinTan; }
					else { ++D.GeosSkinPlain; }
					if (G.bTan) { D.TangentsWritten += G.V.Num(); }
					(Lod == 0 ? D.Geos : D.LodGeos[Lod - 1]).Add(MoveTemp(G));   // RUDE_PEDLOD: High -> Geos, Medium/Low -> LodGeos
				}
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
		// RUDE_PEDLOD: the page plan must cover the LARGEST block of ANY group, and the shader pointer array now
		// holds one entry per geometry across every group (law 6.1), not just the High group's.
		int32 MaxGroupGeos = D.Geos.Num(), AllGeos = D.Geos.Num();
		for (int32 lg = 0; lg < 2; ++lg) { MaxGroupGeos = FMath::Max(MaxGroupGeos, D.LodGeos[lg].Num()); AllGeos += D.LodGeos[lg].Num(); }
		for (int32 lg = -1; lg < 2; ++lg)
		{
			for (const FGeo& G : (lg < 0 ? D.Geos : D.LodGeos[lg]))
			{
				Largest = FMath::Max(Largest, (uint32)G.V.Num() * (uint32)RudeGeoStride(G));   // RUDE_PEDUV1: the SAME stride the packer will write
				Largest = FMath::Max(Largest, (uint32)G.Idx.Num() * 2u);
			}
		}
		const uint32 NG = (uint32)MaxGroupGeos;
		Largest = FMath::Max(Largest, (NG + 1) * 0x20u);       // geoBounds
		Largest = FMath::Max(Largest, NG * 8u);                // geometry pointer array (per group)
		Largest = FMath::Max(Largest, (uint32)AllGeos * 8u);   // RUDE_PEDLOD: the shader pointer array spans every group
		Largest = FMath::Max(Largest, (uint32)FMath::Max(kPedAlloc, kPedAlphaAlloc));      // one shader's parameter block (either template)
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
	// RUDE_PEDLOD: the *All counters cover every LOD group; the three above stay HIGH-only so every number a
	// caller already read keeps its meaning (maintainer lane `ped_lods` (`LAWS.md`) law 10).
	int32 TotalVertsAll = 0, TotalTrisAll = 0, TotalGeosAll = 0;
	for (int32 di = 0; di < N; ++di)
	{
		const FDrawable& D = Ds[di];
		const int32 Base = RecBase[di];
		// RUDE_PEDLOD: the entry's LOD GROUPS. Group 0 = High (D.Geos), 1 = Medium, 2 = Low; an EMPTY group is
		// absent and its record slot (+0x58 / +0x60) and flag word (+0x84 / +0x88) stay RAW ZERO - the shape the
		// game itself ships (172/172 absent Medium and 555/555 absent Low words read 0x0, maintainer lane
		// `ped_lods` (`LAWS.md`) law 3.1). Each present group owns its geometry blocks, geoBounds, shader map,
		// geometry array, model, model array and 0x10 group header outright: the game never shares one between
		// groups (the three header offsets ascend High < Medium < Low on 2,125/2,125 entries, law 4). A ONE-group
		// entry walks this loop once and emits exactly the byte sequence the single-group writer emitted.
		const TArray<FGeo>* Grp[3] = { &D.Geos, &D.LodGeos[0], &D.LodGeos[1] };
		int32 ShaderBase[3] = { 0, 0, 0 };
		int32 OMhGroup[3] = { -1, -1, -1 };
		uint32 BucketBits[3] = { 0, 0, 0 };
		for (int32 gb = 1; gb < 3; ++gb) { ShaderBase[gb] = ShaderBase[gb - 1] + Grp[gb - 1]->Num(); }
		for (int32 gr = 0; gr < 3; ++gr)
		{
			const TArray<FGeo>& GG = *Grp[gr];
			if (GG.Num() == 0) { continue; }
			FVector3f GMn(FLT_MAX), GMx(-FLT_MAX);
			for (const FGeo& GB : GG) { GMn = GMn.ComponentMin(GB.Mn); GMx = GMx.ComponentMax(GB.Mx); }
			const int32 NG = GG.Num();
			const bool bUnion = NG > 1;
			TArray<uint8> GeoBounds; GeoBounds.AddZeroed((bUnion ? NG + 1 : 1) * 0x20);
			TArray<uint8> ShaderMap; ShaderMap.AddZeroed(FMath::Max(NG * 2, 8));
			TArray<int32> OGeo;
			for (int32 gi = 0; gi < NG; ++gi)
			{
				const FGeo& G = GG[gi];
				// RUDE_PEDPROPS: a rigid entry writes the game's own prop layout (mask 0x40F9, stride 64, 2,674/2,677 measured):
				// +0 pos f3 | +12 normal f3 | +24 Colour0 u8x4 | +28 Colour1 u8x4 (0) | +32 uv0 f2 | +40 uv1 f2 | +48 tangent f4
				// RUDE_PEDUV1: three skinned forms now, chosen per geometry from the source (see kSkinUv1Tan*).
				const uint32 GMask = RudeGeoMask(G);
				const int32 Stride = RudeGeoStride(G);
				TArray<uint8> VD; VD.SetNumZeroed(G.V.Num() * Stride);
				for (int32 v = 0; v < G.V.Num(); ++v)
				{
					const FYddVert& X = G.V[v]; const int32 o = v * Stride;
					PVEC3(VD, o + 0, X.P);
					if (G.bRigid)
					{
						PVEC3(VD, o + 12, X.N);
						for (int32 k = 0; k < 4; ++k) { VD[o + 24 + k] = X.C[k]; VD[o + 28 + k] = 0; }   // Colour1 = 0 (2,587/2,676 first vertices)
						PF32(VD, o + 32, X.UV.X); PF32(VD, o + 36, X.UV.Y);
						PF32(VD, o + 40, X.UV1.X); PF32(VD, o + 44, X.UV1.Y);
						PF32(VD, o + 48, X.T.X); PF32(VD, o + 52, X.T.Y); PF32(VD, o + 56, X.T.Z); PF32(VD, o + 60, X.T.W);
						continue;
					}
					// skinned: stride 48, channels in ascending bit order (LAWS section 4)
					for (int32 k = 0; k < 4; ++k) { VD[o + 12 + k] = X.S.W[k]; VD[o + 16 + k] = X.S.I[k]; }
					PVEC3(VD, o + 20, X.N);
					for (int32 k = 0; k < 4; ++k) { VD[o + 32 + k] = X.C[k]; VD[o + 36 + k] = 0; }   // Colour1 = 0 (17/18 measured)
					PF32(VD, o + 40, X.UV.X); PF32(VD, o + 44, X.UV.Y);
					// RUDE_PEDUV1: TexCoord1 then Tangent, in ascending fvf-bit order - tangent slides to +48 when UV1 is absent
					if (G.bUv1) { PF32(VD, o + 48, X.UV1.X); PF32(VD, o + 52, X.UV1.Y); }
					if (G.bTan)
					{
						const int32 t = G.bUv1 ? o + 56 : o + 48;
						PF32(VD, t + 0, X.T.X); PF32(VD, t + 4, X.T.Y); PF32(VD, t + 8, X.T.Z); PF32(VD, t + 12, X.T.W);
					}
				}
				const int32 OV = Emit(VD);
				TArray<uint8> ID; ID.SetNumZeroed(G.Idx.Num() * 2);
				for (int32 i = 0; i < G.Idx.Num(); ++i) { PU16(ID, i * 2, (uint16)G.Idx[i]); }
				const int32 OI = Emit(ID);
				// the bone-id table: identity over the rig (56/56 + 7/7 measured); OWN copy per geometry. A RIGID entry has
				// none (0 <BoneIDs> in 2,677/2,677 prop geometries): +0x68 stays raw NULL (RUDE_PEDPROPS)
				int32 OBid = -1;
				if (!G.bRigid)
				{
					TArray<uint8> Bid; Bid.SetNumZeroed(NumRig * 2);
					for (int32 k = 0; k < NumRig; ++k) { PU16(Bid, k * 2, (uint16)k); }
					OBid = Emit(Bid);
				}
				TArray<uint8> Fvf; Fvf.AddZeroed(0x10);                              // own fvf per geometry (crash #6)
				PU32(Fvf, 0x00, GMask); PU16(Fvf, 0x04, (uint16)Stride); Fvf[0x07] = RudeChanCount(GMask);   // RUDE_PEDUV1 (was RUDE_PEDPROPS)
				PU32(Fvf, 0x08, 0x55996996u); PU32(Fvf, 0x0c, 0x77555555u);
				const int32 OFvf = Emit(Fvf);
				TArray<uint8> Vb; Vb.AddZeroed(0x80);
				PU32(Vb, 0x00, VFT_VB); PU32(Vb, 0x04, 1);
				PU16(Vb, 0x08, (uint16)Stride); PU16(Vb, 0x0a, 0);                   // +0x0A = 0 in the game (7/7; 2,677/2,677 props)
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
				if (!G.bRigid) { PPTR(Ge, 0x68, OBid); }                             // the skinned delta; a rigid entry keeps +0x68 raw NULL
				PU16(Ge, 0x70, (uint16)Stride); PU16(Ge, 0x72, (uint16)(G.bRigid ? 0 : NumRig));  // stride is a u16; +0x72 = bone-id count (0 rigid)
				PPTR(Ge, 0x78, OV);
				const int32 OGe = Emit(Ge);
				OGeo.Add(OGe);
				if (!G.bRigid) { GeoBoneIdSlots.Add(OGe + 0x68); }   // RUDE_PEDPROPS: no table to audit on a rigid entry
				const int32 Pair = (bUnion ? gi + 1 : gi) * 0x20;
				PVEC3(GeoBounds, Pair, G.Mn); PVEC3(GeoBounds, Pair + 0x10, G.Mx);
				PU16(ShaderMap, gi * 2, (uint16)(ShaderBase[gr] + gi));   // RUDE_PEDLOD: a LOD geometry indexes a shader AFTER every High one (1,899/1,953 Medium, 1,525/1,570 Low)
				if (gr == 0) { TotalVerts += G.V.Num(); TotalTris += G.Idx.Num() / 3; ++TotalGeos; }   // RUDE_PEDLOD: the reported totals stay HIGH-only
				TotalVertsAll += G.V.Num(); TotalTrisAll += G.Idx.Num() / 3; ++TotalGeosAll;
			}
			if (bUnion) { PVEC3(GeoBounds, 0x00, GMn); PVEC3(GeoBounds, 0x10, GMx); }   // RUDE_PEDLOD: THIS group's union (identical to D.Mn/D.Mx on a one-group entry)
			const int32 OGb = Emit(GeoBounds);
			const int32 OSm = Emit(ShaderMap);
			TArray<uint8> GeoArr; GeoArr.AddZeroed(NG * 8);
			for (int32 i = 0; i < NG; ++i) { PPTR(GeoArr, i * 8, OGeo[i]); }
			const int32 OGa = Emit(GeoArr);
			TArray<uint8> Model; Model.AddZeroed(0x30);
			PU32(Model, 0x00, VFT_MODEL); PU32(Model, 0x04, 1);
			PPTR(Model, 0x08, OGa); PU16(Model, 0x10, (uint16)NG); PU16(Model, 0x12, (uint16)NG);
			PPTR(Model, 0x18, OGb); PPTR(Model, 0x20, OSm);
			// RUDE_PEDPROPS: a rigid entry keeps the static record's zeros (977/977 prop models: Unknown1 0, Unknown29 0, Flags 0)
			Model[0x28] = D.bRigid ? (uint8)0 : (uint8)NumRig;                       // rig bone count (52/52, 7/7) / 0 rigid
			PU16(Model, 0x29, D.bRigid ? (uint16)0 : (uint16)1);                     // skinned (52/52, 7/7) / 0 rigid
			Model[0x2b] = 0; Model[0x2c] = 0xff; Model[0x2d] = D.bRigid ? (uint8)0 : (uint8)1;   // BoneIndex, RenderMask, skinned flag / 0 rigid
			PU16(Model, 0x2e, (uint16)NG);
			const int32 OM = Emit(Model);
			TArray<uint8> ModelArr; ModelArr.AddZeroed(8); PPTR(ModelArr, 0, OM);
			const int32 OMa = Emit(ModelArr);
			TArray<uint8> ModelsHdr; ModelsHdr.AddZeroed(0x10); PPTR(ModelsHdr, 0x00, OMa); PU16(ModelsHdr, 0x08, 1); PU16(ModelsHdr, 0x0a, 1);
			const int32 OMh = Emit(ModelsHdr);
			OMhGroup[gr] = OMh;
		}

		// shaders: one `ped` shader per geometry, ACROSS the groups in group order (law 6.1). The game gives its
		// Medium/Low groups their own shader appended after the High ones - `Medium == High + 1` on 1,604/1,953
		// entries and `Medium == High` on 1/1,953 - so sharing High's shader is unattested and the writer appends.
		TArray<int32> OSh;
		for (int32 gr = 0; gr < 3; ++gr)
		{
			const TArray<FGeo>& GG = *Grp[gr];
			for (int32 gi = 0; gi < GG.Num(); ++gi)
			{
					const FGeo& G = GG[gi];
					auto Stub = [&](const FString& Name) -> int32
					{
						const int32 ON = EmitStr(Name.ToLower());                      // own string per stub (single ownership)
						TArray<uint8> St; St.AddZeroed(0x50);
						PU32(St, 0x00, VFT_STUB); PU32(St, 0x04, 1); PPTR(St, 0x28, ON); PU32(St, 0x30, 0x00020001u);
						return Emit(St);
					};
					// RUDE_PEDPROPS: the template follows the slot's preset - ped_alpha (bucket 1, 12 params, 4 samplers; the lens of
					// every 2-geometry prop, 932/2,672 prop shaders) for a lens, else `ped`; any other preset is written as `ped`
					// and COUNTED (shaderSubstituted). The entry's +0x80 flags OR every shader's 1<<bucket (1,763/1,763 measured).
					const FShaderTemplate& Tp = (G.Preset == TEXT("ped_alpha")) ? kTplPedAlpha : kTplPed;
					if (gr == 0 && !G.Preset.IsEmpty() && G.Preset != TEXT("ped") && G.Preset != TEXT("ped_alpha")) { ++Ds[di].ShaderSubstituted; }
					if (gr > 0) { ++Ds[di].LodShaderSubstituted; }   // RUDE_PEDLOD: the game names this shader `ped_default` (1,022/1,022 Medium groups); its parameter table is UNMEASURED here, so the High template is written and the difference COUNTED
					BucketBits[gr] |= (1u << Tp.Bucket);   // RUDE_PEDLOD: per GROUP - High 0xFF08 with Medium 0xFF01 on 257 measured entries
					if (gr == 0 && G.Diffuse.IsEmpty()) { ++Ds[di].TexMissing; }
					TArray<uint8> Zero; Zero.AddZeroed(TplAlloc(Tp));
					const int32 OTbl = Emit(Zero);
					int32 VecIdx = 0;
					for (int32 pi = 0; pi < Tp.NPar; ++pi)
					{
						const FPedParam& PP = Tp.Params[pi];
						PU32(Seg, OTbl + pi * 16, (PP.bTexture ? 0u : 1u) | ((uint32)PP.Reg << 8));   // class | register<<8
						if (PP.bTexture)
						{
							const FString PN(PP.Name);
							int32 StubO = -1;
							if (PN == TEXT("DiffuseSampler")) { StubO = Stub(G.Diffuse.IsEmpty() ? FString(TEXT("none")) : G.Diffuse); }
							else if (PN == TEXT("VolumeSampler")) { StubO = Stub(TEXT("givemechecker")); }
							else if (PN == TEXT("BumpSampler") && !G.Normal.IsEmpty()) { StubO = Stub(G.Normal); }   // unbound sampler = raw NULL (measured legal)
							else if (PN == TEXT("SpecSampler") && !G.Spec.IsEmpty()) { StubO = Stub(G.Spec); }
							if (StubO >= 0) { PPTR(Seg, OTbl + pi * 16 + 8, StubO); }
						}
						else
						{
							const int32 VOfs = OTbl + Tp.NPar * 16 + VecIdx * 16;
							for (int32 c = 0; c < 4; ++c) { PF32(Seg, VOfs + c * 4, PP.V[c]); }
							PPTR(Seg, OTbl + pi * 16 + 8, VOfs);
							++VecIdx;
						}
						PU32(Seg, OTbl + TplHashOfs(Tp) + pi * 4, RudeJoaat(PP.Name));
					}
					TArray<uint8> Blk; Blk.AddZeroed(0x30);
					PPTR(Blk, 0x00, OTbl);
					PU32(Blk, 0x08, RudeJoaat(Tp.Name));
					PU32(Blk, 0x10, 0x80000000u | (uint32)Tp.NPar | ((uint32)Tp.Bucket << 8));   // npar | bucket<<8 | 0x8000<<16
					PU32(Blk, 0x14, ((uint32)TplAlloc(Tp) << 16) | (uint32)TplHashOfs(Tp));
					PU32(Blk, 0x18, RudeJoaat(FString(Tp.Name) + TEXT(".sps")));
					PU32(Blk, 0x20, 0x0000ff00u | (1u << Tp.Bucket));
					PU32(Blk, 0x24, (uint32)Tp.NTex << 24);
					OSh.Add(Emit(Blk));
			}
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
			// RUDE_PEDLOD: D.Mn/D.Mx span EVERY present group, not just High - measured (maintainer lane `ped_lods`
			// (`LAWS.md`) law 8.2): on the entries where the two answers differ, the game's stored box SIZE matches
			// the all-group union on 280 and the High-only box on 14, and its sphere radius matches the all-group
			// union on 115 and High-only on 6. ⚠ The stored box is never the raw union on a skinned entry (an exact
			// min/max match on 6/2,125): it is that box TRANSLATED, an offset RUDE does not reproduce and did not
			// reproduce before the LOD groups either - a gap that PREDATES this change, named, not one it introduced.
			const FVector3f C = (D.Mn + D.Mx) * 0.5f;
			PVEC3(Seg, Base + 0x20, C); PF32(Seg, Base + 0x2c, (D.Mx - C).Size());
			PVEC3(Seg, Base + 0x30, D.Mn); PU32(Seg, Base + 0x3c, 0x7f800001u);
			PVEC3(Seg, Base + 0x40, D.Mx); PU32(Seg, Base + 0x4c, 0x7f800001u);
		}
		// RUDE_PEDLOD: one pointer + one flag word per PRESENT group; an absent group keeps both raw zero, and
		// +0x68 / +0x8C (VeryLow) are never written at all (0/2,125 game entries carry a VeryLow group, law 1.1).
		const int32 kLodSlot[3] = { 0x50, 0x58, 0x60 };
		int32 GroupsWritten = 0;
		for (int32 gr = 0; gr < 3; ++gr)
		{
			if (OMhGroup[gr] < 0) { continue; }
			++GroupsWritten;
			PPTR(Seg, Base + kLodSlot[gr], OMhGroup[gr]);
			PU32(Seg, Base + 0x80 + gr * 4, 0x0000ff00u | (BucketBits[gr] ? BucketBits[gr] : 1u));   // 0xFF00 | OR(1<<bucket) over THIS group's shaders (law 3.2)
		}
		// the four lodDist floats AS THE ENTRY SPELLED THEM: `LODDIST=` carries them in (ExportPedReplace fills it
		// from the outfit ImportPed wrote), and the default is the measured modal 9998 x4 - 2,119/2,125 game entries
		// and 1,148/1,152 corpus entries. They are CARRIED, never computed (law 2); the 4 corpus entries that deviate
		// (e.g. a three-group `uppr_000_u` reading 100/9998/9998/9998) are the reason the carry exists.
		for (int32 k = 0; k < 4; ++k) { PF32(Seg, Base + 0x70 + k * 4, D.LodDist[k]); }
		// RUDE_PEDLOD: +0x98 is a per-file word whose MEANING IS UNKNOWN, and it moves with the group count. These
		// are the MODAL values measured over the 700-file / 4,327-entry binary draw (maintainer lane `ped_lods`
		// (`LAWS.md`) law 8.1), indexed by the number of groups this entry writes:
		//   1 group  0x00120000  1,038/2,374   2 groups 0x003E0000  276/383   3 groups 0x005D0000  1,140/1,570
		// ⚠ The one-group figure is the modal over ALL one-group entries; over the SKINNED subset alone the mode is
		// 0x00220000 (62/172) and 0x00120000 is 3/172. RUDE keeps 0x00120000 for one group because it is the modal
		// of the population as a whole, it is the value an in-game-proven static drawable carried, and it
		// keeps a single-group export byte-identical. The word written is REPORTED per drawable (`u98`, `u98Basis`)
		// and read back by ProbeYddBinary, so it is a named suspect, never a silent one.
		static const uint32 kU98ByGroupCount[4] = { 0x00120000u, 0x00120000u, 0x003E0000u, 0x005D0000u };
		Ds[di].U98 = kU98ByGroupCount[FMath::Clamp(GroupsWritten, 0, 3)];
		PU32(Seg, Base + 0x98, Ds[di].U98);
		PPTR(Seg, Base + 0xa0, OMhGroup[0]);                                     // +0xA0 aliases the HIGH group (3/3 binaries)
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
		// RUDE_PEDLOD: RudeBinVerifyDrawable walks +0x50 (High) only, so the Medium / Low subtrees would be
		// UNAUDITED - and a block owned twice is the non-idempotent-fixup crash this self-check exists to stop.
		// The same walk is applied here to every block a LOD group owns, mirroring the drawable check exactly
		// (geo+0x78 and VB+0x20 are sanctioned vertex-data aliases and are NOT counted).
		for (int32 li = 0; li < N; ++li)
		{
			for (int32 gr = 1; gr < 3; ++gr)
			{
				const uint32 PH = RU32(Seg, RecBase[li] + 0x50 + gr * 8);
				if (PH == 0) { continue; }
				RudeBinOwn(InDeg, PH, FString::Printf(TEXT("entry[%d] lod%d group header"), li, gr), Shared, First);
				int32 MH = 0;
				if (!Deref(Seg, PH, 0x10, MH)) { ++DeclBad; if (First.IsEmpty()) { First = TEXT("a LOD group header does not resolve"); } continue; }
				RudeBinOwn(InDeg, RU32(Seg, MH + 0x00), FString::Printf(TEXT("entry[%d] lod%d model array"), li, gr), Shared, First);
				const int32 NMod = (int32)RU16(Seg, MH + 0x08);
				int32 MArr = 0;
				if (NMod <= 0 || !Deref(Seg, RU32(Seg, MH + 0x00), NMod * 8, MArr)) { ++DeclBad; if (First.IsEmpty()) { First = TEXT("a LOD model array does not resolve"); } continue; }
				for (int32 mi = 0; mi < NMod; ++mi)
				{
					RudeBinOwn(InDeg, RU32(Seg, MArr + mi * 8), FString::Printf(TEXT("entry[%d] lod%d model[%d]"), li, gr, mi), Shared, First);
					int32 M = 0;
					if (!Deref(Seg, RU32(Seg, MArr + mi * 8), 0x30, M)) { ++DeclBad; continue; }
					RudeBinOwn(InDeg, RU32(Seg, M + 0x08), TEXT("lod model geoArr"), Shared, First);
					RudeBinOwn(InDeg, RU32(Seg, M + 0x18), TEXT("lod model geoBounds"), Shared, First);
					RudeBinOwn(InDeg, RU32(Seg, M + 0x20), TEXT("lod model shaderMap"), Shared, First);
					const int32 NGeo = (int32)RU16(Seg, M + 0x10);
					const int32 Pairs = (NGeo > 1) ? (NGeo + 1) : 1;   // the geoBounds law, per group
					int32 GB = 0;
					if (!Deref(Seg, RU32(Seg, M + 0x18), Pairs * 0x20, GB)) { ++BoundsBad; if (First.IsEmpty()) { First = TEXT("a LOD group's geoBounds does not span its pairs"); } }
					int32 GArr = 0;
					if (NGeo <= 0 || !Deref(Seg, RU32(Seg, M + 0x08), NGeo * 8, GArr)) { ++DeclBad; continue; }
					for (int32 gi = 0; gi < NGeo; ++gi)
					{
						RudeBinOwn(InDeg, RU32(Seg, GArr + gi * 8), TEXT("lod geometry"), Shared, First);
						int32 G = 0;
						if (!Deref(Seg, RU32(Seg, GArr + gi * 8), 0xa0, G)) { ++DeclBad; continue; }
						RudeBinOwn(InDeg, RU32(Seg, G + 0x18), TEXT("lod geometry VB"), Shared, First);
						RudeBinOwn(InDeg, RU32(Seg, G + 0x38), TEXT("lod geometry IB"), Shared, First);
						int32 VB = 0, IB = 0, Fvf = 0;
						if (Deref(Seg, RU32(Seg, G + 0x18), 0x40, VB))
						{
							RudeBinOwn(InDeg, RU32(Seg, VB + 0x10), TEXT("lod vertex data"), Shared, First);
							RudeBinOwn(InDeg, RU32(Seg, VB + 0x30), TEXT("lod grcFvf"), Shared, First);
							if (Deref(Seg, RU32(Seg, VB + 0x30), 0x10, Fvf))
							{
								int32 Ofs[16]; FString DeclErr;
								if (!RudeBinBuildDecl(RU32(Seg, Fvf + 0x00), RU64(Seg, Fvf + 0x08), (int32)RU16(Seg, G + 0x70), Ofs, DeclErr))
								{
									++DeclBad;
									if (First.IsEmpty()) { First = FString::Printf(TEXT("lod%d geometry declaration: %s"), gr, *DeclErr); }
								}
							}
						}
						if (Deref(Seg, RU32(Seg, G + 0x38), 0x20, IB)) { RudeBinOwn(InDeg, RU32(Seg, IB + 0x10), TEXT("lod index data"), Shared, First); }
					}
				}
			}
		}
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

	FString DJson; int32 RigidEntries = 0;   // RUDE_PEDPROPS
	for (const FDrawable& D : Ds)
	{
		int32 DV = 0, DT = 0; FString Tex;
		// RUDE_PEDLOD: the drawable's vertices / triangles / geometries stay the HIGH group's; the LOD groups are
		// reported beside them, index 0 = High (maintainer lane `ped_lods` (`LAWS.md`)).
		int32 LodV[3] = { 0, 0, 0 }, LodT[3] = { 0, 0, 0 }, LodG[3] = { 0, 0, 0 }, LodGroups = 0;
		for (int32 lg = 0; lg < 3; ++lg)
		{
			const TArray<FGeo>& LG = (lg == 0 ? D.Geos : D.LodGeos[lg - 1]);
			if (LG.Num() == 0) { continue; }
			++LodGroups;
			LodG[lg] = LG.Num();
			for (const FGeo& G : LG) { LodV[lg] += G.V.Num(); LodT[lg] += G.Idx.Num() / 3; }
		}
		// RUDE_PEDLOD: where the +0x98 word this entry carries came from, spelled out with its denominator so the
		// verdict can never be read as a per-shape measurement. The one-group figure is the mode over EVERY
		// one-group entry in the 4,327-entry draw; over the skinned one-group subset alone the mode is different
		// and RUDE does not write it (law 8.1).
		const TCHAR* U98Basis = (LodGroups >= 3) ? TEXT("modal over 3-group game entries, 1,140/1,570")
			: ((LodGroups == 2) ? TEXT("modal over 2-group game entries, 276/383")
			: TEXT("modal over 1-group game entries, 1,038/2,374; over the SKINNED 1-group subset alone the mode is 0x00220000, 62/172, which RUDE does not write"));
		if (D.bRigid) { ++RigidEntries; }
		for (const FGeo& G : D.Geos)
		{
			DV += G.V.Num(); DT += G.Idx.Num() / 3;
			Tex += FString::Printf(TEXT("%s{\"slot\":\"%s\",\"preset\":\"%s\",\"material\":\"%s\",\"diffuse\":\"%s\",\"normal\":\"%s\",\"spec\":\"%s\"}"), Tex.IsEmpty() ? TEXT("") : TEXT(","),
				*RudeJsonEscape(G.Slot), *RudeJsonEscape(G.Preset), *RudeJsonEscape(G.Mat), *RudeJsonEscape(G.Diffuse), *RudeJsonEscape(G.Normal), *RudeJsonEscape(G.Spec));   // RUDE_PEDPROPS: + preset
		}
		DJson += FString::Printf(
			TEXT("%s{\"name\":\"%s\",\"hash\":\"0x%08x\",\"asset\":\"%s\",\"geometries\":%d,\"vertices\":%d,\"sourceVertices\":%d,\"triangles\":%d,")
			TEXT("\"bonesReferenced\":%d,\"meshBonesUnmapped\":%d,\"influencesUnmapped\":%d,\"influencesTruncated\":%d,\"verticesRebound\":%d,")
			TEXT("\"uv1Dropped\":%d,\"uv1Carried\":%d,\"tangentsWritten\":%d,\"tangentsCarried\":%d,\"tangentsSynthesised\":%d,")
			TEXT("\"skinLayouts\":{\"plain0x7f\":%d,\"tangent0x407f\":%d,\"uv1tangent0x40ff\":%d},")   // RUDE_PEDUV1
			TEXT("\"texturesMissing\":%d,\"rigid\":%s,\"shaderSubstituted\":%d,")
			TEXT("\"lodGroups\":%d,\"lodVertices\":[%d,%d,%d],\"lodTriangles\":[%d,%d,%d],\"lodGeometries\":[%d,%d,%d],")   // RUDE_PEDLOD
			TEXT("\"lodShaderSubstituted\":%d,\"lodsSkipped\":%d,\"lodDist\":[%g,%g,%g,%g],\"lodDistCarried\":%s,")
			TEXT("\"u98\":\"0x%08x\",\"u98Basis\":\"%s\",\"textures\":[%s]}"),
			DJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(D.Name), D.Hash, *RudeJsonEscape(D.Asset), D.Geos.Num(), DV, D.SrcVerts, DT,
			D.MaxRig + 1, D.BonesUnmapped, D.InflUnmapped, D.InflTruncated, D.Rebound,
			D.Uv1Dropped, D.Uv1Carried, D.TangentsWritten, D.TangentsCarried, D.TangentsSynthesised,
			D.GeosSkinPlain, D.GeosSkinTan, D.GeosSkinUv1Tan,   // RUDE_PEDUV1
			D.TexMissing, D.bRigid ? TEXT("true") : TEXT("false"), D.ShaderSubstituted,
			LodGroups, LodV[0], LodV[1], LodV[2], LodT[0], LodT[1], LodT[2], LodG[0], LodG[1], LodG[2],
			D.LodShaderSubstituted, D.LodsSkipped, D.LodDist[0], D.LodDist[1], D.LodDist[2], D.LodDist[3], D.bLodDistCarried ? TEXT("true") : TEXT("false"),
			D.U98, U98Basis, *Tex);   // RUDE_PEDLOD
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"yddPath\":\"%s\",\"entries\":%d,\"rigidEntries\":%d,\"drawables\":[%s],\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,")
		TEXT("\"geometriesAllLods\":%d,\"verticesAllLods\":%d,\"trianglesAllLods\":%d,\"lodDistCarried\":%d,\"lodDistMalformed\":%d,")   // RUDE_PEDLOD: the three above stay HIGH-only
		TEXT("\"rigBones\":%d,\"rig\":\"%s\",\"layout\":\"skinned, chosen per geometry from the source: mask 0x7f stride 48 (Position BlendWeights BlendIndices Normal Colour0 Colour1 TexCoord0) | mask 0x407f stride 64 (+Tangent) | mask 0x40ff stride 72 (+TexCoord1 +Tangent); rigid: mask 0x40f9 stride 64 (Position Normal Colour0 Colour1 TexCoord0 TexCoord1 Tangent)\",")
		TEXT("\"shader\":\"ped (13 params, alloc %d, hashOfs %d)\",\"bytes\":%d,\"segSize\":%d,\"page\":%d,\"pages\":%u,\"sysFlags\":\"0x%08x\",")
		TEXT("\"selfCheck\":\"passed (dictionary-wide single ownership + geoBounds/count + declarations)\",")
		TEXT("\"note\":\"the game also needs the matching .ytd (ExportYtdBinary / ExportMeshTextures) and a ped variation (ymt) row for the drawable index; in-game load unverified\"}"),
		*RudeJsonEscape(OutYddPath), N, RigidEntries, *DJson, TotalGeos, TotalVerts, TotalTris,
		TotalGeosAll, TotalVertsAll, TotalTrisAll, LodDistCarried, LodDistMalformed,   // RUDE_PEDLOD
		NumRig, *RudeJsonEscape(RigName), kPedAlloc, kPedHashOfs,
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
	int32 TotalGeosAll = 0, TotalVertsAll = 0, TotalTrisAll = 0, TotalLodGroups = 0, EntriesWithLods = 0;   // RUDE_PEDLOD
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
		// RUDE_PEDLOD: per-LOD-group facts. The entry totals above stay HIGH-only (so `boneIdTablesIdentity ==
		// geometries` keeps meaning what it meant); everything a LOD group adds is reported separately.
		int32 EGeosAll = 0, EVertsAll = 0, ETrisAll = 0, ELodGroups = 0;
		int32 GGeos[4] = { 0, 0, 0, 0 }, GVerts[4] = { 0, 0, 0, 0 }, GTris[4] = { 0, 0, 0, 0 }, GModels[4] = { 0, 0, 0, 0 }, GBones[4] = { -1, -1, -1, -1 };
		TSet<FString> Decls;
		for (int32 lod = 0; lod < 4; ++lod)
		{
			int32 MH = 0;
			const uint32 PL = RU32(Sys, Base + LodSlot[lod]);
			if (PL == 0 || !Deref(Sys, PL, 0x10, MH)) { continue; }
			const int32 NMod = RU16(Sys, MH + 8);
			++ELodGroups; GModels[lod] = NMod;   // RUDE_PEDLOD
			int32 MArr = 0;
			if (NMod <= 0 || !Deref(Sys, RU32(Sys, MH + 0), NMod * 8, MArr)) { continue; }
			for (int32 mi = 0; mi < NMod; ++mi)
			{
				int32 M = 0;
				if (!Deref(Sys, RU32(Sys, MArr + mi * 8), 0x30, M)) { continue; }
				const int32 NGeo = RU16(Sys, M + 0x10);
				if (lod == 0 && mi == 0) { EBoneCount28 = RU8(Sys, M + 0x28); EU29 = RU16(Sys, M + 0x29); EFlag2d = RU8(Sys, M + 0x2d); }
				if (mi == 0) { GBones[lod] = RU8(Sys, M + 0x28); }   // RUDE_PEDLOD: the LOD group's bone count (== High's on 1,949/1,953 Medium)
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
						if (lod == 0) { if (bIdent) { ++EIdentity; } else { ++ENonIdentity; } EBoneIds = FMath::Max(EBoneIds, NBid); }   // RUDE_PEDLOD: High-only, as before
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
					// RUDE_PEDLOD: the entry totals stay HIGH-only; the LOD groups feed the per-group rows and the
					// *AllLods totals, so no number that existed before this change moved.
					++EGeosAll; EVertsAll += VCnt; ETrisAll += (int32)(IdxCount / 3);
					if (lod >= 0 && lod < 4) { ++GGeos[lod]; GVerts[lod] += VCnt; GTris[lod] += (int32)(IdxCount / 3); }
					if (lod == 0)
					{
						++EGeos; EVerts += VCnt; ETris += (int32)(IdxCount / 3); ESum255 += Sum255; EBadIdx += BadIdx;
						if (bSkin) { ESkinned += VCnt; }
						EMaxBlend = FMath::Max(EMaxBlend, MaxBlend);
					}
				}
				MJson += FString::Printf(TEXT("%s{\"lod\":\"%s\",\"model\":%d,\"geoCount\":%d,\"boneCountAt0x28\":%d,\"u16At0x29\":%d,\"flagAt0x2d\":%d,\"renderMask\":%d,\"geos\":[%s]}"),
					MJson.IsEmpty() ? TEXT("") : TEXT(","), LodName[lod], mi, NGeo, RU8(Sys, M + 0x28), RU16(Sys, M + 0x29), RU8(Sys, M + 0x2d), RU8(Sys, M + 0x2c), *GJson);
			}
		}
		RudeBinVerifyDrawable(Sys, Base, InDeg, Shared, DeclBad, BoundsBad, First);
		// RUDE_PEDLOD: one row per PRESENT LOD group, and the four lodDist floats / four flag words as stored.
		FString GrpJson;
		for (int32 lod = 0; lod < 4; ++lod)
		{
			if (RU32(Sys, Base + LodSlot[lod]) == 0) { continue; }
			GrpJson += FString::Printf(TEXT("%s{\"group\":\"%s\",\"models\":%d,\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"boneCountAt0x28\":%d,\"flags\":\"0x%08x\",\"lodDist\":%g}"),
				GrpJson.IsEmpty() ? TEXT("") : TEXT(","), LodName[lod], GModels[lod], GGeos[lod], GVerts[lod], GTris[lod], GBones[lod],
				RU32(Sys, Base + 0x80 + lod * 4), RF32(Sys, Base + 0x70 + lod * 4));
		}
		FString LodDistJson, LodFlagJson;
		for (int32 lod = 0; lod < 4; ++lod)
		{
			LodDistJson += FString::Printf(TEXT("%s%g"), lod ? TEXT(",") : TEXT(""), RF32(Sys, Base + 0x70 + lod * 4));
			LodFlagJson += FString::Printf(TEXT("%s\"0x%08x\""), lod ? TEXT(",") : TEXT(""), RU32(Sys, Base + 0x80 + lod * 4));
		}
		FString DeclJson;
		for (const FString& Dd : Decls) { DeclJson += FString::Printf(TEXT("%s\"%s\""), DeclJson.IsEmpty() ? TEXT("") : TEXT(","), *Dd); }
		EJson += FString::Printf(
			TEXT("%s{\"index\":%d,\"name\":\"%s\",\"hash\":\"0x%08x\",\"nameHashesToEntry\":%s,\"hasSkeleton\":%s,\"hasEmbeddedBound\":%s,\"shaderCount\":%d,\"shaders\":[%s],")
			TEXT("\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"skinnedVertices\":%d,\"weightSum255\":%d,\"maxBlendIndex\":%d,\"blendIndicesOutsideTable\":%d,")
			TEXT("\"boneIdTable\":%d,\"boneIdTablesIdentity\":%d,\"boneIdTablesNonIdentity\":%d,\"boneCountAt0x28\":%d,\"u16At0x29\":%d,\"flagAt0x2d\":%d,\"lodDistHigh\":%g,\"flagsHigh\":\"0x%08x\",")
			TEXT("\"lodGroups\":%d,\"lodDist\":[%s],\"lodFlags\":[%s],\"u98\":\"0x%08x\",\"groups\":[%s],")   // RUDE_PEDLOD
			TEXT("\"geometriesAllLods\":%d,\"verticesAllLods\":%d,\"trianglesAllLods\":%d,")
			TEXT("\"declarations\":[%s],\"models\":[%s]}"),
			EJson.IsEmpty() ? TEXT("") : TEXT(","), i, *RudeJsonEscape(Name), Hashes[i], bNameHash ? TEXT("true") : TEXT("false"), bSkel ? TEXT("true") : TEXT("false"), bBound ? TEXT("true") : TEXT("false"),
			NSh, *ShJson, EGeos, EVerts, ETris, ESkinned, ESum255, EMaxBlend, EBadIdx, EBoneIds, EIdentity, ENonIdentity, EBoneCount28, EU29, EFlag2d,
			RF32(Sys, Base + 0x70), RU32(Sys, Base + 0x80),
			ELodGroups, *LodDistJson, *LodFlagJson, RU32(Sys, Base + 0x98), *GrpJson, EGeosAll, EVertsAll, ETrisAll,   // RUDE_PEDLOD
			*DeclJson, *MJson);
		TotalGeos += EGeos; TotalVerts += EVerts; TotalTris += ETris; TotalSum255 += ESum255; TotalSkinned += ESkinned; TotalBadIdx += EBadIdx;
		TotalGeosAll += EGeosAll; TotalVertsAll += EVertsAll; TotalTrisAll += ETrisAll; TotalLodGroups += ELodGroups;   // RUDE_PEDLOD
		if (ELodGroups > 1) { ++EntriesWithLods; }
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"path\":\"%s\",\"version\":%u,\"sysSize\":%d,\"gfxSize\":%d,\"blockmapSysPages\":%d,\"blockmapGfxPages\":%d,")
		TEXT("\"entries\":%d,\"entriesAt0x38\":%d,\"entriesUnresolved\":%d,\"hashesAscending\":%s,\"namesNotHashingToEntry\":%d,\"entriesWithSkeleton\":%d,\"entriesWithBound\":%d,")
		TEXT("\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"skinnedVertices\":%d,\"weightSum255\":%d,\"blendIndicesOutsideTable\":%d,")
		TEXT("\"entriesWithLodGroups\":%d,\"lodGroups\":%d,\"geometriesAllLods\":%d,\"verticesAllLods\":%d,\"trianglesAllLods\":%d,")   // RUDE_PEDLOD: the six above stay HIGH-only
		TEXT("\"sharedBlocks\":%d,\"declsRejected\":%d,\"boundsProblems\":%d,\"firstProblem\":\"%s\",\"detail\":[%s]}"),
		*RudeJsonEscape(BinPath), Version, Sys.Num(), Gfx.Num(), SysPages, GfxPages,
		N, N2, EntriesUnresolved, bAscending ? TEXT("true") : TEXT("false"), TotalNameMismatch, Skeletons, Bounds,
		TotalGeos, TotalVerts, TotalTris, TotalSkinned, TotalSum255, TotalBadIdx,
		EntriesWithLods, TotalLodGroups, TotalGeosAll, TotalVertsAll, TotalTrisAll,   // RUDE_PEDLOD
		Shared, DeclBad, BoundsBad, *RudeJsonEscape(First), *EJson);
}

// ---- InspectPedLods (RUDE_PEDLOD) ------------------------------------------------------------
FString URudeToolset::InspectPedLods(const FString& SkeletalMeshAssetPath)
{
	using namespace RudeYdd;
#if WITH_EDITORONLY_DATA
	const FString P = SkeletalMeshAssetPath.TrimStartAndEnd();
	if (P.IsEmpty()) { return Fail(TEXT("give a USkeletalMesh content path")); }
	USkeletalMesh* SK = LoadObject<USkeletalMesh>(nullptr, *P);
	if (!SK) { return Fail(FString::Printf(TEXT("SkeletalMesh not found: %s"), *P)); }
	const int32 NumLods = SK->GetLODNum();
	const TArray<FSkeletalMaterial>& Mats = SK->GetMaterials();
	FString LJson;
	int32 Exportable = 0, TotalV = 0, TotalT = 0, SlotsUnmatched = 0;
	for (int32 L = 0; L < NumLods; ++L)
	{
		const bool bHasDesc = SK->HasMeshDescription(L);
		const FMeshDescription* MD = bHasDesc ? SK->GetMeshDescription(L) : nullptr;
		const FSkeletalMeshLODInfo* LI = SK->GetLODInfo(L);
		int32 V = 0, T = 0, Groups = 0, Unmatched = 0;
		if (MD)
		{
			V = MD->Vertices().Num();
			T = MD->Triangles().Num();
			FSkeletalMeshConstAttributes A(*MD);
			TPolygonGroupAttributesConstRef<FName> Slots = A.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID GroupID : MD->PolygonGroups().GetElementIDs())
			{
				++Groups;
				bool bFound = false;
				for (const FSkeletalMaterial& M : Mats) { if (M.MaterialSlotName == Slots[GroupID]) { bFound = true; break; } }
				if (!bFound) { ++Unmatched; }
			}
		}
		if (bHasDesc && L < 3) { ++Exportable; }
		TotalV += V; TotalT += T; SlotsUnmatched += Unmatched;
		LJson += FString::Printf(
			TEXT("%s{\"lod\":%d,\"rageGroup\":\"%s\",\"hasMeshDescription\":%s,\"vertices\":%d,\"triangles\":%d,\"polygonGroups\":%d,")
			TEXT("\"materialSlotsUnmatched\":%d,\"screenSize\":%g,\"lodHysteresis\":%g,\"exported\":%s}"),
			L ? TEXT(",") : TEXT(""), L, (L == 0 ? TEXT("DrawableModelsHigh") : (L == 1 ? TEXT("DrawableModelsMedium") : (L == 2 ? TEXT("DrawableModelsLow") : TEXT("(not written - the game ships no VeryLow group)")))),
			bHasDesc ? TEXT("true") : TEXT("false"), V, T, Groups, Unmatched,
			LI ? (double)LI->ScreenSize.Default : 0.0, LI ? (double)LI->LODHysteresis : 0.0,
			(bHasDesc && L < 3) ? TEXT("true") : TEXT("false"));
	}
	return FString::Printf(
		TEXT("{\"ok\":%s,\"asset\":\"%s\",\"lods\":%d,\"lodsExported\":%d,\"materialSlots\":%d,\"materialSlotsUnmatched\":%d,")
		TEXT("\"vertices\":%d,\"triangles\":%d,\"skeleton\":\"%s\",")
		TEXT("\"note\":\"LOD0/1/2 are written as DrawableModelsHigh/Medium/Low; a 4th+ LOD is NOT exported (0/2,125 game ped entries carry a VeryLow group). ScreenSize is an INFERRED editor preview setting and no exported byte reads it - the ydd's four lodDist floats travel a different road: the outfit carries them from the source entry and ExportPedReplace hands them to ExportYddBinary as LODDIST=.\",")
		TEXT("\"detail\":[%s]}"),
		(NumLods > 0 && SlotsUnmatched == 0) ? TEXT("true") : TEXT("false"), *RudeJsonEscape(P), NumLods, Exportable,
		Mats.Num(), SlotsUnmatched, TotalV, TotalT,
		*RudeJsonEscape(SK->GetSkeleton() ? SK->GetSkeleton()->GetPathName() : FString(TEXT("none"))), *LJson);
#else
	return Fail(TEXT("editor-only"));
#endif
}
