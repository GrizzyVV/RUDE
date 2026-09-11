// RUDE - RAGE <-> Unreal Development Environment
//
// THE SOUND-SET WRITER: a `dat54` audio `.rel` built FROM NOTHING, so a sound RUDE authored can be
// named and played by the game.
//
// WHY THIS IS THE MISSING PIECE. `ExportAwc` already writes the wave, and it writes it in a shape
// the game itself ships: censused all 5,642 `.awc` in the corpus by codec and **80 files are
// entirely PCM16 (codec 0)**, 2,150 entirely ADPCM, 0 mixed. So the encoder was never the blocker.
// What the game will not do is PLAY a wave nobody declared: it reaches a wave through a
// `SimpleSound` in a `dat54` sound set, and RUDE had no way to write one.
//
// CLEAN-ROOM PROVENANCE. Every law below is measured on the game's own files, by the maintainer's
// own exporter (`rout/rel_write.py`, `rout/rel_schema_witnessed.json`, `rout/rel_layout.json` -
// house code, Matt's). No third-party exporter code was read. This file is an INDEPENDENT C++
// implementation: RUDE is a self-contained public plugin and cannot call unpublished local Python.
//
// ===========================  THE CONTAINER (one format across all nine idents)  ===========
//   +0x00 u32 ident            54 for a sound set
//   +0x04 u32 dataLength       end of the data block, in data-block coordinates
//   +0x08 u32 version          data-block offset 0..3 IS this stamp, so records start at 4
//   data block, per record at file offset 8 + off:
//       u8 type                             12 = SimpleSound
//       payload                             begins with the u32 Flags
//   ⛔ THE RECORD HEADER IS ONE BYTE IN A `.rel`, NOT FOUR - and an independent reader is what
//   caught that. The GENERIC .rel record is `{u8 type | u24 ntOffset}`, but for dat54 in a
//   `.rel`-class file those three bytes ARE THE LOW 24 BITS OF THE FLAGS WORD, measured
//   1,389/1,389 / 1,003/1,003 / 360/360 against the reference, which spells no ntOffset on any of
//   its 6,742 dat54 items. So the record is `u8 type | payload`, the payload begins with Flags, and
//   there is no ntOffset to carry. (A BARE `X.dat54` is the other class: there the u24 really is a
//   name-table offset and Flags follows it - 0 of 62,297 twin records are byte-identical across the
//   two classes. RUDE writes the `.rel` class.)
//   name table at 8 + dataLength:
//       u32 ntLength = 4 + 4*count + blobBytes   (then) u32 count, u32 offsets[count], blob
//   item index:
//       u32 itemCount, then { u32 nameHash, u32 offset, u32 length }[itemCount]
//   trailing lists:
//       u32 listACount, u32 listA[], u32 listBCount, u32 listB[]
//
// ⭐ THE LAYOUT LAW, and it is why this can be written rather than copied: an item's data offset is
// NOT stored information a writer must carry. Records are laid out in ascending order from data
// offset 4 with `off = align(previousEnd, A)`, where A is a property of (ident, index encoding,
// record type). Measured at population over 487 containers and 621,223 records: 330/330 variants
// take ONE consistent A, and **every dat54 record type takes A = 1** - packed end to end, no
// padding. So the whole offset vector and dataLength are COMPUTED here, never guessed.
//
// ===========================  THE RECORD (SimpleSound, type 12)  ===========================
// Schema found by search and verified at population (96,473 records across 1,389 files), not
// guessed: a gated Header, then three leaves.
//   Header  - a bit-gated block whose FIRST word is always the u32 Flags. Each set bit appends one
//             optional field in bit order (bit 2 Volume s16, bit 4 Pitch s16, ...). ⭐ With
//             Flags = 0 the header is the flags word ALONE, which is the shape this writer emits
//             and the shape the game's own smallest sound set uses
//             (`dlctu_sounds.dat54.rel.xml`, 1,094 bytes, `<Header><Flags value="0x00000000" /></Header>`).
//   ContainerName  hash32   joaat of the .awc name
//   FileName       hash32   joaat of the stream name inside it
//   WaveSlotIndex  s8
// ⇒ payload = 13 bytes, record = 1 + 13 = 14.
//
// ⛔ WHAT THIS DOES NOT CLAIM. Only `SimpleSound` with an EMPTY header is written. Every other
// sound type, and every gated header field, is refused by name rather than half-emitted - the
// schema for those exists, but nothing here has been measured against a game that loaded one.
// ⛔ And nothing here has been loaded by the game AT ALL. The referee is ROUT's own independent
// reader (`rout/rel2xml.py`), which is a second implementation, not a second opinion from this one.

#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_EDITOR

namespace RudeRel
{
	static const uint32 IDENT_DAT54 = 54;
	static const uint8  TYPE_SIMPLESOUND = 12;
	// The version word the game's own DLC sound sets carry. Carried as a VALUE, and overridable,
	// because it is a witnessed constant rather than something this writer derives.
	static const uint32 DEFAULT_VERSION = 7126027u;

	// RAGE joaat over the lowercased name - the same one-at-a-time hash the ytd and meta lanes
	// already proved against observed hashes. A sound's identity in a `.rel` IS this u32.
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
		H += (H << 3); H ^= (H >> 11); H += (H << 15);
		return H;
	}

	static void PU32(TArray<uint8>& B, int32 At, uint32 V)
	{
		B[At] = (uint8)(V & 0xFF); B[At + 1] = (uint8)((V >> 8) & 0xFF);
		B[At + 2] = (uint8)((V >> 16) & 0xFF); B[At + 3] = (uint8)((V >> 24) & 0xFF);
	}
	static uint32 RU32(const TArray<uint8>& B, int32 At)
	{
		return (uint32)B[At] | ((uint32)B[At + 1] << 8) | ((uint32)B[At + 2] << 16) | ((uint32)B[At + 3] << 24);
	}

	struct FSimpleSound
	{
		FString Name;        // the sound the game asks for
		FString Container;   // the .awc
		FString Wave;        // the stream inside it
		int32   Slot = 0;
		uint32  NameHash = 0, ContainerHash = 0, WaveHash = 0;
		int32   Offset = 0;  // data-block offset, COMPUTED
	};
}

// ---- ExportSoundSet -------------------------------------------------------------------------
FString URudeToolset::ExportSoundSet(const FString& Sounds, const FString& OutRelPath, const FString& Options)
{
	using namespace RudeRel;
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };

	uint32 Version = DEFAULT_VERSION;
	{
		TArray<FString> Parts;
		Options.ParseIntoArray(Parts, TEXT(";"), true);
		for (const FString& P : Parts)
		{
			FString K, V;
			if (!P.Split(TEXT("="), &K, &V)) { continue; }
			K = K.TrimStartAndEnd(); V = V.TrimStartAndEnd();
			if (K.Equals(TEXT("version"), ESearchCase::IgnoreCase))
			{
				Version = V.StartsWith(TEXT("0x"), ESearchCase::IgnoreCase)
					? (uint32)FCString::Strtoui64(*V.Mid(2), nullptr, 16)
					: (uint32)FCString::Strtoui64(*V, nullptr, 10);
			}
		}
	}

	// `name=container/wave[:slot]`, comma separated. Refuses anything it cannot read completely -
	// a half-read sound would still produce a file, and a file the game silently ignores is worse
	// than a refusal that names the row.
	TArray<FSimpleSound> Items;
	{
		TArray<FString> Rows;
		Sounds.ParseIntoArray(Rows, TEXT(","), true);
		if (Rows.Num() == 0) { return Fail(TEXT("give at least one sound as name=container/wave[:slot]")); }
		for (const FString& RawRow : Rows)
		{
			const FString Row = RawRow.TrimStartAndEnd();
			FString Name, Rest;
			if (!Row.Split(TEXT("="), &Name, &Rest))
			{
				return Fail(FString::Printf(TEXT("cannot read \"%s\": expected name=container/wave[:slot]"), *Row));
			}
			// ⚠ The optional `:slot` suffix. The first version passed ONE FString as BOTH out-params of
			// FString::Split, which UE asserts on outright (`LeftStr != RightStr`) and killed the
			// commandlet before any verdict - a crash, not a refusal. Split once, into two variables,
			// and only treat the tail as a slot when it actually reads as a number, so a name that
			// happens to contain a colon is left alone instead of being silently truncated.
			int32 Slot = 0;
			{
				FString Left, Right;
				if (Rest.Split(TEXT(":"), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
				{
					const FString T = Right.TrimStartAndEnd();
					bool bNumeric = !T.IsEmpty();
					for (int32 c = 0; c < T.Len(); ++c)
					{
						const TCHAR Ch = T[c];
						if (!(FChar::IsDigit(Ch) || (c == 0 && (Ch == TEXT('-') || Ch == TEXT('+'))))) { bNumeric = false; break; }
					}
					if (bNumeric) { Slot = FCString::Atoi(*T); Rest = Left; }
				}
			}
			FString Container, Wave;
			if (!Rest.Split(TEXT("/"), &Container, &Wave))
			{
				return Fail(FString::Printf(TEXT("cannot read \"%s\": the wave is named container/wave"), *Row));
			}
			Name = Name.TrimStartAndEnd(); Container = Container.TrimStartAndEnd(); Wave = Wave.TrimStartAndEnd();
			if (Name.IsEmpty() || Container.IsEmpty() || Wave.IsEmpty())
			{
				return Fail(FString::Printf(TEXT("cannot read \"%s\": name, container and wave must all be given"), *Row));
			}
			if (Slot < -128 || Slot > 127)
			{
				return Fail(FString::Printf(TEXT("%s: WaveSlotIndex is a signed byte, %d does not fit"), *Name, Slot));
			}
			FSimpleSound S;
			S.Name = Name; S.Container = Container; S.Wave = Wave; S.Slot = Slot;
			S.NameHash = Joaat(Name); S.ContainerHash = Joaat(Container); S.WaveHash = Joaat(Wave);
			Items.Add(S);
		}
	}
	// Two sounds under one name is a collision the game resolves by hash, so the second silently
	// wins. Refuse instead of shipping a file whose second half is unreachable.
	for (int32 i = 0; i < Items.Num(); ++i)
	{
		for (int32 j = i + 1; j < Items.Num(); ++j)
		{
			if (Items[i].NameHash == Items[j].NameHash)
			{
				return Fail(FString::Printf(TEXT("two sounds hash the same: \"%s\" and \"%s\" (0x%08x) - "
					"the game would only ever reach one of them"), *Items[i].Name, *Items[j].Name, Items[i].NameHash));
			}
		}
	}

	// ---- lay the records out. A = 1 for every dat54 type (330/330 variants measured), so the
	// records pack end to end from data offset 4 and every offset here is COMPUTED.
	const int32 PayloadBytes = 4 + 4 + 4 + 1;     // Flags | ContainerName | FileName | WaveSlotIndex
	const int32 RecordBytes = 1 + PayloadBytes;   // + the ONE-BYTE type header (see the note above)
	int32 Cursor = 4;                             // 0..3 is the version stamp
	for (FSimpleSound& S : Items) { S.Offset = Cursor; Cursor += RecordBytes; }
	const int32 DataLength = Cursor;

	const int32 NtLength = 4;                     // count only: this writer emits no inline names
	const int32 IndexBytes = 4 + 12 * Items.Num();
	const int32 Size = 8 + DataLength + 4 + NtLength + IndexBytes + 4 + 4;

	TArray<uint8> B;
	B.SetNumZeroed(Size);                         // zero-filled: any gap stays LOUD
	PU32(B, 0, IDENT_DAT54);
	PU32(B, 4, (uint32)DataLength);
	PU32(B, 8, Version);
	for (const FSimpleSound& S : Items)
	{
		int32 o = 8 + S.Offset;
		B[o] = TYPE_SIMPLESOUND;                  // ONE byte - the payload starts at o+1, with Flags
		o += 1;
		PU32(B, o, 0u);                           // Header: Flags = 0, and therefore nothing else
		PU32(B, o + 4, S.ContainerHash);
		PU32(B, o + 8, S.WaveHash);
		B[o + 12] = (uint8)(int8)S.Slot;
	}
	int32 p = 8 + DataLength;
	PU32(B, p, (uint32)NtLength); p += 4;
	PU32(B, p, 0u);                               // name-table count
	p += NtLength;
	PU32(B, p, (uint32)Items.Num()); p += 4;
	for (const FSimpleSound& S : Items)
	{
		PU32(B, p, S.NameHash);
		PU32(B, p + 4, (uint32)S.Offset);
		PU32(B, p + 8, (uint32)RecordBytes);
		p += 12;
	}
	PU32(B, p, 0u); p += 4;                       // ListA
	PU32(B, p, 0u); p += 4;                       // ListB

	// ---- SELF-CHECK ON THE BYTES WE JUST BUILT, not on the intent that built them. Reads the
	// container back the way a reader would and confirms every computed field. It cannot prove the
	// game accepts the file - only the game does that - but it CAN prove the writer did not
	// contradict itself, which is the failure a hand-laid binary actually has.
	int32 SelfCheckFailed = 0;
	FString FirstProblem;
	auto Check = [&](bool bCond, const FString& What)
	{
		if (!bCond) { ++SelfCheckFailed; if (FirstProblem.IsEmpty()) { FirstProblem = What; } }
	};
	Check(p == Size, FString::Printf(TEXT("wrote %d bytes into a %d-byte image"), p, Size));
	Check(RU32(B, 0) == IDENT_DAT54, TEXT("ident is not 54"));
	Check((int32)RU32(B, 4) == DataLength, TEXT("dataLength does not read back"));
	{
		int32 q = 8 + DataLength + 4 + NtLength;
		Check((int32)RU32(B, q) == Items.Num(), TEXT("item count does not read back"));
		q += 4;
		for (int32 i = 0; i < Items.Num(); ++i)
		{
			const uint32 H = RU32(B, q), Off = RU32(B, q + 4), Len = RU32(B, q + 8);
			Check(H == Items[i].NameHash, FString::Printf(TEXT("index hash %d"), i));
			Check((int32)Off == Items[i].Offset, FString::Printf(TEXT("index offset %d"), i));
			Check((int32)Len == RecordBytes, FString::Printf(TEXT("index length %d"), i));
			const int32 r = 8 + (int32)Off;
			Check(B[r] == TYPE_SIMPLESOUND, FString::Printf(TEXT("record type %d"), i));
			Check(RU32(B, r + 1) == 0u, FString::Printf(TEXT("record %d header flags"), i));
			Check(RU32(B, r + 5) == Items[i].ContainerHash, FString::Printf(TEXT("record %d container"), i));
			Check(RU32(B, r + 9) == Items[i].WaveHash, FString::Printf(TEXT("record %d wave"), i));
			Check((int8)B[r + 13] == (int8)Items[i].Slot, FString::Printf(TEXT("record %d slot"), i));
			q += 12;
		}
		// the records must tile the data block exactly - no gap, no overlap
		Check(4 + Items.Num() * RecordBytes == DataLength, TEXT("records do not tile the data block"));
	}

	const FString Out = OutRelPath.TrimStartAndEnd();
	bool bWritten = false;
	if (SelfCheckFailed == 0 && !Out.IsEmpty())
	{
		bWritten = FFileHelper::SaveArrayToFile(B, *Out);
		if (!bWritten) { return Fail(FString::Printf(TEXT("could not write %s"), *Out)); }
	}

	FString SoundJson;
	for (const FSimpleSound& S : Items)
	{
		SoundJson += FString::Printf(
			TEXT("%s{\"name\":\"%s\",\"nameHash\":\"0x%08x\",\"container\":\"%s\",\"containerHash\":\"0x%08x\",")
			TEXT("\"wave\":\"%s\",\"waveHash\":\"0x%08x\",\"waveSlotIndex\":%d,\"offset\":%d}"),
			SoundJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(S.Name), S.NameHash,
			*RudeJsonEscape(S.Container), S.ContainerHash, *RudeJsonEscape(S.Wave), S.WaveHash, S.Slot, S.Offset);
	}

	return FString::Printf(
		TEXT("{\"ok\":%s,\"out\":\"%s\",\"ident\":54,\"family\":\"dat54\",\"version\":%u,\"sounds\":%d,")
		TEXT("\"bytes\":%d,\"dataLength\":%d,\"recordBytes\":%d,\"alignment\":1,\"written\":%s,")
		TEXT("\"selfCheckFailed\":%d,\"firstProblem\":\"%s\",\"soundList\":[%s],")
		TEXT("\"note\":\"a dat54 sound set BUILT from nothing, not a donor repack: every offset, length and count "
		     "is computed from the layout law (A=1 for every dat54 type, 330/330 variants measured). Only "
		     "SimpleSound with an EMPTY header is written - every other sound type and every gated header field "
		     "is refused rather than half-emitted. The record header is ONE byte for dat54 in a .rel - the "
		     "three bytes that look like an ntOffset are the low 24 bits of Flags, and an independent reader "
		     "is what caught that. Pair it with ExportAwc for the wave. ⛔ Nothing here has been loaded by "
		     "the game.\"}"),
		SelfCheckFailed == 0 ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Out), Version, Items.Num(),
		Size, DataLength, RecordBytes, bWritten ? TEXT("true") : TEXT("false"),
		SelfCheckFailed, *RudeJsonEscape(FirstProblem), *SoundJson);
}

// ---- ExportAudioResource -----------------------------------------------------------------------
// ONE COMMAND -> ONE FOLDER TO DROP IN. `ExportAwc` writes the wave and `ExportSoundSet` writes the
// declaration, but a test session should not need someone to assemble a resource by hand around
// them - and the reason is not convenience.
//
// ⛔ THE REASON IS FALSE NEGATIVES. If a manifest line is wrong the game does not complain, it just
// ignores the file. During a test sitting that reads exactly like "the format is wrong", and the
// next day is spent chasing a container bug that does not exist. Hand-assembly is the step most
// likely to be got wrong and the least likely to announce it, so it is the step to remove.
//
// ⚠ THE MANIFEST WIRING IS THE PART NOBODY HAS CONFIRMED. The container and the sound set are gated
// on the desk (`ExportSoundSet`, and ROUT's independent reader agrees with both); the `data_file`
// lines below are FiveM CONVENTION, written from the documented meaning of AUDIO_WAVEPACK and
// AUDIO_SOUNDDATA, and no run has proven them. They are reported as `manifestVerified:false` and
// they are exactly what the server row tests. Do not read a silent game as a format failure until
// this half is ruled out.
//
// ⚠ ONE AWC PER SOUND, and that is a real limit rather than a choice: `ExportAwc` writes a
// single-stream container. The game ships plenty of single-stream awcs so the shape is legitimate,
// but a multi-stream wave pack is not something RUDE can build yet.
//
// ⚠ LAYOUT: `audiodirectory/` and `audioconfig/` are not a naming decision - `data_file` resolves
// against those paths, so they are fixed by the thing consuming them. The RESOURCE folder name is
// the caller's.
FString URudeToolset::ExportAudioResource(const FString& Sounds, const FString& OutDir, const FString& Options)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	auto VerdictOk = [](const FString& V)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(V);
		bool b = false;
		return FJsonSerializer::Deserialize(R, Obj) && Obj.IsValid() && Obj->TryGetBoolField(TEXT("ok"), b) && b;
	};

	const FString Root = OutDir.TrimStartAndEnd();
	if (Root.IsEmpty()) { return Fail(TEXT("give an output folder for the resource")); }
	FString SetName = FPaths::GetCleanFilename(Root);
	if (SetName.IsEmpty()) { SetName = TEXT("rude_audio"); }

	// `name=/Game/Path/To/SoundWave`, comma separated
	TArray<FString> Rows;
	Sounds.ParseIntoArray(Rows, TEXT(","), true);
	if (Rows.Num() == 0) { return Fail(TEXT("give at least one sound as name=/Game/Path/To/SoundWave")); }

	const FString WaveDir = Root / TEXT("audiodirectory");
	const FString ConfDir = Root / TEXT("audioconfig");

	FString SoundSpecs, PerSound, Problems;
	int32 Written = 0;
	for (const FString& RawRow : Rows)
	{
		FString Name, Asset;
		if (!RawRow.TrimStartAndEnd().Split(TEXT("="), &Name, &Asset))
		{
			return Fail(FString::Printf(TEXT("cannot read \"%s\": expected name=/Game/Path/To/SoundWave"), *RawRow.TrimStartAndEnd()));
		}
		Name = Name.TrimStartAndEnd().ToLower();
		Asset = Asset.TrimStartAndEnd();
		if (Name.IsEmpty() || Asset.IsEmpty()) { return Fail(TEXT("both a name and a SoundWave path are needed")); }

		// the container and the stream both take the sound's own name: one awc per sound
		const FString AwcPath = WaveDir / (Name + TEXT(".awc"));
		const FString AwcVerdict = URudeToolset::ExportAwc(Asset, AwcPath, Name);
		if (!VerdictOk(AwcVerdict))
		{
			Problems += FString::Printf(TEXT("%s\"%s: %s\""), Problems.IsEmpty() ? TEXT("") : TEXT(","),
				*RudeJsonEscape(Name), *RudeJsonEscape(AwcVerdict.Left(200)));
			continue;
		}
		++Written;
		SoundSpecs += FString::Printf(TEXT("%s%s=%s/%s"), SoundSpecs.IsEmpty() ? TEXT("") : TEXT(","), *Name, *Name, *Name);
		PerSound += FString::Printf(TEXT("%s{\"sound\":\"%s\",\"asset\":\"%s\",\"awc\":\"%s\"}"),
			PerSound.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(Name), *RudeJsonEscape(Asset), *RudeJsonEscape(AwcPath));
	}
	if (Written == 0) { return Fail(FString::Printf(TEXT("no wave was written; first problem: %s"), *Problems.Left(300))); }

	// the sound set that names every wave written above
	const FString RelName = SetName + TEXT("_sounds.dat54.rel");
	const FString RelPath = ConfDir / RelName;
	const FString SetVerdict = URudeToolset::ExportSoundSet(SoundSpecs, RelPath, Options);
	if (!VerdictOk(SetVerdict))
	{
		return Fail(FString::Printf(TEXT("the sound set was refused, so no resource was written: %s"), *SetVerdict.Left(300)));
	}

	// the manifest. ⚠ AUDIO_SOUNDDATA takes the path WITHOUT the .dat54.rel suffix.
	const FString ManifestPath = Root / TEXT("fxmanifest.lua");
	const FString Manifest = FString::Printf(TEXT(
		"fx_version 'cerulean'\ngame 'gta5'\n\n"
		"-- Audio resource written by RUDE (ExportAudioResource).\n"
		"-- audiodirectory/<name>.awc  : one uncompressed PCM16 wave per sound. That codec is not a\n"
		"--   fallback - 80 of the 5,642 .awc in the game are entirely PCM16, so it is a shape the\n"
		"--   game itself ships. One awc per sound because RUDE writes single-stream containers.\n"
		"-- audioconfig/%s : a dat54 sound set built from nothing, declaring one\n"
		"--   SimpleSound per wave (ContainerName = the awc, FileName = the stream inside it).\n"
		"-- AUDIO_SOUNDDATA takes the path WITHOUT the .dat54.rel suffix.\n"
		"-- WARNING: these two data_file lines are the ONE part of this resource nobody has confirmed.\n"
		"-- If the game is silent, rule the manifest out before suspecting the file format.\n\n"
		"files {\n    'audioconfig/*.dat54.rel',\n    'audiodirectory/*.awc',\n}\n\n"
		"data_file 'AUDIO_WAVEPACK' 'audiodirectory'\n"
		"data_file 'AUDIO_SOUNDDATA' 'audioconfig/%s_sounds'\n"),
		*RelName, *SetName);
	const bool bManifest = FFileHelper::SaveStringToFile(Manifest, *ManifestPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	return FString::Printf(
		TEXT("{\"ok\":%s,\"resource\":\"%s\",\"outDir\":\"%s\",\"soundsRequested\":%d,\"wavesWritten\":%d,")
		TEXT("\"soundSet\":\"%s\",\"manifest\":%s,\"manifestVerified\":false,\"sounds\":[%s],\"problems\":[%s],")
		TEXT("\"playInGame\":\"the sound's name is what a script asks for\",")
		TEXT("\"note\":\"one folder to drop into a server. The wave and the sound set are both gated on the "
		     "desk; the two data_file lines in the manifest are FiveM convention and are NOT verified - that "
		     "is what the server row tests, and a silent game should be blamed on them before the format.\"}"),
		(bManifest && Problems.IsEmpty()) ? TEXT("true") : TEXT("false"),
		*RudeJsonEscape(SetName), *RudeJsonEscape(Root), Rows.Num(), Written,
		*RudeJsonEscape(RelPath), bManifest ? TEXT("true") : TEXT("false"), *PerSound, *Problems);
}

#else
FString URudeToolset::ExportSoundSet(const FString&, const FString&, const FString&)
{
	return TEXT("{\"ok\":false,\"error\":\"editor-only\"}");
}
FString URudeToolset::ExportAudioResource(const FString&, const FString&, const FString&)
{
	return TEXT("{\"ok\":false,\"error\":\"editor-only\"}");
}
#endif
