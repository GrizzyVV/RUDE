// RUDE - RAGE <-> Unreal Development Environment
// Binary lanes: the RSC7 .ydr reader (RudeYdrBin), the .ytd writer (RudeYtd), the .ybn writer (RudeYbn),
// and the tools on them (ExportYtdBinary, ExportYdrBinary[Batch], ExportYbnBinary, ProbeYdrBinary).
// Split out of RudeToolset.cpp 2026-09-06. Every reader here consumes UNTRUSTED bytes from the user's
// own game files: bounds-check everything, return an error, never crash the editor.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetCompilingManager.h"
#include "FileHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "Interfaces/Interface_CollisionDataProvider.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Framework/Application/SlateApplication.h"
#include "XmlFile.h"
#include "RudeCorpus.h"
#include "RudeDds.h"
#include "RudeEntityComponent.h"
#include "RudeArchetype.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "ContentStreaming.h"
#include "Containers/Ticker.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/LevelStreaming.h"
#include "ShaderCompiler.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "RudeToolsetInternal.h"

// ======================= RudeYdrBin - READING binary .ydr (RSC7 v165) =======================
// The import side's keystone: RUDE could WRITE binary but only READ the XML form, so QUARRY's
// output (real game binaries) could not reach the importer at all. This parses the container and
// the drawable graph into the SAME RudeYdr::FGeo intermediate the XML lane produces, so the proven
// mesh builder is reused rather than duplicated (today's crash #6 was a duplication bug - do not
// grow a second mesh builder).
// Struct map: ENGINEERING_LOG "ydr binary format". Everything here reads UNTRUSTED bytes from the
// user's own game files, so every access is bounds-checked and a malformed file must return an
// error, never crash the editor.
namespace RudeYdrBin
{
	struct FRes
	{
		TArray<uint8> Sys;      // system (virtual) segment
		TArray<uint8> Gfx;      // graphics (physical) segment - often empty (gfxFlags size 0)
		uint32 Version = 0;

		// A stored pointer is tagged: high nibble 5 = system, 6 = graphics; low 28 bits = offset.
		// Returns the segment and validates the whole [off, off+Need) span before any read.
		const TArray<uint8>* Resolve(uint32 Tagged, int32 Need, int32& OutOff) const
		{
			const uint32 Tag = Tagged >> 28;
			const int32 Off = (int32)(Tagged & 0x0FFFFFFFu);
			const TArray<uint8>* S = (Tag == 5) ? &Sys : ((Tag == 6) ? &Gfx : nullptr);
			if (!S || Off < 0 || Need < 0 || Off + Need > S->Num()) { return nullptr; }
			OutOff = Off;
			return S;
		}
	};

	static bool Rd(const TArray<uint8>& B, int32 Off, void* Dst, int32 N)
	{
		if (Off < 0 || N < 0 || Off + N > B.Num()) { return false; }
		FMemory::Memcpy(Dst, B.GetData() + Off, N);
		return true;
	}
	static bool U16(const TArray<uint8>& B, int32 O, uint16& V) { return Rd(B, O, &V, 2); }
	static bool U32(const TArray<uint8>& B, int32 O, uint32& V) { return Rd(B, O, &V, 4); }
	static bool F32(const TArray<uint8>& B, int32 O, float& V) { return Rd(B, O, &V, 4); }

	// Total bytes described by an RSC7 flag word's page plan. base = 0x200<<ss, class-k page =
	// base<<k, counts packed at fixed bit positions. Same scheme the writer's pager encodes.
	static uint32 SegSizeFromFlags(uint32 Flags)
	{
		static const int32 KBit[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };
		static const uint32 KMask[9] = { 1, 1, 1, 1, 0x7F, 0x3F, 0xF, 3, 1 };
		const uint32 F = Flags & 0x0FFFFFFFu;
		const uint32 Base = 0x200u << (F & 0xFu);
		uint32 Total = 0;
		for (int32 k = 0; k <= 8; ++k)
		{
			Total += ((F >> KBit[k]) & KMask[k]) * (Base << k);
		}
		return Total;
	}

	// RSC7 file -> inflated system + graphics segments.
	static bool LoadFile(const FString& Path, FRes& Out, FString& Error)
	{
		TArray<uint8> File;
		if (!FFileHelper::LoadFileToArray(File, *Path)) { Error = TEXT("cannot read file"); return false; }
		if (File.Num() < 16) { Error = TEXT("shorter than an RSC7 header"); return false; }
		if (File[0] != 'R' || File[1] != 'S' || File[2] != 'C' || File[3] != '7')
		{
			Error = TEXT("not an RSC7 container (no 'RSC7' magic)");
			return false;
		}
		uint32 Ver = 0, SysF = 0, GfxF = 0;
		U32(File, 4, Ver); U32(File, 8, SysF); U32(File, 12, GfxF);
		if (Ver != 165)
		{
			// v159 = the GTA V ENHANCED drawable; loading it into a Legacy pipeline is the
			// classic "Invalid fixup" report, so name the mismatch explicitly.
			Error = FString::Printf(TEXT("version %u is not a Legacy drawable (want v165%s)"), Ver,
				Ver == 159 ? TEXT("; v159 = GTA V Enhanced") : TEXT(""));
			return false;
		}
		const uint32 SysSize = SegSizeFromFlags(SysF);
		const uint32 GfxSize = SegSizeFromFlags(GfxF);
		if (SysSize == 0 || SysSize > (1u << 30)) { Error = TEXT("implausible system segment size"); return false; }

		// The body is headerless (raw) DEFLATE of [system | graphics].
		TArray<uint8> Blob;
		Blob.SetNumUninitialized((int32)(SysSize + GfxSize));
		int32 OutSize = Blob.Num();
		if (!FCompression::UncompressMemory(NAME_Zlib, Blob.GetData(), OutSize,
		                                   File.GetData() + 16, File.Num() - 16,
		                                   COMPRESS_NoFlags, -15))
		{
			Error = TEXT("raw-DEFLATE inflate failed (truncated, or an Oodle-packed resource)");
			return false;
		}
		Out.Version = Ver;
		Out.Sys.Append(Blob.GetData(), (int32)SysSize);
		if (GfxSize > 0) { Out.Gfx.Append(Blob.GetData() + SysSize, (int32)GfxSize); }
		return true;
	}

	// ---- VERTEX DECLARATION DECODE ----------------------------------------------------------
	// Derived empirically from all 3,479 real base-game v165 ydrs / 17,370 geometries (see
	// ENGINEERING_LOG "ydr vertex declaration"). 14 distinct declarations exist; our WRITER only
	// ever emits mask 0x59 / stride 36, which is just 43% of the corpus - so a reader that assumes
	// one layout silently misreads the other 57%.
	// LAW: channels are laid out in ASCENDING BIT-INDEX order; a channel's offset is the sum of the
	// sizes of all lower set bits. The grcFvf u64 is a full 16-slot TYPE TABLE indexed by channel
	// bit (nibble i = type of channel i), NOT packed per used channel - which is why it is the same
	// constant 0x7755555555996996 in 17,370/17,370 geometries.
	enum : int32 { CH_POS = 0, CH_BLENDW = 1, CH_BLENDI = 2, CH_NRM = 3, CH_COL0 = 4,
	               CH_COL1 = 5, CH_TC0 = 6, CH_TANGENT = 14, CH_BINORMAL = 15 };

	// nibble -> byte size. Only 5/6/7/9 occur in the map corpus; everything else is UNOBSERVED.
	// Peds/vehicles/DLC use half-float and packed types that are certainly among the rest, so an
	// unknown nibble must SKIP the geometry with a diagnosable message - never guess a size, or a
	// wrong stride scrambles every vertex after the first (the "deformed mesh" class).
	static int32 NibbleSize(uint8 N)
	{
		switch (N)
		{
			case 5: return 8;    // float2
			case 6: return 12;   // float3
			case 7: return 16;   // float4
			case 9: return 4;    // ubyte4 / D3DCOLOR
			default: return -1;  // NOT OBSERVED in the map corpus - refuse rather than guess
		}
	}

	struct FDecl
	{
		uint32 Mask = 0;
		int32 Stride = 0;
		int32 Ofs[16];    // -1 = channel absent
		FDecl() { for (int32 i = 0; i < 16; ++i) { Ofs[i] = -1; } }
		bool Has(int32 Ch) const { return Ch >= 0 && Ch < 16 && Ofs[Ch] >= 0; }
	};

	static bool BuildDecl(uint32 Mask, uint64 Nibbles, int32 DeclStride, FDecl& Out, FString& Error);

	// ---- SELF-VERIFICATION: the export gate, run on our own freshly-built bytes -----------------
	// Wired into ExportYdrBinary so it CANNOT ship a violation, rather than relying on someone
	// remembering to run an offline script. Uses the reader, so there is ONE implementation of the
	// law (duplication is what caused crash #6 in the first place).
	// Checks, all of which correspond to a real in-game crash we paid for:
	//   #6  SINGLE OWNERSHIP - RAGE's fixup rewrites a pointer slot in place and is NOT idempotent,
	//       so a block reached from 2 owners is resolved twice -> "address is neither virtual nor
	//       physical". Counted by IN-DEGREE; the two sanctioned aliases (hdr+0xa0 == hdr+0x50, and
	//       vertex data reached via VB+0x20 / grmGeometry+0x78) are simply not counted as edges.
	//   ①  geoBounds must be N+1 pairs (union first) when N>1, and grmModel+0x2e == geometry count.
	//   decl - every emitted vertex declaration must decode to exactly the declared stride.
	struct FVerify
	{
		int32 SharedBlocks = 0;
		int32 DeclBad = 0;
		int32 BoundsBad = 0;
		FString FirstProblem;
	};

	static void Note(FVerify& V, const FString& What)
	{
		if (V.FirstProblem.IsEmpty()) { V.FirstProblem = What; }
	}

	// Records an owner for a target; the SECOND owner of any target is a violation.
	static void Own(TMap<int32, int32>& InDeg, FVerify& V, uint32 Tagged, const FString& Label)
	{
		if ((Tagged >> 28) != 5) { return; }              // only system-segment blocks here
		const int32 Off = (int32)(Tagged & 0x0FFFFFFFu);
		if (Off == 0) { return; }
		int32& N = InDeg.FindOrAdd(Off);
		if (++N > 1)
		{
			++V.SharedBlocks;
			Note(V, FString::Printf(TEXT("block @0x%x has %d owners (%s) - non-idempotent fixup "
			                             "will resolve it %d times"), Off, N, *Label, N));
		}
	}

	// Verify a drawable we just built, in place, before it is compressed and written.
	static FVerify VerifyDrawable(const TArray<uint8>& Sys)
	{
		FVerify V;
		TMap<int32, int32> InDeg;
		auto P = [&Sys](int32 O) -> uint32 { uint32 X = 0; U32(Sys, O, X); return X; };
		auto Deref = [&Sys](uint32 T, int32 Need, int32& O) -> bool
		{
			if ((T >> 28) != 5) { return false; }
			O = (int32)(T & 0x0FFFFFFFu);
			return O >= 0 && O + Need <= Sys.Num();
		};

		Own(InDeg, V, P(0x08), TEXT("hdr+0x08 blockmap"));
		Own(InDeg, V, P(0x10), TEXT("hdr+0x10 ShaderGroup"));
		Own(InDeg, V, P(0x50), TEXT("hdr+0x50 ModelsHigh"));
		Own(InDeg, V, P(0xa8), TEXT("hdr+0xa8 name"));
		Own(InDeg, V, P(0xc8), TEXT("hdr+0xc8 Bound"));
		// hdr+0xa0 intentionally NOT counted - byte-identical alias of +0x50 in 3,479/3,479 real files.

		// ShaderGroup -> shaders -> param tables -> texture stubs -> stub name strings
		int32 SG = 0;
		if (Deref(P(0x10), 0x40, SG))
		{
			Own(InDeg, V, P(SG + 0x10), TEXT("SG+0x10 shaderArr"));
			uint16 NSh = 0; U16(Sys, SG + 0x18, NSh);
			int32 Arr = 0;
			if (Deref(P(SG + 0x10), (int32)NSh * 8, Arr))
			{
				for (int32 si = 0; si < (int32)NSh; ++si)
				{
					Own(InDeg, V, P(Arr + si * 8), FString::Printf(TEXT("shaderArr[%d]"), si));
					int32 Blk = 0;
					if (!Deref(P(Arr + si * 8), 0x30, Blk)) { continue; }
					Own(InDeg, V, P(Blk + 0x00), FString::Printf(TEXT("shader%d paramTable"), si));
					uint32 NPar = 0; U32(Sys, Blk + 0x10, NPar);
					const int32 PC = (int32)(NPar & 0xFFFF);
					int32 Tbl = 0;
					if (PC <= 0 || PC > 64 || !Deref(P(Blk + 0x00), PC * 16, Tbl)) { continue; }
					for (int32 pi = 0; pi < PC; ++pi)
					{
						int32 Stub = 0;
						if (!Deref(P(Tbl + pi * 16 + 8), 0x34, Stub)) { continue; }
						uint32 Marker = 0; U32(Sys, Stub + 0x30, Marker);
						if (Marker != 0x00020001u) { continue; }   // inline vec4, not a texture stub
						Own(InDeg, V, P(Tbl + pi * 16 + 8), FString::Printf(TEXT("shader%d.param[%d] stub"), si, pi));
						Own(InDeg, V, P(Stub + 0x28), FString::Printf(TEXT("shader%d stub name"), si));
					}
				}
			}
		}

		// models -> grmModel -> geometries -> VB/IB/fvf/data
		int32 MH = 0;
		if (Deref(P(0x50), 0x10, MH))
		{
			Own(InDeg, V, P(MH + 0x00), TEXT("modelsHdr ptrArr"));
			uint16 NMod = 0; U16(Sys, MH + 0x08, NMod);
			int32 MArr = 0;
			if (Deref(P(MH + 0x00), (int32)NMod * 8, MArr))
			{
				for (int32 mi = 0; mi < (int32)NMod; ++mi)
				{
					Own(InDeg, V, P(MArr + mi * 8), FString::Printf(TEXT("modelArr[%d]"), mi));
					int32 M = 0;
					if (!Deref(P(MArr + mi * 8), 0x30, M)) { continue; }
					Own(InDeg, V, P(M + 0x08), TEXT("model geoArr"));
					Own(InDeg, V, P(M + 0x18), TEXT("model geoBounds"));
					Own(InDeg, V, P(M + 0x20), TEXT("model shaderMap"));
					uint16 NGeo = 0, NGeo2e = 0;
					U16(Sys, M + 0x10, NGeo);
					U16(Sys, M + 0x2e, NGeo2e);
					if (NGeo2e != NGeo)
					{
						++V.BoundsBad;
						Note(V, FString::Printf(TEXT("grmModel+0x2e is %u but geometry count is %u"), NGeo2e, NGeo));
					}
					// geoBounds: N+1 pairs (union first) when N>1, exactly 1 pair when N==1
					const int32 Pairs = (NGeo > 1) ? ((int32)NGeo + 1) : 1;
					int32 GB = 0;
					if (!Deref(P(M + 0x18), Pairs * 0x20, GB))
					{
						++V.BoundsBad;
						Note(V, FString::Printf(TEXT("geoBounds does not span %d pairs for %u geometries"),
						                        Pairs, NGeo));
					}
					int32 GArr = 0;
					if (!Deref(P(M + 0x08), (int32)NGeo * 8, GArr)) { continue; }
					for (int32 gi = 0; gi < (int32)NGeo; ++gi)
					{
						Own(InDeg, V, P(GArr + gi * 8), FString::Printf(TEXT("geoArr[%d]"), gi));
						int32 G = 0;
						if (!Deref(P(GArr + gi * 8), 0x80, G)) { continue; }
						Own(InDeg, V, P(G + 0x18), FString::Printf(TEXT("geo%d VB"), gi));
						Own(InDeg, V, P(G + 0x38), FString::Printf(TEXT("geo%d IB"), gi));
						// geo+0x78 NOT counted - sanctioned vertex-data alias
						int32 VB = 0;
						if (Deref(P(G + 0x18), 0x40, VB))
						{
							Own(InDeg, V, P(VB + 0x10), FString::Printf(TEXT("geo%d vertex data"), gi));
							Own(InDeg, V, P(VB + 0x30), FString::Printf(TEXT("geo%d grcFvf"), gi));
							// VB+0x20 NOT counted - sanctioned vertex-data alias
							uint16 Stride16 = 0; U16(Sys, G + 0x70, Stride16);
							uint32 Mask = 0; uint64 Nib = 0;
							int32 Fvf = 0;
							if (Deref(P(VB + 0x30), 0x10, Fvf))
							{
								U32(Sys, Fvf + 0x00, Mask);
								Rd(Sys, Fvf + 0x08, &Nib, 8);
								FDecl D; FString E;
								if (!BuildDecl(Mask, Nib, (int32)Stride16, D, E))
								{
									++V.DeclBad;
									Note(V, FString::Printf(TEXT("geo%d declaration: %s"), gi, *E));
								}
							}
						}
						int32 IB = 0;
						if (Deref(P(G + 0x38), 0x20, IB))
						{
							Own(InDeg, V, P(IB + 0x10), FString::Printf(TEXT("geo%d index data"), gi));
						}
					}
				}
			}
		}

		// embedded bound: composite -> children -> per-child arrays -> BVH nodes/trees
		int32 Comp = 0;
		// FIXED 2026-08-03 - this gate had NEVER run on the embedded bound. NCh was read from
		// Comp+0x78, which is the CurrentMatrices POINTER (this file writes it at :6375 as
		// PPTR(Comp,0x78,OXf), aliased to +0x80); NumChildren is the u16 at Comp+0xa0 (written
		// at :6378, and read there by quarry's oracle-validated reader, ydr2xml.py:1011). So the
		// U16 read returned the low 16 bits of a tagged pointer - measured 49,440 / 56,720 /
		// 65,472 / 60,464 / 14,192 / 5,136 across the 14 emitted .ydr, where the true count is 1
		// in 14/14. The Deref bounds check below then failed in 13/14 files and the composite
		// branch was skipped entirely (0 children walked); in the 14th the garbage count was
		// small enough to pass, and the walk fabricated 5 spurious shared-block reports.
		// COST: the embedded phBound - poly array, vertex array, materials, per-poly material
		// index, BVH header, node array, m_Trees, the largest and most crash-prone subgraph -
		// was never checked for the double-ownership that causes the non-idempotent-fixup crash,
		// while every export still printed "selfCheck: passed". The span also had to widen from
		// 0x80 to 0xb0: the composite header this writer emits is 0xb0 bytes (:6364) and +0xa0
		// must be inside the checked span or the read is unguarded.
		if (Deref(P(0xc8), 0xb0, Comp))
		{
			Own(InDeg, V, P(Comp + 0x70), TEXT("composite children array"));
			uint16 NCh = 0; U16(Sys, Comp + 0xa0, NCh);
			int32 CArr = 0;
			if (Deref(P(Comp + 0x70), (int32)NCh * 8, CArr))
			{
				for (int32 ci = 0; ci < (int32)NCh; ++ci)
				{
					Own(InDeg, V, P(CArr + ci * 8), FString::Printf(TEXT("child[%d]"), ci));
					int32 Ch = 0;
					if (!Deref(P(CArr + ci * 8), 0x150, Ch)) { continue; }
					static const int32 ChildPtrs[5] = { 0x88, 0xb0, 0xf0, 0x118, 0x130 };
					for (int32 k = 0; k < 5; ++k)
					{
						Own(InDeg, V, P(Ch + ChildPtrs[k]),
						    FString::Printf(TEXT("child[%d]+0x%x"), ci, ChildPtrs[k]));
					}
					int32 Bvh = 0;
					if (Deref(P(Ch + 0x130), 0x80, Bvh))
					{
						Own(InDeg, V, P(Bvh + 0x00), FString::Printf(TEXT("child[%d] BVH nodes"), ci));
						Own(InDeg, V, P(Bvh + 0x70), FString::Printf(TEXT("child[%d] m_Trees"), ci));
					}
				}
			}
		}
		return V;
	}

	static bool BuildDecl(uint32 Mask, uint64 Nibbles, int32 DeclStride, FDecl& Out, FString& Error)
	{
		Out = FDecl();
		Out.Mask = Mask;
		int32 Off = 0;
		for (int32 Bit = 0; Bit < 16; ++Bit)
		{
			if (((Mask >> Bit) & 1u) == 0) { continue; }
			const uint8 Nb = (uint8)((Nibbles >> (Bit * 4)) & 0xFull);
			const int32 Sz = NibbleSize(Nb);
			if (Sz < 0)
			{
				Error = FString::Printf(
					TEXT("unsupported vertex channel type: mask 0x%x bit %d has nibble 0x%x "
					     "(only float2/3/4 and ubyte4 are derived from the map corpus)"), Mask, Bit, Nb);
				return false;
			}
			Out.Ofs[Bit] = Off;
			Off += Sz;
		}
		// The stride is declared in three places and agrees in 17,370/17,370, so a mismatch here
		// means the declaration is not one we understand - refuse instead of misaligning.
		if (Off != DeclStride)
		{
			Error = FString::Printf(TEXT("computed stride %d != declared %d for mask 0x%x"),
			                        Off, DeclStride, Mask);
			return false;
		}
		Out.Stride = Off;
		return true;
	}
}

// ======================= ExportYtdBinary - clean-room .ytd (RSC7 v13) =======================
// Reverse-engineered from our own CW-roundtripped diff pair + verified byte-identical
// (tools/write_ytd.py, docs/ENGINEERING_LOG "RSC7 binary container"). No CodeWalker code read.
// RSC7 header (16B: 'RSC7' | u32 version=13 | u32 sysFlags | u32 gfxFlags) + raw DEFLATE of
// [ system-segment | graphics-pages ]. System = pgDictionary<grcTexture>; graphics = pixel pages.
// Pointers are tagged fixups: 0x50000000|off -> system, 0x60000000|off -> graphics.
namespace RudeYtd
{
	// Jenkins one-at-a-time over the lowercased name (RAGE joaat). CONFIRMED against
	// the observed dictionary hashes (0x97f2c7c3 / 0x9a5d45aa).
	static uint32 Joaat(const FString& S)
	{
		uint32 H = 0;
		const FString L = S.ToLower();
		for (int32 i = 0; i < L.Len(); ++i)
		{
			H += (uint8)L[i];
			H += (H << 10);
			H ^= (H >> 6);
		}
		H += (H << 3);
		H ^= (H >> 11);
		H += (H << 15);
		return H;
	}

	// Low-28 RSC7 flag bits for a segment size: tile into power-of-two pages (largest
	// first, capped 4MB), base = smallest page / 16, class k holds pages of base*(1<<k).
	// Also returns the page count (for the blockmap). The segment-type high nibble
	// (system 0x0 / graphics 0xd, verified vs 400 real ytds) is OR'd in by the caller.
	static uint32 FlagsFromSize(uint32 Size, int32& OutPageCount)
	{
		const uint32 MAXPAGE = 0x400000;
		TArray<uint32> Pages;
		uint32 Rem = Size;
		while (Rem > 0)
		{
			uint32 P = MAXPAGE;
			while (P > Rem) { P >>= 1; }
			Pages.Add(P);
			Rem -= P;
		}
		OutPageCount = Pages.Num();
		uint32 Smallest = 0xFFFFFFFFu;
		for (uint32 P : Pages) { Smallest = FMath::Min(Smallest, P); }
		const uint32 Base = Smallest / 16;
		int32 ss = 0; { uint32 b = Base; while (b > 0x200) { b >>= 1; ++ss; } }
		static const int32 BitPos[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };
		int32 Counts[9] = { 0 };
		for (uint32 P : Pages)
		{
			int32 k = 0; uint32 r = P / Base; while (r > 1) { r >>= 1; ++k; }
			if (k >= 0 && k < 9) { Counts[k]++; }
		}
		uint32 Flag = (uint32)ss;
		for (int32 k = 0; k < 9; ++k) { Flag |= ((uint32)Counts[k]) << BitPos[k]; }
		return Flag;
	}

	static void PutU32(TArray<uint8>& B, int32 Off, uint32 V)
	{
		B[Off] = V & 0xFF; B[Off + 1] = (V >> 8) & 0xFF; B[Off + 2] = (V >> 16) & 0xFF; B[Off + 3] = (V >> 24) & 0xFF;
	}
	static void PutU16(TArray<uint8>& B, int32 Off, uint16 V) { B[Off] = V & 0xFF; B[Off + 1] = (V >> 8) & 0xFF; }

	// Box-downscale BGRA in place by 2x (average each 2x2 block). Power-of-two textures.
	static void HalveBGRA(TArray<uint8>& P, int32& W, int32& H)
	{
		const int32 NW = FMath::Max(1, W / 2), NH = FMath::Max(1, H / 2);
		TArray<uint8> Out; Out.SetNumUninitialized(NW * NH * 4);
		for (int32 y = 0; y < NH; ++y)
		{
			for (int32 x = 0; x < NW; ++x)
			{
				// FIXED 2026-08-03 - the second tap of each pair was unclamped. NW/NH floor at 1
				// (FMath::Max above), but the reads used 2*x+1 / 2*y+1 regardless, so at W==1 or
				// H==1 this indexed one full row/column past the buffer - a TArray bounds fatal in
				// the editor. It was unreachable only because the caller's loop guard bailed out
				// before a dimension reached 1; that guard is the MaxDim bug fixed below, so
				// making this total is what allows the guard to be corrected.
				// NOT AN OUTPUT CHANGE: for W,H >= 2 we have 2*x+1 <= W-1 and 2*y+1 <= H-1 for
				// every x < W/2, y < H/2 (true for odd dimensions too), so the clamp binds only
				// in the 1-pixel case that previously read out of bounds.
				const int32 sx0 = 2 * x, sx1 = FMath::Min(2 * x + 1, W - 1);
				const int32 sy0 = 2 * y, sy1 = FMath::Min(2 * y + 1, H - 1);
				for (int32 c = 0; c < 4; ++c)
				{
					const int32 s0 = P[(sy0 * W + sx0) * 4 + c];
					const int32 s1 = P[(sy0 * W + sx1) * 4 + c];
					const int32 s2 = P[(sy1 * W + sx0) * 4 + c];
					const int32 s3 = P[(sy1 * W + sx1) * 4 + c];
					Out[(y * NW + x) * 4 + c] = (uint8)((s0 + s1 + s2 + s3) / 4);
				}
			}
		}
		P = MoveTemp(Out); W = NW; H = NH;
	}

	// ---- clean-room BC block compressors (faithful port of tools/build_ytd.py's numpy
	// encoders, which render DXT1 + ATI2 in-game). Public S3TC/DX spec, no lifted code. ----
	static uint16 Pack565(int r, int g, int b)
	{
		return (uint16)((((r >> 3) & 0x1F) << 11) | (((g >> 2) & 0x3F) << 5) | ((b >> 3) & 0x1F));
	}
	static void Expand565(uint16 c, int& r, int& g, int& b)
	{
		const int R = (c >> 11) & 0x1F, G = (c >> 5) & 0x3F, B = c & 0x1F;
		r = (R << 3) | (R >> 2); g = (G << 2) | (G >> 4); b = (B << 3) | (B >> 2);
	}
	// BC1 colour block: 16 RGB pixels -> 8 bytes (4-colour mode; c0>=c1 via component max/min).
	static void Bc1(const int rgb[16][3], uint8* out)
	{
		int mx[3] = { rgb[0][0], rgb[0][1], rgb[0][2] }, mn[3] = { rgb[0][0], rgb[0][1], rgb[0][2] };
		for (int i = 1; i < 16; ++i) { for (int c = 0; c < 3; ++c) { mx[c] = FMath::Max(mx[c], rgb[i][c]); mn[c] = FMath::Min(mn[c], rgb[i][c]); } }
		const uint16 c0 = Pack565(mx[0], mx[1], mx[2]), c1 = Pack565(mn[0], mn[1], mn[2]);
		int p[4][3];
		Expand565(c0, p[0][0], p[0][1], p[0][2]);
		Expand565(c1, p[1][0], p[1][1], p[1][2]);
		for (int c = 0; c < 3; ++c) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
		uint32 packed = 0;
		for (int i = 0; i < 16; ++i)
		{
			int best = 0x7FFFFFFF, bk = 0;
			for (int k = 0; k < 4; ++k)
			{
				const int dr = rgb[i][0] - p[k][0], dg = rgb[i][1] - p[k][1], db = rgb[i][2] - p[k][2];
				const int d = dr * dr + dg * dg + db * db;
				if (d < best) { best = d; bk = k; }
			}
			packed |= (uint32)bk << (2 * i);
		}
		out[0] = c0 & 0xFF; out[1] = (c0 >> 8) & 0xFF; out[2] = c1 & 0xFF; out[3] = (c1 >> 8) & 0xFF;
		for (int k = 0; k < 4; ++k) { out[4 + k] = (packed >> (8 * k)) & 0xFF; }
	}
	// BC4 single channel: 16 values -> 8 bytes (8-value interp; v0>=v1).
	static void Bc4(const int v[16], uint8* out)
	{
		int v0 = v[0], v1 = v[0];
		for (int i = 1; i < 16; ++i) { v0 = FMath::Max(v0, v[i]); v1 = FMath::Min(v1, v[i]); }
		// FIXED 2026-08-03 - the palette was off by one index. It built
		// pal[k] = ((7-k)*v0 + k*v1)/7 across k=0..7 and then STAMPED pal[0]=v0, pal[1]=v1
		// over the first two entries. That left every interpolant one rung toward v1 of where
		// the GPU puts it, and made pal[7] == v1 - a duplicate of index 1 that the first-wins
		// tie-break below never selects, so index 7 was unreachable and 1 of the codec's 8
		// slots was discarded on every block. This is the encoder for ATI2/BC5 (every normal
		// map) and for the DXT5 alpha channel.
		// COST, measured (encode here -> decode with a from-spec decoder that agrees with
		// texture2ddecoder on 20,000/20,000 random blocks; source = the real 1024^2 normal map
		// output/rude_rockwall_tex/tex/TX_Stone_01a_NRM_1k.png, 8,000 blocks/channel):
		//   R meanAbsErr 3.748 -> 1.024, max 38 -> 12, PSNR 34.28 -> 44.62 dB
		//   G meanAbsErr 4.561 -> 1.275, max 45 -> 15, PSNR 32.50 -> 42.69 dB
		// ~10 dB of normal-map precision thrown away silently: the DDS was always structurally
		// valid, so no load/parse/screenshot gate could see it. The identical bug was in the
		// Python encoder tools/build_ytd.py, so the C++-vs-Python byte-identity gate agreed
		// while both were wrong - both were fixed in the same change and now match 3000/3000.
		// BC4/RGTC1, red0 > red1: indices 0 and 1 ARE the endpoints; the six interpolants
		// red_2..red_7 walk 6/7..1/7 of the way from v0 to v1 and exclude the endpoints.
		int pal[8];
		pal[0] = v0; pal[1] = v1;
		for (int k = 2; k < 8; ++k) { pal[k] = ((8 - k) * v0 + (k - 1) * v1) / 7; }
		uint64 packed = 0;
		for (int i = 0; i < 16; ++i)
		{
			int best = 0x7FFFFFFF, bk = 0;
			for (int k = 0; k < 8; ++k) { const int d = FMath::Abs(v[i] - pal[k]); if (d < best) { best = d; bk = k; } }
			packed |= (uint64)bk << (3 * i);
		}
		out[0] = (uint8)v0; out[1] = (uint8)v1;
		for (int k = 0; k < 6; ++k) { out[2 + k] = (packed >> (8 * k)) & 0xFF; }
	}
	// Encode one mip level of a BGRA buffer (LW x LH) in Mode -> append blocks to Out.
	// Block order row-major; pixels row-major; edge-replicate pad for sub-4 mips (matches build_ytd).
	static void EncodeLevel(const TArray<uint8>& BGRA, int32 LW, int32 LH, const FString& Mode, TArray<uint8>& Out)
	{
		const int32 BW = (LW + 3) / 4, BH = (LH + 3) / 4;
		const bool bAti2 = (Mode == TEXT("ATI2")), bDxt5 = (Mode == TEXT("DXT5"));
		uint8 blk[8];
		int rgb[16][3], chn[16], Rv[16], Gv[16], Av[16];
		for (int32 by = 0; by < BH; ++by)
		{
			for (int32 bx = 0; bx < BW; ++bx)
			{
				for (int py = 0; py < 4; ++py)
				{
					for (int px = 0; px < 4; ++px)
					{
						const int32 sx = FMath::Min(bx * 4 + px, LW - 1), sy = FMath::Min(by * 4 + py, LH - 1);
						const uint8* pxl = &BGRA[(sy * LW + sx) * 4];
						const int idx = py * 4 + px;
						rgb[idx][0] = pxl[2]; rgb[idx][1] = pxl[1]; rgb[idx][2] = pxl[0];   // R,G,B from BGRA
						Rv[idx] = pxl[2]; Gv[idx] = pxl[1]; Av[idx] = pxl[3];
					}
				}
				if (bAti2)
				{
					for (int i = 0; i < 16; ++i) { chn[i] = Rv[i]; } Bc4(chn, blk); Out.Append(blk, 8);
					for (int i = 0; i < 16; ++i) { chn[i] = Gv[i]; } Bc4(chn, blk); Out.Append(blk, 8);
				}
				else if (bDxt5)
				{
					for (int i = 0; i < 16; ++i) { chn[i] = Av[i]; } Bc4(chn, blk); Out.Append(blk, 8);
					Bc1(rgb, blk); Out.Append(blk, 8);
				}
				else { Bc1(rgb, blk); Out.Append(blk, 8); }
			}
		}
	}
}

// ---- ExportMeshTextures ----------------------------------------------------------------
// The textures a static mesh's material instances reference (the Diffuse / Normal parameters
// the ydr writer names as samplers), packed into ONE .ytd through ExportYtdBinary, downscaled to
// MaxDim. The LOD texture dictionary: MakeLodArchetype names the LOD's txd after the archetype and
// this writes it (e.g. MaxDim 256 for a distant shell). Returns ExportYtdBinary's verdict plus the
// texture list.
FString URudeToolset::ExportMeshTextures(const FString& AssetPath, const FString& OutYtdPath, const FString& MaxDim)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh) { return Fail(TEXT("StaticMesh not found")); }
	TSet<FString> Seen;
	FString Specs, Names;
	int32 N = 0;
	for (const FStaticMaterial& SM : Mesh->GetStaticMaterials())
	{
		const UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(SM.MaterialInterface);
		if (!MIC) { continue; }
		for (const TCHAR* Param : { TEXT("Diffuse"), TEXT("Normal"), TEXT("Specular") })
		{
			UTexture* T = nullptr;
			if (!MIC->GetTextureParameterValue(FMaterialParameterInfo(Param), T) || !T) { continue; }
			const FString Path = T->GetPathName();
			if (Seen.Contains(Path)) { continue; }
			Seen.Add(Path);
			const FString Usage = FString(Param).ToUpper();
			Specs += FString::Printf(TEXT("%s%s;%s;%s"), Specs.IsEmpty() ? TEXT("") : TEXT(","), *Path, *T->GetName(), *Usage);
			Names += FString::Printf(TEXT("%s\"%s\""), Names.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(T->GetName()));
			++N;
		}
	}
	if (N == 0) { return Fail(TEXT("the mesh's materials reference no Diffuse/Normal/Specular textures")); }
	FString R = ExportYtdBinary(Specs, OutYtdPath, MaxDim);
	if (R.EndsWith(TEXT("}"))) { R = R.LeftChop(1) + FString::Printf(TEXT(",\"meshTextures\":%d,\"names\":[%s]}"), N, *Names); }
	return R;
}

FString URudeToolset::ExportYtdBinary(const FString& TextureSpecs, const FString& OutYtdPath,
                                      const FString& MaxDim)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
#if WITH_EDITORONLY_DATA
	struct FTexIn { FString Name; uint32 Hash = 0; int32 W = 0; int32 H = 0; uint32 U40 = 20;
	                uint32 Fmt = 21; int32 Stride = 0; int32 Mips = 1; TArray<uint8> Data; };
	TArray<FTexIn> Texs;

	TArray<FString> Entries;
	TextureSpecs.ParseIntoArray(Entries, TEXT(","), true);
	if (Entries.Num() == 0) { return Fail(TEXT("no texture specs (want ContentPath;RageName;Usage , ...)")); }
	const int32 Cap = FCString::Atoi(*MaxDim);   // 0/empty = no cap; else box-downscale oversized textures

	for (const FString& E : Entries)
	{
		TArray<FString> Fld;
		E.ParseIntoArray(Fld, TEXT(";"), true);
		if (Fld.Num() < 2) { return Fail(TEXT("each spec needs ContentPath;RageName[;Usage[;Format]]")); }
		const FString Path = Fld[0].TrimStartAndEnd();
		const FString Name = Fld[1].TrimStartAndEnd();
		const FString Usage = (Fld.Num() > 2) ? Fld[2].TrimStartAndEnd().ToUpper() : TEXT("DIFFUSE");
		FString FmtSel = (Fld.Num() > 3) ? Fld[3].TrimStartAndEnd().ToUpper() : TEXT("AUTO");

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *Path);
		if (!Tex) { return Fail(FString::Printf(TEXT("texture not found: %s"), *Path)); }
		FTextureSource& Src = Tex->Source;
		if (!Src.IsValid()) { return Fail(FString::Printf(TEXT("no editor source: %s"), *Path)); }
		int32 tw = Src.GetSizeX(), th = Src.GetSizeY();
		const ETextureSourceFormat SF = Src.GetFormat();
		TArray64<uint8> Mip;
		if (!Src.GetMipData(Mip, 0, 0, 0, nullptr)) { return Fail(FString::Printf(TEXT("GetMipData failed: %s"), *Path)); }

		// HARDENING 2026-08-03, not a live-bug fix - stated precisely because the audit rated this
		// "silent-wrong" and it is not reachable today. The old code did
		// Memcpy(dst, src, Min(Mip.Num(), BGRA.Num())) into a SetNumUninitialized buffer, which
		// READS as if a short source were expected and tolerated - and a short source would have
		// left the tail uninitialised (not even zeroed) and encoded raw heap into the texture with
		// ok:true. Checked before writing this: GetMipData returns MipImage.RawData sized exactly
		// SizeX*SizeY*BytesPerPixel and the engine check()s it (UE 5.8 Texture.cpp:2980), and
		// GetSizeX/GetSizeY are block-0's dimensions (Texture.h:412-418) while we ask for block 0
		// layer 0 mip 0 - so for BGRA8/BGRE8/G8 the sizes always agree. The guard earns its place
		// anyway: that engine check() is DO_CHECK-gated and compiles out, the G8 branch below
		// indexes Mip[i] unchecked for tw*th bytes, and an explicit refusal beats a silent Min().
		const int64 WantBytes = (int64)tw * th * ((SF == TSF_G8) ? 1 : 4);
		if (SF != TSF_BGRA8 && SF != TSF_BGRE8 && SF != TSF_G8)
		{
			return Fail(FString::Printf(TEXT("unsupported source fmt %d (want BGRA8/G8): %s"), (int32)SF, *Path));
		}
		if ((int64)Mip.Num() != WantBytes)
		{
			return Fail(FString::Printf(TEXT("source mip is %lld bytes, expected %lld for %dx%d fmt %d: %s"),
			                            (int64)Mip.Num(), WantBytes, tw, th, (int32)SF, *Path));
		}
		TArray<uint8> BGRA; BGRA.SetNumUninitialized(tw * th * 4);
		if (SF == TSF_BGRA8 || SF == TSF_BGRE8)
		{
			FMemory::Memcpy(BGRA.GetData(), Mip.GetData(), WantBytes);
		}
		else if (SF == TSF_G8)
		{
			for (int32 i = 0; i < tw * th; ++i)
			{ const uint8 G = Mip[i]; BGRA[i * 4] = G; BGRA[i * 4 + 1] = G; BGRA[i * 4 + 2] = G; BGRA[i * 4 + 3] = 255; }
		}
		// (the unsupported-format else that used to sit here is now the explicit refusal above,
		//  hoisted so the size guard can compute the right bytes-per-pixel for the format)

		// optional downscale (mainly for RAW; DXT/BC keeps full-res small enough)
		// FIXED 2026-08-03 - MaxDim was not the upper bound it is documented to be. The guard was
		// `tw > 1 && th > 1`, so the loop stopped as soon as EITHER dimension hit 1 and shipped the
		// oversized texture with ok:true. Trace for 4096x2 with MaxDim=512: pass 1 halves to
		// 2048x1, pass 2 fails `th > 1` and exits with tw = 2048, 4x over the requested cap. The
		// caller asked for a VRAM bound and silently did not get one. `||` is correct and still
		// terminates: whichever dimension is > 1 halves every pass, so both reach 1 and the loop
		// ends (and at 1x1 the cap is satisfied for any Cap >= 1). Requires the HalveBGRA tap
		// clamp above - relaxing this guard without it would read out of bounds instead.
		while (Cap > 0 && (tw > Cap || th > Cap) && (tw > 1 || th > 1)) { RudeYtd::HalveBGRA(BGRA, tw, th); }

		// resolve AUTO: NORMAL -> ATI2 (BC5); real alpha -> DXT5; else DXT1 (matches build_ytd)
		if (FmtSel == TEXT("AUTO"))
		{
			if (Usage == TEXT("NORMAL")) { FmtSel = TEXT("ATI2"); }
			else
			{
				bool bAlpha = false;
				for (int32 i = 3; i < BGRA.Num(); i += 4) { if (BGRA[i] < 255) { bAlpha = true; break; } }
				FmtSel = bAlpha ? TEXT("DXT5") : TEXT("DXT1");
			}
		}

		FTexIn T;
		T.Name = Name; T.Hash = RudeYtd::Joaat(Name); T.W = tw; T.H = th;
		T.U40 = (Usage == TEXT("NORMAL")) ? 22u : 20u;   // 🧠 usage-derived (reproduced; pending confirm)
		if (FmtSel == TEXT("RAW") || FmtSel == TEXT("A8R8G8B8"))
		{
			T.Fmt = 21; T.Stride = tw * 4; T.Mips = 1; T.Data = MoveTemp(BGRA);      // uncompressed
		}
		else if (FmtSel == TEXT("DXT1") || FmtSel == TEXT("DXT5") || FmtSel == TEXT("ATI2"))
		{
			const int32 blockBytes = (FmtSel == TEXT("DXT1")) ? 8 : 16;
			T.Fmt = (uint32)FmtSel[0] | ((uint32)FmtSel[1] << 8) | ((uint32)FmtSel[2] << 16) | ((uint32)FmtSel[3] << 24);  // FourCC
			T.Stride = ((tw + 3) / 4) * blockBytes / 4;                              // bytes per pixel-row
			int32 cw = tw, ch = th, mips = 0;
			TArray<uint8> lvl = MoveTemp(BGRA);
			while (true)                                                             // mip chain down to 4x4
			{
				RudeYtd::EncodeLevel(lvl, cw, ch, FmtSel, T.Data);
				++mips;
				if (cw <= 4 || ch <= 4) { break; }   // RAGE stops at the min DXT block (4x4); sub-4 mips break streaming
				RudeYtd::HalveBGRA(lvl, cw, ch);
			}
			T.Mips = mips;
		}
		else { return Fail(FString::Printf(TEXT("unknown format '%s' (AUTO|DXT1|DXT5|ATI2|RAW)"), *FmtSel)); }
		Texs.Add(MoveTemp(T));
	}

	// hash-sorted dictionary order (RAGE stores entries sorted by name hash)
	Texs.Sort([](const FTexIn& A, const FTexIn& B) { return A.Hash < B.Hash; });
	const int32 N = Texs.Num();

	// ---- graphics segment: pixel data, tightly packed. Each texture aligns to TA (8KB,
	// as real ytds do - NOT 4MB per texture, which oversized us). The TOTAL pads to a 4MB
	// page so the segment tiles into uniform 4MB pages -> segment-size flags always valid. ----
	const uint32 TA = 0x2000;         // per-texture alignment (real-ytd convention)
	const uint32 GP = 0x400000;       // total-segment page (keeps FlagsFromSize valid)
	TArray<uint8> Gfx;
	TArray<uint32> GfxOff; GfxOff.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		if (Gfx.Num() % TA) { Gfx.AddZeroed(TA - (Gfx.Num() % TA)); }
		GfxOff[i] = (uint32)Gfx.Num();
		Gfx.Append(Texs[i].Data);
	}
	if (Gfx.Num() % GP) { Gfx.AddZeroed(GP - (Gfx.Num() % GP)); }

	// ---- system segment: pgDictionary<grcTexture> at CW's observed offsets ----
	const int32 TEX_BASE = 0x450, TEX_SZ = 0x90;
	// NAME_SLOT was a fixed 0x20. RAGE texture names routinely exceed 31 chars (~10% of the Cayo
	// set), and the write loop below bounds only against the segment end - so a >=32-char name lost
	// its NUL and bled into the next slot, and the LAST name spilled into the pointer array's
	// never-written high dwords (chars 36+ landed in pointer[0]'s high dword => an invalid tagged
	// 64-bit pointer). The layout is fixup-driven, so the stride does not have to be 0x20: widen it
	// UNIFORMLY to fit the longest name + NUL, 16-byte aligned. Short-name dictionaries (the
	// in-game-proven rude_rockwall_tex, 17 chars) keep the 0x20 stride and stay byte-identical.
	int32 LongestName = 0;
	for (const auto& T : Texs) { LongestName = FMath::Max(LongestName, T.Name.Len()); }
	const int32 NAME_SLOT = FMath::Max(0x20, ((LongestName + 1 + 15) / 16) * 16);
	TArray<int32> TexOff, NameOff;
	for (int32 i = 0; i < N; ++i) { TexOff.Add(TEX_BASE + i * TEX_SZ); }
	const int32 NamesStart = TEX_BASE + N * TEX_SZ;
	for (int32 i = 0; i < N; ++i) { NameOff.Add(NamesStart + i * NAME_SLOT); }
	const int32 PtrArr = NamesStart + N * NAME_SLOT;
	const int32 HashArr = PtrArr + N * 8;
	const int32 SysEnd = HashArr + N * 4;
	uint32 SysSize = 0x2000;
	while (SysSize < (uint32)SysEnd) { SysSize <<= 1; }

	TArray<uint8> Sys; Sys.AddZeroed(SysSize);
	auto Sptr = [](int32 Off) { return 0x50000000u | (uint32)Off; };
	auto Gptr = [](uint32 Off) { return 0x60000000u | Off; };

	// pgDictionary header
	RudeYtd::PutU32(Sys, 0x00, 0); RudeYtd::PutU32(Sys, 0x04, 1);              // VFT const 0x100000000
	RudeYtd::PutU32(Sys, 0x08, Sptr(0x40));                                    // BlockMap*
	RudeYtd::PutU32(Sys, 0x18, 1);                                            // RefCount
	RudeYtd::PutU32(Sys, 0x20, Sptr(HashArr)); RudeYtd::PutU32(Sys, 0x28, (uint32)((N << 16) | N));
	RudeYtd::PutU32(Sys, 0x30, Sptr(PtrArr));  RudeYtd::PutU32(Sys, 0x38, (uint32)((N << 16) | N));

	// grcTexture structs
	for (int32 i = 0; i < N; ++i)
	{
		const int32 b = TexOff[i];
		const FTexIn& T = Texs[i];
		RudeYtd::PutU32(Sys, b + 0x00, 0); RudeYtd::PutU32(Sys, b + 0x04, 1);  // VFT
		RudeYtd::PutU32(Sys, b + 0x28, Sptr(NameOff[i]));                      // name*
		RudeYtd::PutU32(Sys, b + 0x30, 1);
		// FIXED 2026-08-03 - this wrote the BARE usage code (0x14 / 0x16), zeroing the two other
		// sub-fields the word carries. +0x40 packs: bits 0..4 usage, bits 8..27 allocated pixel
		// bytes / 256, bits 28..31 a type/class nibble. Measured on 278 embedded grcTexture
		// records from 400 real base-game .ydr: the high nibble is 2 in 278/278, and the allocated
		// size is >= the texture's own mip-chain byte count in 278/278, never 0. RUDE emitted
		// high nibble 0 and allocated 0 - i.e. "this texture allocates no memory" - on every
		// texture it has ever written.
		// COST: not proven to break in-game (these dictionaries load), but it is the texture's own
		// memory-accounting record, so any downstream reader - a future ImportYtd, a third-party
		// tool, the streamer's budget - reads zero, and it makes RUDE-authored .ytd trivially
		// distinguishable from real ones, against the clean-room "indistinguishable output" goal.
		// HONEST LIMIT: R*'s exact allocation rounding is NOT reproduced. Real words carry more
		// than the payload (e.g. 128x128/6mip: allocated 23,040 vs 10,920 of pixels; 256x256/7mip:
		// 45,056 vs 43,688) and no single rounding explains both. This writes the true payload
		// size rounded up to 256, which satisfies the one invariant that held in 278/278
		// (allocated >= mip-chain bytes) and is strictly closer to real data than 0.
		const uint32 Alloc256 = (uint32)(((int64)T.Data.Num() + 255) / 256) & 0xFFFFFu;
		RudeYtd::PutU32(Sys, b + 0x40, (2u << 28) | (Alloc256 << 8) | (T.U40 & 0x1Fu));
		RudeYtd::PutU16(Sys, b + 0x50, (uint16)T.W); RudeYtd::PutU16(Sys, b + 0x52, (uint16)T.H);
		RudeYtd::PutU16(Sys, b + 0x54, 1); RudeYtd::PutU16(Sys, b + 0x56, (uint16)T.Stride);   // depth, stride
		RudeYtd::PutU32(Sys, b + 0x58, T.Fmt);                                 // D3DFMT enum (21=A8R8G8B8) or FourCC
		Sys[b + 0x5d] = (uint8)T.Mips;                                         // mip level count
		RudeYtd::PutU32(Sys, b + 0x70, Gptr(GfxOff[i]));                       // pixel data*
	}
	// names (ASCII, null-terminated)
	for (int32 i = 0; i < N; ++i)
	{
		const FString& Nm = Texs[i].Name;
		// clamp to the slot (leaving the NUL) as well as the segment - NAME_SLOT is sized to fit
		// the longest name above, so this cannot truncate; it is a backstop, not the mechanism.
		for (int32 k = 0; k < Nm.Len() && k < NAME_SLOT - 1 && (NameOff[i] + k) < (int32)SysSize; ++k)
		{
			Sys[NameOff[i] + k] = (uint8)Nm[k];
		}
	}
	// parallel hash + pointer arrays
	for (int32 i = 0; i < N; ++i)
	{
		RudeYtd::PutU32(Sys, PtrArr + i * 8, Sptr(TexOff[i]));
		RudeYtd::PutU32(Sys, HashArr + i * 4, Texs[i].Hash);
	}

	// flags + blockmap page counts
	int32 SysPages = 0, GfxPages = 0;
	const uint32 SysFlag = RudeYtd::FlagsFromSize(SysSize, SysPages);
	const uint32 GfxFlag = 0xd0000000u | RudeYtd::FlagsFromSize((uint32)Gfx.Num(), GfxPages);
	RudeYtd::PutU32(Sys, 0x48, (uint32)(((GfxPages & 0xFF) << 8) | (SysPages & 0xFF)));

	// ---- raw DEFLATE of [sys | gfx] (RSC7 uses headerless deflate) ----
	TArray<uint8> Payload;
	Payload.Append(Sys);
	Payload.Append(Gfx);
	int32 ZSize = FCompression::CompressMemoryBound(NAME_Zlib, Payload.Num());
	TArray<uint8> Z; Z.SetNumUninitialized(ZSize);
	if (!FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZSize, Payload.GetData(), Payload.Num()))
	{
		return Fail(TEXT("zlib compress failed"));
	}
	if (ZSize < 7 || Z[0] != 0x78) { return Fail(TEXT("unexpected zlib stream (need standard 2-byte header)")); }
	const int32 RawStart = 2, RawLen = ZSize - 6;   // strip 2-byte zlib header + 4-byte adler32

	// ---- assemble RSC7 file ----
	TArray<uint8> Out;
	auto AddU32 = [&](uint32 V) { Out.Add(V & 0xFF); Out.Add((V >> 8) & 0xFF); Out.Add((V >> 16) & 0xFF); Out.Add((V >> 24) & 0xFF); };
	Out.Add('R'); Out.Add('S'); Out.Add('C'); Out.Add('7');
	AddU32(13); AddU32(SysFlag); AddU32(GfxFlag);
	Out.Append(Z.GetData() + RawStart, RawLen);

	if (!FFileHelper::SaveArrayToFile(Out, *OutYtdPath)) { return Fail(TEXT("write .ytd failed")); }
	return FString::Printf(
		TEXT("{\"ok\":true,\"ytdPath\":\"%s\",\"textures\":%d,\"bytes\":%d,\"sysFlags\":\"0x%08x\",\"gfxFlags\":\"0x%08x\"}"),
		*OutYtdPath, N, Out.Num(), SysFlag, GfxFlag);
#else
	return Fail(TEXT("editor-only"));
#endif
}

// ======================= ExportYbnBinary - clean-room .ybn (RSC7 v43) =======================
// UStaticMesh -> phBoundComposite[ phBoundGeometryBVH ], binary, no CodeWalker. P5 step 2.
// Format reversed from our own CW diff pair; every struct is CONSTRUCTED from pinned field
// offsets (docs/ENGINEERING_LOG "ybn binary format") - no template bytes, so it generalizes.
// Validated offline against the rock (2097v/4073t) + a synthetic cube: vertices round-trip
// within quantum, polys in range, BVH covers every poly exactly once.
namespace RudeYbn
{
	static const float UNK_F1 = 7.62962742e-08f;   // child+0x9c (constant in our emitter)
	static const float UNK_F2 = 0.0025f;           // child+0xac
	static const float CHILD_MARGIN = 0.005f;      // child+0x2c ; composite margin = 0
	static const uint32 CHILD_FLAGS1 = 0x3e;       // composite ChildrenFlags1 (single child)
	static const uint32 CHILD_FLAGS2 = 0x3e;       // composite ChildrenFlags2
	// Second u32 of each 16-byte ChildrenFlags entry, copied from CW's known-good binary
	// byte-for-byte (looks like don't-care/uninitialized in CW's writer; kept for parity).
	static const uint32 CHILD_FLAGS_PAD = 0x07f3bec0;
	static const int32 POLYS_PER_LEAF = 4;
	// phOptimizedBvh m_Trees: maximal subtrees of <= this many nodes. Pinned from the real
	// Legacy corpus (41 ybns, Desktop/fxserver): max observed tree span = 127 across every
	// file; spine (uncovered) nodes = treeCount-1 in EVERY file, i.e. trees are the maximal
	// <=127-node subtrees of one flat stackless BVH. Subtree node counts are always odd.
	static const int32 MAX_NODES_PER_TREE = 127;

	static void PU32(TArray<uint8>& B, int32 O, uint32 V)
	{ B[O] = V & 0xFF; B[O+1] = (V>>8) & 0xFF; B[O+2] = (V>>16) & 0xFF; B[O+3] = (V>>24) & 0xFF; }
	static void PU16(TArray<uint8>& B, int32 O, uint16 V) { B[O] = V & 0xFF; B[O+1] = (V>>8) & 0xFF; }
	static void PS16(TArray<uint8>& B, int32 O, int16 V) { PU16(B, O, (uint16)V); }
	static void PF32(TArray<uint8>& B, int32 O, float V)
	{ uint32 U; FMemory::Memcpy(&U, &V, 4); PU32(B, O, U); }
	// 8-byte tagged fixup into the system segment (0x50000000 | offset); high 4 bytes zero.
	static void PPTR(TArray<uint8>& B, int32 O, int32 Target)
	{ PU32(B, O, 0x50000000u | (uint32)Target); PU32(B, O + 4, 0); }
	static void PVEC3(TArray<uint8>& B, int32 O, const float V[3])
	{ PF32(B, O, V[0]); PF32(B, O+4, V[1]); PF32(B, O+8, V[2]); }

	struct FBvhNode
	{
		float Lo[3]; float Hi[3];
		int32 PolyStart = 0; int32 PolyCount = 0;
		bool bLeaf = false; int32 Escape = 0;
	};

	// Recursive median split over Idx[Lo,Hi). Nodes appended DFS pre-order; polygons
	// recorded in LEAF order so each leaf owns a CONTIGUOUS poly range.
	static int32 BuildBvh(const TArray<FVector3f>& Rel, const TArray<int32>& Indices,
		const TArray<FVector3f>& TriCtr, TArray<int32>& Idx, int32 Lo, int32 Hi,
		TArray<FBvhNode>& Nodes, TArray<int32>& PolyOrder)
	{
		const int32 NI = Nodes.Num();
		FBvhNode N;
		float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (int32 k = Lo; k < Hi; ++k)
		{
			const int32 T = Idx[k];
			for (int32 c = 0; c < 3; ++c)
			{
				const FVector3f& V = Rel[Indices[T * 3 + c]];
				for (int32 a = 0; a < 3; ++a)
				{ lo[a] = FMath::Min(lo[a], V[a]); hi[a] = FMath::Max(hi[a], V[a]); }
			}
		}
		for (int32 a = 0; a < 3; ++a) { N.Lo[a] = lo[a]; N.Hi[a] = hi[a]; }

		if (Hi - Lo <= POLYS_PER_LEAF)
		{
			N.bLeaf = true; N.PolyStart = PolyOrder.Num(); N.PolyCount = Hi - Lo;
			for (int32 k = Lo; k < Hi; ++k) { PolyOrder.Add(Idx[k]); }
			Nodes.Add(N);
			return NI;
		}
		N.bLeaf = false; Nodes.Add(N);

		int32 Axis = 0; float Best = hi[0] - lo[0];
		for (int32 a = 1; a < 3; ++a) { const float E = hi[a] - lo[a]; if (E > Best) { Best = E; Axis = a; } }
		{
			TArray<int32> Tmp; Tmp.Append(Idx.GetData() + Lo, Hi - Lo);
			Tmp.Sort([&TriCtr, Axis](const int32& A, const int32& B) { return TriCtr[A][Axis] < TriCtr[B][Axis]; });
			FMemory::Memcpy(Idx.GetData() + Lo, Tmp.GetData(), sizeof(int32) * (Hi - Lo));
		}
		const int32 Mid = Lo + (Hi - Lo) / 2;
		BuildBvh(Rel, Indices, TriCtr, Idx, Lo, Mid, Nodes, PolyOrder);
		BuildBvh(Rel, Indices, TriCtr, Idx, Mid, Hi, Nodes, PolyOrder);
		return NI;
	}

	// Escape index = the node AFTER this node's whole subtree (stackless skip). RELATIVE
	// for internal nodes when serialized.
	static int32 SetEscape(TArray<FBvhNode>& Nodes, int32 i)
	{
		if (Nodes[i].bLeaf) { Nodes[i].Escape = i + 1; return i + 1; }
		int32 n = SetEscape(Nodes, i + 1);
		n = SetEscape(Nodes, n);
		Nodes[i].Escape = n;
		return n;
	}

	// Uniform-page flag encoding for LARGE resources: N pages of size P (pow2 >= 0x2000).
	// Page size must be >= the largest single emitted block, or the block SPANS a page
	// boundary - RAGE pages are independently relocatable and a torn blob crashes the
	// allocator (ERR_MEM_MULTIALLOC_FREE, learned in-game on the first binary ydr: the
	// rock's 120KB vertex blob straddled uniform 64KB pages; CW's oracle avoids it with
	// a 128KB first page). Encoding: P = 0x200 << (ss+k); count of class-k pages lives
	// in a bounded bit-field, so pick the smallest ss whose k-field holds N.
	static uint32 SysPageFlagsUniform(uint32 RawSize, uint32 P, uint32& OutPadded, uint32& OutPages)
	{
		const uint32 N = (RawSize + P - 1) / P;
		OutPadded = N * P; OutPages = N;
		int32 Shift = 0; { uint32 v = P; while (v > 0x200u) { v >>= 1; ++Shift; } }   // ss+k
		// class k: bit position + capacity (k8..k4 usable; k3..k0 capacity 1)
		static const int32 KBit[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };   // k0..k8
		static const uint32 KMax[9] = { 1, 1, 1, 1, 127, 63, 15, 3, 1 };
		for (int32 k = FMath::Min(Shift, 8); k >= 0; --k)
		{
			const int32 ss = Shift - k;
			if (ss > 0xF || KMax[k] < N) { continue; }
			return (uint32)ss | (N << KBit[k]);
		}
		return 0xFFFFFFFFu;   // unencodable (absurd sizes) - caller must fail
	}

	// RSC7 system-segment page flags, reverse-engineered from CW's KNOWN-GOOD ybn output.
	// RAGE caps a system page at 0x10000 (64KB) - a single 128KB page is rejected at load
	// with "Invalid fixup, address is neither virtual nor physical". So:
	//   - segment <= 64KB : one page, size rounded up to a power of two (>=0x2000), base=size/16
	//     (matches real small ybns, e.g. itzmapz 0x4000 -> 0x20020001).
	//   - segment  > 64KB : rounded up to a 64KB multiple, N equal 64KB pages, base 0x200,
	//     class k7 (matches CW: 0x20000 -> two 64KB pages -> 0x20000040).
	// Returns the low-28 flag bits (caller ORs the 0x2 segment-type nibble) and the padded size.
	static uint32 SysPageFlags(uint32 RawSize, uint32& OutSize)
	{
		if (RawSize <= 0x10000u)
		{
			uint32 S = 0x2000u; while (S < RawSize) { S <<= 1; }
			OutSize = S;
			const uint32 Base = S / 16u;              // one k4 page of size S
			int32 ss = 0; { uint32 b = Base; while (b > 0x200u) { b >>= 1; ++ss; } }
			return (uint32)ss | (1u << 17);           // s4 = 1 (one page, class k4)
		}
		const uint32 S = (RawSize + 0xFFFFu) & ~0xFFFFu;   // ceil to 64KB
		OutSize = S;
		const uint32 NPages = S / 0x10000u;
		// 64KB page = (0x200<<ss) * 2^k ; pick the smallest ss whose count field holds NPages
		//   ss=0->k7(bit5,max3)  ss=1->k6(bit7,max15)  ss=2->k5(bit11,max63)  ss=3->k4(bit17,max127)
		const int32 SsT[4] = {0, 1, 2, 3};
		const int32 BitT[4] = {5, 7, 11, 17};
		const uint32 MaxT[4] = {3, 15, 63, 127};
		for (int32 t = 0; t < 4; ++t)
		{
			if (NPages <= MaxT[t]) { return (uint32)SsT[t] | (NPages << BitT[t]); }
		}
		// >8MB collision (unlikely): fall back to the 4MB-cap tiler (best effort)
		int32 dummy = 0;
		return RudeYtd::FlagsFromSize(S, dummy);
	}
}

// ======================= ExportYdrBinary - clean-room .ydr (RSC7 v165) =======================
// The LAST CodeWalker dependency. Every struct pinned against our own CW oracle
// (rude_rockwall.ydr) + its XML ground truth - docs/ENGINEERING_LOG "ydr binary format",
// "COMPLETE STRUCT MAP". Bound serialization: same structures as ExportYbnBinary (the
// phBound code below is intentionally duplicated from the in-game-proven ybn writer with
// only the root-at-zero difference; shared-helper refactor is queued with a byte-identity
// regression gate - do NOT let the two drift).
FString URudeToolset::ExportYdrBinaryBatch(const FString& AssetFolder, const FString& OutDir,
                                           const FString& Filter)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	// \u2b50 THE CONTINUITY PRINCIPLE, made real (Matt, BENCHMARK_ADDON_CITY \u00a75): "everything we're
	// working on should facilitate something at this scale down to importing a simple trash can".
	// Export was per-asset only, so a district could be IMPORTED in one call and then had to be
	// exported one mesh at a time - the difference between one prop and a district was a different
	// workflow, not a batch size. This closes that: same code path, same conventions, N assets.
	//
	// AssetFolder: a content folder ("/Game/RUDE/World/Meshes") walked recursively, OR a text file
	// of content paths, one per line - whichever the caller already has.
	// Filter: optional case-insensitive substring the asset NAME must contain (e.g. "dt1_").
	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();

	TArray<FString> AssetPaths;
	if (FPaths::FileExists(AssetFolder))
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *AssetFolder);
		for (const FString& L : Lines)
		{
			const FString T = L.TrimStartAndEnd();
			if (!T.IsEmpty()) { AssetPaths.Add(T); }
		}
	}
	else
	{
		// \u26d4 Same registry law as the texture library: GetAssets answers from what has been
		// scanned SO FAR and does NOT block, so a batch driven at editor startup silently exports
		// nothing. Scan and wait first (2026-07-29 defect).
		AR.ScanPathsSynchronous({ AssetFolder }, /*bForceRescan*/ false);
		if (AR.IsLoadingAssets()) { AR.WaitForCompletion(); }
		FARFilter F;
		F.PackagePaths.Add(FName(*AssetFolder));
		F.bRecursivePaths = true;
		F.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
		TArray<FAssetData> Found;
		AR.GetAssets(F, Found);
		for (const FAssetData& AD : Found)
		{
			AssetPaths.Add(AD.GetSoftObjectPath().ToString());
		}
	}
	const FString Needle = Filter.TrimStartAndEnd();
	if (!Needle.IsEmpty())
	{
		AssetPaths.RemoveAll([&Needle](const FString& P)
		{
			return !FPaths::GetBaseFilename(P).Contains(Needle, ESearchCase::IgnoreCase);
		});
	}
	if (AssetPaths.Num() == 0)
	{
		return Fail(TEXT("no StaticMesh assets matched - check the folder path and filter"));
	}
	IFileManager::Get().MakeDirectory(*OutDir, true);

	int32 Exported = 0, Failed = 0;
	// #40: the batch must carry the unit's declared collision gap, or a 400-asset export reads as
	// complete while every one of them shipped collision re-derived from its render mesh. A skip
	// with no counter is indistinguishable from "nothing to do" (standing trap 4).
	int32 CollisionFromRenderMesh = 0, BoundsIgnored = 0;
	int64 Bytes = 0;
	FString FailedList;
	for (int32 i = 0; i < AssetPaths.Num(); ++i)
	{
		const FString& A = AssetPaths[i];
		const FString Name = FPaths::GetBaseFilename(A);
		const FString Out = OutDir / (Name + TEXT(".ydr"));
		const FString R = ExportYdrBinary(A, Out, TEXT(""));
		CollisionFromRenderMesh += RudeSumField(R, TEXT("collisionFromRenderMesh"));
		BoundsIgnored           += RudeSumField(R, TEXT("boundsIgnored"));
		if (R.Contains(TEXT("\"ok\":true")))
		{
			++Exported;
			Bytes += IFileManager::Get().FileSize(*Out);
		}
		else
		{
			++Failed;
			if (Failed <= 30)
			{
				FailedList += FString::Printf(TEXT("%s\"%s\""),
					FailedList.IsEmpty() ? TEXT("") : TEXT(","), *Name);
			}
		}
		if ((i + 1) % 50 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ExportYdrBinaryBatch %d/%d (ok %d, fail %d)"),
				i + 1, AssetPaths.Num(), Exported, Failed);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ExportYdrBinaryBatch DONE: %d exported, %d failed, %.1f MB | collision "
		     "re-derived from the render mesh on %d of them, %d authored UE collision primitives "
		     "ignored"),
		Exported, Failed, Bytes / 1048576.0, CollisionFromRenderMesh, BoundsIgnored);
	// ⛔ Computed, not hardcoded - same class as ImportYdrBatch (fixed 2026-08-05). A batch that
	// exported nothing and failed everything must not report success to a headless caller.
	// collisionFromRenderMesh / boundsIgnored deliberately do NOT gate ok - they are the DECLARED
	// GAP of #40 (there is no collision importer, so this is the writer's honest capability), and
	// a gate that fires on every run is a gate nobody reads. They are here so no run can read as
	// complete while substituting.
	const bool bOk = (Failed == 0) && (Exported > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"considered\":%d,\"exported\":%d,\"failed\":%d,\"bytes\":%lld,")
		TEXT("\"collisionFromRenderMesh\":%d,\"boundsIgnored\":%d,")
		TEXT("\"outDir\":\"%s\",\"failedAssets\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"),
		AssetPaths.Num(), Exported, Failed, Bytes, CollisionFromRenderMesh, BoundsIgnored,
		*OutDir, *FailedList);
}

FString URudeToolset::ExportYdrBinary(const FString& AssetPath, const FString& OutYdrPath, const FString& Options)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	// Options: "NOBOUND" = no embedded collision. A LOD drawable carries none in the game's own data
	// (its archetype has no physics; collision belongs to the HD), so MakeLodArchetype's output is
	// exported this way. Default keeps the proven whole-mesh bound.
	const bool bBound = !Options.ToUpper().Contains(TEXT("NOBOUND"));
#if WITH_EDITORONLY_DATA
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh) { return Fail(TEXT("StaticMesh not found")); }
	const FMeshDescription* MeshDesc = Mesh->GetMeshDescription(0);
	if (!MeshDesc) { return Fail(TEXT("no MeshDescription on LOD0")); }
	FStaticMeshConstAttributes Attributes(*MeshDesc);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesConstRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesConstRef<FName> GroupSlots = Attributes.GetPolygonGroupMaterialSlotNames();
	// Drawable name = the OUTPUT FILE's basename (corpus convention: the stream name IS
	// the drawable identity; also keeps the emitted bytes final - no post-hoc patching).
	FString MeshName = FPaths::GetBaseFilename(OutYdrPath);
	MeshName.RemoveFromEnd(TEXT(".ydr"));
	MeshName.ToLowerInline();

	// --- gather per polygon group (same rules as the XML lane: weld by (vid,normal,uv),
	// inverse RUDE transform, preset + texture names from the slot's RUDE MI) ---
	struct FGeo
	{
		FString Preset = TEXT("default");
		FString Diffuse, Normal;
		TArray<FVector3f> Pos; TArray<FVector3f> Nrm; TArray<FVector2f> UV;
		TArray<int32> Indices;
	};
	TArray<FGeo> Geos;
	for (const FPolygonGroupID GroupID : MeshDesc->PolygonGroups().GetElementIDs())
	{
		FGeo G;
		FString SlotName = GroupSlots[GroupID].ToString();
		const int32 Sep = SlotName.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const bool bRudeSlot = (Sep != INDEX_NONE);
		G.Preset = bRudeSlot ? SlotName.Left(Sep) : TEXT("default");
		int32 SlotIdx = INDEX_NONE;
		for (int32 i = 0; i < Mesh->GetStaticMaterials().Num(); ++i)
		{
			if (Mesh->GetStaticMaterials()[i].MaterialSlotName == GroupSlots[GroupID]) { SlotIdx = i; break; }
		}
		if (Mesh->GetStaticMaterials().IsValidIndex(SlotIdx))
		{
			if (const UMaterialInstanceConstant* MIC =
				Cast<UMaterialInstanceConstant>(Mesh->GetStaticMaterials()[SlotIdx].MaterialInterface))
			{
				UTexture* T = nullptr;
				if (MIC->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Diffuse")), T) && T) { G.Diffuse = T->GetName(); }
				T = nullptr;
				if (MIC->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Normal")), T) && T) { G.Normal = T->GetName(); }
			}
		}
		if (!bRudeSlot)
		{
			if (!G.Normal.IsEmpty())       { G.Preset = TEXT("normal_spec"); }
			else if (!G.Diffuse.IsEmpty()) { G.Preset = TEXT("spec"); }
		}
		TMap<FString, int32> Weld;
		for (const FPolygonID PolyID : MeshDesc->GetPolygonGroupPolygonIDs(GroupID))
		{
			for (const FTriangleID TriID : MeshDesc->GetPolygonTriangles(PolyID))
			{
				for (const FVertexInstanceID Inst : MeshDesc->GetTriangleVertexInstances(TriID))
				{
					const FVertexID VID = MeshDesc->GetVertexInstanceVertex(Inst);
					const FVector3f P = Positions[VID];
					const FVector3f N = InstNormals[Inst];
					const FVector2f UV = InstUVs.Get(Inst, 0);
					const FString Key = FString::Printf(TEXT("%d|%.3f,%.3f,%.3f|%.4f,%.4f"),
						VID.GetValue(), N.X, N.Y, N.Z, UV.X, UV.Y);
					int32 Index;
					if (const int32* Found = Weld.Find(Key)) { Index = *Found; }
					else
					{
						Index = G.Pos.Num();
						G.Pos.Add(FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f));
						G.Nrm.Add(FVector3f(N.X, -N.Y, N.Z));
						G.UV.Add(UV);
						Weld.Add(Key, Index);
					}
					G.Indices.Add(Index);
				}
			}
		}
		if (G.Pos.Num() > 0 && G.Indices.Num() >= 3)
		{
			if (G.Pos.Num() > 65535) { return Fail(TEXT("geometry exceeds 65535 verts (u16 indices) - split the mesh")); }
			Geos.Add(MoveTemp(G));
		}
	}
	if (Geos.Num() == 0) { return Fail(TEXT("no polygon groups with geometry")); }

	FVector3f BMin(FLT_MAX), BMax(-FLT_MAX);
	for (const FGeo& G : Geos) { for (const FVector3f& P : G.Pos) { BMin = BMin.ComponentMin(P); BMax = BMax.ComponentMax(P); } }
	const FVector3f Center = (BMin + BMax) * 0.5f;
	const float Radius = (BMax - Center).Size();

	// --- collision soup (welded across all geometries) computed EARLY so the page plan
	// can account for every block size before anything is emitted ---
	// ⛔⛔ THE BINARY LANE RE-DERIVES COLLISION FROM THE RENDER MESH, ALWAYS - AND IT IS WORSE THAN
	// THE XML LANE (measured by reading it, 2026-08-05, open item #40). The XML lane at least mirrors
	// UE's AggGeom when there is one and only falls back on NumChildren == 0; the embedded
	// phBoundComposite written below is built unconditionally from CV/CI, this render-mesh soup.
	// It never reads UBodySetup at all. So a user who hand-authored box/sphere/capsule/convex
	// collision in UE and exported a binary .ydr had that collision SILENTLY DISCARDED, with
	// ok:true and no counter - the register described this as "NumChildren == 0 falls back", which
	// is the XML lane's shape; here there is no condition to fall back FROM.
	// ⇒ Counted, not gated: collisionFromRenderMesh is always 1 for this writer, and boundsIgnored
	// says how many real UE collision primitives were thrown away. boundsIgnored > 0 means the
	// export is lossy against the USER's own authoring, not just against Rockstar's - the number
	// exists so that can never again be invisible. Honouring AggGeom here is expansion (#40).
	int32 BoundsIgnored = 0;
	if (const UBodySetup* BS = Mesh->GetBodySetup())
	{
		const FKAggregateGeom& Agg = BS->AggGeom;
		BoundsIgnored = Agg.BoxElems.Num() + Agg.SphereElems.Num() + Agg.SphylElems.Num()
		              + Agg.ConvexElems.Num();
	}
	TArray<FVector3f> CV; TArray<int32> CI;
	{
		TMap<FString, int32> W2;
		for (const FGeo& G : Geos)
		{
			for (int32 i = 0; i < G.Indices.Num(); ++i)
			{
				const FVector3f P = G.Pos[G.Indices[i]];
				const FString K = FString::Printf(TEXT("%.4f,%.4f,%.4f"), P.X, P.Y, P.Z);
				int32 Idx;
				if (const int32* F = W2.Find(K)) { Idx = *F; }
				else { Idx = CV.Num(); CV.Add(P); W2.Add(K, Idx); }
				CI.Add(Idx);
			}
		}
	}
	// FIXED 2026-08-03 - this guard was 2x too permissive (it read > 65535). The bound polygon
	// record's vertex index is 15 bits: bit 15 is the per-edge flag, so 32,768..65,535 verts
	// passed the guard and emitted indices with bit 15 set, which the loader reads as
	// (index & 0x7FFF) - a completely different vertex - plus a spurious edge flag. Measured over
	// 60 real base-game .ybn (768,438 triangle vertex refs): bit 15 is SET in 12.48% of refs while
	// the largest bound in the sample declares 13,023 verts and the largest index used is 11,706,
	// so a set bit 15 cannot be index data. quarry's oracle-validated reader agrees independently
	// (ydr2xml.py:975 masks with 0x7FFF and takes bit 15 as the flag). Nothing tested this: it
	// passed the writer's own guards and the BVH coverage check (the indices ARE covered, just
	// wrong), so a large export - an MLO shell, a terrain tile, a merged district - would have
	// shipped silently wrong collision with ok:true.
	if (CV.Num() > 32767) { return Fail(TEXT("collision verts exceed 32767 - the bound polygon vertex index is 15 bits (bit 15 is the edge flag) - split the mesh")); }
	// u16 COUNT CEILINGS (pinned 2026-07-26). The vertex guard above is NOT sufficient: BVH leaf
	// PolyStart and the m_Trees start/end node indices are u16 in the real format, and a closed
	// mesh runs ~2 triangles per vertex - so a mesh can pass the vertex guard and still wrap the
	// polygon index, emitting a bound whose tail leaves point back at the start of the poly array
	// (silently wrong collision, no crash). Real large bounds solve this with MULTIPLE composite
	// children; this writer emits one, so refuse until it does. Nodes ~= 0.5-0.8*NP.
	if ((CI.Num() / 3) > 65535)
	{
		return Fail(TEXT("collision triangles exceed 65535 (u16 BVH poly index) - split the mesh"));
	}

	// --- page plan: page size = pow2 >= the largest single block (a block may NEVER
	// span a page boundary; RAGE pages are independently relocatable - the in-game
	// ERR_MEM_MULTIALLOC_FREE law) ---
	uint32 Largest = 0x2000;
	for (const FGeo& G : Geos)
	{
		Largest = FMath::Max(Largest, (uint32)G.Pos.Num() * 36u);
		Largest = FMath::Max(Largest, (uint32)G.Indices.Num() * 2u);
	}
	Largest = FMath::Max(Largest, (uint32)(CI.Num() / 3) * 16u);   // bound polys; node array is smaller
	Largest = FMath::Max(Largest, (uint32)CV.Num() * 6u);
	// Blocks that scale with the GEOMETRY COUNT rather than with vertex/index counts. Omitting
	// these was a hole in the no-span law: a mesh with many tiny polygon groups could emit a
	// geoBounds/ptr-array block larger than PAGE, and Emit() below only page-aligns blocks that
	// FIT in a page - an oversized one straddles the boundary (the ERR_MEM_MULTIALLOC_FREE class).
	{
		const uint32 NG = (uint32)Geos.Num();
		Largest = FMath::Max(Largest, (NG + 1) * 0x20u);   // geoBounds (N+1 pairs when N>1)
		Largest = FMath::Max(Largest, NG * 8u);            // geometry ptr array
		Largest = FMath::Max(Largest, NG * 8u);            // shader ptr array
		Largest = FMath::Max(Largest, NG * 2u);            // shader map
	}
	const int32 PAGE = (int32)FMath::RoundUpToPowerOfTwo(Largest);

	// --- segment writer: header reserved @0, page-aware Emit ---
	TArray<uint8> Seg; Seg.AddZeroed(0xd0);
	bool bPageOverflow = false;
	auto Emit = [&Seg, PAGE, &bPageOverflow](const TArray<uint8>& D, int32 Align = 16) -> int32
	{
		if (Seg.Num() % Align) { Seg.AddZeroed(Align - (Seg.Num() % Align)); }
		if (D.Num() <= PAGE && (Seg.Num() % PAGE) + D.Num() > PAGE)
		{
			Seg.AddZeroed(PAGE - (Seg.Num() % PAGE));
		}
		// Backstop: PAGE is computed from every block we know about, so this cannot fire today.
		// If a future block is added without feeding the plan above, fail LOUDLY rather than
		// emitting a torn file that crashes the client with no diagnosis.
		else if (D.Num() > PAGE) { bPageOverflow = true; }
		const int32 O = Seg.Num(); Seg.Append(D); return O;
	};
	auto EmitStr = [&](const FString& S) -> int32
	{
		TArray<uint8> B; B.SetNumZeroed(S.Len() + 1);
		for (int32 i = 0; i < S.Len(); ++i) { B[i] = (uint8)S[i]; }
		return Emit(B);
	};

	// --- vertex + index data per geometry (GTAV1: Pos 3f, Normal 3f, Colour0 4xu8, UV 2f) ---
	TArray<int32> OVData, OIData;
	for (const FGeo& G : Geos)
	{
		TArray<uint8> VB; VB.SetNumZeroed(G.Pos.Num() * 36);
		for (int32 v = 0; v < G.Pos.Num(); ++v)
		{
			const int32 o = v * 36;
			RudeYbn::PF32(VB, o + 0, G.Pos[v].X); RudeYbn::PF32(VB, o + 4, G.Pos[v].Y); RudeYbn::PF32(VB, o + 8, G.Pos[v].Z);
			RudeYbn::PF32(VB, o + 12, G.Nrm[v].X); RudeYbn::PF32(VB, o + 16, G.Nrm[v].Y); RudeYbn::PF32(VB, o + 20, G.Nrm[v].Z);
			VB[o + 24] = 255; VB[o + 25] = 255; VB[o + 26] = 255; VB[o + 27] = 255;
			RudeYbn::PF32(VB, o + 28, G.UV[v].X); RudeYbn::PF32(VB, o + 32, G.UV[v].Y);
		}
		OVData.Add(Emit(VB));
		TArray<uint8> IB; IB.SetNumZeroed(G.Indices.Num() * 2);
		for (int32 i = 0; i < G.Indices.Num(); ++i) { RudeYbn::PU16(IB, i * 2, (uint16)G.Indices[i]); }
		OIData.Add(Emit(IB));
	}

	// --- grcFvf (GTAV1): mask 0x59, stride 36, 4 channels, format nibbles ---
	// ⛔⛔ CRASH #6: this used to be emitted ONCE here and pointed at by EVERY vertex buffer.
	// RAGE FORBIDS SHARED OWNERSHIP: datResource fixup rewrites a pointer slot IN PLACE and is
	// NOT idempotent, so a block reached from N owners is fixed up N times - the 2nd pass reads
	// an ALREADY-RESOLVED 64-bit address whose high nibble is neither 5 nor 6, which is verbatim
	// "address is neither virtual nor physical". Real files never alias an fvf: 0 of 3,479
	// base-game v165 ydrs share one, and 17,370/17,370 geometries carry their OWN - dt1_02_groundb
	// pays for 7 byte-identical 16-byte fvfs in consecutive slots rather than alias. Impossible to
	// see at N==1, which is exactly why the single-geometry rock loads and every multi-material
	// export died. Now emitted per geometry, inside the loop below.
	auto MakeFvf = [&]() -> int32
	{
		TArray<uint8> Fvf; Fvf.AddZeroed(0x10);
		RudeYbn::PU32(Fvf, 0x00, 0x59); RudeYbn::PU16(Fvf, 0x04, 36); Fvf[0x07] = 4;
		RudeYbn::PU32(Fvf, 0x08, 0x55996996u); RudeYbn::PU32(Fvf, 0x0c, 0x77555555u);
		return Emit(Fvf);
	};
	const int32 OName = EmitStr(MeshName);

	// (vector parameter values are emitted INLINE inside each shader's contiguous
	// parameter allocation below - the oracle's layout, load-bearing for teardown)

	// --- per-shader: texture stubs (0x50: refcount, name*, 0x00020001), param table,
	//     param block, shader struct ---
	// ⛔⛔ CRASH #6 (the other half): these stubs used to be MEMOIZED BY NAME, so two shaders
	// referencing the same texture shared one grcTexture stub AND one ASCII name string. Same
	// non-idempotent-fixup violation as the fvf above: a doubly-owned block is fixed up twice and
	// the second pass sees an already-resolved address -> "neither virtual nor physical".
	// R* never does this in EXTERNAL-ytd mode: 0 of 2,299 external-texdict files share a stub,
	// while 789 of them hit exactly this situation and DUPLICATE instead (db_apart_02_ carries 4
	// separate stubs AND 4 separate copies of "HW_tpageDingB_RO_01"). Sharing IS legal in
	// EMBEDDED-texdict mode, but only because the pgDictionary owns and places the texture once -
	// we emit external stubs, so we must duplicate. Memoization removed deliberately; the few
	// wasted bytes are the price of single ownership.
	auto TexStub = [&](const FString& Name) -> int32
	{
		const FString L = Name.ToLower();
		const int32 NameOfs = EmitStr(L);   // fresh string per stub, also single-owner
		TArray<uint8> St; St.AddZeroed(0x50);
		RudeYbn::PU32(St, 0x04, 1);
		RudeYbn::PPTR(St, 0x28, NameOfs);
		RudeYbn::PU32(St, 0x30, 0x00020001u);
		return Emit(St);
	};
	TArray<int32> ShaderOfs;
	TSet<FString> SubstitutedPresets;   // presets we had no verified param template for (crash #5)
	for (const FGeo& G : Geos)
	{
		// One CONTIGUOUS parameter allocation per shader (the oracle's load-bearing
		// layout - the crash-#2 root cause was missing it):
		//   [N entries x16: {meta, value*}] [V vector values x16, entries point INTO
		//   this] [N x u32 joaat(paramName) - the game binds parameters BY NAME HASH]
		// The param block's +0x14 encodes it: (allocSize<<16) | hashArrayOffset
		// (= 0x01500100 for the 9-param normal_spec template, matching the oracle).
		struct FPar { uint32 Meta; int32 StubOfs; const TCHAR* Name; };   // StubOfs<0 = inline vector
		TArray<FPar> Pars;
		// ALWAYS emit both samplers, so the block is always the full 9-register normal_spec
		// layout we declare (see crash #5 below). Shortening the block for a mesh with no
		// normal map would reintroduce exactly the count mismatch that crashes the loader.
		// A missing bump falls back to the stock `flatnormal`; a missing diffuse emits an
		// unresolved stub, which renders untextured rather than failing to load.
		Pars.Add({ 0x200u, TexStub(G.Diffuse.IsEmpty() ? TEXT("none") : G.Diffuse), TEXT("DiffuseSampler") });
		Pars.Add({ 0x300u, TexStub(G.Normal.IsEmpty() ? TEXT("flatnormal") : G.Normal), TEXT("BumpSampler") });
		static const uint32 VecMeta[7] = { 0xa601, 0xa501, 0xa401, 0xa301, 0xa201, 0xa101, 0xa001 };
		static const float VecVals[7] = { 0.9f, 40.f, 0.3f, 1.f, 1.f, 0.f, 1.f };
		static const TCHAR* VecName[7] = { TEXT("specularFresnel"), TEXT("specularFalloffMult"),
			TEXT("specularIntensityMult"), TEXT("bumpiness"), TEXT("wetnessMultiplier"),
			TEXT("useTessellation"), TEXT("HardAlphaBlend") };
		for (int32 i = 0; i < 7; ++i) { Pars.Add({ VecMeta[i], -1, VecName[i] }); }
		const int32 NPar = Pars.Num(), NVec = 7;
		const int32 HashOfs = NPar * 16 + NVec * 16;
		const int32 AllocSize = (NPar == 9) ? 0x150 : ((HashOfs + NPar * 4 + 15) & ~15);
		TArray<uint8> Zero; Zero.AddZeroed(AllocSize);
		const int32 OTbl = Emit(Zero);
		{
			int32 VecIdx = 0;
			for (int32 i = 0; i < NPar; ++i)
			{
				RudeYbn::PU32(Seg, OTbl + i * 16, Pars[i].Meta);
				if (Pars[i].StubOfs >= 0)
				{
					RudeYbn::PPTR(Seg, OTbl + i * 16 + 8, Pars[i].StubOfs);
				}
				else
				{
					const int32 VOfs = OTbl + NPar * 16 + VecIdx * 16;
					RudeYbn::PF32(Seg, VOfs, VecVals[VecIdx]);
					RudeYbn::PPTR(Seg, OTbl + i * 16 + 8, VOfs);
					++VecIdx;
				}
				RudeYbn::PU32(Seg, OTbl + HashOfs + i * 4, RudeYtd::Joaat(Pars[i].Name));
			}
		}
		// ⛔⛔ IN-GAME CRASH #5 ("Invalid fixup", 2026-07-26, Matt-witnessed on the first
		// multi-material export) - ROOT CAUSE AND FIX.
		// The old behaviour declared the shader by its OWN name hash while handing it
		// normal_spec's 9-register parameter block. That is harmless in the XML lane (CW
		// rebuilds the params) and FATAL in binary: the game resolves the shader by hash,
		// then walks THAT shader's real register layout over our block. A preset wanting
		// more params than normal_spec (e.g. normal_spec_detail) reads straight past our
		// 0x150 allocation into the next struct and interprets garbage as pointers ->
		// "Invalid fixup, address is neither virtual nor physical". Proof: a real 2-geo
		// oracle's shader carries 11 params with +0x14 = 0x01800130, not 9 / 0x01500100.
		// The rock survived a year of testing only because its preset genuinely WAS
		// normal_spec. v1 has exactly ONE verified parameter template, so v1 may only ever
		// DECLARE that preset: substitute rather than lie, and report what was substituted.
		// The real fix is a per-preset register table (the P2 material lane).
		// The shader OBJECT *is* the 0x30 param block (oracle: shader ptr-array entries
		// point straight at it - there is NO intermediate struct).
		const FString RawPreset = (G.Preset == TEXT("default")) ? TEXT("normal_spec") : G.Preset;
		const bool bTemplated = RawPreset.Equals(TEXT("normal_spec"), ESearchCase::IgnoreCase);
		if (!bTemplated) { SubstitutedPresets.Add(RawPreset); }
		const FString Preset = bTemplated ? RawPreset : TEXT("normal_spec");
		TArray<uint8> Blk; Blk.AddZeroed(0x30);
		RudeYbn::PPTR(Blk, 0x00, OTbl);
		RudeYbn::PU32(Blk, 0x08, RudeYtd::Joaat(Preset));
		RudeYbn::PU32(Blk, 0x10, 0x80000000u | (uint32)NPar);
		RudeYbn::PU32(Blk, 0x14, ((uint32)AllocSize << 16) | (uint32)HashOfs);
		RudeYbn::PU32(Blk, 0x18, RudeYtd::Joaat(Preset + TEXT(".sps")));
		RudeYbn::PU32(Blk, 0x20, 0x0000ff01u);
		RudeYbn::PU32(Blk, 0x24, 0x02000000u);
		ShaderOfs.Add(Emit(Blk));
	}
	TArray<uint8> ShArr; ShArr.AddZeroed(ShaderOfs.Num() * 8);
	for (int32 i = 0; i < ShaderOfs.Num(); ++i) { RudeYbn::PPTR(ShArr, i * 8, ShaderOfs[i]); }
	const int32 OShArr = Emit(ShArr);
	TArray<uint8> SG; SG.AddZeroed(0x40);
	RudeYbn::PU32(SG, 0x00, 0x406137f0u); RudeYbn::PU32(SG, 0x04, 1);   // VFT 0x1406137f0
	RudeYbn::PPTR(SG, 0x10, OShArr);
	RudeYbn::PU16(SG, 0x18, (uint16)ShaderOfs.Num()); RudeYbn::PU16(SG, 0x1a, (uint16)ShaderOfs.Num());
	RudeYbn::PU32(SG, 0x30, 4);
	const int32 OSG = Emit(SG);

	// --- blockmap (page count patched after final size) ---
	TArray<uint8> Bm; Bm.AddZeroed(0x40);
	const int32 OBm = Emit(Bm);

	// --- per-geometry: VB struct, IB struct, geometry struct ---
	TArray<int32> OGeoStructs;
	// geoBounds: for N>1 the real format is N+1 vec4-pairs - pair[0] = the UNION AABB, then one
	// pair per geometry. For N==1 it is exactly ONE pair (no union). Pinned 2026-07-26 against 47
	// grmModels in 25 real v165 ydrs (12 multi-geo drawables recomputed from their own vertex
	// buffers: pair[1+i]==geo[i] accepted 12/12, pair[i]==geo[i] rejected 12/12). The old code
	// emitted N pairs unconditionally - correct only for the single-geometry oracle it came from.
	// See ENGINEERING_LOG "ydr binary format" CORRECTED block.
	const int32 NGeo = Geos.Num();
	const bool bGeoUnion = NGeo > 1;
	const int32 GeoBoundsPairs = bGeoUnion ? NGeo + 1 : 1;
	TArray<uint8> GeoBounds; GeoBounds.AddZeroed(GeoBoundsPairs * 0x20);
	FVector3f UnionMin(FLT_MAX), UnionMax(-FLT_MAX);
	TArray<uint8> ShaderMap; ShaderMap.AddZeroed(FMath::Max(Geos.Num() * 2, 8));
	for (int32 gi = 0; gi < Geos.Num(); ++gi)
	{
		const FGeo& G = Geos[gi];
		// Struct sizes padded to the ORACLE's observed inter-struct spacing (its gaps are
		// zero tails = null pointer slots the loader fixes up; packing the next struct's
		// live data there reads as a bogus pointer -> "Invalid fixup", crash #4).
		TArray<uint8> Vb; Vb.AddZeroed(0x80);
		RudeYbn::PU32(Vb, 0x00, 0x4061d3f8u); RudeYbn::PU32(Vb, 0x04, 1);
		RudeYbn::PU16(Vb, 0x08, 36); RudeYbn::PU16(Vb, 0x0a, 0x59);
		RudeYbn::PPTR(Vb, 0x10, OVData[gi]);
		RudeYbn::PU32(Vb, 0x18, (uint32)G.Pos.Num());
		RudeYbn::PPTR(Vb, 0x20, OVData[gi]);
		RudeYbn::PPTR(Vb, 0x30, MakeFvf());   // OWN fvf per geometry - never shared (crash #6)
		const int32 OVb = Emit(Vb);
		TArray<uint8> Ib; Ib.AddZeroed(0x60);
		RudeYbn::PU32(Ib, 0x00, 0x4061d158u); RudeYbn::PU32(Ib, 0x04, 1);
		RudeYbn::PU32(Ib, 0x08, (uint32)G.Indices.Num());
		RudeYbn::PPTR(Ib, 0x10, OIData[gi]);
		const int32 OIb = Emit(Ib);
		TArray<uint8> Ge; Ge.AddZeroed(0xa0);
		RudeYbn::PU32(Ge, 0x00, 0x40618798u); RudeYbn::PU32(Ge, 0x04, 1);
		RudeYbn::PPTR(Ge, 0x18, OVb);
		RudeYbn::PPTR(Ge, 0x38, OIb);
		RudeYbn::PU32(Ge, 0x58, (uint32)G.Indices.Num());
		RudeYbn::PU32(Ge, 0x5c, (uint32)(G.Indices.Num() / 3));
		RudeYbn::PU16(Ge, 0x60, (uint16)G.Pos.Num()); RudeYbn::PU16(Ge, 0x62, 3);
		RudeYbn::PU32(Ge, 0x70, 36);
		RudeYbn::PPTR(Ge, 0x78, OVData[gi]);
		OGeoStructs.Add(Emit(Ge));
		FVector3f GMin(FLT_MAX), GMax(-FLT_MAX);
		for (const FVector3f& P : G.Pos) { GMin = GMin.ComponentMin(P); GMax = GMax.ComponentMax(P); }
		const float Mn[3] = { GMin.X, GMin.Y, GMin.Z }, Mx[3] = { GMax.X, GMax.Y, GMax.Z };
		const int32 GbPair = (bGeoUnion ? gi + 1 : gi) * 0x20;   // pair 0 is the union when N>1
		RudeYbn::PVEC3(GeoBounds, GbPair, Mn);
		RudeYbn::PVEC3(GeoBounds, GbPair + 0x10, Mx);
		UnionMin = UnionMin.ComponentMin(GMin); UnionMax = UnionMax.ComponentMax(GMax);
		RudeYbn::PU16(ShaderMap, gi * 2, (uint16)gi);
	}
	if (bGeoUnion)
	{
		const float Un[3] = { UnionMin.X, UnionMin.Y, UnionMin.Z };
		const float Ux[3] = { UnionMax.X, UnionMax.Y, UnionMax.Z };
		RudeYbn::PVEC3(GeoBounds, 0x00, Un);
		RudeYbn::PVEC3(GeoBounds, 0x10, Ux);
	}
	const int32 OGeoBounds = Emit(GeoBounds);
	const int32 OShaderMap = Emit(ShaderMap);
	TArray<uint8> GeoArr; GeoArr.AddZeroed(OGeoStructs.Num() * 8);
	for (int32 i = 0; i < OGeoStructs.Num(); ++i) { RudeYbn::PPTR(GeoArr, i * 8, OGeoStructs[i]); }
	const int32 OGeoArr = Emit(GeoArr);
	TArray<uint8> Model; Model.AddZeroed(0x30);
	RudeYbn::PU32(Model, 0x00, 0x40610a98u); RudeYbn::PU32(Model, 0x04, 1);
	RudeYbn::PPTR(Model, 0x08, OGeoArr);
	RudeYbn::PU16(Model, 0x10, (uint16)Geos.Num()); RudeYbn::PU16(Model, 0x12, (uint16)Geos.Num());
	RudeYbn::PPTR(Model, 0x18, OGeoBounds);
	RudeYbn::PPTR(Model, 0x20, OShaderMap);
	// +0x2c = RenderMask (u8 @+0x2c, 0xff = the XML lane's RenderMask 255) | 0 @+0x2d |
	// GEOMETRY COUNT (u16 @+0x2e). Pinned 2026-07-26: +0x2e == ngeo in 47/47 real v165 grmModels.
	// Was hardcoded 0x000100ffu, which is this expression at N==1 - so the in-game-proven
	// single-geometry artifact is byte-unchanged by this fix.
	RudeYbn::PU32(Model, 0x2c, 0x000000ffu | ((uint32)Geos.Num() << 16));
	const int32 OModel = Emit(Model);
	TArray<uint8> ModelArr; ModelArr.AddZeroed(8);
	RudeYbn::PPTR(ModelArr, 0, OModel);
	const int32 OModelArr = Emit(ModelArr);
	TArray<uint8> ModelsHdr; ModelsHdr.AddZeroed(0x10);
	RudeYbn::PPTR(ModelsHdr, 0x00, OModelArr);
	RudeYbn::PU16(ModelsHdr, 0x08, 1); RudeYbn::PU16(ModelsHdr, 0x0a, 1);
	const int32 OModelsHdr = Emit(ModelsHdr);

	// ---------- embedded phBoundComposite (whole-mesh GeometryBVH), duplicated from the
	// in-game-proven ExportYbnBinary with the composite emitted in place (not @0) ----------
	int32 OComposite = 0;
	if (bBound)
	{
		const int32 NV = CV.Num(), NP = CI.Num() / 3;
		FVector3f WMin(FLT_MAX), WMax(-FLT_MAX);
		for (const FVector3f& V : CV) { WMin = WMin.ComponentMin(V); WMax = WMax.ComponentMax(V); }
		const FVector3f Ctr = (WMin + WMax) * 0.5f;
		TArray<FVector3f> Rel; Rel.Reserve(NV);
		for (const FVector3f& V : CV) { Rel.Add(V - Ctr); }
		float RMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, RMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const FVector3f& R : Rel) { for (int32 a = 0; a < 3; ++a) { RMin[a] = FMath::Min(RMin[a], R[a]); RMax[a] = FMath::Max(RMax[a], R[a]); } }
		float Quant[3], Half[3];
		for (int32 a = 0; a < 3; ++a)
		{
			Half[a] = FMath::Max(FMath::Abs(RMin[a]), FMath::Abs(RMax[a]));
			Quant[a] = (Half[a] > 0.f) ? (Half[a] / 32767.0f) : 1.0f;
		}
		const float CornerR = FMath::Sqrt(Half[0]*Half[0] + Half[1]*Half[1] + Half[2]*Half[2]);
		float VertR = 0.f;
		for (const FVector3f& R : Rel) { VertR = FMath::Max(VertR, R.Size()); }
		const float WorldMin[3] = { WMin.X, WMin.Y, WMin.Z };
		const float WorldMax[3] = { WMax.X, WMax.Y, WMax.Z };
		const float WorldCtr[3] = { Ctr.X, Ctr.Y, Ctr.Z };

		TArray<FVector3f> TriCtr; TriCtr.Reserve(NP);
		for (int32 j = 0; j < NP; ++j) { TriCtr.Add((Rel[CI[j*3]] + Rel[CI[j*3+1]] + Rel[CI[j*3+2]]) / 3.0f); }
		TArray<int32> Order; Order.Reserve(NP);
		for (int32 j = 0; j < NP; ++j) { Order.Add(j); }
		TArray<RudeYbn::FBvhNode> Nodes; TArray<int32> PolyOrder;
		RudeYbn::BuildBvh(Rel, CI, TriCtr, Order, 0, NP, Nodes, PolyOrder);
		RudeYbn::SetEscape(Nodes, 0);
		if (PolyOrder.Num() != NP) { return Fail(TEXT("BVH leaf coverage broken")); }
		TArray<TPair<int32, int32>> Trees;
		{
			TArray<int32> Stack; Stack.Add(0);
			while (Stack.Num() > 0)
			{
				const int32 i = Stack.Pop();
				const int32 Size = Nodes[i].Escape - i;
				if (Size <= RudeYbn::MAX_NODES_PER_TREE || Nodes[i].bLeaf) { Trees.Add(TPair<int32, int32>(i, Nodes[i].Escape)); }
				else { Stack.Add(Nodes[i + 1].Escape); Stack.Add(i + 1); }
			}
		}
		float NBMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, NBMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const RudeYbn::FBvhNode& N : Nodes)
		{
			for (int32 a = 0; a < 3; ++a) { NBMin[a] = FMath::Min(NBMin[a], N.Lo[a]); NBMax[a] = FMath::Max(NBMax[a], N.Hi[a]); }
		}
		float NQ[3], NInv[3];
		for (int32 a = 0; a < 3; ++a)
		{
			const float M = FMath::Max(FMath::Abs(NBMin[a]), FMath::Abs(NBMax[a]));
			NQ[a] = (M > 0.f) ? (M / 32767.0f) : 1.0f;
			NInv[a] = (NQ[a] > 0.f) ? (1.0f / NQ[a]) : 0.f;
		}
		auto QS = [](float V, float Q) -> int16
		{ return (int16)FMath::Clamp<int32>(FMath::RoundToInt(V / Q), -32768, 32767); };

		TArray<uint8> PolyB; PolyB.AddZeroed(NP * 16);
		for (int32 k = 0; k < NP; ++k)
		{
			const int32 j = PolyOrder[k];
			const FVector3f& A = Rel[CI[j*3]]; const FVector3f& B2 = Rel[CI[j*3+1]]; const FVector3f& C = Rel[CI[j*3+2]];
			const float Area = 0.5f * FVector3f::CrossProduct(B2 - A, C - A).Size();
			uint32 AreaBits; FMemory::Memcpy(&AreaBits, &Area, 4);
			AreaBits &= 0xFFFFFFF8u;
			RudeYbn::PU32(PolyB, k*16, AreaBits);
			RudeYbn::PU16(PolyB, k*16+4, (uint16)CI[j*3]);
			RudeYbn::PU16(PolyB, k*16+6, (uint16)CI[j*3+1]);
			RudeYbn::PU16(PolyB, k*16+8, (uint16)CI[j*3+2]);
			// FIXED 2026-08-03 - PolyB is AddZeroed and only bytes 0..9 were written, so the three
			// edge-neighbour slots shipped as 0 under a comment claiming "0 = none". 0 is not none,
			// it is polygon 0: measured over 60 real base-game .ybn (256,146 triangles, 768,438
			// slots) the no-neighbour sentinel is 0xFFFF (14.40%), a valid neighbour index appears
			// in 85.57%, and 0 occurs in 0.03% as a GENUINE reference to polygon 0. Leaving 0 told
			// the solver every edge of every RUDE triangle adjoins polygon 0.
			// COST: phBoundGeometryBVH uses edge adjacency for contact-normal selection and
			// internal-edge rejection, so this is wrong contact normals / catching on seams - not a
			// crash, and invisible to load-succeeds and to the BVH coverage check (both pass).
			// Real adjacency (matching triangles by shared edge) is still the proper follow-up;
			// the sentinel is what the format means by "no neighbour".
			RudeYbn::PU16(PolyB, k*16+10, 0xFFFF);
			RudeYbn::PU16(PolyB, k*16+12, 0xFFFF);
			RudeYbn::PU16(PolyB, k*16+14, 0xFFFF);
		}
		TArray<uint8> VertB; VertB.AddZeroed(NV * 6);
		for (int32 i = 0; i < NV; ++i)
		{
			for (int32 a = 0; a < 3; ++a) { RudeYbn::PS16(VertB, i*6 + a*2, QS(Rel[i][a], Quant[a])); }
		}
		TArray<uint8> MatB; MatB.AddZeroed(NP);
		TArray<uint8> NodeB; NodeB.AddZeroed(Nodes.Num() * 16);
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			const RudeYbn::FBvhNode& N = Nodes[i];
			for (int32 a = 0; a < 3; ++a)
			{
				RudeYbn::PS16(NodeB, i*16 + a*2,     QS(N.Lo[a], NQ[a]));
				RudeYbn::PS16(NodeB, i*16 + 6 + a*2, QS(N.Hi[a], NQ[a]));
			}
			RudeYbn::PU16(NodeB, i*16 + 12, (uint16)(N.bLeaf ? N.PolyStart : (N.Escape - i)));
			RudeYbn::PU16(NodeB, i*16 + 14, (uint16)(N.bLeaf ? N.PolyCount : 0));
		}
		TArray<uint8> TreeB; TreeB.AddZeroed(Trees.Num() * 16);
		for (int32 t = 0; t < Trees.Num(); ++t)
		{
			const RudeYbn::FBvhNode& RootN = Nodes[Trees[t].Key];
			for (int32 a = 0; a < 3; ++a)
			{
				RudeYbn::PS16(TreeB, t*16 + a*2,     QS(RootN.Lo[a], NQ[a]));
				RudeYbn::PS16(TreeB, t*16 + 6 + a*2, QS(RootN.Hi[a], NQ[a]));
			}
			RudeYbn::PU16(TreeB, t*16 + 12, (uint16)Trees[t].Key);
			RudeYbn::PU16(TreeB, t*16 + 14, (uint16)Trees[t].Value);
		}
		const int32 OPoly = Emit(PolyB);
		const int32 ONode = Emit(NodeB);
		const int32 OVert = Emit(VertB);
		const int32 OMidx = Emit(MatB);
		const int32 OTrees = Emit(TreeB);
		TArray<uint8> Bvh; Bvh.AddZeroed(0x80);
		RudeYbn::PPTR(Bvh, 0x00, ONode);
		RudeYbn::PU32(Bvh, 0x08, (uint32)Nodes.Num()); RudeYbn::PU32(Bvh, 0x0c, (uint32)Nodes.Num());
		{
			const float WB0[3] = { NBMin[0]+WorldCtr[0], NBMin[1]+WorldCtr[1], NBMin[2]+WorldCtr[2] };
			const float WB1[3] = { NBMax[0]+WorldCtr[0], NBMax[1]+WorldCtr[1], NBMax[2]+WorldCtr[2] };
			RudeYbn::PVEC3(Bvh, 0x20, WB0); RudeYbn::PU32(Bvh, 0x2c, 0xffc00000u);
			RudeYbn::PVEC3(Bvh, 0x30, WB1); RudeYbn::PU32(Bvh, 0x3c, 0xffc00000u);
			RudeYbn::PVEC3(Bvh, 0x40, WorldCtr); RudeYbn::PU32(Bvh, 0x4c, 0xffc00000u);
			RudeYbn::PVEC3(Bvh, 0x50, NInv); RudeYbn::PU32(Bvh, 0x5c, 0xffc00000u);
			RudeYbn::PVEC3(Bvh, 0x60, NQ);   RudeYbn::PU32(Bvh, 0x6c, 0xffc00000u);
			RudeYbn::PPTR(Bvh, 0x70, OTrees);
			RudeYbn::PU16(Bvh, 0x78, (uint16)Trees.Num()); RudeYbn::PU16(Bvh, 0x7a, (uint16)Trees.Num());
		}
		const int32 OBvh = Emit(Bvh);
		TArray<uint8> Xf; Xf.AddZeroed(0x40);
		RudeYbn::PF32(Xf, 0x00, 1.f); RudeYbn::PF32(Xf, 0x14, 1.f); RudeYbn::PU32(Xf, 0x1c, 1);
		RudeYbn::PF32(Xf, 0x28, 1.f); RudeYbn::PU32(Xf, 0x2c, 1);
		const int32 OXf = Emit(Xf);
		TArray<uint8> F0; F0.AddZeroed(0x20);
		const int32 OF0 = Emit(F0);
		TArray<uint8> CBox; CBox.AddZeroed(0x20);
		RudeYbn::PVEC3(CBox, 0x00, WorldMin); RudeYbn::PU32(CBox, 0x0c, 1);
		RudeYbn::PVEC3(CBox, 0x10, WorldMax); RudeYbn::PF32(CBox, 0x1c, RudeYbn::CHILD_MARGIN);
		const int32 OCBox = Emit(CBox);
		TArray<uint8> F1; F1.AddZeroed(16);
		RudeYbn::PU32(F1, 0, RudeYbn::CHILD_FLAGS1); RudeYbn::PU32(F1, 4, RudeYbn::CHILD_FLAGS_PAD);
		const int32 OF1 = Emit(F1);
		TArray<uint8> F2; F2.AddZeroed(16);
		RudeYbn::PU32(F2, 0, RudeYbn::CHILD_FLAGS2); RudeYbn::PU32(F2, 4, RudeYbn::CHILD_FLAGS_PAD);
		const int32 OF2 = Emit(F2);
		TArray<uint8> Ch; Ch.AddZeroed(0x150);
		RudeYbn::PF32(Ch, 0x00, VertR); RudeYbn::PU32(Ch, 0x04, 1);
		Ch[0x10] = 0x08;
		RudeYbn::PF32(Ch, 0x14, CornerR);
		RudeYbn::PVEC3(Ch, 0x20, WorldMax); RudeYbn::PF32(Ch, 0x2c, RudeYbn::CHILD_MARGIN);
		RudeYbn::PVEC3(Ch, 0x30, WorldMin); RudeYbn::PU32(Ch, 0x3c, 1);
		RudeYbn::PVEC3(Ch, 0x40, WorldCtr);
		RudeYbn::PVEC3(Ch, 0x50, WorldCtr);
		RudeYbn::PF32(Ch, 0x60, 1.f); RudeYbn::PF32(Ch, 0x64, 1.f);
		RudeYbn::PF32(Ch, 0x68, 1.f); RudeYbn::PF32(Ch, 0x6c, 1.f);
		RudeYbn::PU32(Ch, 0x84, (uint32)NV);
		RudeYbn::PPTR(Ch, 0x88, OPoly);
		RudeYbn::PVEC3(Ch, 0x90, Quant); RudeYbn::PF32(Ch, 0x9c, RudeYbn::UNK_F1);
		RudeYbn::PVEC3(Ch, 0xa0, WorldCtr); RudeYbn::PF32(Ch, 0xac, RudeYbn::UNK_F2);
		RudeYbn::PPTR(Ch, 0xb0, OVert);
		RudeYbn::PU32(Ch, 0xd0, (uint32)NV); RudeYbn::PU32(Ch, 0xd4, (uint32)NP);
		RudeYbn::PPTR(Ch, 0xf0, OF0);
		RudeYbn::PPTR(Ch, 0x118, OMidx);
		RudeYbn::PU32(Ch, 0x120, 1);
		RudeYbn::PPTR(Ch, 0x130, OBvh);
		RudeYbn::PU16(Ch, 0x140, 0xffff);
		const int32 OChild = Emit(Ch);
		TArray<uint8> CArr; CArr.AddZeroed(8);
		RudeYbn::PPTR(CArr, 0, OChild);
		const int32 OCArr = Emit(CArr);
		TArray<uint8> Comp; Comp.AddZeroed(0xb0);
		RudeYbn::PF32(Comp, 0x00, VertR); RudeYbn::PU32(Comp, 0x04, 1);
		Comp[0x10] = 0x0a;
		RudeYbn::PF32(Comp, 0x14, CornerR);
		RudeYbn::PVEC3(Comp, 0x20, WorldMax); RudeYbn::PF32(Comp, 0x2c, 0.f);
		RudeYbn::PVEC3(Comp, 0x30, WorldMin); RudeYbn::PU32(Comp, 0x3c, 1);
		RudeYbn::PVEC3(Comp, 0x40, WorldCtr);
		RudeYbn::PVEC3(Comp, 0x50, WorldCtr);
		RudeYbn::PF32(Comp, 0x60, 1.f); RudeYbn::PF32(Comp, 0x64, 1.f);
		RudeYbn::PF32(Comp, 0x68, 1.f); RudeYbn::PF32(Comp, 0x6c, 1.f);
		RudeYbn::PPTR(Comp, 0x70, OCArr);
		RudeYbn::PPTR(Comp, 0x78, OXf); RudeYbn::PPTR(Comp, 0x80, OXf);
		RudeYbn::PPTR(Comp, 0x88, OCBox);
		RudeYbn::PPTR(Comp, 0x90, OF1); RudeYbn::PPTR(Comp, 0x98, OF2);
		RudeYbn::PU16(Comp, 0xa0, 1); RudeYbn::PU16(Comp, 0xa2, 1);
		OComposite = Emit(Comp);
	}

	// --- gtaDrawable header @0 ---
	RudeYbn::PU32(Seg, 0x00, 0x40573178u); RudeYbn::PU32(Seg, 0x04, 1);   // VFT 0x140573178
	RudeYbn::PPTR(Seg, 0x08, OBm);
	RudeYbn::PPTR(Seg, 0x10, OSG);
	{
		const float C[3] = { Center.X, Center.Y, Center.Z };
		const float Mn[3] = { BMin.X, BMin.Y, BMin.Z }, Mx[3] = { BMax.X, BMax.Y, BMax.Z };
		RudeYbn::PVEC3(Seg, 0x20, C); RudeYbn::PF32(Seg, 0x2c, Radius);
		RudeYbn::PVEC3(Seg, 0x30, Mn); RudeYbn::PU32(Seg, 0x3c, 0x7f800001u);
		RudeYbn::PVEC3(Seg, 0x40, Mx); RudeYbn::PU32(Seg, 0x4c, 0x7f800001u);
	}
	RudeYbn::PPTR(Seg, 0x50, OModelsHdr);
	for (int32 k = 0; k < 4; ++k) { RudeYbn::PF32(Seg, 0x70 + k*4, 9998.f); }
	RudeYbn::PU32(Seg, 0x80, 0x0000ff01u);
	RudeYbn::PU16(Seg, 0x9a, 0x0012);
	RudeYbn::PPTR(Seg, 0xa0, OModelsHdr);
	RudeYbn::PPTR(Seg, 0xa8, OName);
	// no bound = a NULL pointer (PPTR would encode 0x50000000|0: a live pointer INTO THE HEADER)
	if (bBound) { RudeYbn::PPTR(Seg, 0xc8, OComposite); }
	else { RudeYbn::PU32(Seg, 0xc8, 0u); RudeYbn::PU32(Seg, 0xcc, 0u); }

	// --- container: RSC7 v165, sys hi-nibble 0xa, gfx 0x5 (gfx=0), uniform pages of PAGE ---
	uint32 Padded = 0, NPages = 0;
	const uint32 LowFlags = RudeYbn::SysPageFlagsUniform((uint32)Seg.Num(), (uint32)PAGE, Padded, NPages);
	if (LowFlags == 0xFFFFFFFFu) { return Fail(TEXT("unencodable page plan - resource too large")); }
	const uint32 SysFlag = 0xa0000000u | LowFlags;
	const uint32 GfxFlag = 0x50000000u;
	Seg.SetNumZeroed((int32)Padded);
	RudeYbn::PU32(Seg, OBm + 0x08, NPages);   // blockmap page count

	if (bPageOverflow)
	{
		return Fail(TEXT("internal: a block exceeded the page size and would span a page boundary ")
		            TEXT("(no-span law) - a block was added without feeding the page plan"));
	}

	// ⭐ SELF-VERIFY BEFORE WRITING. Every check here maps to an in-game crash already paid for:
	// single ownership (#6), the geoBounds N+1 / +0x2e geometry-count law (①), and vertex
	// declarations that decode to the declared stride. Wired in rather than left to an offline
	// script, because a gate nobody remembers to run is not a gate. Refuse rather than emit - a
	// corrupt resource costs an editor rebuild plus a game restart to diagnose.
	{
		const RudeYdrBin::FVerify V = RudeYdrBin::VerifyDrawable(Seg);
		if (V.SharedBlocks > 0 || V.DeclBad > 0 || V.BoundsBad > 0)
		{
			return Fail(FString::Printf(
				TEXT("SELF-CHECK FAILED, refusing to write: %d shared block(s), %d bad declaration(s), ")
				TEXT("%d bounds/count problem(s). First: %s"),
				V.SharedBlocks, V.DeclBad, V.BoundsBad, *V.FirstProblem));
		}
	}

	int32 ZSize = FCompression::CompressMemoryBound(NAME_Zlib, Seg.Num());
	TArray<uint8> Z; Z.SetNumUninitialized(ZSize);
	if (!FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZSize, Seg.GetData(), Seg.Num()))
	{
		return Fail(TEXT("zlib compress failed"));
	}
	if (ZSize < 7 || Z[0] != 0x78) { return Fail(TEXT("unexpected zlib stream")); }
	TArray<uint8> Out;
	auto AddU32 = [&](uint32 V) { Out.Add(V & 0xFF); Out.Add((V>>8) & 0xFF); Out.Add((V>>16) & 0xFF); Out.Add((V>>24) & 0xFF); };
	Out.Add('R'); Out.Add('S'); Out.Add('C'); Out.Add('7');
	AddU32(165); AddU32(SysFlag); AddU32(GfxFlag);
	Out.Append(Z.GetData() + 2, ZSize - 6);
	if (!FFileHelper::SaveArrayToFile(Out, *OutYdrPath)) { return Fail(TEXT("write .ydr failed")); }

	int32 TotalVerts = 0, TotalTris = 0;
	for (const FGeo& G : Geos) { TotalVerts += G.Pos.Num(); TotalTris += G.Indices.Num() / 3; }
	FString SubList;
	for (const FString& S : SubstitutedPresets)
	{
		SubList += (SubList.IsEmpty() ? TEXT("\"") : TEXT(",\"")) + S + TEXT("\"");
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"ydrPath\":\"%s\",\"geometries\":%d,\"vertices\":%d,\"triangles\":%d,\"bytes\":%d,")
		TEXT("\"segSize\":%d,\"sysFlags\":\"0x%08x\",\"selfCheck\":\"passed (single-ownership + ")
		TEXT("geoBounds/count + declarations)\",\"collisionFromRenderMesh\":%d,\"embeddedBound\":%s,\"boundsIgnored\":%d,")
		TEXT("\"presetsSubstitutedToNormalSpec\":[%s]}"),
		*OutYdrPath, Geos.Num(), TotalVerts, TotalTris, Out.Num(), Seg.Num(), SysFlag,
		bBound ? 1 : 0, bBound ? TEXT("true") : TEXT("false"), BoundsIgnored, *SubList);
#else
	return Fail(TEXT("editor-only"));
#endif
}

FString URudeToolset::ExportYbnBinary(const FString& AssetPath, const FString& OutYbnPath,
                                      const FString& WorldOffset)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
#if WITH_EDITORONLY_DATA
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh) { return Fail(TEXT("StaticMesh not found")); }
	const FMeshDescription* MeshDesc = Mesh->GetMeshDescription(0);
	if (!MeshDesc) { return Fail(TEXT("no MeshDescription on LOD0")); }
	FStaticMeshConstAttributes Attributes(*MeshDesc);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

	// Optional world placement: static map collision stores ABSOLUTE world coords.
	FVector3f Offset(0.f, 0.f, 0.f);
	if (!WorldOffset.TrimStartAndEnd().IsEmpty())
	{
		TArray<FString> C;
		WorldOffset.ParseIntoArray(C, TEXT(","), true);
		if (C.Num() != 3) { return Fail(TEXT("WorldOffset must be \"x,y,z\" (gta world metres)")); }
		// FIXED 2026-08-03 - the component COUNT was validated but the component VALUES were not.
		// FCString::Atof returns 0.0f for unparseable text with no error channel, so
		// "1500,-800,abc" exported successfully with Z = 0 and a plausible-looking
		// vertices/triangles/bvhNodes report. Static map collision stores ABSOLUTE world coords
		// (170 of 183 oracle .ybn have |root BoxCenter| > 100 m, 0 of 183 sit at the origin) and
		// nothing downstream re-places a .ybn, so a silently-zeroed component drops that axis to
		// the world origin - invisible until someone walks through a wall.
		// LexTryParseString is used rather than FString::IsNumeric because IsNumeric rejects
		// scientific notation (CString.h:148 allows only [+-], digits and one '.'), and a caller
		// formatting floats from Python emits "1e-05" for small offsets; rejecting that would
		// have been a new defect. Traced against UE 5.8 UnrealString.h.inl:2210 - it refuses "",
		// "abc", "-" and "." and accepts "1e-05". The IsFinite check is separate and independent:
		// Atof("inf")/"nan" parse non-zero, and a non-finite offset propagates into
		// Quantum = Half/32767 and NaNs every stored vertex.
		for (int32 i = 0; i < 3; ++i)
		{
			const FString Tok = C[i].TrimStartAndEnd();
			float Val = 0.f;
			if (!LexTryParseString(Val, *Tok))
			{
				return Fail(FString::Printf(TEXT("WorldOffset component %d ('%s') is not a number"), i, *Tok));
			}
			if (!FMath::IsFinite(Val))
			{
				return Fail(FString::Printf(TEXT("WorldOffset component %d ('%s') is not finite"), i, *Tok));
			}
			Offset[i] = Val;
		}
	}

	// --- collision soup, welded by position, inverse RUDE transform (cm->m, Y mirror) ---
	TArray<FVector3f> Verts;
	TArray<int32> Indices;
	TMap<FString, int32> Weld;
	for (const FTriangleID TriID : MeshDesc->Triangles().GetElementIDs())
	{
		for (const FVertexID VID : MeshDesc->GetTriangleVertices(TriID))
		{
			const FVector3f P = Positions[VID];
			const FVector3f G = FVector3f(P.X / 100.f, -P.Y / 100.f, P.Z / 100.f) + Offset;
			const FString Key = FString::Printf(TEXT("%.4f,%.4f,%.4f"), G.X, G.Y, G.Z);
			int32 Idx;
			if (const int32* F = Weld.Find(Key)) { Idx = *F; }
			else { Idx = Verts.Num(); Verts.Add(G); Weld.Add(Key, Idx); }
			Indices.Add(Idx);
		}
	}
	const int32 NV = Verts.Num(), NP = Indices.Num() / 3;
	if (NV == 0 || NP == 0) { return Fail(TEXT("no collision geometry")); }
	// FIXED 2026-08-03 - was > 65535, i.e. 2x too permissive. The bound polygon record's vertex
	// index is 15 bits (bit 15 = the per-edge flag), so 32,768..65,535 verts passed and emitted
	// indices whose bit 15 the loader strips: it reads (index & 0x7FFF), a different vertex, and
	// takes a spurious edge flag. Measured over 60 real base-game .ybn (768,438 triangle vertex
	// refs): bit 15 SET in 12.48%, while the largest bound declares 13,023 verts and the largest
	// index used is 11,706 - a set bit 15 provably is not index data. quarry's reader agrees
	// (ydr2xml.py:975: index & 0x7FFF, bit 15 -> f1/f2/f3). The NP > 65535 guard below stays:
	// the BVH PolyStart really is a full u16.
	if (NV > 32767) { return Fail(TEXT("vertex count exceeds 32767 - the bound polygon vertex index is 15 bits (bit 15 is the edge flag) - split the mesh")); }
	// u16 COUNT CEILING (pinned 2026-07-26) - the vertex guard above is NOT sufficient. BVH leaf
	// PolyStart (node+0x0c) and the m_Trees start/end node indices are u16 in the real format;
	// a closed mesh runs ~2 triangles per vertex, so NP wraps at ~32.8k verts - INSIDE the range
	// the vertex guard allows. The result passed every in-code check and emitted a bound whose
	// tail leaves index back into the start of the poly array: silently wrong collision, ok:true.
	// Real large bounds use MULTIPLE composite children; this writer emits exactly one.
	if (NP > 65535) { return Fail(TEXT("triangle count exceeds 65535 (u16 BVH poly index) - split the mesh")); }
	// (page size is derived from the largest block below - the uniform-P pager, same as
	// ExportYdrBinary; no struct may span a page boundary, and none can by construction)

	// --- WORLD aabb + center; vertices are stored RELATIVE to CenterGeom ---
	FVector3f WMin(FLT_MAX), WMax(-FLT_MAX);
	for (const FVector3f& V : Verts) { WMin = WMin.ComponentMin(V); WMax = WMax.ComponentMax(V); }
	const FVector3f Center = (WMin + WMax) * 0.5f;
	TArray<FVector3f> Rel; Rel.Reserve(NV);
	for (const FVector3f& V : Verts) { Rel.Add(V - Center); }

	float RMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, RMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const FVector3f& R : Rel)
	{
		for (int32 a = 0; a < 3; ++a) { RMin[a] = FMath::Min(RMin[a], R[a]); RMax[a] = FMath::Max(RMax[a], R[a]); }
	}
	float Quant[3], Half[3];
	for (int32 a = 0; a < 3; ++a)
	{
		Half[a] = FMath::Max(FMath::Abs(RMin[a]), FMath::Abs(RMax[a]));
		Quant[a] = (Half[a] > 0.f) ? (Half[a] / 32767.0f) : 1.0f;
	}
	const float CornerR = FMath::Sqrt(Half[0]*Half[0] + Half[1]*Half[1] + Half[2]*Half[2]);  // +0x14
	float VertR = 0.f;                                                                       // +0x00
	for (const FVector3f& R : Rel) { VertR = FMath::Max(VertR, R.Size()); }
	const float WorldMin[3] = { WMin.X, WMin.Y, WMin.Z };
	const float WorldMax[3] = { WMax.X, WMax.Y, WMax.Z };
	const float WorldCtr[3] = { Center.X, Center.Y, Center.Z };

	// --- BVH ---
	TArray<FVector3f> TriCtr; TriCtr.Reserve(NP);
	for (int32 j = 0; j < NP; ++j)
	{
		TriCtr.Add((Rel[Indices[j*3]] + Rel[Indices[j*3+1]] + Rel[Indices[j*3+2]]) / 3.0f);
	}
	TArray<int32> Order; Order.Reserve(NP);
	for (int32 j = 0; j < NP; ++j) { Order.Add(j); }
	TArray<RudeYbn::FBvhNode> Nodes; TArray<int32> PolyOrder;
	RudeYbn::BuildBvh(Rel, Indices, TriCtr, Order, 0, NP, Nodes, PolyOrder);
	RudeYbn::SetEscape(Nodes, 0);
	if (PolyOrder.Num() != NP) { return Fail(TEXT("BVH leaf coverage broken")); }

	// m_Trees: cut the flat stackless BVH into its maximal subtrees of <= MAX_NODES_PER_TREE
	// nodes (each entry = (startNode, endNode) absolute). MANDATORY: every real ybn has this
	// array; its absence was the final "Invalid fixup" root cause (the loader fixes up the
	// m_Trees pointer at bvhHdr+0x70, which our 0x60-byte header did not contain).
	TArray<TPair<int32, int32>> Trees;
	{
		TArray<int32> Stack; Stack.Add(0);
		while (Stack.Num() > 0)
		{
			const int32 i = Stack.Pop();
			const int32 Size = Nodes[i].Escape - i;   // Escape is absolute here: subtree = [i, Escape)
			if (Size <= RudeYbn::MAX_NODES_PER_TREE || Nodes[i].bLeaf)
			{
				Trees.Add(TPair<int32, int32>(i, Nodes[i].Escape));
			}
			else
			{
				// right child first so the stack pops left-to-right (entries stay in node order)
				Stack.Add(Nodes[i + 1].Escape);
				Stack.Add(i + 1);
			}
		}
	}

	// node quantization (relative to center, same frame as the stored vertices)
	float NBMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, NBMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const RudeYbn::FBvhNode& N : Nodes)
	{
		for (int32 a = 0; a < 3; ++a) { NBMin[a] = FMath::Min(NBMin[a], N.Lo[a]); NBMax[a] = FMath::Max(NBMax[a], N.Hi[a]); }
	}
	float NQ[3], NInv[3];
	for (int32 a = 0; a < 3; ++a)
	{
		const float M = FMath::Max(FMath::Abs(NBMin[a]), FMath::Abs(NBMax[a]));
		NQ[a] = (M > 0.f) ? (M / 32767.0f) : 1.0f;
		NInv[a] = (NQ[a] > 0.f) ? (1.0f / NQ[a]) : 0.f;
	}
	auto QuantS16 = [](float V, float Q) -> int16
	{ return (int16)FMath::Clamp<int32>(FMath::RoundToInt(V / Q), -32768, 32767); };

	// --- geometry blocks ---
	TArray<uint8> PolyBytes; PolyBytes.AddZeroed(NP * 16);
	for (int32 k = 0; k < NP; ++k)
	{
		const int32 j = PolyOrder[k];
		const FVector3f& A = Rel[Indices[j*3]];
		const FVector3f& B = Rel[Indices[j*3+1]];
		const FVector3f& C = Rel[Indices[j*3+2]];
		const float Area = 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
		uint32 AreaBits; FMemory::Memcpy(&AreaBits, &Area, 4);
		AreaBits &= 0xFFFFFFF8u;                       // low 3 bits = polygon TYPE (0 = triangle)
		RudeYbn::PU32(PolyBytes, k*16, AreaBits);
		RudeYbn::PU16(PolyBytes, k*16+4, (uint16)Indices[j*3]);
		RudeYbn::PU16(PolyBytes, k*16+6, (uint16)Indices[j*3+1]);
		RudeYbn::PU16(PolyBytes, k*16+8, (uint16)Indices[j*3+2]);
		// +10/+12/+14 = edge-neighbour indices. FIXED 2026-08-03: these shipped as 0 (PolyBytes is
		// AddZeroed and only bytes 0..9 were written) under a comment claiming "0 = none". 0 is not
		// none, it is polygon 0. Measured over 60 real base-game .ybn (256,146 triangles, 768,438
		// slots): 0xFFFF = 14.40% (the sentinel), a valid index < npolys = 85.57%, and 0 = 0.03%
		// as a genuine reference to polygon 0. Cost: every RUDE collision triangle told RAGE's
		// contact-normal / internal-edge-rejection pass that all three of its edges adjoin
		// polygon 0 - bad contact normals and seam catching, not a crash, so nothing caught it.
		// 0xFFFF = no neighbour; real adjacency is still a later refinement.
		RudeYbn::PU16(PolyBytes, k*16+10, 0xFFFF);
		RudeYbn::PU16(PolyBytes, k*16+12, 0xFFFF);
		RudeYbn::PU16(PolyBytes, k*16+14, 0xFFFF);
	}
	TArray<uint8> VertBytes; VertBytes.AddZeroed(NV * 6);
	for (int32 i = 0; i < NV; ++i)
	{
		for (int32 a = 0; a < 3; ++a) { RudeYbn::PS16(VertBytes, i*6 + a*2, QuantS16(Rel[i][a], Quant[a])); }
	}
	TArray<uint8> MatIdxBytes; MatIdxBytes.AddZeroed(NP);          // u8 material index per poly (0)
	TArray<uint8> NodeBytes; NodeBytes.AddZeroed(Nodes.Num() * 16);
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const RudeYbn::FBvhNode& N = Nodes[i];
		for (int32 a = 0; a < 3; ++a)
		{
			RudeYbn::PS16(NodeBytes, i*16 + a*2,     QuantS16(N.Lo[a], NQ[a]));
			RudeYbn::PS16(NodeBytes, i*16 + 6 + a*2, QuantS16(N.Hi[a], NQ[a]));
		}
		// leaf: (polyStart, polyCount) ; internal: (RELATIVE escape, 0)
		RudeYbn::PU16(NodeBytes, i*16 + 12, (uint16)(N.bLeaf ? N.PolyStart : (N.Escape - i)));
		RudeYbn::PU16(NodeBytes, i*16 + 14, (uint16)(N.bLeaf ? N.PolyCount : 0));
	}

	// --- system segment: composite header reserved @0, then blocks, then structs ---
	TArray<uint8> Seg; Seg.AddZeroed(0xb0);
	// Page-aware emit: NO struct/array may span a page boundary (RAGE pages are independently
	// relocatable; a straddling block = "Invalid fixup"/torn-memory crash). Uniform-P pager:
	// page size = pow2 >= the largest single block, min 64KB (preserves the proven small-mesh
	// layout byte-for-byte: for <=64KB blocks this is exactly the old 64KB pager).
	uint32 LargestBlk = 0x10000;
	LargestBlk = FMath::Max(LargestBlk, (uint32)PolyBytes.Num());
	LargestBlk = FMath::Max(LargestBlk, (uint32)NodeBytes.Num());
	LargestBlk = FMath::Max(LargestBlk, (uint32)VertBytes.Num());
	const int32 YBN_PAGE = (int32)FMath::RoundUpToPowerOfTwo(LargestBlk);
	auto Emit = [&Seg, YBN_PAGE](const TArray<uint8>& D, int32 Align = 16) -> int32
	{
		if (Seg.Num() % Align) { Seg.AddZeroed(Align - (Seg.Num() % Align)); }
		if (D.Num() <= YBN_PAGE && (Seg.Num() % YBN_PAGE) + D.Num() > YBN_PAGE)
		{
			Seg.AddZeroed(YBN_PAGE - (Seg.Num() % YBN_PAGE));   // don't straddle a page boundary
		}
		const int32 O = Seg.Num(); Seg.Append(D); return O;
	};
	const int32 OPoly = Emit(PolyBytes);
	const int32 ONode = Emit(NodeBytes);
	const int32 OVert = Emit(VertBytes);
	// NOTE: child +0xb8 (m_CompressedShrunkVertices) is left NULL - CW's known-good binary
	// leaves it null and loads fine, so it is NOT required (an earlier theory that it caused
	// the fixup crash was disproven by diffing CW's working output).
	const int32 OMidx = Emit(MatIdxBytes);

	// m_Trees array: 16 bytes/entry = the root node's quantized AABB (s16 min/max, same
	// quantum+frame as the node array) + u16 startNode + u16 endNode (absolute, exclusive).
	TArray<uint8> TreeBytes; TreeBytes.AddZeroed(Trees.Num() * 16);
	for (int32 t = 0; t < Trees.Num(); ++t)
	{
		const RudeYbn::FBvhNode& RootN = Nodes[Trees[t].Key];
		for (int32 a = 0; a < 3; ++a)
		{
			RudeYbn::PS16(TreeBytes, t*16 + a*2,     QuantS16(RootN.Lo[a], NQ[a]));
			RudeYbn::PS16(TreeBytes, t*16 + 6 + a*2, QuantS16(RootN.Hi[a], NQ[a]));
		}
		RudeYbn::PU16(TreeBytes, t*16 + 12, (uint16)Trees[t].Key);
		RudeYbn::PU16(TreeBytes, t*16 + 14, (uint16)Trees[t].Value);
	}
	const int32 OTrees = Emit(TreeBytes);

	// phOptimizedBvh header is 0x80 bytes (NOT 0x60 - the 0x60 truncation dropped the
	// m_Trees pointer slot and was the final in-game "Invalid fixup" root cause):
	// +0x60 forward quantum vec4, +0x70 m_Trees ptr, +0x78 u16 count / +0x7a u16 capacity.
	TArray<uint8> Bvh; Bvh.AddZeroed(0x80);
	RudeYbn::PPTR(Bvh, 0x00, ONode);
	RudeYbn::PU32(Bvh, 0x08, (uint32)Nodes.Num()); RudeYbn::PU32(Bvh, 0x0c, (uint32)Nodes.Num());
	{
		const float WB0[3] = { NBMin[0]+WorldCtr[0], NBMin[1]+WorldCtr[1], NBMin[2]+WorldCtr[2] };
		const float WB1[3] = { NBMax[0]+WorldCtr[0], NBMax[1]+WorldCtr[1], NBMax[2]+WorldCtr[2] };
		RudeYbn::PVEC3(Bvh, 0x20, WB0); RudeYbn::PU32(Bvh, 0x2c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x30, WB1); RudeYbn::PU32(Bvh, 0x3c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x40, WorldCtr); RudeYbn::PU32(Bvh, 0x4c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x50, NInv); RudeYbn::PU32(Bvh, 0x5c, 0xffc00000u);
		RudeYbn::PVEC3(Bvh, 0x60, NQ);   RudeYbn::PU32(Bvh, 0x6c, 0xffc00000u);
		RudeYbn::PPTR(Bvh, 0x70, OTrees);
		RudeYbn::PU16(Bvh, 0x78, (uint16)Trees.Num());
		RudeYbn::PU16(Bvh, 0x7a, (uint16)Trees.Num());
	}
	const int32 OBvh = Emit(Bvh);

	TArray<uint8> Xf; Xf.AddZeroed(0x40);                         // child transform = identity
	RudeYbn::PF32(Xf, 0x00, 1.f); RudeYbn::PF32(Xf, 0x14, 1.f); RudeYbn::PU32(Xf, 0x1c, 1);
	RudeYbn::PF32(Xf, 0x28, 1.f); RudeYbn::PU32(Xf, 0x2c, 1);
	const int32 OXf = Emit(Xf);

	TArray<uint8> F0; F0.AddZeroed(0x20);                          // child+0xf0 block (zero)
	const int32 OF0 = Emit(F0);
	TArray<uint8> BlockMap; BlockMap.AddZeroed(0x40);   // page count patched after the plan is known
	const int32 OBm = Emit(BlockMap);

	TArray<uint8> ChildBox; ChildBox.AddZeroed(0x20);              // [BoxMin.vec4, BoxMax.vec4] WORLD
	RudeYbn::PVEC3(ChildBox, 0x00, WorldMin); RudeYbn::PU32(ChildBox, 0x0c, 1);
	RudeYbn::PVEC3(ChildBox, 0x10, WorldMax); RudeYbn::PF32(ChildBox, 0x1c, RudeYbn::CHILD_MARGIN);
	const int32 OBbox = Emit(ChildBox);
	// ChildrenFlags arrays: 16 bytes PER CHILD (not 4 - CW and real ybns use a 16-byte
	// stride; second word copied from the known-good binary, trailing 8 bytes zero).
	TArray<uint8> Fl1; Fl1.AddZeroed(16);
	RudeYbn::PU32(Fl1, 0, RudeYbn::CHILD_FLAGS1); RudeYbn::PU32(Fl1, 4, RudeYbn::CHILD_FLAGS_PAD);
	const int32 OFl1 = Emit(Fl1);
	TArray<uint8> Fl2; Fl2.AddZeroed(16);
	RudeYbn::PU32(Fl2, 0, RudeYbn::CHILD_FLAGS2); RudeYbn::PU32(Fl2, 4, RudeYbn::CHILD_FLAGS_PAD);
	const int32 OFl2 = Emit(Fl2);

	// --- phBoundGeometryBVH child header (0x150: 0x140 of fields + the 0x0000ffff
	// sentinel at +0x140, present in CW's output and EVERY real corpus ybn) ---
	TArray<uint8> Ch; Ch.AddZeroed(0x150);
	RudeYbn::PF32(Ch, 0x00, VertR); RudeYbn::PU32(Ch, 0x04, 1);
	Ch[0x10] = 0x08;                                               // BoundType = GeometryBVH
	RudeYbn::PF32(Ch, 0x14, CornerR);
	RudeYbn::PVEC3(Ch, 0x20, WorldMax); RudeYbn::PF32(Ch, 0x2c, RudeYbn::CHILD_MARGIN);
	RudeYbn::PVEC3(Ch, 0x30, WorldMin); RudeYbn::PU32(Ch, 0x3c, 1);
	RudeYbn::PVEC3(Ch, 0x40, WorldCtr);
	RudeYbn::PVEC3(Ch, 0x50, WorldCtr);
	RudeYbn::PF32(Ch, 0x60, 1.f); RudeYbn::PF32(Ch, 0x64, 1.f);
	RudeYbn::PF32(Ch, 0x68, 1.f); RudeYbn::PF32(Ch, 0x6c, 1.f);    // Inertia + Volume
	RudeYbn::PU32(Ch, 0x84, (uint32)NV);
	RudeYbn::PPTR(Ch, 0x88, OPoly);
	RudeYbn::PVEC3(Ch, 0x90, Quant); RudeYbn::PF32(Ch, 0x9c, RudeYbn::UNK_F1);
	RudeYbn::PVEC3(Ch, 0xa0, WorldCtr); RudeYbn::PF32(Ch, 0xac, RudeYbn::UNK_F2);  // CenterGeom
	RudeYbn::PPTR(Ch, 0xb0, OVert);                // +0xb8 (shrunk verts) intentionally NULL, matches CW
	RudeYbn::PU32(Ch, 0xd0, (uint32)NV); RudeYbn::PU32(Ch, 0xd4, (uint32)NP);
	RudeYbn::PPTR(Ch, 0xf0, OF0);                  // materials array (one all-zero default material)
	RudeYbn::PPTR(Ch, 0x118, OMidx);
	RudeYbn::PU32(Ch, 0x120, 1);                   // material count = 1 (CW known-good; every real ybn >= 1)
	RudeYbn::PPTR(Ch, 0x130, OBvh);
	RudeYbn::PU16(Ch, 0x140, 0xffff);              // sentinel, universal in CW + real corpus
	const int32 OChild = Emit(Ch);

	TArray<uint8> CArr; CArr.AddZeroed(8); RudeYbn::PPTR(CArr, 0, OChild);
	const int32 OCArr = Emit(CArr);

	// --- phBoundComposite header @0 ---
	RudeYbn::PF32(Seg, 0x00, VertR); RudeYbn::PU32(Seg, 0x04, 1);
	RudeYbn::PPTR(Seg, 0x08, OBm);
	Seg[0x10] = 0x0a;                                              // BoundType = Composite
	RudeYbn::PF32(Seg, 0x14, CornerR);
	RudeYbn::PVEC3(Seg, 0x20, WorldMax); RudeYbn::PF32(Seg, 0x2c, 0.f);   // composite margin = 0
	RudeYbn::PVEC3(Seg, 0x30, WorldMin); RudeYbn::PU32(Seg, 0x3c, 1);
	RudeYbn::PVEC3(Seg, 0x40, WorldCtr);
	RudeYbn::PVEC3(Seg, 0x50, WorldCtr);
	RudeYbn::PF32(Seg, 0x60, 1.f); RudeYbn::PF32(Seg, 0x64, 1.f);
	RudeYbn::PF32(Seg, 0x68, 1.f); RudeYbn::PF32(Seg, 0x6c, 1.f);
	RudeYbn::PPTR(Seg, 0x70, OCArr);
	RudeYbn::PPTR(Seg, 0x78, OXf); RudeYbn::PPTR(Seg, 0x80, OXf);
	RudeYbn::PPTR(Seg, 0x88, OBbox);
	RudeYbn::PPTR(Seg, 0x90, OFl1); RudeYbn::PPTR(Seg, 0x98, OFl2);
	RudeYbn::PU16(Seg, 0xa0, 1); RudeYbn::PU16(Seg, 0xa2, 1);      // NumChildren, capacity

	// --- container: RSC7 v43 (system segment only), uniform pages of YBN_PAGE.
	// For <=64KB blocks this reproduces the in-game-proven plan exactly (rock wall:
	// 2x64KB, flags 0x20000040, byte-identity regression-gated). ---
	uint32 Padded = 0, NPages = 0;
	const uint32 LowFlags = RudeYbn::SysPageFlagsUniform((uint32)Seg.Num(), (uint32)YBN_PAGE, Padded, NPages);
	if (LowFlags == 0xFFFFFFFFu) { return Fail(TEXT("unencodable page plan - mesh too large")); }
	const uint32 SysFlag = 0x20000000u | LowFlags;
	const uint32 GfxFlag = 0xb0000000u;
	Seg.SetNumZeroed((int32)Padded);
	RudeYbn::PU32(Seg, OBm + 0x08, NPages);

	int32 ZSize = FCompression::CompressMemoryBound(NAME_Zlib, Seg.Num());
	TArray<uint8> Z; Z.SetNumUninitialized(ZSize);
	if (!FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZSize, Seg.GetData(), Seg.Num()))
	{
		return Fail(TEXT("zlib compress failed"));
	}
	if (ZSize < 7 || Z[0] != 0x78) { return Fail(TEXT("unexpected zlib stream (need standard 2-byte header)")); }

	TArray<uint8> Out;
	auto AddU32 = [&](uint32 V) { Out.Add(V & 0xFF); Out.Add((V>>8) & 0xFF); Out.Add((V>>16) & 0xFF); Out.Add((V>>24) & 0xFF); };
	Out.Add('R'); Out.Add('S'); Out.Add('C'); Out.Add('7');
	AddU32(43); AddU32(SysFlag); AddU32(GfxFlag);
	Out.Append(Z.GetData() + 2, ZSize - 6);                        // strip zlib header + adler32

	if (!FFileHelper::SaveArrayToFile(Out, *OutYbnPath)) { return Fail(TEXT("write .ybn failed")); }
	return FString::Printf(
		TEXT("{\"ok\":true,\"ybnPath\":\"%s\",\"vertices\":%d,\"triangles\":%d,\"bvhNodes\":%d,\"bytes\":%d,\"segSize\":%d,\"sysFlags\":\"0x%08x\"}"),
		*OutYbnPath, NV, NP, Nodes.Num(), Out.Num(), Seg.Num(), SysFlag);
#else
	return Fail(TEXT("editor-only"));
#endif
}

// ======================= ProbeYdrBinary - verify the binary READ path =======================
// Step 1 of the import side: prove the C++ parse of a real binary .ydr before wiring it to the
// mesh builder. Reports the whole drawable graph as JSON so it can be diffed against an
// independent parse and against ExportYdrBinary's own output (round-trip).
FString URudeToolset::ProbeYdrBinary(const FString& BinPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};

	RudeYdrBin::FRes R;
	FString Err;
	if (!RudeYdrBin::LoadFile(BinPath, R, Err)) { return Fail(Err); }
	const TArray<uint8>& S = R.Sys;

	uint32 PtrSG = 0, PtrModels = 0, PtrName = 0, PtrBound = 0;
	RudeYdrBin::U32(S, 0x10, PtrSG);
	RudeYdrBin::U32(S, 0x50, PtrModels);
	RudeYdrBin::U32(S, 0xa8, PtrName);
	RudeYdrBin::U32(S, 0xc8, PtrBound);

	// drawable name (plain ASCII, NUL-terminated)
	FString DrawName;
	{
		int32 O = 0;
		if (R.Resolve(PtrName, 1, O))
		{
			while (O < S.Num() && S[O] != 0 && DrawName.Len() < 128) { DrawName.AppendChar((TCHAR)S[O++]); }
		}
	}

	// --- shaders: hash + the texture-stub names each one references ---
	int32 NumShaders = 0;
	FString ShaderJson;
	if (PtrSG != 0)
	{
		int32 SG = 0;
		if (!R.Resolve(PtrSG, 0x40, SG)) { return Fail(TEXT("ShaderGroup pointer does not resolve")); }
		uint32 PtrArr = 0; uint16 NSh = 0;
		RudeYdrBin::U32(S, SG + 0x10, PtrArr);
		RudeYdrBin::U16(S, SG + 0x18, NSh);
		int32 Arr = 0;
		if (NSh > 0 && R.Resolve(PtrArr, (int32)NSh * 8, Arr))
		{
			NumShaders = (int32)NSh;
			for (int32 si = 0; si < NSh; ++si)
			{
				uint32 PtrBlk = 0;
				RudeYdrBin::U32(S, Arr + si * 8, PtrBlk);
				int32 Blk = 0;
				if (!R.Resolve(PtrBlk, 0x30, Blk)) { continue; }
				uint32 Hash = 0, NPar = 0, PtrTbl = 0;
				RudeYdrBin::U32(S, Blk + 0x08, Hash);
				RudeYdrBin::U32(S, Blk + 0x10, NPar);
				RudeYdrBin::U32(S, Blk + 0x00, PtrTbl);
				const int32 ParCount = (int32)(NPar & 0xFFFF);
				FString Texs;
				int32 Tbl = 0;
				if (ParCount > 0 && ParCount <= 64 && R.Resolve(PtrTbl, ParCount * 16, Tbl))
				{
					for (int32 pi = 0; pi < ParCount; ++pi)
					{
						uint32 PtrVal = 0;
						RudeYdrBin::U32(S, Tbl + pi * 16 + 8, PtrVal);
						int32 Stub = 0;
						if (!R.Resolve(PtrVal, 0x34, Stub)) { continue; }
						uint32 Marker = 0;
						RudeYdrBin::U32(S, Stub + 0x30, Marker);
						if (Marker != 0x00020001u) { continue; }   // not a grcTexture stub
						uint32 PtrTexName = 0;
						RudeYdrBin::U32(S, Stub + 0x28, PtrTexName);
						int32 NO = 0;
						if (!R.Resolve(PtrTexName, 1, NO)) { continue; }
						FString TN;
						while (NO < S.Num() && S[NO] != 0 && TN.Len() < 96) { TN.AppendChar((TCHAR)S[NO++]); }
						if (!TN.IsEmpty()) { Texs += (Texs.IsEmpty() ? TEXT("\"") : TEXT(",\"")) + TN + TEXT("\""); }
					}
				}
				ShaderJson += FString::Printf(
					TEXT("%s{\"hash\":\"0x%08x\",\"params\":%d,\"textures\":[%s]}"),
					si ? TEXT(",") : TEXT(""), Hash, ParCount, *Texs);
			}
		}
	}

	// --- models -> geometries, over ALL FOUR LOD arrays ---
	// +0x50 High, +0x58 Med, +0x60 Low, +0x68 Vlow are all real {ptrArray*, u16 count} arrays.
	// Walking only +0x50 loses 21.2% of the corpus's geometries (3,677 of 17,370). ⚠ +0xa0 is a
	// byte-identical ALIAS of +0x50 in 3,479/3,479 files - walking it too would double-count.
	static const int32 LodSlot[4] = { 0x50, 0x58, 0x60, 0x68 };
	static const TCHAR* LodName[4] = { TEXT("high"), TEXT("med"), TEXT("low"), TEXT("vlow") };

	int32 TotalGeo = 0, TotalVerts = 0, TotalTris = 0, BadIdx = 0;
	int32 DeclOk = 0, DeclBad = 0, PosInAabb = 0, PosOutAabb = 0, NanVerts = 0, NoNormal = 0;
	// ADDED 2026-08-03 - every `continue` below used to drop a model or a geometry with no counter,
	// and the probe still reported ok:true with a short geometry count. A dropped MODEL was fully
	// invisible: it emits no JSON entry at all and the declared model count was never reported.
	// COST: this probe is the instrument used to prove the binary READ path and to round-trip
	// ExportYdrBinary's output, so a silent drop lets a reader bug and a writer bug agree on a
	// wrong-but-matching count - exactly the failure the round-trip exists to catch. It also
	// under-reports coverage when pointed at real game files carrying declarations we do not
	// support. A probe that drops anything must not be reportable as a clean parse.
	int32 ModelsDeclared = 0, ModelsDropped = 0, GeosDropped = 0;
	int32 IdxUnreadable = 0, IdxCountMismatch = 0, LodHeadersDropped = 0;
	FString FirstDeclError;
	TSet<FString> Decls;
	FString ModelJson;
	bool bAnyModels = false;

	for (int32 lod = 0; lod < 4; ++lod)
	{
	uint32 PtrLod = 0;
	RudeYdrBin::U32(S, LodSlot[lod], PtrLod);
	if (PtrLod == 0) { continue; }
	int32 MH = 0;
	if (!R.Resolve(PtrLod, 0x10, MH)) { ++LodHeadersDropped; continue; }
	uint32 PtrMArr = 0; uint16 NMod = 0;
	RudeYdrBin::U32(S, MH + 0x00, PtrMArr);
	RudeYdrBin::U16(S, MH + 0x08, NMod);
	int32 MArr = 0;
	ModelsDeclared += (int32)NMod;
	// a non-empty LOD whose model array will not resolve loses every model it declared
	if (NMod == 0 || !R.Resolve(PtrMArr, (int32)NMod * 8, MArr)) { ModelsDropped += (int32)NMod; continue; }
	bAnyModels = true;

	for (int32 mi = 0; mi < NMod; ++mi)
	{
		uint32 PtrM = 0;
		RudeYdrBin::U32(S, MArr + mi * 8, PtrM);
		int32 M = 0;
		if (!R.Resolve(PtrM, 0x30, M)) { ++ModelsDropped; continue; }
		uint32 PtrGArr = 0, PtrGB = 0, Rm = 0;
		uint16 NGeo = 0, NGeo2e = 0;
		RudeYdrBin::U32(S, M + 0x08, PtrGArr);
		RudeYdrBin::U16(S, M + 0x10, NGeo);
		RudeYdrBin::U32(S, M + 0x18, PtrGB);
		RudeYdrBin::U32(S, M + 0x2c, Rm);
		RudeYdrBin::U16(S, M + 0x2e, NGeo2e);
		int32 GArr = 0;
		// this `continue` skips BEFORE ModelJson is appended, so the model vanished entirely
		if (NGeo == 0 || !R.Resolve(PtrGArr, (int32)NGeo * 8, GArr))
		{ ++ModelsDropped; GeosDropped += (int32)NGeo; continue; }

		// geoBounds is N+1 vec4-pairs when N>1 (union first), exactly 1 pair when N==1
		const int32 Pairs = (NGeo > 1) ? (NGeo + 1) : 1;
		int32 GB = 0;
		const bool bGB = R.Resolve(PtrGB, Pairs * 0x20, GB) != nullptr;

		FString GeoJson;
		for (int32 gi = 0; gi < NGeo; ++gi)
		{
			uint32 PtrG = 0;
			RudeYdrBin::U32(S, GArr + gi * 8, PtrG);
			int32 G = 0;
			if (!R.Resolve(PtrG, 0x80, G)) { ++GeosDropped; continue; }
			uint32 PtrVB = 0, PtrIB = 0, IdxCount = 0, TriCount = 0;
			uint16 VCnt = 0, Stride16 = 0;
			RudeYdrBin::U32(S, G + 0x18, PtrVB);
			RudeYdrBin::U32(S, G + 0x38, PtrIB);
			RudeYdrBin::U32(S, G + 0x58, IdxCount);
			RudeYdrBin::U32(S, G + 0x5c, TriCount);
			RudeYdrBin::U16(S, G + 0x60, VCnt);
			// ⚠ grmGeometry+0x70 is a U16, not a u32. +0x72 is non-zero on skinned geometries
			// (mask 0x405f), where a u32 read yields 983,100 instead of 60 - prop_bin_14b.ydr.
			RudeYdrBin::U16(S, G + 0x70, Stride16);
			const uint32 Stride = (uint32)Stride16;

			// vertex buffer + its declaration
			uint32 Mask = 0; uint16 FvfStride = 0; uint8 ChanCount = 0; uint64 Nibbles = 0;
			int32 VB = 0, VData = 0; bool bVGfx = false, bHaveVData = false;
			if (R.Resolve(PtrVB, 0x40, VB))
			{
				uint32 PtrVData = 0, PtrFvf = 0;
				RudeYdrBin::U32(S, VB + 0x10, PtrVData);
				RudeYdrBin::U32(S, VB + 0x30, PtrFvf);
				bVGfx = (PtrVData >> 28) == 6;
				bHaveVData = R.Resolve(PtrVData, (int32)VCnt * (int32)FMath::Max(Stride, 1u), VData) != nullptr;
				int32 Fvf = 0;
				if (R.Resolve(PtrFvf, 0x10, Fvf))
				{
					RudeYdrBin::U32(S, Fvf + 0x00, Mask);
					RudeYdrBin::U16(S, Fvf + 0x04, FvfStride);
					uint8 CC = 0; RudeYdrBin::Rd(S, Fvf + 0x07, &CC, 1); ChanCount = CC;
					RudeYdrBin::Rd(S, Fvf + 0x08, &Nibbles, 8);
				}
			}
			Decls.Add(FString::Printf(TEXT("mask=0x%x,stride=%u,chans=%u"), Mask, Stride, ChanCount));

			// --- DECODE the declaration and verify positions against this geometry's own AABB ---
			// This is the gate that catches a wrong layout: a misread position lands outside the
			// box the file itself declares. 17,370/17,370 real geometries pass, so anything less
			// than 100% here means the decode is wrong, not the data.
			RudeYdrBin::FDecl Decl;
			FString DeclErr;
			const bool bDecl = RudeYdrBin::BuildDecl(Mask, Nibbles, (int32)Stride, Decl, DeclErr);
			if (bDecl) { ++DeclOk; } else { ++DeclBad; if (FirstDeclError.IsEmpty()) { FirstDeclError = DeclErr; } }
			if (bDecl && !Decl.Has(RudeYdrBin::CH_NRM)) { ++NoNormal; }
			if (bDecl && bHaveVData && bGB)
			{
				// per-geometry AABB: pair[gi+1] when N>1 (pair[0] is the union), else pair[0]
				const int32 PairOfs = GB + ((NGeo > 1) ? (gi + 1) : 0) * 0x20;
				float Mn[3], Mx[3];
				bool bBox = true;
				for (int32 a = 0; a < 3; ++a)
				{
					bBox &= RudeYdrBin::F32(S, PairOfs + a * 4, Mn[a]);
					bBox &= RudeYdrBin::F32(S, PairOfs + 0x10 + a * 4, Mx[a]);
				}
				if (bBox)
				{
					const int32 PO = Decl.Ofs[RudeYdrBin::CH_POS];
					for (int32 v = 0; v < (int32)VCnt; ++v)
					{
						float P[3];
						bool bR = true;
						for (int32 a = 0; a < 3; ++a) { bR &= RudeYdrBin::F32(S, VData + v * (int32)Stride + PO + a * 4, P[a]); }
						if (!bR) { break; }
						if (!FMath::IsFinite(P[0]) || !FMath::IsFinite(P[1]) || !FMath::IsFinite(P[2]))
						{
							++NanVerts; continue;   // real shipped assets contain NaN verts
						}
						const float E = 0.01f;
						const bool bIn = P[0] >= Mn[0]-E && P[0] <= Mx[0]+E && P[1] >= Mn[1]-E
						              && P[1] <= Mx[1]+E && P[2] >= Mn[2]-E && P[2] <= Mx[2]+E;
						if (bIn) { ++PosInAabb; } else { ++PosOutAabb; }
					}
				}
			}

			// index buffer: confirm u16 indices stay inside the vertex count
			int32 MaxIdx = -1;
			int32 IB = 0;
			bool bIdxRead = false;
			if (R.Resolve(PtrIB, 0x20, IB))
			{
				uint32 PtrIData = 0, IBCount = 0;
				RudeYdrBin::U32(S, IB + 0x08, IBCount);
				RudeYdrBin::U32(S, IB + 0x10, PtrIData);
				int32 IData = 0;
				// grmGeometry+0x58 and IndexBuffer+0x08 are two independent declarations of the
				// same count; taking the Min silently papered over any disagreement, so count it.
				if (IdxCount != IBCount) { ++IdxCountMismatch; }
				const int32 NIdx = (int32)FMath::Min(IdxCount, IBCount);
				if (NIdx > 0 && R.Resolve(PtrIData, NIdx * 2, IData))
				{
					bIdxRead = true;
					for (int32 k = 0; k < NIdx; ++k)
					{
						uint16 V = 0; RudeYdrBin::U16(S, IData + k * 2, V);
						MaxIdx = FMath::Max(MaxIdx, (int32)V);
					}
				}
			}
			// MaxIdx stays -1 when the index buffer does not resolve, so the range check below
			// can never fire and an UNREADABLE index buffer used to count as clean.
			if (!bIdxRead) { ++IdxUnreadable; }
			if (MaxIdx >= (int32)VCnt) { ++BadIdx; }

			GeoJson += FString::Printf(
				TEXT("%s{\"geo\":%d,\"verts\":%u,\"tris\":%u,\"stride\":%u,\"fvfStride\":%u,")
				TEXT("\"mask\":\"0x%x\",\"chans\":%u,\"maxIdx\":%d,\"vertsInGfxSeg\":%s}"),
				gi ? TEXT(",") : TEXT(""), gi, VCnt, TriCount, Stride, FvfStride, Mask, ChanCount,
				MaxIdx, bVGfx ? TEXT("true") : TEXT("false"));
			TotalVerts += (int32)VCnt; TotalTris += (int32)TriCount; ++TotalGeo;
		}
		ModelJson += FString::Printf(
			TEXT("%s{\"lod\":\"%s\",\"model\":%d,\"geoCount\":%u,\"countAt0x2e\":%u,\"renderMask\":%u,")
			TEXT("\"geoBoundsPairs\":%d,\"geoBoundsResolves\":%s,\"geos\":[%s]}"),
			ModelJson.IsEmpty() ? TEXT("") : TEXT(","), LodName[lod], mi, NGeo, NGeo2e, Rm & 0xFF,
			Pairs, bGB ? TEXT("true") : TEXT("false"), *GeoJson);
	}
	}   // LOD arrays

	if (!bAnyModels) { return Fail(TEXT("no model arrays resolve at hdr+0x50/58/60/68")); }

	FString DeclJson;
	for (const FString& D : Decls) { DeclJson += (DeclJson.IsEmpty() ? TEXT("\"") : TEXT(",\"")) + D + TEXT("\""); }

	// Run the same single-ownership gate the writer uses, so ANY ydr can be audited - not just one
	// we just built. ⚠ ADVISORY here, not a verdict: sharing a grcTexture stub is LEGAL in
	// EMBEDDED-texdict mode (the pgDictionary owns the texture once), which real R* files use, so a
	// non-zero count on a game file is not necessarily a defect. The HARD refusal stays in
	// ExportYdrBinary, where we know we emit external-ytd stubs and sharing is always wrong.
	const RudeYdrBin::FVerify Vf = RudeYdrBin::VerifyDrawable(R.Sys);

	return FString::Printf(
		TEXT("{\"ok\":true,\"name\":\"%s\",\"version\":%u,\"sysSize\":%d,\"gfxSize\":%d,")
		TEXT("\"sharedBlocks\":%d,\"declsRejected\":%d,\"boundsProblems\":%d,\"firstProblem\":\"%s\",")
		TEXT("\"hasEmbeddedBound\":%s,\"shaderCount\":%d,\"geometries\":%d,")
		TEXT("\"vertices\":%d,\"triangles\":%d,\"indicesOutOfRange\":%d,")
		TEXT("\"declsDecoded\":%d,\"declsUnsupported\":%d,\"declError\":\"%s\",")
		TEXT("\"posInAabb\":%d,\"posOutOfAabb\":%d,\"nanVerts\":%d,\"geosWithoutNormal\":%d,")
		TEXT("\"modelsDeclared\":%d,\"modelsDropped\":%d,\"geometriesDropped\":%d,")
		TEXT("\"lodHeadersDropped\":%d,\"indexBuffersUnreadable\":%d,\"indexCountMismatch\":%d,")
		TEXT("\"declarations\":[%s],\"shaders\":[%s],\"detail\":[%s]}"),
		*DrawName, R.Version, R.Sys.Num(), R.Gfx.Num(),
		Vf.SharedBlocks, Vf.DeclBad, Vf.BoundsBad, *Vf.FirstProblem,
		PtrBound ? TEXT("true") : TEXT("false"), NumShaders, TotalGeo,
		TotalVerts, TotalTris, BadIdx,
		DeclOk, DeclBad, *FirstDeclError,
		PosInAabb, PosOutAabb, NanVerts, NoNormal,
		ModelsDeclared, ModelsDropped, GeosDropped,
		LodHeadersDropped, IdxUnreadable, IdxCountMismatch,
		*DeclJson, *ShaderJson, *ModelJson);
}
