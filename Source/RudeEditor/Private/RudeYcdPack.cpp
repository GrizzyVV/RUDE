// RUDE - RAGE <-> Unreal Development Environment
//
// WP13 ycd_pack lane: clip-dictionary XML -> a loadable binary .ycd, by DONOR REPACK. The block that
// stood between RudeYcdExport.cpp's XML and the game. Every structural claim below was MEASURED by
// maintainer lane `ycd_pack` (`LAWS.md` + `measure_bin.json`, 2026-09-07) over EVERY .ycd binary
// reachable on the authoring machine - 10 files, 10/10 RSC7 v46, 20 animations, 20 sequences,
// 751 BoneIds rows, 1,417 QuantizeFloat channels, 1,341 frames.
//
// ⛔ THE DENOMINATOR, STATED FIRST BECAUSE IT LIMITS EVERY NUMBER BELOW: those 10 binaries are all
// PRODUCED FILES (the maintainer's own container repacks of real game animations). The corpus holds
// .ycd only as XML - 24,844 of them, 0 binaries (`find *.ycd` over the filebase: 0/24,844). So every
// container law here is measured at denominator 10 files / 20 sequences and is NOT a population law,
// and no claim on this page has been checked against a file shipped by the game itself.
//
// WHAT THIS FILE DOES, AND THE SHAPE OF THE HONEST ANSWER:
//   A .ycd is an RSC7 v46 container: a 16-byte header, then raw DEFLATE of the system segment. Inside
//   it, each animation's SEQUENCE BLOCK is self-contained - reached by arithmetic, holding no internal
//   pointers - so a channel edit that does not change any SIZE can be applied IN PLACE, in the
//   inflated segment, and every pointer, atMap bucket, string, page-count record and trailer in the
//   file stays exactly where it was. That is the whole design: this lane does NOT assemble a
//   container. It patches a donor's own image and re-wraps it with the donor's own header.
//
//   Two size-preserving edits exist and both are built:
//     1. THE DESCRIPTOR. A QuantizeFloat channel is described by 12 bytes - u32 numBits, f32 Quantum,
//        f32 Offset - so Quantum and Offset are an 8-byte overwrite (12-byte stride, 1,417/1,417
//        descriptors tiled with no hole).
//     2. THE RAWS. Frames are laid frame-major in a bit-packed block, LSB-first, each frame padded up
//        to a multiple of 32 bits, channels in descriptor order. Decoding every channel's raws and
//        writing them straight back reproduced the block BYTE-IDENTICALLY on 20/20 sequences, and the
//        padding bits after the last channel were zero on 1,341/1,341 frames - which is what makes a
//        re-write of one channel's bits provably local.
//   A raw that will not fit its channel's own numBits is a SIZE CHANGE. It is REFUSED by name and
//   counted; it is never widened, never clamped silently.
//
// THE MEASURE THIS LANE IS HELD TO, and how the tool checks itself:
//   * pack an UNTOUCHED export and the bytes must equal the donor. Enforced by construction AND by
//     measurement: the patched image is diffed against the donor's own inflated image, and when 0
//     bytes differ the donor's FILE BYTES are copied verbatim - no re-deflate, so byte identity is
//     not a hope about a compressor.
//   * pack an edited one and only the edited channel's payload may differ. Every differing byte is
//     attributed to a declared extent (a 12-byte descriptor, or the bit range of an edited channel
//     inside its packed block); `bytesDifferingOutsideDeclaredExtents` is in the verdict and inside
//     `ok`. A byte that moved where nothing was authored fails the pack.
//
// ⚠ DEFLATE IS NOT CANONICAL AND IS NOT RELIED ON. Python zlib reproduced the donor's compressed
// stream at level 8 or 9 on 10/10 files, but all 10 came from ONE producer and Unreal's zlib is a
// different build with its own default level - so an EDITED pack re-deflates and its compressed bytes
// will differ from the donor's everywhere. That is legal (the loader inflates) and it is why the
// byte-identity measure is stated on the INFLATED SEGMENT, and why the no-op path never compresses.
//
// THE REFUSAL IS ATOMIC. Every channel is validated and its bytes STAGED in a first pass; nothing is
// committed to the image and no file is written until every channel has been accepted. A half-applied
// pack - a new Quantum committed over the donor's old raws because the value list was rejected a step
// later - silently RESCALES that channel, which is worse than either edit alone. One refusal and the
// verdict says `written:false` and no output file exists. (Same shape as ExportNewMlo, which computes
// ok before anything is written.)
//
// ⛔ ONE MORE REFUSAL, BY NAME: the donor's graphics segment must be EMPTY. This lane keeps only the
// system bytes and re-wraps them with the donor's own 16-byte header, which still carries the graphics
// flag word - so on a donor with graphics bytes that header would announce a segment the stream no
// longer holds. Empty on 10/10 measured (LAWS.md A1); 10 produced files is not a licence, so a file
// outside that measurement is refused rather than guessed at.
//
// ⛔ NOT CLAIMED, AND NOT BUILT: nothing here has been loaded by the game. This file has not been
// compiled or run in the editor by the lane that wrote it. This lane EDITS a .ycd binary it is handed
// and cannot BUILD one: no part of RUDE, and no part of the maintainer's own exporter in either
// language, turns a clip-dictionary XML into a .ycd container from scratch. Structure changes of any
// kind - a new animation, a new bone, a different frame count, a channel that changes shape or width -
// are REFUSED, because they need a container assembler this lane did not build.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"

#include "HAL/FileManager.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace RudeYcdPk
{
	static FString Fail(const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	}
	static FString JStr(const FString& S) { return TEXT("\"") + RudeJsonEscape(S) + TEXT("\""); }
	static FString JBool(bool B) { return B ? TEXT("true") : TEXT("false"); }

	// ---- RSC7 page plan. base = 0x200<<ss, class-k page = base<<k, counts at fixed bit positions.
	// Same scheme RudeBinaryLanes.cpp's v165 reader decodes; re-checked here at v46: the plan's total size
	// equalled the inflated length on 10/10 files and the blockmap's page-count record agreed with the
	// flag word on 10/10.
	static uint32 SegSizeFromFlags(uint32 Flags)
	{
		static const int32 KBit[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };
		static const uint32 KMask[9] = { 1, 1, 1, 1, 0x7F, 0x3F, 0xF, 3, 1 };
		const uint32 F = Flags & 0x0FFFFFFFu;
		const uint32 Base = 0x200u << (F & 0xFu);
		uint32 Total = 0;
		for (int32 K = 0; K <= 8; ++K) { Total += ((F >> KBit[K]) & KMask[K]) * (Base << K); }
		return Total;
	}
	static uint32 PageCountFromFlags(uint32 Flags)
	{
		static const int32 KBit[9] = { 27, 26, 25, 24, 17, 11, 7, 5, 4 };
		static const uint32 KMask[9] = { 1, 1, 1, 1, 0x7F, 0x3F, 0xF, 3, 1 };
		const uint32 F = Flags & 0x0FFFFFFFu;
		uint32 Total = 0;
		for (int32 K = 0; K <= 8; ++K) { Total += (F >> KBit[K]) & KMask[K]; }
		return Total;
	}

	static uint32 Ptr28(uint32 V) { return V & 0x0FFFFFFFu; }
	static int32 RoundUp4(int32 N) { return (N + 3) & ~3; }

	struct FYcdImage
	{
		TArray<uint8> File;       // the donor's bytes, exactly as they sit on disk
		TArray<uint8> Sys;        // the inflated system segment
		uint32 Version = 0, SysFlags = 0, GfxFlags = 0;

		bool In(int32 Off, int32 Len) const { return Off >= 0 && Len >= 0 && Off + Len <= Sys.Num(); }
		uint16 U16(int32 O) const { uint16 V = 0; FMemory::Memcpy(&V, Sys.GetData() + O, 2); return V; }
		uint32 U32(int32 O) const { uint32 V = 0; FMemory::Memcpy(&V, Sys.GetData() + O, 4); return V; }
		float  F32(int32 O) const { float  V = 0; FMemory::Memcpy(&V, Sys.GetData() + O, 4); return V; }
		uint8  U8(int32 O)  const { return Sys[O]; }
	};

	// RSC7 v46 file -> inflated system segment. A version that is not 46 is refused BY NAME: a v159
	// image is the Enhanced build and loading one into a Legacy pipeline is the classic "invalid
	// fixup" report, so the message says which it got.
	static bool LoadYcd(const FString& Path, FYcdImage& Out, FString& Error)
	{
		if (!FFileHelper::LoadFileToArray(Out.File, *Path))
		{
			Error = FString::Printf(TEXT("cannot read %s"), *Path);
			return false;
		}
		if (Out.File.Num() < 16)
		{
			Error = TEXT("shorter than an RSC7 header (16 bytes)");
			return false;
		}
		if (Out.File[0] != 'R' || Out.File[1] != 'S' || Out.File[2] != 'C' || Out.File[3] != '7')
		{
			Error = TEXT("not an RSC7 container (no 'RSC7' magic). A header-less streamed image is not handled by this lane");
			return false;
		}
		FMemory::Memcpy(&Out.Version, Out.File.GetData() + 4, 4);
		FMemory::Memcpy(&Out.SysFlags, Out.File.GetData() + 8, 4);
		FMemory::Memcpy(&Out.GfxFlags, Out.File.GetData() + 12, 4);
		if (Out.Version != 46)
		{
			Error = FString::Printf(TEXT("version %u is not a clip dictionary (want v46%s)"), Out.Version,
				Out.Version == 159 ? TEXT("; v159 = GTA V Enhanced, not Legacy") : TEXT(""));
			return false;
		}
		const uint32 SysSize = SegSizeFromFlags(Out.SysFlags);
		const uint32 GfxSize = SegSizeFromFlags(Out.GfxFlags);
		// REFUSED BY NAME, not assumed away: this lane keeps only the system bytes and re-wraps them
		// with the DONOR'S OWN 16-byte header, which still carries the graphics flag word. On a donor
		// with graphics bytes that header would declare a segment the stream no longer holds - silent
		// corruption reported as success. Every binary measured here had an empty graphics segment
		// (10/10, maintainer lane `ycd_pack`, LAWS.md A1), and 10 produced files is not a licence, so
		// a file outside that measurement is refused rather than guessed at. Carrying the graphics
		// bytes through the re-wrap is the alternative and it is UNMEASURED.
		if (GfxSize != 0)
		{
			Error = FString::Printf(TEXT("this lane repacks clip dictionaries whose graphics segment is EMPTY (measured on 10/10 of the .ycd binaries available to the maintainer); this file declares %u graphics bytes, which the re-wrap would drop while the donor's header still announced them"), GfxSize);
			return false;
		}
		if (SysSize == 0 || SysSize > (1u << 28))
		{
			Error = FString::Printf(TEXT("implausible system segment size %u from flags 0x%08X"), SysSize, Out.SysFlags);
			return false;
		}
		TArray<uint8> Blob;
		Blob.SetNumZeroed((int32)(SysSize + GfxSize));
		int32 OutSize = Blob.Num();
		// The body is headerless (raw) DEFLATE of [system | graphics]; -15 window bits says so.
		if (!FCompression::UncompressMemory(NAME_Zlib, Blob.GetData(), OutSize,
		                                   Out.File.GetData() + 16, Out.File.Num() - 16,
		                                   COMPRESS_NoFlags, -15))
		{
			Error = TEXT("raw-DEFLATE inflate failed (truncated, or a packing this lane does not read)");
			return false;
		}
		Out.Sys.Append(Blob.GetData(), (int32)SysSize);
		return true;
	}

	// Re-wrap a patched system image with the DONOR's own header. The segment size never changes here,
	// so the donor's flag words still describe it exactly and no page plan has to be re-derived.
	static bool WrapYcd(const FYcdImage& Donor, const TArray<uint8>& Sys, TArray<uint8>& OutFile, FString& Error)
	{
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
		OutFile.Append(Donor.File.GetData(), 16);                 // magic, version, both flag words
		OutFile.Append(Z.GetData() + 2, ZSize - 6);               // strip zlib header + adler32
		return true;
	}

	// ---- the container walk ------------------------------------------------------------------------
	struct FChanRef
	{
		int32 Desc = 0;          // absolute offset of the 12-byte descriptor in the system segment
		uint32 NumBits = 0;
		float Quantum = 0.f, Offset = 0.f;
		int32 Bone = -1, Track = -1, Comp = -1, Item = -1;
		int32 BitInFrame = 0;    // bit offset of this channel inside a frame's packed region
	};

	struct FSeqRef
	{
		int32 Off = 0;
		uint32 Total = 0, FrameCount = 0, QuantOff = 0, FrameBits = 0;
		int32 CountTable = -1, Packed = 0, StrideBytes = 0, BlockBytes = 0;
		uint16 Counts[9] = { 0,0,0,0,0,0,0,0,0 };
		TArray<FChanRef> Chans;
		int32 Candidates = 0;
	};

	struct FAnimRef
	{
		uint32 Key = 0;
		int32 Off = 0, BoneIdsOff = 0, NumBones = 0;
		TArray<FSeqRef> Seqs;
	};

	// atMap walk: bucket index then chain order. Same order the XML is written in.
	static void AtMap(const FYcdImage& Img, int32 BucketPtr, int32 NBuckets, TArray<TPair<uint32, int32>>& Out)
	{
		for (int32 B = 0; B < NBuckets; ++B)
		{
			if (!Img.In(BucketPtr + B * 8, 4)) { return; }
			uint32 Node = Img.U32(BucketPtr + B * 8);
			int32 Guard = 0;
			while ((Node & 0xF0000000u) != 0)
			{
				const int32 N = (int32)Ptr28(Node);
				if (!Img.In(N, 0x18)) { break; }
				Out.Add(TPair<uint32, int32>(Img.U32(N), (int32)Ptr28(Img.U32(N + 8))));
				Node = Img.U32(N + 0x10);
				if (++Guard > 65535) { break; }
			}
		}
	}

	// The count table of one sequence, found by the four constraints that pin it: the SIZE EQUATION
	// (the table plus its nine padded map lists must end exactly at the sequence's declared end), the
	// DESCRIPTOR WALK (the pools must consume exactly quantOffset bytes), the PACKED-BLOCK EXTENT (the
	// frame stride times the frame count must land exactly on the table) and the MAP-LIST SENTINEL
	// (every live entry below nbones*4, every pad slot equal to it). Exactly one candidate must
	// survive - 20/20 sequences measured, 0 ambiguous. An ambiguous sequence REFUSES rather than
	// silently taking the first fit.
	static bool LocateSequence(const FYcdImage& Img, int32 SeqOff, int32 NBones, int32 BoneIdsOff, FSeqRef& Out)
	{
		Out.Off = SeqOff;
		if (!Img.In(SeqOff, 0x20)) { return false; }
		Out.Total = Img.U32(SeqOff + 0x10);
		Out.FrameCount = Img.U16(SeqOff + 0x16);
		Out.QuantOff = Img.U32(SeqOff + 0x0C);
		const int32 Data = SeqOff + 0x20;
		const int32 Packed = Data + (int32)Out.QuantOff;
		const int32 SeqEnd = SeqOff + (int32)Out.Total;
		if (SeqEnd > Img.Sys.Num() || Packed > Img.Sys.Num() || Packed < Data) { return false; }

		for (int32 C = Packed; C + 18 <= SeqEnd; C += 2)
		{
			uint16 CC[9];
			bool bRead = true;
			for (int32 I = 0; I < 9; ++I)
			{
				if (!Img.In(C + I * 2, 2)) { bRead = false; break; }
				CC[I] = Img.U16(C + I * 2);
			}
			if (!bRead) { break; }
			int32 MaxCount = 0, SumPadded = 0;
			for (int32 I = 0; I < 9; ++I) { MaxCount = FMath::Max<int32>(MaxCount, CC[I]); SumPadded += RoundUp4(CC[I]); }
			if (NBones > 0 && MaxCount > NBones * 4) { continue; }
			if (C + (9 + SumPadded) * 2 != SeqEnd) { continue; }

			const int32 NQuat = CC[0], NVec = CC[1], NFlt = CC[2], NRaw = CC[3];
			const int32 NQz = CC[4], NInd = CC[5], NInl = CC[6];
			int32 O = Data + NQuat * 12 + NVec * 12 + NFlt * 4;
			TArray<int32> QzDesc, IndDesc;
			QzDesc.Reserve(NQz);
			for (int32 K = 0; K < NQz; ++K) { QzDesc.Add(O + K * 12); }
			O += NQz * 12;
			bool bBad = false;
			for (int32 K = 0; K < NInd && !bBad; ++K)
			{
				if (!Img.In(O, 0x14)) { bBad = true; break; }
				IndDesc.Add(O);
				O += 0x14 + (int32)Img.U32(O + 8) * 4;
			}
			if (bBad) { continue; }
			for (int32 K = 0; K < NInl && !bBad; ++K)
			{
				if (!Img.In(O, 16)) { bBad = true; break; }
				const uint32 SizeWords = Img.U32(O);
				if (SizeWords < 4 || O + (int32)SizeWords * 4 > Img.Sys.Num()) { bBad = true; break; }
				O += (int32)SizeWords * 4;
			}
			if (bBad || (O - Data) != (int32)Out.QuantOff) { continue; }

			uint32 Bits = 0;
			for (const int32 D : QzDesc) { if (!Img.In(D, 12)) { bBad = true; break; } Bits += Img.U32(D); }
			if (bBad) { continue; }
			for (const int32 D : IndDesc) { Bits += Img.U32(D); }
			const uint32 FrameBits = ((Bits + 31u) / 32u) * 32u;
			const int32 Stride = (int32)(FrameBits / 8) + NRaw * 4;
			if (Packed + Stride * (int32)Out.FrameCount != C) { continue; }

			if (NBones > 0)
			{
				int32 M = C + 18;
				bool bOk = true;
				for (int32 I = 0; I < 9 && bOk; ++I)
				{
					const int32 Padded = RoundUp4(CC[I]);
					if (!Img.In(M, Padded * 2)) { bOk = false; break; }
					for (int32 J = 0; J < CC[I]; ++J) { if (Img.U16(M + J * 2) >= (uint16)(NBones * 4)) { bOk = false; break; } }
					for (int32 J = CC[I]; J < Padded && bOk; ++J) { if (Img.U16(M + J * 2) != (uint16)(NBones * 4)) { bOk = false; } }
					M += Padded * 2;
				}
				if (!bOk) { continue; }
			}

			++Out.Candidates;
			if (Out.Candidates > 1) { continue; }        // keep counting, but the first fit is kept
			Out.CountTable = C;
			for (int32 I = 0; I < 9; ++I) { Out.Counts[I] = CC[I]; }
			Out.FrameBits = FrameBits;
			Out.Packed = Packed;
			Out.StrideBytes = Stride;
			Out.BlockBytes = Stride * (int32)Out.FrameCount;

			// The pool-4 map list is the FIFTH of the nine (the four before it are the static pools
			// and RawFloat). Entry e ties descriptor i to BoneIds row e/4 and component e%4.
			int32 MapAt = C + 18;
			for (int32 I = 0; I < 4; ++I) { MapAt += RoundUp4(CC[I]) * 2; }
			int32 BitCursor = NRaw * 32;
			Out.Chans.Reset();
			for (int32 I = 0; I < QzDesc.Num(); ++I)
			{
				FChanRef Ch;
				Ch.Desc = QzDesc[I];
				Ch.NumBits = Img.U32(Ch.Desc);
				Ch.Quantum = Img.F32(Ch.Desc + 4);
				Ch.Offset = Img.F32(Ch.Desc + 8);
				const uint16 E = Img.In(MapAt + I * 2, 2) ? Img.U16(MapAt + I * 2) : 0;
				Ch.Item = E / 4;
				Ch.Comp = E % 4;
				if (BoneIdsOff > 0 && Img.In(BoneIdsOff + Ch.Item * 4, 4))
				{
					Ch.Bone = Img.U16(BoneIdsOff + Ch.Item * 4);
					Ch.Track = Img.U8(BoneIdsOff + Ch.Item * 4 + 3);
				}
				Ch.BitInFrame = BitCursor;
				BitCursor += (int32)Ch.NumBits;
				Out.Chans.Add(Ch);
			}
		}
		return Out.Candidates == 1;
	}

	static bool WalkDictionary(const FYcdImage& Img, TArray<FAnimRef>& OutAnims, int32& OutClips, FString& Error)
	{
		if (!Img.In(0x40, 0)) { Error = TEXT("system segment shorter than the root object"); return false; }
		const int32 ADict = (int32)Ptr28(Img.U32(0x18));
		if (!Img.In(ADict, 0x30)) { Error = TEXT("the animation dictionary is outside the segment"); return false; }
		TArray<TPair<uint32, int32>> Clips, Anims;
		AtMap(Img, (int32)Ptr28(Img.U32(0x28)), Img.U16(0x30), Clips);
		AtMap(Img, (int32)Ptr28(Img.U32(ADict + 0x18)), Img.U16(ADict + 0x20), Anims);
		OutClips = Clips.Num();

		TSet<int32> Seen;
		for (const TPair<uint32, int32>& E : Anims)
		{
			if (Seen.Contains(E.Value)) { continue; }   // one object under two keys: relocate once
			Seen.Add(E.Value);
			const int32 AV = E.Value;
			if (!Img.In(AV, 0x60)) { Error = TEXT("an animation object is outside the segment"); return false; }
			FAnimRef A;
			A.Key = E.Key;
			A.Off = AV;
			A.NumBones = Img.U16(AV + 0x58);
			A.BoneIdsOff = (int32)Ptr28(Img.U32(AV + 0x50));
			const int32 SeqPtrs = (int32)Ptr28(Img.U32(AV + 0x40));
			const int32 NSeq = Img.U16(AV + 0x48);
			for (int32 S = 0; S < NSeq; ++S)
			{
				if (!Img.In(SeqPtrs + S * 8, 4)) { Error = TEXT("a sequence pointer is outside the segment"); return false; }
				FSeqRef Seq;
				LocateSequence(Img, (int32)Ptr28(Img.U32(SeqPtrs + S * 8)), A.NumBones, A.BoneIdsOff, Seq);
				A.Seqs.Add(MoveTemp(Seq));
			}
			OutAnims.Add(MoveTemp(A));
		}
		return true;
	}

	// ---- the packed block: LSB-first bits, byte-addressed from the block's own base ----------------
	static uint32 GetBits(const TArray<uint8>& Buf, int32 Base, int32 Bit, uint32 N)
	{
		uint32 V = 0;
		for (uint32 I = 0; I < N; ++I)
		{
			const int32 PP = Bit + (int32)I;
			V |= (uint32)((Buf[Base + (PP >> 3)] >> (PP & 7)) & 1) << I;
		}
		return V;
	}
	static void PutBits(TArray<uint8>& Buf, int32 Base, int32 Bit, uint32 N, uint32 V)
	{
		for (uint32 I = 0; I < N; ++I)
		{
			const int32 PP = Bit + (int32)I;
			const uint8 Mask = (uint8)(1u << (PP & 7));
			if ((V >> I) & 1u) { Buf[Base + (PP >> 3)] |= Mask; }
			else { Buf[Base + (PP >> 3)] &= (uint8)~Mask; }
		}
	}

	// ---- the XML side: read the exporter's own output BY LINE ---------------------------------------
	// Line-oriented on purpose, the same reason RudeYcdExport.cpp writes that way: FXmlFile does not
	// preserve line structure (AGENTS.md section 6.5), and a value list is a LINE law - at most ten
	// numbers on one line, eleven or more as a block. Tags are matched by their WHOLE name: "<Values>"
	// starts with "<Value", and a prefix match there reads every value list as a static scalar and
	// produces a dictionary with no animation data in it (maintainer lane `ycd_export`, law I8).
	struct FXmlChan
	{
		FString Type;
		bool bHasQuantum = false, bHasOffset = false;
		double Quantum = 0.0, Offset = 0.0;
		TArray<double> Values;
	};
	struct FXmlSeq { TArray<TArray<FXmlChan>> Items; };   // Items[i] = the channels of BoneIds row i
	struct FXmlAnim { TArray<FXmlSeq> Seqs; };

	static bool AttrValue(const FString& Line, const FString& Tag, double& Out)
	{
		const FString Open = FString::Printf(TEXT("<%s value=\""), *Tag);
		const int32 At = Line.Find(*Open, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE) { return false; }
		const int32 From = At + Open.Len();
		const int32 End = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (End == INDEX_NONE) { return false; }
		Out = FCString::Atod(*Line.Mid(From, End - From));
		return true;
	}
	static bool AttrString(const FString& Line, const FString& Tag, FString& Out)
	{
		const FString Open = FString::Printf(TEXT("<%s value=\""), *Tag);
		const int32 At = Line.Find(*Open, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE) { return false; }
		const int32 From = At + Open.Len();
		const int32 End = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (End == INDEX_NONE) { return false; }
		Out = Line.Mid(From, End - From);
		return true;
	}
	static void AppendNumbers(const FString& Text, TArray<double>& Out)
	{
		TArray<FString> Toks;
		Text.ParseIntoArrayWS(Toks);
		for (const FString& T : Toks) { Out.Add(FCString::Atod(*T)); }
	}

	// A small hand-rolled scanner over the dictionary's own line structure.
	// AN <Item> BELONGS TO ITS NEAREST ENCLOSING LIST, so this keeps a STACK of list names and never
	// infers nesting from a flag. Two measured traps make the stack necessary rather than tidy:
	//  * a dictionary spells <BoneIds> as a list of <Item> rows. A scanner that treats any <Item>
	//    outside <Sequences> as a new animation read 79 animations out of a 2-animation file.
	//  * a clip of Type=AnimationList carries its OWN <Animations> list of references, so the tag
	//    name alone does not identify the dictionary's animation list. The dictionary's is the one
	//    that opens with NOTHING else open; the clip's is tracked under a different name and ignored.
	// An <Item> in a list this scanner does not know is COUNTED in OutUnknownItems, never guessed at.
	static const TCHAR* const GYcdLists[] = { TEXT("Clips"), TEXT("Animations"), TEXT("BoneIds"),
		TEXT("Sequences"), TEXT("SequenceData"), TEXT("Channels"), TEXT("Tags"), TEXT("Properties"),
		TEXT("Attributes"), TEXT("RecordUnknown00") };

	static bool IsListName(const FString& Name)
	{
		for (const TCHAR* const L : GYcdLists) { if (Name == L) { return true; } }
		return false;
	}

	static bool ScanXml(const TArray<FString>& Lines, TArray<FXmlAnim>& Out, int32& OutUnknownItems, FString& Error)
	{
		// INDICES, NOT POINTERS. A pointer into a TArray element dies the moment that array grows,
		// and this scanner grows four nested arrays as it walks. Indices survive every reallocation
		// and cost nothing; the alternative compiles clean and reads freed memory.
		TArray<FString> Stack;
		bool bInValues = false;
		int32 AnimIx = INDEX_NONE, SeqIx = INDEX_NONE, ItemIx = INDEX_NONE, ChanIx = INDEX_NONE;
		OutUnknownItems = 0;
		for (int32 I = 0; I < Lines.Num(); ++I)
		{
			const FString T = Lines[I].TrimStartAndEnd();
			if (bInValues)
			{
				if (T.StartsWith(TEXT("</Values>"), ESearchCase::CaseSensitive)) { bInValues = false; continue; }
				if (ChanIx != INDEX_NONE) { AppendNumbers(T, Out[AnimIx].Seqs[SeqIx].Items[ItemIx][ChanIx].Values); }
				continue;
			}
			if (T.StartsWith(TEXT("</"), ESearchCase::CaseSensitive) && T.EndsWith(TEXT(">"), ESearchCase::CaseSensitive))
			{
				const FString Name = T.Mid(2, T.Len() - 3);
				if (Stack.Num() > 0 && (Stack.Last() == Name || (Name == TEXT("Animations") && Stack.Last() == TEXT("AnimationRefs"))))
				{
					Stack.Pop();
					if (Name == TEXT("Channels")) { ChanIx = INDEX_NONE; }
					else if (Name == TEXT("SequenceData")) { ItemIx = INDEX_NONE; }
					else if (Name == TEXT("Sequences")) { SeqIx = INDEX_NONE; }
				}
				continue;
			}
			if (T.StartsWith(TEXT("<"), ESearchCase::CaseSensitive) && T.EndsWith(TEXT(">"), ESearchCase::CaseSensitive)
				&& !T.Contains(TEXT(" ")) && !T.Contains(TEXT("/")))
			{
				const FString Name = T.Mid(1, T.Len() - 2);
				if (IsListName(Name))
				{
					Stack.Add((Name == TEXT("Animations") && Stack.Num() > 0) ? FString(TEXT("AnimationRefs")) : Name);
					continue;
				}
			}
			if (T == TEXT("<Item>"))
			{
				const FString Top = Stack.Num() > 0 ? Stack.Last() : FString();
				if (Top == TEXT("Channels") && ItemIx != INDEX_NONE)
				{
					ChanIx = Out[AnimIx].Seqs[SeqIx].Items[ItemIx].AddDefaulted();
				}
				else if (Top == TEXT("SequenceData") && SeqIx != INDEX_NONE)
				{
					ItemIx = Out[AnimIx].Seqs[SeqIx].Items.AddDefaulted();
					ChanIx = INDEX_NONE;
				}
				else if (Top == TEXT("Sequences") && AnimIx != INDEX_NONE)
				{
					SeqIx = Out[AnimIx].Seqs.AddDefaulted();
					ItemIx = INDEX_NONE; ChanIx = INDEX_NONE;
				}
				else if (Top == TEXT("Animations"))
				{
					AnimIx = Out.AddDefaulted();
					SeqIx = INDEX_NONE; ItemIx = INDEX_NONE; ChanIx = INDEX_NONE;
				}
				else if (Top != TEXT("BoneIds") && Top != TEXT("Clips") && Top != TEXT("Tags")
					&& Top != TEXT("Properties") && Top != TEXT("Attributes")
					&& Top != TEXT("RecordUnknown00") && Top != TEXT("AnimationRefs")) { ++OutUnknownItems; }
				continue;
			}
			if (ChanIx == INDEX_NONE || Stack.Num() == 0 || Stack.Last() != TEXT("Channels")) { continue; }
			FXmlChan& Cur = Out[AnimIx].Seqs[SeqIx].Items[ItemIx][ChanIx];
			if (T.StartsWith(TEXT("<Type "), ESearchCase::CaseSensitive)) { AttrString(T, TEXT("Type"), Cur.Type); continue; }
			if (T.StartsWith(TEXT("<Quantum "), ESearchCase::CaseSensitive)) { Cur.bHasQuantum = AttrValue(T, TEXT("Quantum"), Cur.Quantum); continue; }
			if (T.StartsWith(TEXT("<Offset "), ESearchCase::CaseSensitive)) { Cur.bHasOffset = AttrValue(T, TEXT("Offset"), Cur.Offset); continue; }
			// WHOLE-NAME match, never a prefix: "<Values>" and "<Value " are different tags.
			if (T.StartsWith(TEXT("<Values>"), ESearchCase::CaseSensitive))
			{
				FString Inner = T.Mid(8);
				const int32 Close = Inner.Find(TEXT("</Values>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
				if (Close != INDEX_NONE) { AppendNumbers(Inner.Left(Close), Cur.Values); }
				else { AppendNumbers(Inner, Cur.Values); bInValues = true; }
				continue;
			}
		}
		if (Out.Num() == 0) { Error = TEXT("the XML declared no <Animations> items - nothing to pack"); return false; }
		return true;
	}

	struct FPkOpts
	{
		FString TemplatePath;
		bool bAuthorRoot = false;       // maintainer lane `ycd_export` law G5 / the in-game root-translation scar
		bool bValues = true;            // write the raws, not just the descriptors
		bool bDescriptors = true;       // write Quantum/Offset when the XML moved them
		// `expect=` puts the caller's claim about the OUTCOME inside the computed ok, which is what
		// lets a gate row go red on its own: `expect=identical` on a no-op pack (THE MEASURE - the
		// donor's file bytes must come back), `expect=edited` on an authored one (a pack that
		// silently wrote nothing would otherwise report noOp:true, ok:true and pass).
		FString Expect;                 // "" (no claim), "identical", or "edited"
	};
	static void ParseOpts(const FString& In, FPkOpts& O)
	{
		TArray<FString> Parts;
		In.ParseIntoArray(Parts, TEXT(";"), true);
		for (FString P : Parts)
		{
			P.TrimStartAndEndInline();
			FString K, V;
			if (!P.Split(TEXT("="), &K, &V)) { continue; }
			K = K.TrimStartAndEnd().ToLower();
			V = V.TrimStartAndEnd();
			const bool bOn = (V == TEXT("1") || V.ToLower() == TEXT("true"));
			if (K == TEXT("template")) { O.TemplatePath = V; }
			else if (K == TEXT("authorroot")) { O.bAuthorRoot = bOn; }
			else if (K == TEXT("values")) { O.bValues = bOn; }
			else if (K == TEXT("descriptors")) { O.bDescriptors = bOn; }
			else if (K == TEXT("expect")) { O.Expect = V.ToLower(); }
		}
	}
}

// ---- ProbeYcdBinary ------------------------------------------------------------------------------
FString URudeToolset::ProbeYcdBinary(const FString& YcdPath)
{
	using namespace RudeYcdPk;
	const FString P = YcdPath.TrimStartAndEnd();
	if (P.IsEmpty()) { return Fail(TEXT("give the .ycd binary path to read")); }

	FYcdImage Img;
	FString Err;
	if (!LoadYcd(P, Img, Err)) { return Fail(Err); }

	TArray<FAnimRef> Anims;
	int32 NClips = 0;
	if (!WalkDictionary(Img, Anims, NClips, Err)) { return Fail(Err); }

	int32 SeqUnique = 0, SeqAmbiguous = 0, QzChannels = 0, QuantumNonPositive = 0, BitsOutOfRange = 0;
	int32 RoundTripOk = 0, RoundTripBad = 0, PadNonZero = 0, PadChecked = 0, SeqPadSkippedIndirect = 0;
	int32 Pools[9] = { 0,0,0,0,0,0,0,0,0 };
	FString FirstProblem;
	for (const FAnimRef& A : Anims)
	{
		for (const FSeqRef& S : A.Seqs)
		{
			if (S.Candidates != 1)
			{
				++SeqAmbiguous;
				if (FirstProblem.IsEmpty())
				{
					FirstProblem = FString::Printf(TEXT("sequence at 0x%X in animation 0x%08X: %d count-table candidates survived, want exactly 1"),
						S.Off, A.Key, S.Candidates);
				}
				continue;
			}
			++SeqUnique;
			for (int32 I = 0; I < 9; ++I) { Pools[I] += S.Counts[I]; }
			for (const FChanRef& C : S.Chans)
			{
				++QzChannels;
				if (!(C.Quantum > 0.f)) { ++QuantumNonPositive; }
				if (C.NumBits == 0 || C.NumBits > 32) { ++BitsOutOfRange; }
			}
			// The self-check that makes the bit layout evidence rather than a transcription: decode
			// every channel of every frame and write it straight back. Anything but byte identity
			// means the frame geometry is wrong, and a packer built on it would corrupt the payload.
			if (S.BlockBytes > 0 && Img.In(S.Packed, S.BlockBytes))
			{
				TArray<uint8> Orig, Fresh;
				Orig.Append(Img.Sys.GetData() + S.Packed, S.BlockBytes);
				Fresh = Orig;
				const int32 RawFloats = S.Counts[3];
				// Zero each QuantizeFloat channel's OWN bits, then write the decoded raw back. Zeroing
				// first is what makes this a test of the writer rather than of the copy; zeroing only
				// the channel's own bits is what keeps an IndirectQuantizeFloat channel sharing the
				// same frame from being wiped by a check that cannot rewrite it.
				for (uint32 Fr = 0; Fr < S.FrameCount; ++Fr)
				{
					const int32 BaseBit = (int32)Fr * S.StrideBytes * 8;
					for (const FChanRef& C : S.Chans)
					{
						const uint32 Keep = GetBits(Orig, 0, BaseBit + C.BitInFrame, C.NumBits);
						PutBits(Fresh, 0, BaseBit + C.BitInFrame, C.NumBits, 0);
						PutBits(Fresh, 0, BaseBit + C.BitInFrame, C.NumBits, Keep);
					}
				}
				bool bSame = true;
				for (int32 I = 0; I < S.BlockBytes && bSame; ++I) { bSame = (Fresh[I] == Orig[I]); }
				if (bSame) { ++RoundTripOk; }
				else
				{
					++RoundTripBad;
					if (FirstProblem.IsEmpty())
					{
						FirstProblem = FString::Printf(TEXT("sequence at 0x%X: the packed block did not survive a decode/re-encode, so the frame geometry is not right"), S.Off);
					}
				}
				// The bits after the last channel of a frame: zero on every frame measured. GUARDED,
				// because `S.Chans` holds the QuantizeFloat channels only while the frame's bit region
				// also carries the IndirectQuantizeFloat descriptors' widths (see FrameBits above). On
				// a sequence carrying an Indirect channel the "padding" would really be that channel's
				// bits, and this check would call a perfectly good file broken. No such sequence was
				// witnessed here (Indirect 0/20 sequences, LAWS.md F2), which is exactly why it is
				// guarded rather than assumed: the check is SKIPPED and counted, never faked.
				int32 Used = 0;
				for (const FChanRef& C : S.Chans) { Used += (int32)C.NumBits; }
				const int32 PadBits = (int32)S.FrameBits - Used;
				if (S.Counts[5] > 0)
				{
					++SeqPadSkippedIndirect;
				}
				else if (PadBits > 0)
				{
					for (uint32 Fr = 0; Fr < S.FrameCount; ++Fr)
					{
						++PadChecked;
						const int32 At = (int32)Fr * S.StrideBytes * 8 + RawFloats * 32 + Used;
						bool bZero = true;
						for (int32 B = 0; B < PadBits && bZero; ++B) { bZero = (GetBits(Orig, 0, At + B, 1) == 0); }
						if (!bZero) { ++PadNonZero; }
					}
				}
			}
		}
	}

	const uint32 SysPages = PageCountFromFlags(Img.SysFlags);
	const int32 BlockMap = (int32)Ptr28(Img.U32(0x08));
	bool bPageRecordAgrees = false;
	if (Img.In(BlockMap + 8, 4))
	{
		const uint32 W = Img.U32(BlockMap + 8);
		bPageRecordAgrees = ((W & 0xFFu) == SysPages) && (((W >> 8) & 0xFFu) == PageCountFromFlags(Img.GfxFlags));
	}

	const bool bOk = (SeqAmbiguous == 0) && (RoundTripBad == 0) && (QuantumNonPositive == 0)
		&& (BitsOutOfRange == 0) && (PadNonZero == 0) && bPageRecordAgrees && (SeqUnique > 0);

	FString J = TEXT("{");
	J += FString::Printf(TEXT("\"ok\":%s"), *JBool(bOk));
	J += TEXT(",\"file\":") + JStr(P);
	J += FString::Printf(TEXT(",\"version\":%u,\"sysFlags\":\"0x%08X\",\"gfxFlags\":\"0x%08X\""), Img.Version, Img.SysFlags, Img.GfxFlags);
	J += FString::Printf(TEXT(",\"fileBytes\":%d,\"systemSegmentBytes\":%d,\"systemPages\":%u"), Img.File.Num(), Img.Sys.Num(), SysPages);
	J += FString::Printf(TEXT(",\"blockmapPageRecordAgrees\":%s"), *JBool(bPageRecordAgrees));
	J += FString::Printf(TEXT(",\"clips\":%d,\"animations\":%d"), NClips, Anims.Num());
	J += FString::Printf(TEXT(",\"sequencesLocated\":%d,\"sequencesAmbiguous\":%d"), SeqUnique, SeqAmbiguous);
	J += FString::Printf(TEXT(",\"quantizeFloatChannels\":%d,\"quantumNonPositive\":%d,\"numBitsOutOfRange\":%d"), QzChannels, QuantumNonPositive, BitsOutOfRange);
	J += FString::Printf(TEXT(",\"packedBlocksRoundTripped\":%d,\"packedBlocksBroken\":%d"), RoundTripOk, RoundTripBad);
	J += FString::Printf(TEXT(",\"framesPadChecked\":%d,\"framesWithNonZeroPad\":%d,\"sequencesPadCheckSkippedIndirect\":%d"),
		PadChecked, PadNonZero, SeqPadSkippedIndirect);
	J += FString::Printf(TEXT(",\"poolStaticQuaternion\":%d,\"poolStaticVector3\":%d,\"poolStaticFloat\":%d,\"poolRawFloat\":%d"),
		Pools[0], Pools[1], Pools[2], Pools[3]);
	J += FString::Printf(TEXT(",\"poolQuantizeFloat\":%d,\"poolIndirect\":%d,\"poolInlinePool6\":%d,\"poolCachedQuat1\":%d,\"poolCachedQuat2\":%d"),
		Pools[4], Pools[5], Pools[6], Pools[7], Pools[8]);
	J += TEXT(",\"firstProblem\":") + JStr(FirstProblem);
	J += TEXT(",\"note\":\"Container laws measured at denominator 10 .ycd binaries / 20 sequences / 1,417 channels (maintainer lane `ycd_pack`). No game-shipped .ycd binary was available to measure, and nothing this lane writes has been loaded by the game.\"");
	J += TEXT("}");
	return J;
}

// ---- PackYcdBinary -------------------------------------------------------------------------------
FString URudeToolset::PackYcdBinary(const FString& XmlPath, const FString& OutYcdPath, const FString& Options)
{
	using namespace RudeYcdPk;
	FPkOpts Opt;
	ParseOpts(Options, Opt);
	const FString Xml = XmlPath.TrimStartAndEnd();
	const FString Dest = OutYcdPath.TrimStartAndEnd();
	if (Xml.IsEmpty()) { return Fail(TEXT("give XmlPath - the clip-dictionary XML to pack (ExportClipDictionary writes one)")); }
	if (Dest.IsEmpty()) { return Fail(TEXT("give OutYcdPath, e.g. C:/out/mydict.ycd")); }
	if (Opt.TemplatePath.TrimStartAndEnd().IsEmpty())
	{
		return Fail(TEXT("give Options \"template=<path to a .ycd BINARY>\" - this lane packs by DONOR REPACK. It patches the donor's own image in place and re-wraps it with the donor's own header; it does not assemble a container, so there is nothing to write into without one"));
	}
	if (!FPaths::FileExists(Opt.TemplatePath)) { return Fail(FString::Printf(TEXT("no donor binary at %s"), *Opt.TemplatePath)); }
	if (!Opt.Expect.IsEmpty() && Opt.Expect != TEXT("identical") && Opt.Expect != TEXT("edited"))
	{
		return Fail(FString::Printf(TEXT("expect=%s is not a claim this tool knows. Use expect=identical (packing an untouched export must reproduce the donor's file bytes) or expect=edited (this pack must have changed at least one channel). A claim it does not understand is refused rather than quietly ignored, because an ignored claim is a gate that cannot fail"), *Opt.Expect));
	}

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Xml)) { return Fail(FString::Printf(TEXT("cannot read %s"), *Xml)); }
	TArray<FString> Lines;
	Raw.ParseIntoArray(Lines, TEXT("\n"), false);

	FYcdImage Img;
	FString Err;
	if (!LoadYcd(Opt.TemplatePath, Img, Err)) { return Fail(Err); }
	TArray<FAnimRef> Anims;
	int32 NClips = 0;
	if (!WalkDictionary(Img, Anims, NClips, Err)) { return Fail(Err); }

	TArray<FXmlAnim> XAnims;
	int32 UnknownXmlItems = 0;
	if (!ScanXml(Lines, XAnims, UnknownXmlItems, Err)) { return Fail(Err); }

	// STRUCTURE IS NOT AUTHORED HERE. A pack that changes a count is a container assembly, which this
	// lane did not build - so a mismatch is refused by name rather than half-applied.
	if (XAnims.Num() != Anims.Num())
	{
		return Fail(FString::Printf(TEXT("the XML holds %d animations and the donor holds %d - this lane patches a donor in place and cannot add or remove one"),
			XAnims.Num(), Anims.Num()));
	}
	for (int32 A = 0; A < Anims.Num(); ++A)
	{
		if (XAnims[A].Seqs.Num() != Anims[A].Seqs.Num())
		{
			return Fail(FString::Printf(TEXT("animation %d: the XML holds %d sequences and the donor holds %d - a sequence count change is a container assembly this lane does not do"),
				A, XAnims[A].Seqs.Num(), Anims[A].Seqs.Num()));
		}
	}

	TArray<uint8> Patched = Img.Sys;
	// One byte-mask over the image marks every byte this pack DECLARED it would write. It is cheaper
	// and stricter than a list of ranges: the attribution below asks the mask about each differing
	// byte, so a write nobody declared cannot hide behind an overlapping range.
	TArray<uint8> Declared;
	Declared.SetNumZeroed(Patched.Num());
	auto Declare = [&Declared](int32 From, int32 To)
	{
		for (int32 I = FMath::Max(0, From); I < FMath::Min(To, Declared.Num()); ++I) { Declared[I] = 1; }
	};

	// TWO PASSES, because a refusal has to be ATOMIC. Pass 1 validates every channel and STAGES the
	// bytes it would write; nothing is committed to the image and no file is written until every
	// channel has been accepted. A half-applied pack - a new Quantum committed over the donor's old
	// raws, because the value list was rejected afterwards - silently rescales that channel, which is
	// worse than either edit alone. The repo's own precedent (ExportNewMlo) computes ok before
	// anything is written; this does the same, and reports `written` so no caller has to infer it.
	struct FStaged
	{
		int32 Desc = -1;              // -1 = this channel asks for no descriptor edit
		float Q = 0.f, O = 0.f;
		int32 PackedBase = 0, StrideBytes = 0, BitInFrame = 0;
		uint32 NumBits = 0;
		TArray<uint32> Raws;          // empty = this channel asks for no value edit
	};
	TArray<FStaged> Staged;
	int32 ChannelsMatched = 0, ChannelsAuthored = 0, ChannelsChanged = 0, DescriptorsWritten = 0, ChannelsUnchanged = 0;
	int32 ChannelsRefused = 0, RootGated = 0, ShapeMismatch = 0, FrameCountMismatch = 0;
	int32 CarriedStatic = 0, CarriedRawFloat = 0, CarriedIndirect = 0, CarriedInlinePool6 = 0;
	int32 FramesWritten = 0, RawsOutOfRange = 0, XmlChannelsWithoutDonor = 0;
	FString FirstRefusal;
	auto Refuse = [&FirstRefusal](const FString& Why) { if (FirstRefusal.IsEmpty()) { FirstRefusal = Why; } };

	for (int32 AI = 0; AI < Anims.Num(); ++AI)
	{
		const FAnimRef& A = Anims[AI];
		for (int32 SI = 0; SI < A.Seqs.Num(); ++SI)
		{
			const FSeqRef& S = A.Seqs[SI];
			if (S.Candidates != 1)
			{
				return Fail(FString::Printf(TEXT("animation %d sequence %d (at 0x%X): %d count-table candidates survived, want exactly 1. An ambiguous sequence is refused rather than patched at a guessed offset"),
					AI, SI, S.Off, S.Candidates));
			}
			CarriedStatic += S.Counts[0] + S.Counts[1] + S.Counts[2];
			CarriedRawFloat += S.Counts[3];
			CarriedIndirect += S.Counts[5];
			CarriedInlinePool6 += S.Counts[6];

			const FXmlSeq& XS = XAnims[AI].Seqs[SI];
			for (const FChanRef& C : S.Chans)
			{
				if (!XS.Items.IsValidIndex(C.Item) || !XS.Items[C.Item].IsValidIndex(C.Comp))
				{
					++XmlChannelsWithoutDonor;
					Refuse(FString::Printf(TEXT("animation %d sequence %d: the donor's channel (bone %d, track %d, component %d) has no matching channel in the XML at SequenceData item %d"),
						AI, SI, C.Bone, C.Track, C.Comp, C.Item));
					++ChannelsRefused;
					continue;
				}
				const FXmlChan& X = XS.Items[C.Item][C.Comp];
				++ChannelsMatched;
				if (X.Type != TEXT("QuantizeFloat"))
				{
					++ShapeMismatch;
					++ChannelsRefused;
					Refuse(FString::Printf(TEXT("animation %d sequence %d bone %d track %d component %d: the donor holds a QuantizeFloat and the XML holds a '%s'. A shape change resizes the pools and is a container assembly this lane does not do"),
						AI, SI, C.Bone, C.Track, C.Comp, *X.Type));
					continue;
				}
				// The root-translation gate. Bone 0 / track 0 is the channel a prior in-game test
				// crashed on, so it is only written when asked for by name.
				if (C.Bone == 0 && C.Track == 0 && !Opt.bAuthorRoot) { ++RootGated; continue; }

				float Q = C.Quantum;
				float O = C.Offset;
				const bool bWantDesc = Opt.bDescriptors
					&& ((X.bHasQuantum && (float)X.Quantum != C.Quantum) || (X.bHasOffset && (float)X.Offset != C.Offset));
				if (bWantDesc)
				{
					if (X.bHasQuantum) { Q = (float)X.Quantum; }
					if (X.bHasOffset) { O = (float)X.Offset; }
					if (!(Q > 0.f))
					{
						++ChannelsRefused;
						Refuse(FString::Printf(TEXT("animation %d sequence %d bone %d track %d component %d: the XML asks for Quantum %g. A quantum that is not positive is divided by on every frame, so it is refused before anything is written"),
							AI, SI, C.Bone, C.Track, C.Comp, Q));
						continue;
					}
				}

				FStaged St;
				if (bWantDesc)
				{
					St.Desc = C.Desc;
					St.Q = Q;
					St.O = O;
				}

				if (Opt.bValues && X.Values.Num() > 0)
				{
					if (X.Values.Num() != (int32)S.FrameCount)
					{
						++FrameCountMismatch;
						++ChannelsRefused;
						Refuse(FString::Printf(TEXT("animation %d sequence %d bone %d track %d component %d: the XML carries %d values and the sequence holds %u frames. A frame-count change resizes the packed block and is a container assembly this lane does not do"),
							AI, SI, C.Bone, C.Track, C.Comp, X.Values.Num(), S.FrameCount));
						continue;
					}
					const uint32 Limit = (C.NumBits >= 32) ? 0xFFFFFFFFu : ((1u << C.NumBits) - 1u);
					bool bFits = true;
					TArray<uint32> Raws;
					Raws.Reserve(X.Values.Num());
					for (const double V : X.Values)
					{
						// raw = round((v - Offset) / Quantum), ties away from zero, spelled without
						// depending on a rounding helper's name. A value below the channel's own
						// Offset has no non-negative raw and is refused, never clamped to 0.
						const double Xd = ((double)V - (double)O) / (double)Q;
						if (Xd < -0.5) { bFits = false; break; }
						const double R = FMath::FloorToDouble(Xd + 0.5);
						if (R < 0.0 || R > (double)Limit) { bFits = false; break; }
						Raws.Add((uint32)R);
					}
					if (!bFits)
					{
						++RawsOutOfRange;
						++ChannelsRefused;
						Refuse(FString::Printf(TEXT("animation %d sequence %d bone %d track %d component %d: a value does not fit the donor channel's own %u bits. Widening the channel changes the frame stride and every offset after it, which is a container assembly this lane does not do"),
							AI, SI, C.Bone, C.Track, C.Comp, C.NumBits));
						continue;
					}
					St.PackedBase = S.Packed;
					St.StrideBytes = S.StrideBytes;
					St.BitInFrame = C.BitInFrame;
					St.NumBits = C.NumBits;
					St.Raws = MoveTemp(Raws);
				}
				if (St.Desc >= 0 || St.Raws.Num() > 0)
				{
					Staged.Add(MoveTemp(St));
					++ChannelsAuthored;
				}
			}
		}
	}

	// THE ATOMIC REFUSAL. One refused channel and this pack commits NOTHING: no byte reaches the
	// image, no file is written, `written` says so, and ok is false. Nothing partial ever lands.
	const bool bRefused = (ChannelsRefused > 0);
	int32 BytesDiffering = 0, BytesOutside = 0, FirstDiff = -1, OutFileBytes = 0;
	bool bNoOp = false, bFileIdenticalToDonor = false, bWritten = false;
	if (!bRefused)
	{
		// PASS 2 - commit. Every staged channel was accepted in pass 1, so this loop cannot refuse.
		for (const FStaged& St : Staged)
		{
			bool bChanged = false;
			if (St.Desc >= 0)
			{
				float OldQ = 0.f, OldO = 0.f;
				FMemory::Memcpy(&OldQ, Patched.GetData() + St.Desc + 4, 4);
				FMemory::Memcpy(&OldO, Patched.GetData() + St.Desc + 8, 4);
				FMemory::Memcpy(Patched.GetData() + St.Desc + 4, &St.Q, 4);
				FMemory::Memcpy(Patched.GetData() + St.Desc + 8, &St.O, 4);
				Declare(St.Desc + 4, St.Desc + 12);
				++DescriptorsWritten;
				bChanged = (FMemory::Memcmp(&OldQ, &St.Q, 4) != 0) || (FMemory::Memcmp(&OldO, &St.O, 4) != 0);
			}
			for (int32 Fr = 0; Fr < St.Raws.Num(); ++Fr)
			{
				const int32 Bit = Fr * St.StrideBytes * 8 + St.BitInFrame;
				// "changed" is asked of the VALUE, not of the byte. Channels share bytes in a
				// bit-packed frame, so a byte-level answer would credit a neighbour's edit to this
				// channel - which is how a write-back count came to read as a change count.
				if (GetBits(Patched, St.PackedBase, Bit, St.NumBits) != St.Raws[Fr]) { bChanged = true; }
				PutBits(Patched, St.PackedBase, Bit, St.NumBits, St.Raws[Fr]);
				Declare(St.PackedBase + (Bit >> 3), St.PackedBase + ((Bit + (int32)St.NumBits + 7) >> 3));
			}
			FramesWritten += St.Raws.Num();
			if (bChanged) { ++ChannelsChanged; } else { ++ChannelsUnchanged; }
		}

		// THE MEASURE, computed rather than asserted: diff the patched image against the donor's own,
		// and account for every byte that moved. A byte outside a declared extent is a write nobody
		// authored.
		for (int32 I = 0; I < Patched.Num(); ++I)
		{
			if (Patched[I] != Img.Sys[I])
			{
				++BytesDiffering;
				if (FirstDiff < 0) { FirstDiff = I; }
				if (Declared[I] == 0) { ++BytesOutside; }
			}
		}
		bNoOp = (BytesDiffering == 0);

		TArray<uint8> OutFile;
		if (bNoOp)
		{
			// Byte identity is not a hope about a compressor here: the donor's own file bytes are copied.
			OutFile = Img.File;
		}
		else if (!WrapYcd(Img, Patched, OutFile, Err))
		{
			return Fail(Err);
		}
		if (!FFileHelper::SaveArrayToFile(OutFile, *Dest))
		{
			return Fail(FString::Printf(TEXT("cannot write %s"), *Dest));
		}
		bWritten = true;
		OutFileBytes = OutFile.Num();
		bFileIdenticalToDonor = (OutFile.Num() == Img.File.Num());
		for (int32 I = 0; I < OutFile.Num() && bFileIdenticalToDonor; ++I) { bFileIdenticalToDonor = (OutFile[I] == Img.File[I]); }
	}

	// "authored but wrote nothing" is the shape a silent scan failure produces: channels were matched
	// and none was even staged. A genuine no-op (staged, written back, nothing moved) is not that.
	const bool bAuthoredNothing = (ChannelsMatched > 0) && (ChannelsAuthored == 0) && (RootGated < ChannelsMatched);

	// The caller's claim about the outcome, checked here so a gate row can go red on its own.
	bool bExpectMet = true;
	if (Opt.Expect == TEXT("identical"))
	{
		bExpectMet = bWritten && bNoOp && bFileIdenticalToDonor;
	}
	else if (Opt.Expect == TEXT("edited"))
	{
		bExpectMet = bWritten && (BytesDiffering > 0) && (ChannelsChanged > 0) && (BytesOutside == 0);
	}

	// An <Item> the scanner could not place is an unread part of the document, and an unread part
	// of the document is a channel that may have been tied to the wrong descriptor. It is inside ok.
	const bool bOk = !bRefused && bWritten && (BytesOutside == 0) && !bAuthoredNothing
		&& (XmlChannelsWithoutDonor == 0) && (ChannelsMatched > 0) && (UnknownXmlItems == 0) && bExpectMet;

	FString J = TEXT("{");
	J += FString::Printf(TEXT("\"ok\":%s"), *JBool(bOk));
	J += TEXT(",\"file\":") + JStr(Dest);
	J += TEXT(",\"xml\":") + JStr(Xml);
	J += TEXT(",\"template\":") + JStr(Opt.TemplatePath);
	J += FString::Printf(TEXT(",\"version\":%u,\"systemSegmentBytes\":%d,\"fileBytes\":%d"), Img.Version, Img.Sys.Num(), OutFileBytes);
	J += FString::Printf(TEXT(",\"written\":%s"), *JBool(bWritten));
	J += TEXT(",\"expect\":") + JStr(Opt.Expect);
	J += FString::Printf(TEXT(",\"expectationMet\":%s"), *JBool(bExpectMet));
	J += FString::Printf(TEXT(",\"animations\":%d,\"clips\":%d"), Anims.Num(), NClips);
	// channelsAuthored counts write-BACKS (channels this pack staged bytes for); channelsChanged
	// counts the channels whose VALUE actually moved. They differ on a no-op, which is the point.
	J += FString::Printf(TEXT(",\"channelsMatched\":%d,\"channelsAuthored\":%d,\"channelsChanged\":%d,\"channelsUnchanged\":%d,\"descriptorsWritten\":%d,\"framesWritten\":%d"),
		ChannelsMatched, ChannelsAuthored, ChannelsChanged, ChannelsUnchanged, DescriptorsWritten, FramesWritten);
	J += FString::Printf(TEXT(",\"channelsRefused\":%d,\"shapeMismatch\":%d,\"frameCountMismatch\":%d,\"rawsOutOfRange\":%d,\"xmlChannelsWithoutDonor\":%d"),
		ChannelsRefused, ShapeMismatch, FrameCountMismatch, RawsOutOfRange, XmlChannelsWithoutDonor);
	J += FString::Printf(TEXT(",\"rootChannelsGated\":%d,\"xmlItemsInUnknownList\":%d"), RootGated, UnknownXmlItems);
	J += FString::Printf(TEXT(",\"carriedStatic\":%d,\"carriedRawFloat\":%d,\"carriedIndirect\":%d,\"carriedInlinePool6\":%d"),
		CarriedStatic, CarriedRawFloat, CarriedIndirect, CarriedInlinePool6);
	J += FString::Printf(TEXT(",\"segmentBytesDiffering\":%d,\"bytesDifferingOutsideDeclaredExtents\":%d,\"firstDifferingOffset\":%d"),
		BytesDiffering, BytesOutside, FirstDiff);
	J += FString::Printf(TEXT(",\"noOp\":%s,\"byteIdenticalToTemplate\":%s,\"reDeflated\":%s,\"authoredButWroteNothing\":%s"),
		*JBool(bNoOp), *JBool(bFileIdenticalToDonor), *JBool(!bNoOp), *JBool(bAuthoredNothing));
	J += TEXT(",\"firstRefusal\":") + JStr(FirstRefusal);
	J += TEXT(",\"note\":\"Donor repack, size-preserving only: this lane EDITS a .ycd binary it is handed and cannot BUILD one - nothing in RUDE, and nothing in the maintainer's own exporter in either language, turns a clip-dictionary XML into a .ycd container from scratch. A refusal is atomic: one refused channel and no file is written (see written). A no-op pack copies the donor's file bytes so byte identity does not depend on a compressor; an edited pack re-deflates, so its compressed bytes will differ from the donor's everywhere and the byte measure is on the inflated segment. Nothing this lane writes has been loaded by the game.\"");
	J += TEXT("}");
	return J;
}

// ---- CompareYcdBinary ----------------------------------------------------------------------------
FString URudeToolset::CompareYcdBinary(const FString& APath, const FString& BPath)
{
	using namespace RudeYcdPk;
	const FString PA = APath.TrimStartAndEnd();
	const FString PB = BPath.TrimStartAndEnd();
	if (PA.IsEmpty() || PB.IsEmpty()) { return Fail(TEXT("give both .ycd binary paths to compare")); }

	FYcdImage A, B;
	FString Err;
	if (!LoadYcd(PA, A, Err)) { return Fail(FString::Printf(TEXT("A: %s"), *Err)); }
	if (!LoadYcd(PB, B, Err)) { return Fail(FString::Printf(TEXT("B: %s"), *Err)); }

	bool bFileIdentical = (A.File.Num() == B.File.Num());
	for (int32 I = 0; I < A.File.Num() && bFileIdentical; ++I) { bFileIdentical = (A.File[I] == B.File[I]); }

	if (A.Sys.Num() != B.Sys.Num())
	{
		return Fail(FString::Printf(TEXT("the two system segments are %d and %d bytes. A size change is not a channel edit, so there is nothing to attribute"),
			A.Sys.Num(), B.Sys.Num()));
	}

	// Attribute every differing byte to A's own declared channel extents: a 12-byte descriptor, or a
	// sequence's packed block. A byte that moved anywhere else is a structural difference.
	TArray<FAnimRef> Anims;
	int32 NClips = 0;
	if (!WalkDictionary(A, Anims, NClips, Err)) { return Fail(FString::Printf(TEXT("A: %s"), *Err)); }

	TArray<uint8> Kind;                      // 0 elsewhere, 1 descriptor, 2 packed block
	Kind.SetNumZeroed(A.Sys.Num());
	for (const FAnimRef& An : Anims)
	{
		for (const FSeqRef& S : An.Seqs)
		{
			if (S.Candidates != 1) { continue; }
			for (int32 I = 0; I < S.BlockBytes; ++I)
			{
				if (Kind.IsValidIndex(S.Packed + I)) { Kind[S.Packed + I] = 2; }
			}
			for (const FChanRef& C : S.Chans)
			{
				for (int32 I = 4; I < 12; ++I) { if (Kind.IsValidIndex(C.Desc + I)) { Kind[C.Desc + I] = 1; } }
			}
		}
	}

	int32 Differing = 0, InDescriptor = 0, InPacked = 0, Elsewhere = 0, FirstDiff = -1;
	for (int32 I = 0; I < A.Sys.Num(); ++I)
	{
		if (A.Sys[I] == B.Sys[I]) { continue; }
		++Differing;
		if (FirstDiff < 0) { FirstDiff = I; }
		if (Kind[I] == 1) { ++InDescriptor; }
		else if (Kind[I] == 2) { ++InPacked; }
		else { ++Elsewhere; }
	}

	// ok is COMPUTED: the two images are the same, or every byte that moved sits inside a channel this
	// reader can name. Anything else is a structural difference and this comparator says so.
	const bool bOk = (Differing == 0) || (Elsewhere == 0);
	FString J = TEXT("{");
	J += FString::Printf(TEXT("\"ok\":%s"), *JBool(bOk));
	J += TEXT(",\"a\":") + JStr(PA);
	J += TEXT(",\"b\":") + JStr(PB);
	J += FString::Printf(TEXT(",\"fileBytesIdentical\":%s,\"aFileBytes\":%d,\"bFileBytes\":%d"), *JBool(bFileIdentical), A.File.Num(), B.File.Num());
	J += FString::Printf(TEXT(",\"systemSegmentBytes\":%d,\"segmentIdentical\":%s"), A.Sys.Num(), *JBool(Differing == 0));
	J += FString::Printf(TEXT(",\"segmentBytesDiffering\":%d,\"inChannelDescriptor\":%d,\"inPackedBlock\":%d,\"elsewhere\":%d,\"firstDifferingOffset\":%d"),
		Differing, InDescriptor, InPacked, Elsewhere, FirstDiff);
	J += TEXT(",\"note\":\"A compressed .ycd is not canonical - two files holding the same image can differ in every compressed byte - so the comparison that means anything is on the inflated system segment, and that is what these counts are over.\"");
	J += TEXT("}");
	return J;
}
