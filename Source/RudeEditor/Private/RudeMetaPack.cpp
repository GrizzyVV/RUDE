// RudeMetaPack - the RSC7 v2 META container round-trip (ytyp / ymap / scenario ymt).
//
// WHY THIS EXISTS. RUDE exports archetypes, scenarios and paths in the TEXT form, and whether FiveM
// Legacy loads a text-form file is an open question nobody has answered. This lane removes the
// question rather than answering it: if RUDE can hand back the BINARY the game already loads, the
// text form never has to be accepted by anything.
//
// WHAT IT IS, AND IS NOT. A DONOR REPACK, exactly as `PackYcdBinary` is for clip dictionaries:
// it unwraps the game's own binary, keeps the system image, and re-wraps it with the donor's OWN
// 16-byte header. It does not assemble a container and it cannot change a segment's SIZE - the
// donor's flag words still have to describe the image exactly, and re-deriving a page plan is a
// different job. A size-preserving value patch is what this shape supports; adding an archetype is
// not, and is refused rather than guessed at.
//
// WHAT WAS MEASURED BEFORE A LINE OF THIS WAS WRITTEN (2026-09-11):
//   * `rout/meta_write.py` proves the FORMAT is writable: read a real binary into a value model,
//     write it back, demand the bytes match. Re-measured today with `tools/roundtrip_coverage.py`
//     over a 240-file board per lane: ytyp 235/235, ymt 240/240, ymap 238/238 EXACT, coverage
//     100.0000%. The August debt figures (25% / 21% / 8%) were stale by a wide margin.
//   * That writer is LOCAL PYTHON with a `--selftest` entry point only, and ROUT's C++ has the meta
//     READER and no writer. RUDE is a public, self-contained C++ plugin and cannot depend on
//     unpublished local Python - which is why this lives here rather than being "a port".
//   * The container shape is the one `PackYcdBinary` already handles: RSC7, 16-byte header, raw
//     DEFLATE of [system | graphics]. Both extracted binary `.ytyp` decode to sys 16,384 / gfx 0, so
//     the same empty-graphics discipline applies. A donor with graphics bytes is REFUSED by name -
//     the donor's header would declare a segment the re-wrapped stream no longer holds, which is
//     silent corruption reported as success.
//
// THE GATE IS BYTE IDENTITY ON A NO-OP. Anything less proves nothing: a tolerant compare would pass
// a re-wrap that quietly re-spelled the stream.

#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Compression.h"

namespace
{
	// The RSC7 flag word encodes a segment's size as a sum of page buckets. Same scheme the ycd lane
	// decodes (RudeYcdPack.cpp: "the plan's total size equalled the inflated length on 10/10 files").
	static uint32 RudeMetaSegSizeFromFlags(uint32 Flags)
	{
		static const int32 KBit[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };
		static const uint32 KMask[9] = { 1, 1, 1, 1, 0x7F, 0x3F, 0xF, 3, 1 };
		const uint32 F = Flags & 0x0FFFFFFFu;
		const uint32 Base = 0x200u << (F & 0xFu);
		uint32 Total = 0;
		for (int32 K = 0; K <= 8; ++K) { Total += ((F >> KBit[K]) & KMask[K]) * (Base << K); }
		return Total;
	}

	// RAGE joaat over the lowercased name - the same one-at-a-time hash the ytd lane already proved
	// against observed dictionary hashes. An archetype's identity in a binary meta IS this u32.
	static uint32 RudeMetaJoaat(const FString& S)
	{
		uint32 H = 0;
		const FString L = S.ToLower();
		for (int32 i = 0; i < L.Len(); ++i)
		{
			H += (uint8)L[i];
			H += (H << 10);
			H ^= (H >> 6);
		}
		H += (H << 3); H ^= (H >> 11); H += (H << 15);
		return H;
	}

	struct FMetaImage
	{
		TArray<uint8> File;      // the donor's bytes, verbatim
		TArray<uint8> Sys;       // the inflated system segment
		uint32 Version = 0;
		uint32 SysFlags = 0;
		uint32 GfxFlags = 0;
	};

	static bool RudeMetaUnwrap(const FString& Path, FMetaImage& Out, FString& Error)
	{
		if (!FFileHelper::LoadFileToArray(Out.File, *Path))
		{
			Error = FString::Printf(TEXT("cannot read %s"), *Path);
			return false;
		}
		if (Out.File.Num() < 16 || Out.File[0] != 'R' || Out.File[1] != 'S' || Out.File[2] != 'C' || Out.File[3] != '7')
		{
			Error = TEXT("not an RSC7 container (no 'RSC7' magic)");
			return false;
		}
		FMemory::Memcpy(&Out.Version,  Out.File.GetData() + 4, 4);
		FMemory::Memcpy(&Out.SysFlags, Out.File.GetData() + 8, 4);
		FMemory::Memcpy(&Out.GfxFlags, Out.File.GetData() + 12, 4);
		if (Out.Version != 2)
		{
			// v2 IS the META family. Refusing by version keeps a ydr/yft/ycd from being repacked by a
			// lane that knows nothing about its layout.
			Error = FString::Printf(TEXT("RSC7 version %u is not META v2 (ytyp/ymap/ymt); this lane refuses anything else"), Out.Version);
			return false;
		}
		const uint32 SysSize = RudeMetaSegSizeFromFlags(Out.SysFlags);
		const uint32 GfxSize = RudeMetaSegSizeFromFlags(Out.GfxFlags);
		if (GfxSize != 0)
		{
			Error = FString::Printf(
				TEXT("this donor declares a %u-byte GRAPHICS segment. The re-wrap keeps the donor's own header, ")
				TEXT("which would then describe a segment the stream no longer holds - silent corruption reported as ")
				TEXT("success. Refused by name; carrying graphics bytes through is UNMEASURED."), GfxSize);
			return false;
		}
		if (SysSize == 0 || SysSize > (1u << 28))
		{
			Error = FString::Printf(TEXT("implausible system segment size %u from flags 0x%08X"), SysSize, Out.SysFlags);
			return false;
		}
		TArray<uint8> Blob;
		Blob.SetNumZeroed((int32)SysSize);
		int32 OutSize = Blob.Num();
		// headerless (raw) DEFLATE: -15 window bits.
		if (!FCompression::UncompressMemory(NAME_Zlib, Blob.GetData(), OutSize,
		                                   Out.File.GetData() + 16, Out.File.Num() - 16,
		                                   COMPRESS_NoFlags, -15))
		{
			Error = TEXT("raw-DEFLATE inflate failed (truncated, or a packing this lane does not read)");
			return false;
		}
		Out.Sys = MoveTemp(Blob);
		return true;
	}

	static bool RudeMetaWrap(const FMetaImage& Donor, const TArray<uint8>& Sys, TArray<uint8>& OutFile, FString& Error)
	{
		if (Sys.Num() != Donor.Sys.Num())
		{
			// The donor's flag words describe the image exactly. A different size makes them a lie.
			Error = FString::Printf(TEXT("patched image is %d bytes, donor is %d - this lane is SIZE-PRESERVING"),
				Sys.Num(), Donor.Sys.Num());
			return false;
		}
		int32 ZBound = FCompression::CompressMemoryBound(NAME_Zlib, Sys.Num());
		TArray<uint8> Z;
		Z.SetNumUninitialized(ZBound);
		int32 ZSize = ZBound;
		if (!FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZSize, Sys.GetData(), Sys.Num()))
		{
			Error = TEXT("zlib compress failed");
			return false;
		}
		if (ZSize < 7 || Z[0] != 0x78)
		{
			Error = TEXT("unexpected zlib stream (need the standard 2-byte header to strip)");
			return false;
		}
		OutFile.Reset();
		OutFile.Append(Donor.File.GetData(), 16);      // magic, version, both flag words - the donor's own
		OutFile.Append(Z.GetData() + 2, ZSize - 6);    // strip the 2-byte zlib header and the 4-byte adler32
		return true;
	}
}

FString URudeToolset::PackMetaBinary(const FString& TemplateBinPath, const FString& OutBinPath, const FString& Options)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};

	// ---- THE VALUE-PATCH LAYER (2026-09-11), sitting on a container already proven lossless ----
	// WHAT THIS IS NOT: a schema-aware meta editor. The binary meta resource is a graph of tagged
	// pointers into a system image, and RUDE does not model its structures - so it will not pretend to
	// know where "lodDist" lives. Guessing an offset from a shape that looks right is how a file gets
	// silently corrupted and reported as written.
	// WHAT IT IS: operations that are size-preserving AND cannot corrupt silently, because each one
	// states what it expects to find and REFUSES when it does not find it.
	//   hash=<old>:<new>  rewrite every ALIGNED u32 equal to joaat(old) as joaat(new). This is what real
	//                     authoring needs first - the same ytyp under a new name. A name lives in a meta
	//                     as its hash, so the edit is exact and the slot count is the evidence.
	//   was=<v>           REQUIRED before either @ form: the value the caller believes is there now.
	//   u32@<off>=<v>     one 32-bit integer at a byte offset INTO THE INFLATED IMAGE
	//   f32@<off>=<v>     one 32-bit float, likewise
	//   find=u32:<v>      no write at all - report every offset holding that value, so an offset can be
	//   find=f32:<v>      LOCATED BY MEASUREMENT instead of assumed. This is the honest way in.
	struct FMetaOp { int32 Kind = 0; int64 Off = -1; uint32 New = 0; uint32 Was = 0; FString Src; };
	TArray<FMetaOp> Ops;
	TArray<FString> FindWhat;
	FString Expect = TEXT("identical");
	FString ParseError;
	{
		TArray<FString> Parts;
		Options.ParseIntoArray(Parts, TEXT(";"), true);
		uint32 PendingWas = 0; bool bPendingWas = false;
		auto ToU32 = [](const FString& T, uint32& Out) -> bool
		{
			const FString T2 = T.TrimStartAndEnd();
			if (T2.IsEmpty()) { return false; }
			if (T2.StartsWith(TEXT("0x"), ESearchCase::IgnoreCase))
			{
				Out = (uint32)FCString::Strtoui64(*T2.Mid(2), nullptr, 16); return true;
			}
			Out = (uint32)FCString::Strtoui64(*T2, nullptr, 10); return true;
		};
		for (const FString& RawP : Parts)
		{
			const FString P = RawP.TrimStartAndEnd();
			FString K, V;
			if (!P.Split(TEXT("="), &K, &V)) { continue; }
			K = K.TrimStartAndEnd(); V = V.TrimStartAndEnd();
			if (K.Equals(TEXT("expect"), ESearchCase::IgnoreCase)) { Expect = V.ToLower(); continue; }
			if (K.Equals(TEXT("find"), ESearchCase::IgnoreCase)) { FindWhat.Add(V); continue; }
			if (K.Equals(TEXT("was"), ESearchCase::IgnoreCase))
			{
				// `was` may be spelled as a float when it guards an f32 - read it the way it is written
				if (V.Contains(TEXT(".")))
				{
					const float F = FCString::Atof(*V); FMemory::Memcpy(&PendingWas, &F, 4);
				}
				else if (!ToU32(V, PendingWas)) { ParseError = FString::Printf(TEXT("cannot read was=%s"), *V); break; }
				bPendingWas = true;
				continue;
			}
			if (K.Equals(TEXT("hash"), ESearchCase::IgnoreCase))
			{
				FString A, B;
				if (!V.Split(TEXT(":"), &A, &B) || A.IsEmpty() || B.IsEmpty())
				{
					ParseError = TEXT("hash= needs <oldName>:<newName>"); break;
				}
				FMetaOp Op; Op.Kind = 1; Op.Was = RudeMetaJoaat(A); Op.New = RudeMetaJoaat(B);
				Op.Src = FString::Printf(TEXT("hash %s(0x%08x) -> %s(0x%08x)"), *A, Op.Was, *B, Op.New);
				Ops.Add(Op);
				continue;
			}
			if (K.StartsWith(TEXT("u32@")) || K.StartsWith(TEXT("f32@")))
			{
				const bool bFloat = K.StartsWith(TEXT("f32@"));
				uint32 OffU = 0;
				if (!ToU32(K.Mid(4), OffU)) { ParseError = FString::Printf(TEXT("cannot read the offset in %s"), *K); break; }
				FMetaOp Op; Op.Kind = 2; Op.Off = (int64)OffU;
				if (bFloat) { const float F = FCString::Atof(*V); FMemory::Memcpy(&Op.New, &F, 4); }
				else if (!ToU32(V, Op.New)) { ParseError = FString::Printf(TEXT("cannot read the value %s"), *V); break; }
				if (!bPendingWas)
				{
					ParseError = FString::Printf(TEXT("%s needs a was= immediately before it: a raw offset with no "
						"guard can corrupt a file silently, so this lane refuses one"), *K);
					break;
				}
				Op.Was = PendingWas; bPendingWas = false;
				Op.Src = FString::Printf(TEXT("%s at 0x%llx"), bFloat ? TEXT("f32") : TEXT("u32"), (long long)Op.Off);
				Ops.Add(Op);
				continue;
			}
		}
	}
	if (!ParseError.IsEmpty()) { return Fail(ParseError); }

	FMetaImage Donor;
	FString Error;
	if (!RudeMetaUnwrap(TemplateBinPath.TrimStartAndEnd(), Donor, Error)) { return Fail(Error); }

	// The value-patch layer sits ON TOP of a container proven lossless, never the other way round.
	TArray<uint8> Patched = Donor.Sys;
	int32 PatchesApplied = 0, HashSlotsRewritten = 0;
	FString OpJson, FindJson;
	for (const FMetaOp& Op : Ops)
	{
		if (Op.Kind == 1)
		{
			// every ALIGNED u32 equal to the old hash. Alignment is not a detail: an unaligned match is a
			// byte coincidence, not a field, and rewriting one corrupts whatever value actually lives there.
			int32 Hits = 0;
			for (int32 o = 0; o + 4 <= Patched.Num(); o += 4)
			{
				uint32 Cur = 0; FMemory::Memcpy(&Cur, Patched.GetData() + o, 4);
				if (Cur == Op.Was) { FMemory::Memcpy(Patched.GetData() + o, &Op.New, 4); ++Hits; }
			}
			if (Hits == 0)
			{
				return Fail(FString::Printf(TEXT("%s: that hash appears nowhere in this file - nothing was "
					"written. A no-op reported as success is exactly the failure this refuses"), *Op.Src));
			}
			HashSlotsRewritten += Hits; ++PatchesApplied;
			OpJson += FString::Printf(TEXT("%s{\"op\":\"%s\",\"slots\":%d}"),
				OpJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(Op.Src), Hits);
			continue;
		}
		if (Op.Off < 0 || Op.Off + 4 > (int64)Patched.Num())
		{
			return Fail(FString::Printf(TEXT("%s: offset is outside the %d-byte image"), *Op.Src, Patched.Num()));
		}
		uint32 Cur = 0; FMemory::Memcpy(&Cur, Patched.GetData() + Op.Off, 4);
		if (Cur != Op.Was)
		{
			return Fail(FString::Printf(TEXT("%s: guard failed - the image holds 0x%08x there, not the was=0x%08x "
				"named. The offset is wrong, or this donor is not the file you think it is"), *Op.Src, Cur, Op.Was));
		}
		FMemory::Memcpy(Patched.GetData() + Op.Off, &Op.New, 4);
		++PatchesApplied;
		OpJson += FString::Printf(TEXT("%s{\"op\":\"%s\",\"was\":\"0x%08x\",\"now\":\"0x%08x\"}"),
			OpJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(Op.Src), Op.Was, Op.New);
	}
	// find= is a READOUT, never a write: it is how an offset gets located by measurement rather than guessed.
	for (const FString& W : FindWhat)
	{
		FString Ty, Val;
		if (!W.Split(TEXT(":"), &Ty, &Val)) { continue; }
		Ty = Ty.TrimStartAndEnd(); Val = Val.TrimStartAndEnd();
		uint32 Target = 0;
		if (Ty.Equals(TEXT("f32"), ESearchCase::IgnoreCase))
		{
			const float F = FCString::Atof(*Val); FMemory::Memcpy(&Target, &F, 4);
		}
		else if (Val.StartsWith(TEXT("0x"), ESearchCase::IgnoreCase))
		{
			Target = (uint32)FCString::Strtoui64(*Val.Mid(2), nullptr, 16);
		}
		else { Target = (uint32)FCString::Strtoui64(*Val, nullptr, 10); }
		FString Offs; int32 Hits = 0;
		for (int32 o = 0; o + 4 <= Donor.Sys.Num(); o += 4)
		{
			uint32 Cur = 0; FMemory::Memcpy(&Cur, Donor.Sys.GetData() + o, 4);
			if (Cur != Target) { continue; }
			++Hits;
			if (Hits <= 64) { Offs += FString::Printf(TEXT("%s\"0x%x\""), Offs.IsEmpty() ? TEXT("") : TEXT(","), o); }
		}
		FindJson += FString::Printf(TEXT("%s{\"find\":\"%s\",\"hits\":%d,\"offsets\":[%s]}"),
			FindJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(W), Hits, *Offs);
	}

	TArray<uint8> OutFile;
	if (!RudeMetaWrap(Donor, Patched, OutFile, Error)) { return Fail(Error); }

	const bool bIdentical = (OutFile.Num() == Donor.File.Num())
		&& FMemory::Memcmp(OutFile.GetData(), Donor.File.GetData(), OutFile.Num()) == 0;

	// ⛔ THE GATE IS THE IMAGE, NOT THE COMPRESSED BYTES (corrected 2026-09-11 by measurement).
	// The first version demanded byte identity with the donor FILE and the no-op failed: 5,293 bytes
	// out against 5,281 in. That is a DEFLATE-settings difference, not a loss - the game's compressor
	// is not UE's, and `rout/meta_write.py` records that it needed zlib level 9 + memLevel 9 to
	// reproduce the game's stream on 40/40 samples, which `FCompression` does not expose.
	// It is also the wrong thing to demand: the header's flag words describe the UNCOMPRESSED size,
	// the compressed length appears nowhere, and the game inflates whatever valid stream it is given.
	// What must hold is that INFLATING THE OUTPUT REPRODUCES THE DONOR'S IMAGE EXACTLY. So the output
	// is re-inflated here and compared - a real round-trip, independent of the compressor.
	// ⚠ `byteIdenticalToTemplate` is kept and REPORTED because it is informative (it would be true if
	// the settings ever matched), but it is NOT the gate. A gate that cannot be met by a correct
	// implementation is a gate that teaches you to ignore it.
	bool bImageIdentical = false;
	FString ReinflateNote;
	{
		TArray<uint8> Back;
		Back.SetNumZeroed(Donor.Sys.Num());
		int32 BackSize = Back.Num();
		if (OutFile.Num() > 16 && FCompression::UncompressMemory(NAME_Zlib, Back.GetData(), BackSize,
				OutFile.GetData() + 16, OutFile.Num() - 16, COMPRESS_NoFlags, -15))
		{
			bImageIdentical = (BackSize == Donor.Sys.Num())
				&& FMemory::Memcmp(Back.GetData(), Patched.GetData(), Donor.Sys.Num()) == 0;
			if (!bImageIdentical) { ReinflateNote = TEXT("re-inflated image differs from the one written"); }
		}
		else
		{
			ReinflateNote = TEXT("the output could not be re-inflated at all");
		}
	}

	const bool bExpectIdentical = Expect.Equals(TEXT("identical"));
	// "identical" means the image survives the round trip untouched. "edited" additionally requires that
	// the IMAGE actually changed - not merely that the compressed bytes differ, which they can do for
	// compressor reasons alone (see the note above). An edit that changed nothing must not pass as one.
	const bool bImageChanged = (Patched.Num() != Donor.Sys.Num())
		|| FMemory::Memcmp(Patched.GetData(), Donor.Sys.GetData(), Donor.Sys.Num()) != 0;
	if (bExpectIdentical && bImageChanged)
	{
		return Fail(TEXT("expect=identical but the patch options changed the image - say expect=edited when editing"));
	}
	const bool bMet = bImageIdentical && (bExpectIdentical || bImageChanged);

	bool bWritten = false;
	const FString Out = OutBinPath.TrimStartAndEnd();
	if (bMet && !Out.IsEmpty())
	{
		bWritten = FFileHelper::SaveArrayToFile(OutFile, *Out);
		if (!bWritten) { return Fail(FString::Printf(TEXT("could not write %s"), *Out)); }
	}

	return FString::Printf(
		TEXT("{\"ok\":%s,\"template\":\"%s\",\"out\":\"%s\",\"version\":%u,\"sysBytes\":%d,\"gfxBytes\":0,")
		TEXT("\"templateFileBytes\":%d,\"outFileBytes\":%d,\"byteIdenticalToTemplate\":%s,\"expect\":\"%s\",")
		TEXT("\"imageIdenticalAfterReinflate\":%s,\"reinflateNote\":\"%s\",")
		TEXT("\"expectationMet\":%s,\"written\":%s,\"imageChanged\":%s,")
		TEXT("\"patchesApplied\":%d,\"hashSlotsRewritten\":%d,\"patches\":[%s],\"finds\":[%s],")
		TEXT("\"note\":\"donor repack: unwrap, patch the image in place, re-wrap with the donor's own header. ")
		TEXT("SIZE-PRESERVING by construction - the donor's flag words describe the image exactly, so a ")
		TEXT("different size would make them a lie. Patching is by HASH or by guarded offset; RUDE does not ")
		TEXT("model the meta schema and will not guess where a named field lives. Use find= to locate one.\"}"),
		bMet ? TEXT("true") : TEXT("false"),
		*RudeJsonEscape(TemplateBinPath), *RudeJsonEscape(Out), Donor.Version,
		Donor.Sys.Num(), Donor.File.Num(), OutFile.Num(),
		bIdentical ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Expect),
		bImageIdentical ? TEXT("true") : TEXT("false"), *RudeJsonEscape(ReinflateNote),
		bMet ? TEXT("true") : TEXT("false"), bWritten ? TEXT("true") : TEXT("false"),
		bImageChanged ? TEXT("true") : TEXT("false"),
		PatchesApplied, HashSlotsRewritten, *OpJson, *FindJson);
}
