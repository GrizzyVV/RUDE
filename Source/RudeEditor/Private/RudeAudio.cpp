// RUDE - RAGE <-> Unreal Development Environment
// WP10 AUDIO lane: ExportAwc / ImportAwc.
//
// CLEAN-ROOM. The container laws below are measured on the game's own files (corpus chicken.awc.xml,
// rapid_distant.awc.xml, halloween_2015.awc.xml + ROUT's population census; see
// scratchpad/wp10/audio_passthrough/LAWS.md) and ported from ROUT's own awc_write.py / awc2xml.py
// (house code, Matt's). No CodeWalker / Sollumz code was read.
//
// THE PLAINTEXT 'ADAT' CONTAINER (little-endian; ROUT round-trips 5,642/5,642 plaintext files byte-exact):
//   +0x00 u32 magic 'ADAT'   +0x04 u16 version(1)   +0x06 u16 flags   +0x08 u32 streamCount   +0x0C u32 dataStart
//   per-stream table : [u16 chunkStartIndex x N] ONLY if flags bit 0, then [u32 streamWord x N] - two arrays
//                      streamWord = (chunkCount << 29) | (joaat(name) & 0x1FFFFFFF)   (STREAM_WORD_LAW, 97,024/97,024)
//   chunk index table: one u64 per chunk until dataStart: offset = bits 0..27 | size = bits 28..55 | type = bits 56..63
//   chunk bodies     : tile [dataStart, fileSize) end-to-end, no alignment (pads measured 0 on both samples;
//                      3,942 B of zero pad over 739 files in ROUT's census), file order = table order
//   type tags        : data 0x55, format 0xFA, peak 0x36 (joaat(name) & 0xFF); stream-table shape uses 0x48 + 0xA3
//   format body 24 B : u32 Samples | i32 LoopPoint(-1) | u16 SampleRate | i16 Headroom | u16 PlayBegin | u16 PlayEnd
//                      | u16 LoopBegin | u8 Unk12(0) | u8 Codec (0x00 PCM16 = 16.000 bits/sample, 0x04 ADPCM ~4.008)
//                      | u16 PeakUnk | u16 Unk16(0)
//   flags            : bit 0 = u16 table present; bit 2 = 0x48 stream-table shape (ADPCM); bit 3 = payload
//                      encrypted (99.982%). 0xFF01 = the game's 50 plaintext-PCM files - what ExportAwc writes.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Audio.h"
#include "Factories/SoundFactory.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "XmlFile.h"

namespace RudeAwc
{
	static const uint32 MAGIC = 0x54414441u;              // 'A','D','A','T' as a little-endian u32
	static const int32 HEADER = 16;
	static const uint8 T_PEAK = 0x36, T_DATA = 0x55, T_FORMAT = 0xFA, T_STREAMTABLE = 0x48;
	static const uint8 CODEC_PCM16 = 0x00, CODEC_ADPCM = 0x04;
	static const uint16 FLAGS_PLAIN_PCM = 0xFF01;          // bit 0 only: u16 table present, payload plaintext (50 game files)
	static const uint32 MAX28 = 0x0FFFFFFFu;

	static uint16 RdU16(const TArray<uint8>& B, int32 O) { return (uint16)(B[O] | (B[O + 1] << 8)); }
	static uint32 RdU32(const TArray<uint8>& B, int32 O) { return (uint32)B[O] | ((uint32)B[O + 1] << 8) | ((uint32)B[O + 2] << 16) | ((uint32)B[O + 3] << 24); }
	static uint64 RdU64(const TArray<uint8>& B, int32 O) { return (uint64)RdU32(B, O) | ((uint64)RdU32(B, O + 4) << 32); }
	static void PutU16(TArray<uint8>& B, int32 O, uint16 V) { B[O] = V & 0xFF; B[O + 1] = (V >> 8) & 0xFF; }
	static void PutU32(TArray<uint8>& B, int32 O, uint32 V) { for (int32 k = 0; k < 4; ++k) { B[O + k] = (uint8)((V >> (8 * k)) & 0xFF); } }
	static void PutU64(TArray<uint8>& B, int32 O, uint64 V) { for (int32 k = 0; k < 8; ++k) { B[O + k] = (uint8)((V >> (8 * k)) & 0xFF); } }

	struct FChunk { uint8 Type = 0; uint32 Offset = 0; uint32 Size = 0; };
	struct FFormat
	{
		uint32 Samples = 0; int32 LoopPoint = -1; uint16 SampleRate = 0; int16 Headroom = 0;
		uint16 PlayBegin = 0, PlayEnd = 0, LoopBegin = 0; uint8 Unk12 = 0, Codec = 0; uint16 PeakUnk = 0, Unk16 = 0;
		int32 BodySize = 0;
	};
	struct FParsed
	{
		uint16 Version = 0, Flags = 0; int32 Width = 4; uint32 DataStart = 0; int32 PadBytes = 0;
		TArray<uint16> ChunkStart; TArray<uint32> StreamWord; TArray<FChunk> Chunks;
		bool PayloadEncrypted() const { return ((Flags >> 3) & 1) != 0; }
		bool StreamTableShape() const { return ((Flags >> 2) & 1) != 0; }
	};

	// The reader: header, the two per-stream arrays, the chunk table, and THE TILING CHECK (spans meet
	// end-to-end across [dataStart, size) with every gap zero) - a wrong offset/size/width cannot pass.
	static bool Parse(const TArray<uint8>& F, FParsed& P, FString& Err)
	{
		if (F.Num() < HEADER) { Err = TEXT("shorter than the 16-byte header"); return false; }
		if (RdU32(F, 0) != MAGIC) { Err = TEXT("no 'ADAT' magic: whole-file encrypted, or not an awc (a counted refusal, not a parse failure)"); return false; }
		P.Version = RdU16(F, 4); P.Flags = RdU16(F, 6);
		const uint32 N = RdU32(F, 8); P.DataStart = RdU32(F, 12);
		P.Width = 4 + 2 * (P.Flags & 1);
		const int64 Tbl = (int64)HEADER + (int64)N * P.Width;
		if (N > 100000u || Tbl > (int64)P.DataStart || ((P.DataStart - Tbl) % 8) != 0 || P.DataStart > (uint32)F.Num())
		{
			Err = FString::Printf(TEXT("header does not tile: %u streams at width %d put the chunk table at %lld, dataStart %u, file %d B"), N, P.Width, Tbl, P.DataStart, F.Num());
			return false;
		}
		int32 O = HEADER;
		if (P.Width == 6) { for (uint32 i = 0; i < N; ++i) { P.ChunkStart.Add(RdU16(F, O)); O += 2; } }
		for (uint32 i = 0; i < N; ++i) { P.StreamWord.Add(RdU32(F, O)); O += 4; }
		while (O + 8 <= (int32)P.DataStart)
		{
			const uint64 V = RdU64(F, O); O += 8;
			FChunk C; C.Type = (uint8)(V >> 56); C.Size = (uint32)((V >> 28) & MAX28); C.Offset = (uint32)(V & MAX28);
			P.Chunks.Add(C);
		}
		TArray<FChunk> Spans = P.Chunks;
		Spans.Sort([](const FChunk& A, const FChunk& B) { return A.Offset < B.Offset; });
		uint32 At = P.DataStart; P.PadBytes = 0;
		for (const FChunk& C : Spans)
		{
			if (C.Size == 0 || C.Offset < P.DataStart || (uint64)C.Offset + C.Size > (uint64)F.Num())
			{ Err = FString::Printf(TEXT("chunk type 0x%02x (off %u size %u) lies outside the data region [%u, %d)"), C.Type, C.Offset, C.Size, P.DataStart, F.Num()); return false; }
			if (C.Offset < At) { Err = FString::Printf(TEXT("chunk spans overlap at offset %u"), C.Offset); return false; }
			for (uint32 k = At; k < C.Offset; ++k) { if (F[k] != 0) { Err = FString::Printf(TEXT("inter-chunk gap at %u carries non-zero bytes the model does not account for"), At); return false; } }
			P.PadBytes += (int32)(C.Offset - At);
			At = C.Offset + C.Size;
		}
		if (At != (uint32)F.Num()) { Err = FString::Printf(TEXT("chunks end at %u but the file is %d B - the table does not tile the file"), At, F.Num()); return false; }
		return true;
	}

	static bool ReadFormat(const TArray<uint8>& F, const FChunk& C, FFormat& O)
	{
		if (C.Size != 20 && C.Size != 24) { return false; }
		const int32 b = (int32)C.Offset;
		O.BodySize = (int32)C.Size;
		O.Samples = RdU32(F, b); O.LoopPoint = (int32)RdU32(F, b + 4); O.SampleRate = RdU16(F, b + 8); O.Headroom = (int16)RdU16(F, b + 10);
		O.PlayBegin = RdU16(F, b + 12); O.PlayEnd = RdU16(F, b + 14); O.LoopBegin = RdU16(F, b + 16); O.Unk12 = F[b + 18]; O.Codec = F[b + 19];
		if (C.Size == 24) { O.PeakUnk = RdU16(F, b + 20); O.Unk16 = RdU16(F, b + 22); }
		return true;
	}
	static void WriteFormat(TArray<uint8>& B, int32 b, const FFormat& I)
	{
		PutU32(B, b, I.Samples); PutU32(B, b + 4, (uint32)I.LoopPoint); PutU16(B, b + 8, I.SampleRate); PutU16(B, b + 10, (uint16)I.Headroom);
		PutU16(B, b + 12, I.PlayBegin); PutU16(B, b + 14, I.PlayEnd); PutU16(B, b + 16, I.LoopBegin); B[b + 18] = I.Unk12; B[b + 19] = I.Codec;
		PutU16(B, b + 20, I.PeakUnk); PutU16(B, b + 22, I.Unk16);
	}

	// The engine's own WAV import (USoundFactory, AudioEditor) over an in-memory RIFF/WAVE. Overwrite is
	// suppressed to "use existing settings" so a re-run refills the asset without a dialog.
	static USoundWave* ImportWav(const TArray<uint8>& Wav, const FString& DestFolder, const FString& AssetName, FString& Err)
	{
		const FString PkgName = DestFolder / AssetName;
		if (!FPackageName::IsValidLongPackageName(PkgName)) { Err = FString::Printf(TEXT("invalid package name %s"), *PkgName); return nullptr; }
		UPackage* Pkg = CreatePackage(*PkgName);
		Pkg->FullyLoad();
		USoundFactory* Factory = NewObject<USoundFactory>();
		Factory->bAutoCreateCue = false;
		Factory->SuppressImportDialogs();   // the option enum is protected in 5.8; this is the public switch
		const uint8* Buf = Wav.GetData();
		UObject* Obj = Factory->FactoryCreateBinary(USoundWave::StaticClass(), Pkg, FName(*AssetName), RF_Public | RF_Standalone, nullptr, TEXT("WAV"), Buf, Buf + Wav.Num(), GWarn);
		USoundWave* SW = Cast<USoundWave>(Obj);
		if (!SW) { Err = FString::Printf(TEXT("USoundFactory refused the WAV for %s (see LogAudio / LogFactory)"), *AssetName); return nullptr; }
		FAssetRegistryModule::AssetCreated(SW);
		SW->MarkPackageDirty();
		return SW;
	}

	static FString SafeName(const FString& In)
	{
		FString O; O.Reserve(In.Len());
		for (TCHAR C : In) { O.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_')); }
		return O.IsEmpty() ? FString(TEXT("_")) : O;
	}
}

// ---- ExportAwc ------------------------------------------------------------------------------
// USoundWave -> ONE plaintext .awc with ONE PCM16 stream. Layout (measured, LAWS.md): header 16 B, per-stream
// [u16 0][u32 word] (flags 0xFF01 -> width 6), three u64 table entries (data, format, peak - the game's own
// order in chicken.awc), bodies back-to-back from dataStart = 16 + 6 + 24 = 46 with no padding.
// PCM comes from USoundWave::GetImportedSoundWaveData (SoundWave.h:1345, WITH_EDITOR): it decodes the editor
// RawData payload through FWaveModInfo and REFUSES non-16-bit sources (SoundWave.cpp "Expected 16bit audio").
// Stereo is downmixed to mono (averaged) and REPORTED - the single-stream plaintext form has no measured
// stereo layout (ROUT: "channel-interleave layouts NOT SEARCHED"). Peak table: round(frames/8192) entries
// (min 1; fits 5/5 chicken PCM streams), each the window's max |sample| * 2 - the value semantics are
// UNMEASURED (marked in LAWS.md); Headroom / PeakUnk are written 0 (unmeasured, neutral).
// Self-check: the file is re-read by the same tiling reader and the decoded format must agree with the input.
FString URudeToolset::ExportAwc(const FString& SoundWaveAssetPath, const FString& OutAwcPath, const FString& StreamName)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
#if WITH_EDITOR
	USoundWave* Wave = LoadObject<USoundWave>(nullptr, *SoundWaveAssetPath);
	if (!Wave) { return Fail(FString::Printf(TEXT("SoundWave not found: %s"), *SoundWaveAssetPath)); }
	if (OutAwcPath.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("OutAwcPath is empty")); }
	TArray<uint8> Pcm; uint32 Rate = 0; uint16 Channels = 0;
	if (!Wave->GetImportedSoundWaveData(Pcm, Rate, Channels))
	{
		return Fail(TEXT("GetImportedSoundWaveData returned false: the wave has no editor source payload, or its samples are not 16-bit (the engine logs 'Expected 16bit audio' in LogAudio)"));
	}
	if (Channels < 1 || Channels > 2) { return Fail(FString::Printf(TEXT("%d channels: only mono or stereo sources"), (int32)Channels)); }
	if (Rate == 0 || Rate > 65535u) { return Fail(FString::Printf(TEXT("sample rate %u does not fit the format chunk's u16"), Rate)); }
	if (Pcm.Num() == 0 || (Pcm.Num() % (2 * Channels)) != 0) { return Fail(FString::Printf(TEXT("PCM byte count %d is not a whole number of %d-channel 16-bit frames"), Pcm.Num(), (int32)Channels)); }
	const int32 Frames = Pcm.Num() / (2 * Channels);
	TArray<uint8> Mono;
	if (Channels == 2)
	{
		Mono.SetNumUninitialized(Frames * 2);
		for (int32 i = 0; i < Frames; ++i)
		{
			const int16 L = (int16)(Pcm[i * 4] | (Pcm[i * 4 + 1] << 8));
			const int16 R = (int16)(Pcm[i * 4 + 2] | (Pcm[i * 4 + 3] << 8));
			const int16 M = (int16)(((int32)L + (int32)R) / 2);
			Mono[i * 2] = (uint8)(M & 0xFF); Mono[i * 2 + 1] = (uint8)((M >> 8) & 0xFF);
		}
	}
	const TArray<uint8>& Data = (Channels == 2) ? Mono : Pcm;
	const FString Name = StreamName.TrimStartAndEnd().IsEmpty() ? FPaths::GetBaseFilename(OutAwcPath).ToLower() : StreamName.TrimStartAndEnd().ToLower();
	const uint32 Hash = RudeJoaat(Name);

	// peak table (see the note above the function)
	const int32 PeakCount = FMath::Max(1, (int32)FMath::RoundToInt((double)Frames / 8192.0));
	const int32 Window = (Frames + PeakCount - 1) / PeakCount;
	TArray<uint16> Peaks; Peaks.SetNumZeroed(PeakCount);
	for (int32 i = 0; i < Frames; ++i)
	{
		const int16 S = (int16)(Data[i * 2] | (Data[i * 2 + 1] << 8));
		const int32 A = FMath::Min(65535, 2 * FMath::Abs((int32)S));
		uint16& Slot = Peaks[FMath::Min(i / Window, PeakCount - 1)];
		if ((int32)Slot < A) { Slot = (uint16)A; }
	}

	// layout
	const int32 NStreams = 1, NChunks = 3, Width = 6;
	const uint32 DataStart = (uint32)(RudeAwc::HEADER + NStreams * Width + NChunks * 8);   // 46
	const uint32 DataSize = (uint32)Data.Num(), FmtSize = 24, PeakSize = (uint32)PeakCount * 2;
	const uint32 DataOff = DataStart, FmtOff = DataOff + DataSize, PeakOff = FmtOff + FmtSize;
	const uint64 Total = (uint64)PeakOff + PeakSize;
	if (Total > RudeAwc::MAX28) { return Fail(FString::Printf(TEXT("file would be %llu B; chunk offsets/sizes are 28-bit fields (max %u)"), Total, RudeAwc::MAX28)); }
	TArray<uint8> Out; Out.AddZeroed((int32)Total);
	RudeAwc::PutU32(Out, 0, RudeAwc::MAGIC);
	RudeAwc::PutU16(Out, 4, 1);
	RudeAwc::PutU16(Out, 6, RudeAwc::FLAGS_PLAIN_PCM);
	RudeAwc::PutU32(Out, 8, (uint32)NStreams);
	RudeAwc::PutU32(Out, 12, DataStart);
	RudeAwc::PutU16(Out, 16, 0);                                                       // chunk-start index of stream 0
	const uint32 Word = ((uint32)NChunks << 29) | (Hash & 0x1FFFFFFFu);
	RudeAwc::PutU32(Out, 18, Word);
	auto Entry = [](uint8 Type, uint32 Off, uint32 Size) { return ((uint64)Type << 56) | ((uint64)(Size & RudeAwc::MAX28) << 28) | (uint64)(Off & RudeAwc::MAX28); };
	RudeAwc::PutU64(Out, 22, Entry(RudeAwc::T_DATA, DataOff, DataSize));
	RudeAwc::PutU64(Out, 30, Entry(RudeAwc::T_FORMAT, FmtOff, FmtSize));
	RudeAwc::PutU64(Out, 38, Entry(RudeAwc::T_PEAK, PeakOff, PeakSize));
	FMemory::Memcpy(Out.GetData() + DataOff, Data.GetData(), DataSize);
	RudeAwc::FFormat Fmt;
	Fmt.Samples = (uint32)Frames; Fmt.LoopPoint = -1; Fmt.SampleRate = (uint16)Rate; Fmt.Headroom = 0;
	Fmt.PlayBegin = 0; Fmt.PlayEnd = 0; Fmt.LoopBegin = 0; Fmt.Unk12 = 0; Fmt.Codec = RudeAwc::CODEC_PCM16; Fmt.PeakUnk = 0; Fmt.Unk16 = 0;
	RudeAwc::WriteFormat(Out, (int32)FmtOff, Fmt);
	for (int32 i = 0; i < PeakCount; ++i) { RudeAwc::PutU16(Out, (int32)PeakOff + 2 * i, Peaks[i]); }
	if (!FFileHelper::SaveArrayToFile(Out, *OutAwcPath)) { return Fail(FString::Printf(TEXT("write failed: %s"), *OutAwcPath)); }

	// self-check: read the bytes back through the tiling reader and compare what it decodes to what went in
	RudeAwc::FParsed P; FString Err;
	if (!RudeAwc::Parse(Out, P, Err)) { return Fail(TEXT("self-check: the written file does not parse: ") + Err); }
	RudeAwc::FFormat Back;
	const bool bFmt = P.Chunks.Num() == 3 && P.Chunks[1].Type == RudeAwc::T_FORMAT && RudeAwc::ReadFormat(Out, P.Chunks[1], Back);
	const bool bOk = bFmt && P.StreamWord.Num() == 1 && P.StreamWord[0] == Word && P.ChunkStart.Num() == 1 && P.ChunkStart[0] == 0
		&& P.DataStart == DataStart && P.Chunks[0].Type == RudeAwc::T_DATA && P.Chunks[0].Size == DataSize && P.Chunks[0].Size == 2u * Back.Samples
		&& Back.Samples == (uint32)Frames && Back.SampleRate == (uint16)Rate && Back.Codec == RudeAwc::CODEC_PCM16 && P.Chunks[2].Type == RudeAwc::T_PEAK
		&& P.PadBytes == 0 && !P.PayloadEncrypted() && !P.StreamTableShape();
	return FString::Printf(
		TEXT("{\"ok\":%s,\"awcPath\":\"%s\",\"streamName\":\"%s\",\"nameHash\":%u,\"streamWord\":%u,\"frames\":%d,\"sampleRate\":%u,\"channelsIn\":%d,\"downmixed\":%s,")
		TEXT("\"bytes\":%d,\"dataStart\":%u,\"flags\":%u,\"chunks\":[{\"type\":\"data\",\"off\":%u,\"size\":%u},{\"type\":\"format\",\"off\":%u,\"size\":%u},{\"type\":\"peak\",\"off\":%u,\"size\":%u}],")
		TEXT("\"peakEntries\":%d,\"selfCheck\":{\"parsed\":true,\"streams\":%d,\"chunks\":%d,\"samplesRead\":%u,\"sampleRateRead\":%u,\"codecRead\":%u,\"padBytes\":%d,\"agrees\":%s},")
		TEXT("\"unmeasured\":\"peak values, Headroom=0, PeakUnk=0 (LAWS.md); the in-game load judges\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(OutAwcPath), *RudeJsonEscape(Name), Hash, Word, Frames, Rate, (int32)Channels, Channels == 2 ? TEXT("true") : TEXT("false"),
		Out.Num(), DataStart, (uint32)RudeAwc::FLAGS_PLAIN_PCM, DataOff, DataSize, FmtOff, FmtSize, PeakOff, PeakSize,
		PeakCount, P.StreamWord.Num(), P.Chunks.Num(), Back.Samples, (uint32)Back.SampleRate, (uint32)Back.Codec, P.PadBytes, bOk ? TEXT("true") : TEXT("false"));
#else
	return Fail(TEXT("editor-only"));
#endif
}

// ---- ImportAwc ------------------------------------------------------------------------------
// One corpus .awc -> USoundWave per PCM16 stream. Two source forms, both through the ledger:
//   * converted (bConverted): ROUT's AudioWaveContainer XML; data chunk i pairs with format chunk i (the
//     0xFA shape; ROUT's pairing, proven by 16.000 bits/sample) or with StreamTable record i (0x48 shape).
//     Payload kind="pcm16" -> the .wav sidecar in <SidecarDirOf(row)>/<file>; kind="raw" with codec 0 -> the
//     raw PCM16 bytes wrapped in a WAV (SerializeWaveFile, Audio.h:999); kind="encrypted" / "none" / ADPCM ->
//     COUNTED and skipped. WARNING: the 2026-09-04 corpus was exported with --textures none: every awc carries
//     kind="none" (measured on chicken, rapid_distant, halloween_2015 + 5 more), so this branch reports
//     payloadAbsent until ROUT re-exports the audio lane with sidecars.
//   * kept binary (!bConverted): parsed with the tiling reader. No 'ADAT' magic = whole-file encrypted
//     (2,100 of the game's 7,742) = a named refusal. Payload-encrypted plaintext containers (bit 3) skip
//     every data chunk as encrypted; the rest import their PCM16 streams straight from the bytes.
FString URudeToolset::ImportAwc(const FString& CorpusRoot, const FString& AwcName, const FString& DestFolder)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
#if WITH_EDITOR
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }
	const FString Name = AwcName.TrimStartAndEnd().ToLower();
	if (Name.IsEmpty()) { return Fail(TEXT("AwcName is empty")); }
	const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("awc"), Name);
	if (!Row)
	{
		TArray<const FRudeCorpusEntry*> Near; Corpus->ByPrefix(TEXT("awc"), Name.Left(3), Near);
		FString Hint; for (int32 i = 0; i < Near.Num() && i < 8; ++i) { Hint += (i ? TEXT(", ") : TEXT("")) + Near[i]->Name; }
		return Fail(FString::Printf(TEXT("the corpus has no awc named '%s' (names starting '%s': %s)"), *Name, *Name.Left(3), *Hint));
	}
	const FString Path = Corpus->PathOf(*Row);
	const FString Dest = DestFolder.TrimStartAndEnd().IsEmpty() ? (TEXT("/Game/RUDE/Audio/") + RudeAwc::SafeName(Name)) : DestFolder.TrimStartAndEnd();

	int32 DataChunks = 0, PcmImported = 0, AdpcmSkipped = 0, EncryptedSkipped = 0, PayloadAbsent = 0, CodecUnknown = 0, SidecarMissing = 0, Failed = 0;
	FString Assets, Errors, Form;
	auto AddAsset = [&](const FString& A) { Assets += FString::Printf(TEXT("%s\"%s\""), Assets.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(A)); };
	auto AddErr = [&](const FString& E) { ++Failed; if (Errors.Len() < 1500) { Errors += FString::Printf(TEXT("%s\"%s\""), Errors.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(E)); } };

	if (Row->bConverted)
	{
		Form = TEXT("xml");
		FXmlFile Xml(Path);
		if (!Xml.IsValid() || !Xml.GetRootNode()) { return Fail(FString::Printf(TEXT("cannot parse %s: %s"), *Path, *Xml.GetLastError())); }
		const FXmlNode* RootN = Xml.GetRootNode();
		if (RootN->GetTag() != TEXT("AudioWaveContainer")) { return Fail(FString::Printf(TEXT("root is <%s>, not <AudioWaveContainer>"), *RootN->GetTag())); }
		const FXmlNode* ChunksN = RootN->FindChildNode(TEXT("Chunks"));
		if (!ChunksN) { return Fail(TEXT("no <Chunks> element")); }
		TArray<FString> StreamNames;
		if (const FXmlNode* StreamsN = RootN->FindChildNode(TEXT("Streams")))
		{
			for (const FXmlNode* S : StreamsN->GetChildrenNodes()) { const FXmlNode* Nm = S->FindChildNode(TEXT("Name")); StreamNames.Add(Nm ? Nm->GetContent().TrimStartAndEnd() : FString()); }
		}
		// the data / format / stream-table items in file order
		TArray<const FXmlNode*> Datas, Formats; TArray<int32> TableCodecs;
		for (const FXmlNode* It : ChunksN->GetChildrenNodes())
		{
			const FXmlNode* T = It->FindChildNode(TEXT("Type"));
			const FString Type = T ? T->GetContent().TrimStartAndEnd() : FString();
			if (Type == TEXT("data")) { Datas.Add(It); }
			else if (Type == TEXT("format")) { Formats.Add(It); }
			else if (const FXmlNode* ST = It->FindChildNode(TEXT("StreamTable")))
			{
				for (const FXmlNode* R : ST->GetChildrenNodes()) { TableCodecs.Add(FCString::Atoi(*R->GetAttribute(TEXT("codec")))); }
			}
		}
		DataChunks = Datas.Num();
		const FString SidecarDir = Corpus->SidecarDirOf(*Row);
		for (int32 i = 0; i < Datas.Num(); ++i)
		{
			const FXmlNode* D = Datas[i];
			int32 Codec = -1; uint32 Samples = 0; uint32 Rate = 0;
			if (Formats.Num() == Datas.Num())
			{
				const FXmlNode* F = Formats[i];
				auto V = [&](const TCHAR* K) -> int64 { const FXmlNode* N = F->FindChildNode(K); return N ? FCString::Atoi64(*N->GetAttribute(TEXT("value"))) : -1; };
				Codec = (int32)V(TEXT("Codec")); Samples = (uint32)FMath::Max<int64>(0, V(TEXT("Samples"))); Rate = (uint32)FMath::Max<int64>(0, V(TEXT("SampleRate")));
			}
			else if (TableCodecs.Num() == Datas.Num()) { Codec = TableCodecs[i]; }
			const FXmlNode* PL = D->FindChildNode(TEXT("Payload"));
			const FString Kind = PL ? PL->GetAttribute(TEXT("kind")) : FString(TEXT("(missing)"));
			if (Kind == TEXT("none")) { ++PayloadAbsent; continue; }
			if (Kind == TEXT("encrypted")) { ++EncryptedSkipped; continue; }
			if (Codec == RudeAwc::CODEC_ADPCM) { ++AdpcmSkipped; continue; }
			if (Codec != RudeAwc::CODEC_PCM16) { ++CodecUnknown; continue; }
			const FString File = PL ? PL->GetAttribute(TEXT("file")) : FString();
			if (File.IsEmpty()) { AddErr(FString::Printf(TEXT("data chunk %d: Payload kind=%s names no file"), i, *Kind)); continue; }
			FString SidePath = SidecarDir / File;
			if (!FPaths::FileExists(SidePath)) { const FString Alt = FPaths::GetPath(Path) / Row->Name / File; if (FPaths::FileExists(Alt)) { SidePath = Alt; } }
			TArray<uint8> Side;
			if (!FFileHelper::LoadFileToArray(Side, *SidePath)) { ++SidecarMissing; AddErr(FString::Printf(TEXT("data chunk %d: sidecar not found at %s"), i, *SidePath)); continue; }
			TArray<uint8> Wav;
			if (Kind == TEXT("pcm16"))
			{
				if (Side.Num() < 12 || Side[0] != 'R' || Side[1] != 'I' || Side[2] != 'F' || Side[3] != 'F') { AddErr(FString::Printf(TEXT("data chunk %d: %s is not RIFF/WAVE"), i, *SidePath)); continue; }
				Wav = MoveTemp(Side);
			}
			else if (Kind == TEXT("raw"))
			{
				const int32 R = (int32)(PL->GetAttribute(TEXT("sampleRate")).IsEmpty() ? Rate : (uint32)FCString::Atoi(*PL->GetAttribute(TEXT("sampleRate"))));
				if (R <= 0) { AddErr(FString::Printf(TEXT("data chunk %d: raw PCM16 with no sample rate"), i)); continue; }
				SerializeWaveFile(Wav, Side.GetData(), Side.Num(), 1, R);
			}
			else { AddErr(FString::Printf(TEXT("data chunk %d: unknown Payload kind '%s'"), i, *Kind)); continue; }
			const FString Label = (StreamNames.IsValidIndex(i) && !StreamNames[i].IsEmpty()) ? StreamNames[i] : FString::Printf(TEXT("stream_%d"), i);
			const FString AssetName = RudeAwc::SafeName(Name + TEXT("__") + Label);
			FString Err;
			if (USoundWave* SW = RudeAwc::ImportWav(Wav, Dest, AssetName, Err)) { ++PcmImported; AddAsset(SW->GetPathName()); }
			else { AddErr(Err); }
		}
	}
	else
	{
		Form = TEXT("binary");
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) { return Fail(FString::Printf(TEXT("cannot read %s"), *Path)); }
		RudeAwc::FParsed P; FString Err;
		if (!RudeAwc::Parse(Bytes, P, Err))
		{
			return FString::Printf(TEXT("{\"ok\":false,\"form\":\"binary\",\"refused\":\"%s\",\"path\":\"%s\",\"bytes\":%d,\"note\":\"a refusal with a named reason - not a writer/reader failure\"}"), *RudeJsonEscape(Err), *RudeJsonEscape(Path), Bytes.Num());
		}
		TArray<RudeAwc::FChunk> Datas, Formats; TArray<int32> TableCodecs;
		for (const RudeAwc::FChunk& C : P.Chunks)
		{
			if (C.Type == RudeAwc::T_DATA) { Datas.Add(C); }
			else if (C.Type == RudeAwc::T_FORMAT) { Formats.Add(C); }
			else if (C.Type == RudeAwc::T_STREAMTABLE && C.Size >= 12)
			{
				const uint32 NRec = RudeAwc::RdU32(Bytes, (int32)C.Offset + 8);
				if (C.Size == 12 + 16 * NRec) { for (uint32 r = 0; r < NRec; ++r) { TableCodecs.Add((int32)RudeAwc::RdU32(Bytes, (int32)C.Offset + 12 + 16 * r + 12)); } }
			}
		}
		DataChunks = Datas.Num();
		for (int32 i = 0; i < Datas.Num(); ++i)
		{
			if (P.PayloadEncrypted()) { ++EncryptedSkipped; continue; }
			int32 Codec = -1; uint32 Rate = 0; RudeAwc::FFormat F;
			if (Formats.Num() == Datas.Num() && RudeAwc::ReadFormat(Bytes, Formats[i], F)) { Codec = F.Codec; Rate = F.SampleRate; }
			else if (TableCodecs.Num() == Datas.Num()) { Codec = TableCodecs[i]; }
			if (Codec == RudeAwc::CODEC_ADPCM) { ++AdpcmSkipped; continue; }
			if (Codec != RudeAwc::CODEC_PCM16 || Rate == 0) { ++CodecUnknown; continue; }
			if (Datas[i].Size % 2) { AddErr(FString::Printf(TEXT("data chunk %d: odd PCM16 byte count %u"), i, Datas[i].Size)); continue; }
			TArray<uint8> Wav;
			SerializeWaveFile(Wav, Bytes.GetData() + Datas[i].Offset, (int32)Datas[i].Size, 1, (int32)Rate);
			const FString Label = P.StreamWord.IsValidIndex(i) ? FString::Printf(TEXT("hash_%08X"), P.StreamWord[i] & 0x1FFFFFFFu) : FString::Printf(TEXT("stream_%d"), i);
			const FString AssetName = RudeAwc::SafeName(Name + TEXT("__") + Label);
			FString IErr;
			if (USoundWave* SW = RudeAwc::ImportWav(Wav, Dest, AssetName, IErr)) { ++PcmImported; AddAsset(SW->GetPathName()); }
			else { AddErr(IErr); }
		}
	}
	// ok is COMPUTED: a run that imported nothing and skipped nothing for a named reason is a failure
	const bool bOk = Failed == 0 && (PcmImported + AdpcmSkipped + EncryptedSkipped + PayloadAbsent + CodecUnknown) == DataChunks;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"awc\":\"%s\",\"form\":\"%s\",\"slot\":\"%s\",\"path\":\"%s\",\"destFolder\":\"%s\",\"dataChunks\":%d,\"pcmImported\":%d,\"adpcmSkipped\":%d,")
		TEXT("\"encryptedSkipped\":%d,\"payloadAbsent\":%d,\"codecUnknown\":%d,\"sidecarMissing\":%d,\"failed\":%d,\"assets\":[%s],\"errors\":[%s],")
		TEXT("\"note\":\"payloadAbsent = ROUT exported this lane with --textures none (no sidecars); ADPCM content is not modelled (ROUT law) and is never guessed\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Name), *Form, *RudeJsonEscape(Row->Slot), *RudeJsonEscape(Path), *RudeJsonEscape(Dest), DataChunks, PcmImported, AdpcmSkipped,
		EncryptedSkipped, PayloadAbsent, CodecUnknown, SidecarMissing, Failed, *Assets, *Errors);
#else
	return Fail(TEXT("editor-only"));
#endif
}
