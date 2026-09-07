// RUDE - RAGE <-> Unreal Development Environment
//
// WP13 CUTSCENE EXPORT lane: the write half of ImportCutscene. The read half (RudeAnims.cpp) builds a Level
// Sequence plus a URudeCutsceneEvents sidecar that carries EVERY event verbatim; this file writes an edited
// cutscene back by SPLICING the source document's own bytes, so an untouched cutscene comes back byte-identical
// and an edited time changes exactly one line.
//
// Every structural claim below was MEASURED on the maintainer's filebase (maintainer lane `cutscene_export`
// (`LAWS.md`), 2026-09-07) over 816 .cut.pso.xml files / 201,718,385 bytes / 226,559 events:
//   * CRLF everywhere (816/816 files, 0 LF-only) - the splicer is newline-agnostic and keeps what it finds.
//   * The six top-level lists cut into <Item> slices that concatenate back to the block's inner bytes
//     exactly: 816/816 for each of pCutsceneObjects, pCutsceneLoadEventList, pCutsceneEventList,
//     pCutsceneEventArgsList, concatDataList, discardFrameList.
//   * An event carries exactly one <fTime> line, at three-space indent, as line 1 of its own slice
//     (226,559/226,559), and no event item nests another <Item> (0/226,559). So a time edit is ONE line.
//   * Events are stored in time order (816/816 files, both lists) - an edit that jumps a neighbour is a
//     REORDER, refused by name.
//   * Args items are SHARED (11,929 of 125,684 serve more than one event) - an event's arguments are not the
//     event's private property, so argument edits are refused and the args list is re-emitted verbatim.
//   * <cameraCutList> is NOT the camera-cut event times: its count matches the camera-cut event count in only
//     53 of 816 files. RUDE never derives one from the other; it leaves that line exactly as the file spells it
//     and says so in the verdict.
//
// STATUS (2026-09-07, maintainer lane `cutscene_export`): the measurements above are of the GAME'S OWN FILES
// and stand on their own. This translation unit's OWN behaviour is not measured - it has never been compiled,
// and no gate row has been run in the editor, so every sentence here describing what the tool does describes
// the CODE, not a run. Nobody has loaded an exported .cut back into the game either: byte identity against the
// source file is this lane's whole measure, and that is a strictly weaker claim than "the game accepts it".
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeAnims.h"
#include "RudeCorpus.h"

#include "HAL/FileManager.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "UObject/Package.h"

namespace RudeCutExport
{
	static FString Fail(const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	}
	static FString JStr(const FString& S) { return TEXT("\"") + RudeJsonEscape(S) + TEXT("\""); }

	// One <Item> slice, as offsets into the document (never a copy: the splice writes the source's own bytes).
	struct FCutRawItem
	{
		int32 Start = -1;
		int32 Len = 0;
	};
	struct FCutBlock
	{
		bool bFound = false;
		bool bEmpty = false;          // "<Tag />"
		int32 InnerStart = -1;
		int32 InnerEnd = -1;
		bool bSliceExact = false;     // the item slices concatenate back to the inner bytes
		TArray<FCutRawItem> Items;
	};

	// The document's newline, taken from the document itself (measured CRLF in 816/816 - but read, never assumed).
	static FString NewlineOf(const FString& Doc)
	{
		const int32 N = Doc.Find(TEXT("\n"));
		return (N > 0 && Doc[N - 1] == TEXT('\r')) ? FString(TEXT("\r\n")) : FString(TEXT("\n"));
	}

	// Cut one top-level list into its <Item> slices. False with a reason when the file does not have the
	// measured shape - the caller refuses rather than guessing.
	static bool SliceBlock(const FString& Doc, const TCHAR* Tag, const FString& NL, FCutBlock& Out, FString& Err)
	{
		const FString OpenKey = FString(TEXT("\n <")) + Tag;
		const int32 Key = Doc.Find(OpenKey, ESearchCase::CaseSensitive);
		if (Key == INDEX_NONE) { Err = FString::Printf(TEXT("no top-level <%s> block"), Tag); return false; }
		const int32 AfterTag = Key + OpenKey.Len();
		if (!Doc.IsValidIndex(AfterTag)) { Err = FString::Printf(TEXT("<%s> is truncated"), Tag); return false; }
		const TCHAR Next = Doc[AfterTag];
		if (Next != TEXT('>') && Next != TEXT(' ') && Next != TEXT('/'))
		{
			Err = FString::Printf(TEXT("<%s> is not a top-level list here"), Tag); return false;
		}
		int32 Close = AfterTag;
		while (Doc.IsValidIndex(Close) && Doc[Close] != TEXT('>')) { ++Close; }
		if (!Doc.IsValidIndex(Close)) { Err = FString::Printf(TEXT("<%s> has no closing angle"), Tag); return false; }
		Out.bFound = true;
		if (Close > 0 && Doc[Close - 1] == TEXT('/'))
		{
			Out.bEmpty = true; Out.bSliceExact = true;
			Out.InnerStart = Out.InnerEnd = Close + 1;
			return true;
		}
		int32 InnerStart = Close + 1;
		if (Doc.Mid(InnerStart, NL.Len()) == NL) { InnerStart += NL.Len(); }
		const FString CloseKey = FString(TEXT("\n </")) + Tag + TEXT(">");
		const int32 CloseAt = Doc.Find(CloseKey, ESearchCase::CaseSensitive, ESearchDir::FromStart, InnerStart);
		if (CloseAt == INDEX_NONE) { Err = FString::Printf(TEXT("<%s> is never closed"), Tag); return false; }
		Out.InnerStart = InnerStart;
		Out.InnerEnd = CloseAt + 1;   // the newline that ends the last item belongs to the item

		const FString ItemOpen(TEXT("  <Item"));
		const FString ItemClose = FString(TEXT("\n  </Item>"));
		int32 Cur = Out.InnerStart;
		while (Cur < Out.InnerEnd)
		{
			if (Doc.Mid(Cur, ItemOpen.Len()) != ItemOpen)
			{
				Err = FString::Printf(TEXT("<%s> holds something other than a two-space <Item> at offset %d"), Tag, Cur);
				return false;
			}
			const int32 Eol = Doc.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cur);
			if (Eol == INDEX_NONE || Eol >= Out.InnerEnd) { Err = FString::Printf(TEXT("<%s> item is truncated"), Tag); return false; }
			const FString FirstLine = Doc.Mid(Cur, Eol - Cur).TrimEnd();
			if (FirstLine.EndsWith(TEXT("/>")))
			{
				FCutRawItem It; It.Start = Cur; It.Len = Eol + 1 - Cur;
				Out.Items.Add(It);
				Cur = Eol + 1;
				continue;
			}
			const int32 CloseLine = Doc.Find(ItemClose, ESearchCase::CaseSensitive, ESearchDir::FromStart, Eol);
			if (CloseLine == INDEX_NONE || CloseLine >= Out.InnerEnd) { Err = FString::Printf(TEXT("<%s> item is never closed"), Tag); return false; }
			int32 End = CloseLine + 1 + FString(TEXT("  </Item>")).Len();
			if (Doc.Mid(End, NL.Len()) == NL) { End += NL.Len(); }
			FCutRawItem It; It.Start = Cur; It.Len = End - Cur;
			Out.Items.Add(It);
			Cur = End;
		}
		int32 Sum = 0;
		for (const FCutRawItem& It : Out.Items) { Sum += It.Len; }
		Out.bSliceExact = (Sum == Out.InnerEnd - Out.InnerStart);
		if (!Out.bSliceExact)
		{
			Err = FString::Printf(TEXT("<%s> item slices cover %d of %d bytes - refusing rather than guessing"),
				Tag, Sum, Out.InnerEnd - Out.InnerStart);
			return false;
		}
		return true;
	}

	// "<Tag value="X" />" or "<Tag>X</Tag>" inside one item slice; empty when the tag is absent.
	static FString ValueIn(const FString& Doc, const FCutRawItem& It, const TCHAR* Tag, bool& bFound)
	{
		bFound = false;
		const FString Slice = Doc.Mid(It.Start, It.Len);
		const FString Attr = FString::Printf(TEXT("<%s value=\""), Tag);
		int32 At = Slice.Find(Attr, ESearchCase::CaseSensitive);
		if (At != INDEX_NONE)
		{
			const int32 VS = At + Attr.Len();
			const int32 VE = Slice.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, VS);
			if (VE != INDEX_NONE) { bFound = true; return Slice.Mid(VS, VE - VS); }
		}
		const FString OpenT = FString::Printf(TEXT("<%s>"), Tag);
		At = Slice.Find(OpenT, ESearchCase::CaseSensitive);
		if (At != INDEX_NONE)
		{
			const int32 VS = At + OpenT.Len();
			const int32 VE = Slice.Find(FString::Printf(TEXT("</%s>"), Tag), ESearchCase::CaseSensitive, ESearchDir::FromStart, VS);
			if (VE != INDEX_NONE) { bFound = true; return Slice.Mid(VS, VE - VS); }
		}
		const FString SelfT = FString::Printf(TEXT("<%s />"), Tag);
		if (Slice.Contains(SelfT, ESearchCase::CaseSensitive)) { bFound = true; return FString(); }
		return FString();
	}

	// The item's ONE fTime line (measured: line 1 of the slice, three-space indent, 226,559/226,559). Returns the
	// span of the VALUE text inside the document, so a rewrite touches nothing but the digits.
	static bool FTimeValueSpan(const FString& Doc, const FCutRawItem& It, int32& OutStart, int32& OutLen, FString& OutText)
	{
		const FString Slice = Doc.Mid(It.Start, It.Len);
		const FString Key(TEXT("   <fTime value=\""));
		const int32 At = Slice.Find(Key, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE) { return false; }
		if (Slice.Find(Key, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1) != INDEX_NONE) { return false; }
		const int32 VS = At + Key.Len();
		const int32 VE = Slice.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, VS);
		if (VE == INDEX_NONE) { return false; }
		OutStart = It.Start + VS;
		OutLen = VE - VS;
		OutText = Slice.Mid(VS, VE - VS);
		return true;
	}

	static FString TypeAttr(const FString& Doc, const FCutRawItem& It)
	{
		const FString Slice = Doc.Mid(It.Start, FMath::Min(It.Len, 200));
		const FString Key(TEXT("<Item type=\""));
		const int32 At = Slice.Find(Key, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE) { return FString(); }
		const int32 VS = At + Key.Len();
		const int32 VE = Slice.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, VS);
		return VE == INDEX_NONE ? FString() : Slice.Mid(VS, VE - VS);
	}

	// The source document, its newline, and the six lists - everything both tools need.
	struct FCutSource
	{
		FString Path;
		FString Doc;
		FString NL;
		int32 RawBytes = 0;
		bool bEncodingRoundTrip = false;
		FCutBlock Objects, LoadEvents, Events, Args, Concat, Discard;
		TArray<FCutRawItem> AllEvents;      // load list first, then the event list - the sidecar's own order
		TArray<bool> EventIsLoadList;
	};

	// Load the file and prove UE's text loader round-trips it byte-for-byte before anything is spliced
	// (measured: 0 of 201,718,385 corpus bytes are non-ASCII, so this guard should never fire - but a
	// splice that cannot prove its own input is a splice that can corrupt one).
	static bool ReadSource(const FString& Path, FCutSource& Out, FString& Err)
	{
		TArray<uint8> Raw;
		if (!FFileHelper::LoadFileToArray(Raw, *Path)) { Err = TEXT("cannot read ") + Path; return false; }
		if (!FFileHelper::LoadFileToString(Out.Doc, *Path)) { Err = TEXT("cannot read ") + Path; return false; }
		Out.Path = Path;
		Out.RawBytes = Raw.Num();
		const FTCHARToUTF8 Enc(*Out.Doc);
		Out.bEncodingRoundTrip = (Enc.Length() == Raw.Num()) && (Raw.Num() == 0 || FMemory::Memcmp(Enc.Get(), Raw.GetData(), Raw.Num()) == 0);
		if (!Out.bEncodingRoundTrip)
		{
			Err = FString::Printf(TEXT("%s does not survive the editor's text loader byte-for-byte (%d bytes in, %d out) - refusing to splice a document RUDE cannot re-emit"),
				*Path, Raw.Num(), Enc.Length());
			return false;
		}
		Out.NL = NewlineOf(Out.Doc);
		struct FBlockSpec { const TCHAR* Tag; FCutBlock* Block; };
		const FBlockSpec Specs[] = {
			{ TEXT("pCutsceneObjects"), &Out.Objects },
			{ TEXT("pCutsceneLoadEventList"), &Out.LoadEvents },
			{ TEXT("pCutsceneEventList"), &Out.Events },
			{ TEXT("pCutsceneEventArgsList"), &Out.Args },
			{ TEXT("concatDataList"), &Out.Concat },
			{ TEXT("discardFrameList"), &Out.Discard },
		};
		for (const FBlockSpec& S : Specs)
		{
			FString Why;
			if (!SliceBlock(Out.Doc, S.Tag, Out.NL, *S.Block, Why)) { Err = Why; return false; }
		}
		for (const FCutRawItem& It : Out.LoadEvents.Items) { Out.AllEvents.Add(It); Out.EventIsLoadList.Add(true); }
		for (const FCutRawItem& It : Out.Events.Items) { Out.AllEvents.Add(It); Out.EventIsLoadList.Add(false); }
		return true;
	}

	// Where the .cut lives: the corpus ledger when CorpusRoot is one, the sidecar's own SourceFile when it is not.
	static bool ResolveCut(const FString& CorpusRoot, const FString& CutName, const FString& SidecarSource,
	                       FString& OutPath, FString& OutHow, FString& Err)
	{
		const FString Root = CorpusRoot.TrimStartAndEnd();
		if (!Root.IsEmpty() && FRudeCorpus::LooksLikeCorpus(Root))
		{
			FString CorpusErr;
			const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(Root, CorpusErr);
			if (!Corpus.IsValid()) { Err = CorpusErr; return false; }
			const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("cut"), CutName);
			if (!Row) { Err = FString::Printf(TEXT("the corpus has no cut named '%s'"), *CutName); return false; }
			OutPath = Corpus->PathOf(*Row);
			OutHow = TEXT("corpus");
		}
		else if (!Root.IsEmpty())
		{
			const FString Dir = FPaths::DirectoryExists(Root / TEXT("cut")) ? Root / TEXT("cut") : Root;
			OutPath = Dir / (CutName + TEXT(".cut.pso.xml"));
			OutHow = TEXT("folder");
		}
		else
		{
			OutPath = SidecarSource;
			OutHow = TEXT("sidecar");
		}
		if (OutPath.IsEmpty() || !FPaths::FileExists(OutPath))
		{
			Err = FString::Printf(TEXT("source cutscene not found: %s (resolved from %s)"), *OutPath, *OutHow);
			return false;
		}
		return true;
	}

	static URudeCutsceneEvents* FindSidecar(const ULevelSequence* LS, FString& OutPath)
	{
		if (!LS) { return nullptr; }
		const FString Pkg = LS->GetOutermost()->GetName();
		const FString Folder = FPackageName::GetLongPackagePath(Pkg);
		FString Short = FPackageName::GetShortName(Pkg);
		if (Short.StartsWith(TEXT("LS_"))) { Short.RightChopInline(3); }
		OutPath = Folder / (TEXT("DA_") + Short + TEXT("_events"));
		return LoadObject<URudeCutsceneEvents>(nullptr, *(OutPath + TEXT(".DA_") + Short + TEXT("_events")));
	}
}

// ---- ProbeCutsceneSource -----------------------------------------------------------------------------------
FString URudeToolset::ProbeCutsceneSource(const FString& CorpusRoot, const FString& CutName)
{
	using namespace RudeCutExport;
	FString Name = CutName.TrimStartAndEnd().ToLower();
	Name.RemoveFromEnd(TEXT(".xml")); Name.RemoveFromEnd(TEXT(".pso")); Name.RemoveFromEnd(TEXT(".cut"));
	if (Name.IsEmpty()) { return Fail(TEXT("give a cutscene name, e.g. ah_1_int")); }
	FString Path, How, Why;
	if (!ResolveCut(CorpusRoot, Name, FString(), Path, How, Why)) { return Fail(Why); }
	FCutSource Src;
	if (!ReadSource(Path, Src, Why)) { return Fail(Why); }

	int32 CamCuts = 0, ArgsMinusOne = 0, TimeOrderBreaks = 0;
	double PrevTime = -1.0;
	bool bPrevWasLoadList = true;
	for (int32 i = 0; i < Src.AllEvents.Num(); ++i)
	{
		bool bHas = false;
		const FString TimeText = ValueIn(Src.Doc, Src.AllEvents[i], TEXT("fTime"), bHas);
		const double T = FCString::Atod(*TimeText);
		if (Src.EventIsLoadList[i] != bPrevWasLoadList) { PrevTime = -1.0; bPrevWasLoadList = Src.EventIsLoadList[i]; }
		if (T + 1e-6 < PrevTime) { ++TimeOrderBreaks; }
		PrevTime = T;
		const FString ArgsIdx = ValueIn(Src.Doc, Src.AllEvents[i], TEXT("iEventArgsIndex"), bHas);
		const int32 AI = FCString::Atoi(*ArgsIdx);
		if (AI < 0) { ++ArgsMinusOne; continue; }
		if (Src.Args.Items.IsValidIndex(AI) && TypeAttr(Src.Doc, Src.Args.Items[AI]) == TEXT("rage__cutfCameraCutEventArgs")) { ++CamCuts; }
	}
	int32 CutListNumbers = 0;
	{
		const int32 At = Src.Doc.Find(TEXT("\n <cameraCutList>"), ESearchCase::CaseSensitive);
		if (At != INDEX_NONE)
		{
			const int32 VS = At + FString(TEXT("\n <cameraCutList>")).Len();
			const int32 VE = Src.Doc.Find(TEXT("</cameraCutList>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, VS);
			if (VE != INDEX_NONE)
			{
				TArray<FString> Tok;
				Src.Doc.Mid(VS, VE - VS).ParseIntoArrayWS(Tok);
				CutListNumbers = Tok.Num();
			}
		}
	}
	const bool bOk = Src.bEncodingRoundTrip && Src.Objects.bSliceExact && Src.LoadEvents.bSliceExact
		&& Src.Events.bSliceExact && Src.Args.bSliceExact && Src.Concat.bSliceExact && Src.Discard.bSliceExact;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"cut\":%s,\"file\":%s,\"resolvedBy\":%s,\"bytes\":%d,\"lineEndings\":%s,\"encodingRoundTrip\":%s,")
		TEXT("\"objects\":%d,\"loadEvents\":%d,\"events\":%d,\"eventsTotal\":%d,\"eventArgs\":%d,\"concatRows\":%d,\"discardRows\":%d,")
		TEXT("\"cameraCutEvents\":%d,\"eventsWithoutArgs\":%d,\"timeOrderBreaks\":%d,\"cameraCutListNumbers\":%d,\"sliceExact\":%s,")
		TEXT("\"note\":\"cameraCutList is a different list from the camera-cut events (its count matches in 53 of 816 measured files); RUDE never derives one from the other\"}"),
		bOk ? TEXT("true") : TEXT("false"), *JStr(Name), *JStr(Path), *JStr(How), Src.RawBytes,
		*JStr(Src.NL == TEXT("\r\n") ? TEXT("CRLF") : TEXT("LF")), Src.bEncodingRoundTrip ? TEXT("true") : TEXT("false"),
		Src.Objects.Items.Num(), Src.LoadEvents.Items.Num(), Src.Events.Items.Num(), Src.AllEvents.Num(),
		Src.Args.Items.Num(), Src.Concat.Items.Num(), Src.Discard.Items.Num(),
		CamCuts, ArgsMinusOne, TimeOrderBreaks, CutListNumbers, bOk ? TEXT("true") : TEXT("false"));
}

// ---- SetCutsceneEventTime ----------------------------------------------------------------------------------
FString URudeToolset::SetCutsceneEventTime(const FString& EventsAssetPath, const FString& EventSelector, const FString& NewTimeSeconds)
{
	using namespace RudeCutExport;
	const FString AssetPath = EventsAssetPath.TrimStartAndEnd();
	if (AssetPath.IsEmpty()) { return Fail(TEXT("give the cutscene events asset, e.g. /Game/RUDE/Cutscenes/ah_1_int/DA_ah_1_int_events")); }
	URudeCutsceneEvents* DA = LoadObject<URudeCutsceneEvents>(nullptr, *AssetPath);
	if (!DA)
	{
		const FString Short = FPackageName::GetShortName(AssetPath);
		DA = LoadObject<URudeCutsceneEvents>(nullptr, *(AssetPath + TEXT(".") + Short));
	}
	if (!DA) { return Fail(FString::Printf(TEXT("no cutscene events asset at %s (run ImportCutscene first)"), *AssetPath)); }
	const FString Sel = EventSelector.TrimStartAndEnd();
	if (Sel.IsEmpty()) { return Fail(TEXT("give an event ordinal, or CAMERACUT:<k> for the k-th camera cut")); }
	int32 Index = -1;
	if (Sel.StartsWith(TEXT("CAMERACUT:"), ESearchCase::IgnoreCase))
	{
		const int32 Want = FCString::Atoi(*Sel.Mid(10));
		int32 Seen = 0;
		for (int32 i = 0; i < DA->Events.Num(); ++i)
		{
			if (DA->Events[i].ArgsType != TEXT("rage__cutfCameraCutEventArgs")) { continue; }
			if (Seen == Want) { Index = i; break; }
			++Seen;
		}
		if (Index < 0) { return Fail(FString::Printf(TEXT("this cutscene has %d camera-cut events, so there is no CAMERACUT:%d"), Seen, Want)); }
	}
	else
	{
		if (!Sel.IsNumeric()) { return Fail(FString::Printf(TEXT("'%s' is neither an ordinal nor CAMERACUT:<k>"), *Sel)); }
		Index = FCString::Atoi(*Sel);
	}
	if (!DA->Events.IsValidIndex(Index))
	{
		return Fail(FString::Printf(TEXT("event %d is outside this cutscene's %d events"), Index, DA->Events.Num()));
	}
	const FString TimeText = NewTimeSeconds.TrimStartAndEnd();
	if (TimeText.IsEmpty() || !TimeText.IsNumeric()) { return Fail(FString::Printf(TEXT("'%s' is not a time in seconds"), *TimeText)); }
	const float NewTime = FCString::Atof(*TimeText);
	if (NewTime < 0.f) { return Fail(TEXT("a cutscene event cannot sit before the cutscene starts")); }
	const float Was = DA->Events[Index].Time;
	DA->Events[Index].Time = NewTime;
	DA->MarkPackageDirty();
	return FString::Printf(
		TEXT("{\"ok\":true,\"asset\":%s,\"cut\":%s,\"index\":%d,\"list\":%s,\"eventId\":%d,\"argsType\":%s,\"from\":%g,\"to\":%g,")
		TEXT("\"note\":\"the sidecar is the truth for an event's time; ExportCutscene splices this one line back into the source document\"}"),
		*JStr(AssetPath), *JStr(DA->CutName), Index, *JStr(DA->Events[Index].List), DA->Events[Index].EventId,
		*JStr(DA->Events[Index].ArgsType), Was, NewTime);
}

// ---- ExportCutscene ----------------------------------------------------------------------------------------
FString URudeToolset::ExportCutscene(const FString& LevelSequenceAssetPath, const FString& OutPath,
                                     const FString& CorpusRoot, const FString& Options)
{
	using namespace RudeCutExport;
	const FString LsPath = LevelSequenceAssetPath.TrimStartAndEnd();
	if (LsPath.IsEmpty()) { return Fail(TEXT("give the Level Sequence, e.g. /Game/RUDE/Cutscenes/ah_1_int/LS_ah_1_int")); }
	ULevelSequence* LS = LoadObject<ULevelSequence>(nullptr, *LsPath);
	if (!LS)
	{
		const FString Short = FPackageName::GetShortName(LsPath);
		LS = LoadObject<ULevelSequence>(nullptr, *(LsPath + TEXT(".") + Short));
	}
	if (!LS) { return Fail(FString::Printf(TEXT("no Level Sequence at %s (run ImportCutscene first)"), *LsPath)); }

	FString DaPath;
	URudeCutsceneEvents* DA = FindSidecar(LS, DaPath);
	if (!DA)
	{
		return Fail(FString::Printf(TEXT("no cutscene events sidecar at %s - a Level Sequence without its DA_<cut>_events asset has no source cutscene to splice against; re-run ImportCutscene"), *DaPath));
	}
	const FString Opt = Options.ToUpper();
	const bool bDryRun = Opt.Contains(TEXT("DRYRUN"));
	const bool bStrictCutList = Opt.Contains(TEXT("STRICTCUTLIST"));

	FString SrcPath, How, Why;
	if (!ResolveCut(CorpusRoot, DA->CutName, DA->SourceFile, SrcPath, How, Why)) { return Fail(Why); }
	FCutSource Src;
	if (!ReadSource(SrcPath, Src, Why)) { return Fail(Why); }

	// ---- 1) the sidecar against the source: anything but a time is a refusal, by name ------------------------
	TArray<FString> Refusals;
	auto Refuse = [&Refusals](const FString& Text) { if (Refusals.Num() < 24) { Refusals.Add(Text); } };
	if (DA->Events.Num() != Src.AllEvents.Num())
	{
		Refuse(FString::Printf(TEXT("the sidecar lists %d events and %s holds %d - an event was added or removed, which RUDE cannot express"),
			DA->Events.Num(), *FPaths::GetCleanFilename(SrcPath), Src.AllEvents.Num()));
	}
	if (DA->Objects.Num() != Src.Objects.Items.Num())
	{
		Refuse(FString::Printf(TEXT("the sidecar lists %d cutscene objects and the file holds %d - adding or removing an object is not expressible"),
			DA->Objects.Num(), Src.Objects.Items.Num()));
	}
	if (DA->ConcatXml.Num() != Src.Concat.Items.Num())
	{
		Refuse(FString::Printf(TEXT("the sidecar lists %d concat rows and the file holds %d - the concat model is carried, never rebuilt"),
			DA->ConcatXml.Num(), Src.Concat.Items.Num()));
	}
	if (DA->EventArgsXml.Num() != Src.Args.Items.Num())
	{
		Refuse(FString::Printf(TEXT("the sidecar lists %d event-argument records and the file holds %d - a new event type or argument record is not expressible (11,929 of 125,684 measured argument records are shared between events, so RUDE never writes one)"),
			DA->EventArgsXml.Num(), Src.Args.Items.Num()));
	}

	struct FCutEdit
	{
		int32 Index = -1;
		int32 ValStart = -1;
		int32 ValLen = 0;
		FString From;
		FString To;
		FString Source;   // "sidecar" | "sequence"
		bool bCameraCut = false;
	};
	TArray<FCutEdit> Edits;
	TArray<int32> CamCutEventIndex;        // into DA->Events / Src.AllEvents, in file order
	TArray<double> CamCutSourceTime;
	int32 FieldMismatches = 0, TimesUnchanged = 0;

	if (Refusals.Num() == 0)
	{
		for (int32 i = 0; i < Src.AllEvents.Num(); ++i)
		{
			const FCutRawItem& It = Src.AllEvents[i];
			const FRudeCutEvent& E = DA->Events[i];
			bool bHas = false;
			const FString ListName = Src.EventIsLoadList[i] ? TEXT("pCutsceneLoadEventList") : TEXT("pCutsceneEventList");
			if (!E.List.IsEmpty() && E.List != ListName)
			{
				Refuse(FString::Printf(TEXT("event %d says it belongs to %s but the file holds it in %s - the two lists cannot be reordered"), i, *E.List, *ListName));
				continue;
			}
			const FString SrcType = TypeAttr(Src.Doc, It);
			if (!E.EventType.IsEmpty() && E.EventType != SrcType)
			{
				Refuse(FString::Printf(TEXT("event %d is a %s in the sidecar and a %s in the file - a new event type is not expressible"), i, *E.EventType, *SrcType));
				continue;
			}
			const int32 SrcId = FCString::Atoi(*ValueIn(Src.Doc, It, TEXT("iEventId"), bHas));
			if (E.EventId != SrcId)
			{
				Refuse(FString::Printf(TEXT("event %d carries id %d in the sidecar and %d in the file - an event's id is not editable"), i, E.EventId, SrcId));
				continue;
			}
			const FString ArgsIdxText = ValueIn(Src.Doc, It, TEXT("iEventArgsIndex"), bHas);
			const int32 SrcArgs = bHas ? FCString::Atoi(*ArgsIdxText) : -1;
			if (E.ArgsIndex != SrcArgs)
			{
				Refuse(FString::Printf(TEXT("event %d points at argument record %d in the sidecar and %d in the file - re-pointing an event at other arguments is not expressible"), i, E.ArgsIndex, SrcArgs));
				continue;
			}
			const FString ObjText = ValueIn(Src.Doc, It, TEXT("iObjectId"), bHas);
			const int32 SrcObj = bHas ? FCString::Atoi(*ObjText) : -1;
			if (E.ObjectId != SrcObj) { ++FieldMismatches; }

			const bool bIsCamCut = Src.Args.Items.IsValidIndex(SrcArgs)
				&& TypeAttr(Src.Doc, Src.Args.Items[SrcArgs]) == TEXT("rage__cutfCameraCutEventArgs");

			int32 VS = -1, VL = 0; FString TimeText;
			if (!FTimeValueSpan(Src.Doc, It, VS, VL, TimeText))
			{
				Refuse(FString::Printf(TEXT("event %d has no single <fTime> line at the measured shape (226,559 of 226,559 corpus events do) - refusing rather than guessing where its time lives"), i));
				continue;
			}
			if (bIsCamCut) { CamCutEventIndex.Add(i); CamCutSourceTime.Add(FCString::Atod(*TimeText)); }
			const float SrcTime = (float)FCString::Atod(*TimeText);
			if (E.Time == SrcTime) { ++TimesUnchanged; continue; }
			FCutEdit Ed;
			Ed.Index = i; Ed.ValStart = VS; Ed.ValLen = VL; Ed.From = TimeText;
			Ed.To = RudeNumText(E.Time);
			Ed.Source = TEXT("sidecar");
			Ed.bCameraCut = bIsCamCut;
			Edits.Add(Ed);
		}
	}

	// ---- 2) the Level Sequence's camera cuts: structure always, times when the sidecar did not move them -----
	int32 LsCuts = 0, CutsPaired = 0, SequenceEdits = 0;
	TArray<double> LsCutSeconds;
	if (UMovieScene* MS = LS->GetMovieScene())
	{
		const FFrameRate Tick = MS->GetTickResolution();
		if (const UMovieSceneTrack* CC = MS->GetCameraCutTrack())
		{
			for (const UMovieSceneSection* S : CC->GetAllSections())
			{
				if (!S || !S->HasStartFrame()) { continue; }
				LsCutSeconds.Add(Tick.AsSeconds(FFrameTime(S->GetInclusiveStartFrame())));
			}
		}
	}
	LsCutSeconds.Sort();
	LsCuts = LsCutSeconds.Num();
	if (Refusals.Num() == 0 && LsCuts > 0)
	{
		// ImportCutscene inserts a cut at 0 when the cutscene's first camera cut is later than 0; that section
		// stands for no event, so the sequence may legitimately hold exactly one more cut than the file does.
		int32 Offset = 0;
		if (LsCuts == CamCutEventIndex.Num() + 1 && LsCutSeconds.Num() > 0 && FMath::IsNearlyZero(LsCutSeconds[0], 1e-4)
			&& (CamCutSourceTime.Num() == 0 || CamCutSourceTime[0] > 1e-4))
		{
			Offset = 1;
		}
		if (LsCuts - Offset != CamCutEventIndex.Num())
		{
			Refuse(FString::Printf(TEXT("the Level Sequence has %d camera cuts and the cutscene has %d camera-cut events - adding or removing a cut is not expressible; edit the times of the cuts the cutscene already has"),
				LsCuts, CamCutEventIndex.Num()));
		}
		else
		{
			for (int32 k = 0; k < CamCutEventIndex.Num(); ++k)
			{
				++CutsPaired;
				const double SeqTime = LsCutSeconds[k + Offset];
				const double SourceTime = CamCutSourceTime[k];
				if (FMath::Abs(SeqTime - SourceTime) <= 1e-3) { continue; }
				// the sequence moved this cut - does the sidecar agree?
				FCutEdit* Existing = Edits.FindByPredicate([&](const FCutEdit& Cand) { return Cand.Index == CamCutEventIndex[k]; });
				if (Existing)
				{
					if (FMath::Abs(FCString::Atod(*Existing->To) - SeqTime) > 1e-3)
					{
						Refuse(FString::Printf(TEXT("camera cut %d is at %.4f s in the Level Sequence and %s s in the sidecar - RUDE will not pick a winner; make them agree"),
							k, SeqTime, *Existing->To));
					}
					continue;
				}
				const int32 Idx = CamCutEventIndex[k];
				int32 VS = -1, VL = 0; FString TimeText;
				if (!FTimeValueSpan(Src.Doc, Src.AllEvents[Idx], VS, VL, TimeText)) { continue; }
				FCutEdit Ed;
				Ed.Index = Idx; Ed.ValStart = VS; Ed.ValLen = VL; Ed.From = TimeText;
				Ed.To = RudeNumText(SeqTime);
				Ed.Source = TEXT("sequence");
				Ed.bCameraCut = true;
				Edits.Add(Ed);
				++SequenceEdits;
			}
		}
	}

	// ---- 3) time order: the file stores events in time order (816/816 measured), so a jump is a REORDER ------
	int32 CameraCutEdits = 0;
	if (Refusals.Num() == 0 && Edits.Num() > 0)
	{
		TArray<double> Times;
		Times.Reserve(Src.AllEvents.Num());
		for (int32 i = 0; i < Src.AllEvents.Num(); ++i)
		{
			int32 VS = -1, VL = 0; FString TimeText;
			FTimeValueSpan(Src.Doc, Src.AllEvents[i], VS, VL, TimeText);
			Times.Add(FCString::Atod(*TimeText));
		}
		for (const FCutEdit& Ed : Edits)
		{
			Times[Ed.Index] = FCString::Atod(*Ed.To);
			if (Ed.bCameraCut) { ++CameraCutEdits; }
		}
		for (int32 i = 1; i < Times.Num(); ++i)
		{
			if (Src.EventIsLoadList[i] != Src.EventIsLoadList[i - 1]) { continue; }
			if (Times[i] + 1e-6 < Times[i - 1])
			{
				Refuse(FString::Printf(TEXT("moving an event to %g s puts it before event %d at %g s - the file stores its events in time order (816 of 816 measured files), so this is a reorder, which RUDE refuses to write"),
					Times[i], i - 1, Times[i - 1]));
				break;
			}
		}
	}
	if (bStrictCutList && CameraCutEdits > 0)
	{
		Refuse(FString::Printf(TEXT("STRICTCUTLIST: %d camera-cut times changed and <cameraCutList> cannot be derived from them (its count matches the camera-cut event count in only 53 of 816 measured files)"), CameraCutEdits));
	}

	// ---- 4) the splice: the source's own bytes, with only the edited digits replaced ------------------------
	FString OutDoc = Src.Doc;
	if (Refusals.Num() == 0 && Edits.Num() > 0)
	{
		TArray<FCutEdit> Ordered = Edits;
		Ordered.Sort([](const FCutEdit& A, const FCutEdit& B) { return A.ValStart > B.ValStart; });
		for (const FCutEdit& Ed : Ordered)
		{
			OutDoc = OutDoc.Left(Ed.ValStart) + Ed.To + OutDoc.RightChop(Ed.ValStart + Ed.ValLen);
		}
	}
	const bool bIdentical = OutDoc.Equals(Src.Doc, ESearchCase::CaseSensitive);

	// <cameraCutList> is carried verbatim (law 14 - it is NOT the camera-cut event times). Rather than SAY so in
	// the verdict, MEASURE it: pull the list's span out of the source and out of the document RUDE would write
	// and compare them. A splice that ever landed inside the list, or a source whose list is malformed, turns
	// this false - which is the only way the field can carry information.
	bool bCutListSame = true;
	{
		const FString ClKey(TEXT("\n <cameraCutList>"));
		const FString ClEndKey(TEXT("</cameraCutList>"));
		const auto ClSpan = [&ClKey, &ClEndKey](const FString& ClDoc, FString& ClOut) -> bool
		{
			const int32 ClAt = ClDoc.Find(ClKey, ESearchCase::CaseSensitive);
			if (ClAt == INDEX_NONE) { ClOut.Empty(); return false; }
			const int32 ClEnd = ClDoc.Find(ClEndKey, ESearchCase::CaseSensitive, ESearchDir::FromStart, ClAt);
			if (ClEnd == INDEX_NONE) { ClOut.Empty(); return false; }
			ClOut = ClDoc.Mid(ClAt, ClEnd + ClEndKey.Len() - ClAt);
			return true;
		};
		FString ClFrom, ClTo;
		const bool bClInSrc = ClSpan(Src.Doc, ClFrom);
		const bool bClInOut = ClSpan(OutDoc, ClTo);
		bCutListSame = (bClInSrc == bClInOut) && ClFrom.Equals(ClTo, ESearchCase::CaseSensitive);
	}

	FString Written = OutPath.TrimStartAndEnd();
	bool bWrote = false;
	if (Refusals.Num() == 0 && !bDryRun)
	{
		if (Written.IsEmpty()) { return Fail(TEXT("give an OutPath - a file ending .xml, or a folder to drop <cut>.cut.pso.xml into")); }
		if (!Written.EndsWith(TEXT(".xml"), ESearchCase::IgnoreCase)) { Written = Written / (DA->CutName + TEXT(".cut.pso.xml")); }
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Written), true);
		if (!FFileHelper::SaveStringToFile(OutDoc, *Written, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return Fail(TEXT("cannot write ") + Written);
		}
		bWrote = true;
	}

	FString EditJson;
	for (const FCutEdit& Ed : Edits)
	{
		EditJson += (EditJson.IsEmpty() ? TEXT("") : TEXT(",")) + FString::Printf(
			TEXT("{\"event\":%d,\"list\":%s,\"cameraCut\":%s,\"editor\":%s,\"from\":%s,\"to\":%s}"),
			Ed.Index, *JStr(Src.EventIsLoadList[Ed.Index] ? TEXT("pCutsceneLoadEventList") : TEXT("pCutsceneEventList")),
			Ed.bCameraCut ? TEXT("true") : TEXT("false"), *JStr(Ed.Source), *JStr(Ed.From), *JStr(Ed.To));
	}
	FString RefuseJson;
	for (const FString& R : Refusals)
	{
		RefuseJson += (RefuseJson.IsEmpty() ? TEXT("") : TEXT(",")) + JStr(R);
	}
	const bool bOk = Refusals.Num() == 0 && (bWrote || bDryRun);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"cut\":%s,\"levelSequence\":%s,\"eventsAsset\":%s,\"source\":%s,\"resolvedBy\":%s,\"out\":%s,\"written\":%s,")
		TEXT("\"sourceBytes\":%d,\"bytesIdentical\":%s,\"events\":%d,\"eventsUnchanged\":%d,\"eventsEdited\":%d,\"cameraCutEdits\":%d,")
		TEXT("\"editsFromSequence\":%d,\"cameraCutSections\":%d,\"cameraCutEvents\":%d,\"cutsPaired\":%d,\"objectIdMismatches\":%d,")
		TEXT("\"objects\":%d,\"eventArgs\":%d,\"concatRows\":%d,\"discardRows\":%d,\"lineEndings\":%s,\"cameraCutListUntouched\":%s,")
		TEXT("\"edits\":[%s],\"refusals\":[%s],")
		TEXT("\"note\":\"untouched records are the source file's own bytes; only an edited event's <fTime> digits are rewritten. cameraCutListUntouched is COMPUTED: the <cameraCutList> span in the source compared with the same span in the document RUDE would write. bytesIdentical describes THAT document, not the outcome - a refusal splices nothing, so it reads true while written reads false: read written and refusals[] for the outcome.\"}"),
		bOk ? TEXT("true") : TEXT("false"), *JStr(DA->CutName), *JStr(LsPath), *JStr(DaPath), *JStr(SrcPath), *JStr(How),
		*JStr(Written), bWrote ? TEXT("true") : TEXT("false"), Src.RawBytes, bIdentical ? TEXT("true") : TEXT("false"),
		Src.AllEvents.Num(), TimesUnchanged, Edits.Num(), CameraCutEdits, SequenceEdits, LsCuts, CamCutEventIndex.Num(),
		CutsPaired, FieldMismatches, Src.Objects.Items.Num(), Src.Args.Items.Num(), Src.Concat.Items.Num(),
		Src.Discard.Items.Num(), *JStr(Src.NL == TEXT("\r\n") ? TEXT("CRLF") : TEXT("LF")),
		bCutListSame ? TEXT("true") : TEXT("false"), *EditJson, *RefuseJson);
}
