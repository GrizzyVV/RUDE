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

	FString Expect = TEXT("identical");
	{
		TArray<FString> Parts;
		Options.ParseIntoArray(Parts, TEXT(";"), true);
		for (const FString& P : Parts)
		{
			FString K, V;
			if (P.Split(TEXT("="), &K, &V) && K.TrimStartAndEnd().Equals(TEXT("expect"), ESearchCase::IgnoreCase))
			{
				Expect = V.TrimStartAndEnd().ToLower();
			}
		}
	}

	FMetaImage Donor;
	FString Error;
	if (!RudeMetaUnwrap(TemplateBinPath.TrimStartAndEnd(), Donor, Error)) { return Fail(Error); }

	// No value patching yet - this is the CONTAINER round-trip, and its only claim is that unwrapping
	// and re-wrapping loses nothing. The value-patch layer sits on top of a container that is proven
	// lossless, never the other way round.
	TArray<uint8> Patched = Donor.Sys;

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
	// "identical" now means: the image survives the round trip. "edited" additionally requires that
	// the file actually changed.
	const bool bMet = bImageIdentical && (bExpectIdentical || !bIdentical);

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
		TEXT("\"expectationMet\":%s,\"written\":%s,")
		TEXT("\"note\":\"container round-trip only - unwrap and re-wrap with the donor's own header. No value ")
		TEXT("patching yet, and the lane is SIZE-PRESERVING by construction: the donor's flag words describe ")
		TEXT("the image exactly, so a different size would make them a lie.\"}"),
		bMet ? TEXT("true") : TEXT("false"),
		*RudeJsonEscape(TemplateBinPath), *RudeJsonEscape(Out), Donor.Version,
		Donor.Sys.Num(), Donor.File.Num(), OutFile.Num(),
		bIdentical ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Expect),
		bImageIdentical ? TEXT("true") : TEXT("false"), *RudeJsonEscape(ReinflateNote),
		bMet ? TEXT("true") : TEXT("false"), bWritten ? TEXT("true") : TEXT("false"));
}
