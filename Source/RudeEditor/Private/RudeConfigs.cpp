// RUDE - RAGE <-> Unreal Development Environment
// Three small config lanes (wp10/configs): timecycle modifiers, text (gxt2), the blip catalog.
// Every layout here was MEASURED on the corpus's own files (scratchpad/wp10/configs/LAWS.md); nothing
// is derived from CodeWalker or Sollumz. Exporters splice or rebuild bytes, never re-spell a document.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "RudeTimecycle.h"
#include "RudeBlipCatalog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "XmlFile.h"

// ---- shared ------------------------------------------------------------------------------------
static FString RudeCfgFail(const FString& Why)
{
	return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
}

// A content-safe object name: lower-case, [a-z0-9_-] kept, everything else '_'.
static FString RudeCfgSlug(const FString& In)
{
	FString S = In.ToLower();
	for (TCHAR& C : S)
	{
		const bool bOk = (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_' || C == '-';
		if (!bOk) { C = '_'; }
	}
	return S;
}

static bool RudeCfgIsHex8(const FString& S, uint32& Out)
{
	if (S.Len() != 8) { return false; }
	for (TCHAR C : S) { if (!FChar::IsHexDigit(C)) { return false; } }
	Out = FParse::HexNumber(*S);
	return true;
}

// ================================================================================================
// TIMECYCLES
// ================================================================================================
// The document, measured over the corpus's 12 timecycle_mods_*.xml (4 base, 4 update, 4 DLC copies of
// _1; LAWS.md T1-T5): CRLF throughout; head '<?xml ...?>' NL NL '<timecycle_modifier_data version="1.000000">'
// NL; foot '</timecycle_modifier_data>' with NO trailing newline; each modifier is EITHER
//   '  <modifier name="N" numMods="K" userFlags="F">' NL ('    <mod>%.3f %.3f</mod>' NL)* '  </modifier>' NL
// or, when it has no mods, the self-closing '  <modifier name="N" numMods="0" userFlags="F" />' NL
// (13 of 2,974 modifiers). Anything the scanner cannot read is kept as a GAP slice and re-emitted
// verbatim, so an unknown shape can never be dropped (LAWS T5: 0 gaps outside those two shapes today).
struct FRudeTcSlice
{
	bool bGap = false;          // bytes between modifiers (none measured today; kept verbatim if ever)
	bool bSelfClosing = false;
	bool bIrregular = false;    // a block whose inner lines are not all '<mod>a b</mod>' (kept verbatim)
	FString Name;
	uint32 UserFlags = 0;
	int32 NumMods = 0;
	FString Raw;                // the slice's own bytes, line endings included
	TArray<TPair<FName, FRudeTimecycleMod>> Mods;
};

struct FRudeTcDoc
{
	FString Text;
	FString NL;
	int32 BodyStart = 0;        // first byte after the root open tag's line
	int32 FootStart = 0;        // index of "</timecycle_modifier_data>"
	TArray<FRudeTcSlice> Slices;
	int32 Modifiers = 0;
};

static bool RudeTcScan(const FString& Text, FRudeTcDoc& D, FString& Err)
{
	D.Text = Text;
	D.NL = Text.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
	const int32 RootAt = Text.Find(TEXT("<timecycle_modifier_data"), ESearchCase::CaseSensitive);
	if (RootAt == INDEX_NONE) { Err = TEXT("no <timecycle_modifier_data> root"); return false; }
	const int32 RootClose = Text.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, RootAt);
	if (RootClose == INDEX_NONE) { Err = TEXT("unterminated root tag"); return false; }
	D.BodyStart = RootClose + 1;
	if (Text.Mid(D.BodyStart, D.NL.Len()) == D.NL) { D.BodyStart += D.NL.Len(); }
	D.FootStart = Text.Find(TEXT("</timecycle_modifier_data>"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (D.FootStart == INDEX_NONE || D.FootStart < D.BodyStart) { Err = TEXT("no </timecycle_modifier_data> footer"); return false; }
	const FString Open = TEXT("  <modifier name=\"");
	const FString CloseBlock = TEXT("  </modifier>") + D.NL;
	int32 Pos = D.BodyStart;
	while (Pos < D.FootStart)
	{
		int32 M = Text.Find(Open, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
		if (M != INDEX_NONE && M >= D.FootStart) { M = INDEX_NONE; }
		if (M == INDEX_NONE)
		{
			FRudeTcSlice G; G.bGap = true; G.Raw = Text.Mid(Pos, D.FootStart - Pos); D.Slices.Add(G);
			break;
		}
		if (M > Pos) { FRudeTcSlice G; G.bGap = true; G.Raw = Text.Mid(Pos, M - Pos); D.Slices.Add(G); }
		// attributes, in the measured order: name, numMods, userFlags
		int32 Q = M + Open.Len();
		const int32 QE = Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Q);
		if (QE == INDEX_NONE) { Err = FString::Printf(TEXT("unterminated modifier name at %d"), M); return false; }
		FRudeTcSlice S;
		S.Name = Text.Mid(Q, QE - Q);
		Q = QE + 1;
		auto Attr = [&](const TCHAR* Prefix, uint32& OutVal) -> bool
		{
			const FString P(Prefix);
			if (Text.Mid(Q, P.Len()) != P) { return false; }
			Q += P.Len();
			const int32 E = Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Q);
			if (E == INDEX_NONE) { return false; }
			OutVal = (uint32)FCString::Atoi64(*Text.Mid(Q, E - Q));
			Q = E + 1;
			return true;
		};
		uint32 NumMods = 0, Flags = 0;
		if (!Attr(TEXT(" numMods=\""), NumMods) || !Attr(TEXT(" userFlags=\""), Flags))
		{
			Err = FString::Printf(TEXT("modifier '%s': attributes are not name/numMods/userFlags"), *S.Name); return false;
		}
		S.NumMods = (int32)NumMods; S.UserFlags = Flags;
		const FString SelfEnd = TEXT(" />") + D.NL;
		const FString BlockOpenEnd = TEXT(">") + D.NL;
		int32 End;
		if (Text.Mid(Q, SelfEnd.Len()) == SelfEnd)
		{
			S.bSelfClosing = true;
			End = Q + SelfEnd.Len();
		}
		else if (Text.Mid(Q, BlockOpenEnd.Len()) == BlockOpenEnd)
		{
			const int32 InnerStart = Q + BlockOpenEnd.Len();
			const int32 C = Text.Find(CloseBlock, ESearchCase::CaseSensitive, ESearchDir::FromStart, InnerStart);
			if (C == INDEX_NONE || C > D.FootStart) { Err = FString::Printf(TEXT("modifier '%s': no closing tag"), *S.Name); return false; }
			End = C + CloseBlock.Len();
			// inner lines: '    <tag>a b</tag>' NL, strictly
			FString Inner = Text.Mid(InnerStart, C - InnerStart);
			TArray<FString> Lines;
			Inner.ParseIntoArray(Lines, *D.NL, false);
			if (Lines.Num() > 0 && Lines.Last().IsEmpty()) { Lines.Pop(); }
			for (const FString& L : Lines)
			{
				bool bOk = L.StartsWith(TEXT("    <")) && !L.StartsWith(TEXT("     "));
				int32 TagEnd = INDEX_NONE;
				if (bOk) { TagEnd = L.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 5); bOk = TagEnd != INDEX_NONE; }
				FString Tag, Body;
				if (bOk)
				{
					Tag = L.Mid(5, TagEnd - 5);
					const FString CloseTag = TEXT("</") + Tag + TEXT(">");
					bOk = L.EndsWith(CloseTag) && L.Len() >= TagEnd + 1 + CloseTag.Len();
					if (bOk) { Body = L.Mid(TagEnd + 1, L.Len() - CloseTag.Len() - (TagEnd + 1)); }
				}
				TArray<FString> Vals;
				if (bOk) { Body.ParseIntoArray(Vals, TEXT(" "), true); bOk = Vals.Num() == 2 && FCString::IsNumeric(*Vals[0]) && FCString::IsNumeric(*Vals[1]); }
				if (!bOk) { S.bIrregular = true; break; }
				FRudeTimecycleMod V; V.Value = FCString::Atof(*Vals[0]); V.Weight = FCString::Atof(*Vals[1]);
				S.Mods.Add(TPair<FName, FRudeTimecycleMod>(FName(*Tag), V));
			}
		}
		else { Err = FString::Printf(TEXT("modifier '%s': neither ' />' nor '>' after userFlags"), *S.Name); return false; }
		S.Raw = Text.Mid(M, End - M);
		D.Slices.Add(S);
		++D.Modifiers;
		Pos = End;
	}
	return true;
}

// A modifier rebuilt from its asset, in the file's own spelling.
static FString RudeTcRebuild(const URudeTimecycle* A, const FString& NL)
{
	TArray<FName> Order = A->ModOrder;
	TSet<FName> Seen;
	Seen.Append(Order);
	TArray<FName> Rest;
	for (const auto& KV : A->Mods) { if (!Seen.Contains(KV.Key)) { Rest.Add(KV.Key); } }
	Rest.Sort(FNameLexicalLess());
	Order.Append(Rest);
	int32 Count = 0;
	FString Body;
	for (const FName& M : Order)
	{
		const FRudeTimecycleMod* V = A->Mods.Find(M);
		if (!V) { continue; }
		++Count;
		Body += FString::Printf(TEXT("    <%s>%.3f %.3f</%s>%s"), *M.ToString(), V->Value, V->Weight, *M.ToString(), *NL);
	}
	if (Count == 0)
	{
		return FString::Printf(TEXT("  <modifier name=\"%s\" numMods=\"0\" userFlags=\"%u\" />%s"), *A->Name, A->UserFlags, *NL);
	}
	return FString::Printf(TEXT("  <modifier name=\"%s\" numMods=\"%d\" userFlags=\"%u\">%s%s  </modifier>%s"),
		*A->Name, Count, A->UserFlags, *NL, *Body, *NL);
}

static void RudeTcFill(URudeTimecycle* A, const FRudeTcSlice& S, const FRudeCorpusEntry& E, int32 Ordinal)
{
	A->Name = S.Name;
	A->UserFlags = S.UserFlags;
	A->NumModsAsSpelled = S.NumMods;
	A->Mods.Reset();
	A->ModOrder.Reset();
	for (const auto& P : S.Mods) { A->Mods.Add(P.Key, P.Value); A->ModOrder.Add(P.Key); }
	A->SourceSlot = E.Slot;
	A->SourceFile = E.File;
	A->SourceIndex = Ordinal;
	A->SourceXml = S.Raw;
	A->SourceFieldsKey = A->FieldsKey();
}

// Every timecycle_mods_* document the corpus carries, in load order. NOT Effective(): the ledger keys
// these as (other, timecycle_mods_1) so a DLC copy would shadow update's, but the game loads each
// DLC's file additively; every copy is its own document here (LAWS T2).
static void RudeTcDocs(const FRudeCorpus& Corpus, TArray<const FRudeCorpusEntry*>& Out)
{
	TArray<const FRudeCorpusEntry*> Rows;
	Corpus.AllOfType(TEXT("other"), Rows);
	for (const FRudeCorpusEntry* E : Rows)
	{
		if (E->Name.StartsWith(TEXT("timecycle_mods_"))) { Out.Add(E); }
	}
}

FString URudeToolset::ImportTimecycles(const FString& CorpusRoot, const FString& DestFolder)
{
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return RudeCfgFail(CorpusErr); }
	if (!FPackageName::IsValidLongPackageName(DestFolder / TEXT("x"))) { return RudeCfgFail(TEXT("DestFolder must be a content path like /Game/RUDE/Timecycles")); }
	TArray<const FRudeCorpusEntry*> Docs;
	RudeTcDocs(*Corpus, Docs);
	if (Docs.Num() == 0) { return RudeCfgFail(TEXT("the corpus has no timecycle_mods_*.xml (type 'other')")); }
	int32 Files = 0, Seen = 0, Made = 0, Overwritten = 0, Invalid = 0, SelfClosing = 0, Irregular = 0, Gaps = 0, ModsTotal = 0;
	TSet<FString> Done;
	TSet<FString> Vocab;
	FString Refused;
	for (const FRudeCorpusEntry* E : Docs)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Corpus->PathOf(*E))) { Refused += FString::Printf(TEXT("%s\"%s/%s: unreadable\""), Refused.IsEmpty() ? TEXT("") : TEXT(","), *E->Slot, *E->File); continue; }
		FRudeTcDoc D; FString Err;
		if (!RudeTcScan(Text, D, Err)) { Refused += FString::Printf(TEXT("%s\"%s/%s: %s\""), Refused.IsEmpty() ? TEXT("") : TEXT(","), *E->Slot, *E->File, *RudeJsonEscape(Err)); continue; }
		++Files;
		int32 Ordinal = -1;
		for (const FRudeTcSlice& S : D.Slices)
		{
			if (S.bGap) { ++Gaps; continue; }
			++Ordinal; ++Seen;
			if (S.bSelfClosing) { ++SelfClosing; }
			if (S.bIrregular) { ++Irregular; }
			ModsTotal += S.Mods.Num();
			for (const auto& P : S.Mods) { Vocab.Add(P.Key.ToString()); }
			const FString Slug = RudeCfgSlug(S.Name);
			const FString PkgName = DestFolder / Slug;
			if (Slug.IsEmpty() || !FPackageName::IsValidLongPackageName(PkgName)) { ++Invalid; continue; }
			// Load the existing asset FIRST (the 2026-09-05 palette rail: a package created over an
			// unloaded file is re-serialised from disk by a later LoadObject, overwriting fresh fields).
			URudeTimecycle* A = LoadObject<URudeTimecycle>(nullptr, *(PkgName + TEXT(".") + Slug));
			UPackage* Pkg = A ? A->GetOutermost() : CreatePackage(*PkgName);
			bool bNew = false;
			if (!A) { A = NewObject<URudeTimecycle>(Pkg, FName(*Slug), RF_Public | RF_Standalone); bNew = true; }
			RudeTcFill(A, S, *E, Ordinal);
			Pkg->MarkPackageDirty();
			if (Done.Contains(Slug)) { ++Overwritten; } else { Done.Add(Slug); if (bNew) { ++Made; } else { ++Overwritten; } }
		}
	}
	const bool bOk = Done.Num() > 0 && Invalid == 0 && Refused.IsEmpty();
	return FString::Printf(
		TEXT("{\"ok\":%s,\"files\":%d,\"modifiersSeen\":%d,\"assets\":%d,\"created\":%d,\"overwrittenByLaterCopy\":%d,\"invalidNames\":%d,")
		TEXT("\"selfClosing\":%d,\"irregular\":%d,\"gaps\":%d,\"mods\":%d,\"modVocabulary\":%d,\"refused\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), Files, Seen, Done.Num(), Made, Overwritten, Invalid, SelfClosing, Irregular, Gaps, ModsTotal, Vocab.Num(), *Refused);
}

FString URudeToolset::ExportTimecycles(const FString& OutDir, const FString& DestFolder, const FString& CorpusRoot)
{
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return RudeCfgFail(CorpusErr); }
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return RudeCfgFail(TEXT("give an OutDir")); }
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	ARM.Get().ScanPathsSynchronous({ DestFolder }, true);
	ARM.Get().GetAssetsByPath(FName(*DestFolder), Assets, true);
	// group by source document; assets without one are RUDE-authored and go to their own file
	TMap<FString, TArray<URudeTimecycle*>> Groups;
	TArray<URudeTimecycle*> Orphans;
	int32 Loaded = 0;
	for (const FAssetData& AD : Assets)
	{
		URudeTimecycle* A = Cast<URudeTimecycle>(AD.GetAsset());
		if (!A) { continue; }
		++Loaded;
		if (A->SourceFile.IsEmpty()) { Orphans.Add(A); continue; }
		Groups.FindOrAdd(A->SourceSlot + TEXT("|") + A->SourceFile).Add(A);
	}
	if (Loaded == 0) { return RudeCfgFail(FString::Printf(TEXT("no URudeTimecycle assets under %s"), *DestFolder)); }
	TArray<const FRudeCorpusEntry*> Docs;
	RudeTcDocs(*Corpus, Docs);
	TMap<FString, const FRudeCorpusEntry*> DocByKey;
	for (const FRudeCorpusEntry* E : Docs) { DocByKey.Add(E->Slot + TEXT("|") + E->File, E); }
	int32 Written = 0, Refused = 0, Kept = 0, Edited = 0, Added = 0, NotRebuilt = 0;
	FString Files, RefusedJson, Manifest;
	auto Refuse = [&](const FString& Why) { ++Refused; RefusedJson += FString::Printf(TEXT("%s\"%s\""), RefusedJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(Why)); };
	auto Emit = [&](const FString& Rel, const FString& Doc) -> bool
	{
		const FString OutPath = OutDir / Rel;
		if (!FFileHelper::SaveStringToFile(Doc, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { Refuse(FString::Printf(TEXT("cannot write %s"), *OutPath)); return false; }
		++Written;
		Files += FString::Printf(TEXT("%s\"%s\""), Files.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(OutPath));
		Manifest += FString::Printf(TEXT("data_file 'TIMECYCLEMOD_FILE' '%s'\n"), *Rel);
		return true;
	};
	for (auto& KV : Groups)
	{
		const FRudeCorpusEntry* const* Row = DocByKey.Find(KV.Key);
		if (!Row) { Refuse(FString::Printf(TEXT("%s: no such document in the corpus"), *KV.Key)); continue; }
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Corpus->PathOf(**Row))) { Refuse(FString::Printf(TEXT("%s: unreadable"), *KV.Key)); continue; }
		FRudeTcDoc D; FString Err;
		if (!RudeTcScan(Text, D, Err)) { Refuse(FString::Printf(TEXT("%s: %s"), *KV.Key, *Err)); continue; }
		TMap<int32, const URudeTimecycle*> ByOrdinal;
		TArray<const URudeTimecycle*> New;
		for (const URudeTimecycle* A : KV.Value)
		{
			if (A->SourceIndex < 0) { New.Add(A); }
			else if (ByOrdinal.Contains(A->SourceIndex)) { Refuse(FString::Printf(TEXT("%s: two assets claim ordinal %d"), *KV.Key, A->SourceIndex)); }
			else { ByOrdinal.Add(A->SourceIndex, A); }
		}
		FString Body;
		int32 Ordinal = -1;
		for (const FRudeTcSlice& S : D.Slices)
		{
			if (S.bGap) { Body += S.Raw; continue; }
			++Ordinal;
			const URudeTimecycle* const* Found = ByOrdinal.Find(Ordinal);
			if (!Found) { Body += S.Raw; ++Kept; continue; }
			const URudeTimecycle* A = *Found;
			const bool bUntouched = !A->SourceXml.IsEmpty() && A->FieldsKey() == A->SourceFieldsKey;
			if (bUntouched) { Body += S.Raw; ++Kept; }
			else if (S.bIrregular) { Body += S.Raw; ++Kept; ++NotRebuilt; }
			else { Body += RudeTcRebuild(A, D.NL); ++Edited; }
		}
		New.Sort([](const URudeTimecycle& X, const URudeTimecycle& Y) { return X.Name < Y.Name; });
		for (const URudeTimecycle* A : New) { Body += RudeTcRebuild(A, D.NL); ++Added; }
		const FString Doc = D.Text.Left(D.BodyStart) + Body + D.Text.Mid(D.FootStart);
		Emit((*Row)->Slot / FPaths::GetCleanFilename((*Row)->File), Doc);
	}
	if (Orphans.Num() > 0)
	{
		// RUDE-authored modifiers with no source document: their own file in the measured spelling.
		Orphans.Sort([](const URudeTimecycle& X, const URudeTimecycle& Y) { return X.Name < Y.Name; });
		const FString NL = TEXT("\r\n");
		FString Doc = TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") + NL + NL + TEXT("<timecycle_modifier_data version=\"1.000000\">") + NL;
		for (const URudeTimecycle* A : Orphans) { Doc += RudeTcRebuild(A, NL); ++Added; }
		Doc += TEXT("</timecycle_modifier_data>");
		Emit(TEXT("rude/timecycle_mods_rude.xml"), Doc);
	}
	if (Written > 0)
	{
		FFileHelper::SaveStringToFile(TEXT("fx_version 'cerulean'\ngame 'gta5'\n\n") + Manifest, *(OutDir / TEXT("fxmanifest.lua")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	const bool bOk = Written > 0 && Refused == 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"assets\":%d,\"filesWritten\":%d,\"refused\":%d,\"kept\":%d,\"edited\":%d,\"added\":%d,\"notRebuilt\":%d,\"files\":[%s],\"refusals\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), Loaded, Written, Refused, Kept, Edited, Added, NotRebuilt, *Files, *RefusedJson);
}

// ================================================================================================
// TEXT (gxt2)
// ================================================================================================
// The layout, measured on 600 of the corpus's 19,477 gxt2 files (310,320 entries, 0 exceptions; LAWS.md X1-X6):
//   u32 '2TXG' | u32 count | count x (u32 hash, u32 offset) | u32 '2TXG' | u32 totalSize | strings
// Pairs sorted by hash ascending; offsets ascending; strings start at 16 + 8*count, each NUL-terminated
// and contiguous (no padding, no sharing); UTF-8; totalSize == file size. An empty table is 16 bytes.
struct FRudeGxt2Entry { uint32 Hash; FString Text; };

static bool RudeGxt2Parse(const TArray<uint8>& B, TArray<FRudeGxt2Entry>& Out, FString& Err)
{
	auto U32 = [&B](int32 At) { return (uint32)B[At] | ((uint32)B[At + 1] << 8) | ((uint32)B[At + 2] << 16) | ((uint32)B[At + 3] << 24); };
	if (B.Num() < 16) { Err = TEXT("shorter than the 16-byte minimum"); return false; }
	if (!(B[0] == '2' && B[1] == 'T' && B[2] == 'X' && B[3] == 'G')) { Err = TEXT("magic is not 2TXG"); return false; }
	const uint32 Count = U32(4);
	const int64 StringsAt = 16 + 8 * (int64)Count;
	if (StringsAt > B.Num()) { Err = FString::Printf(TEXT("count %u overruns the file"), Count); return false; }
	const int32 Magic2 = 8 + 8 * (int32)Count;
	if (!(B[Magic2] == '2' && B[Magic2 + 1] == 'T' && B[Magic2 + 2] == 'X' && B[Magic2 + 3] == 'G')) { Err = TEXT("second magic is not 2TXG"); return false; }
	if (U32(Magic2 + 4) != (uint32)B.Num()) { Err = FString::Printf(TEXT("size field %u != file size %d"), U32(Magic2 + 4), B.Num()); return false; }
	uint32 PrevHash = 0, PrevOff = 0;
	for (uint32 I = 0; I < Count; ++I)
	{
		const uint32 H = U32(8 + 8 * I), O = U32(12 + 8 * I);
		if (I > 0 && H <= PrevHash) { Err = FString::Printf(TEXT("hashes not ascending at entry %u"), I); return false; }
		if (I > 0 && O <= PrevOff) { Err = FString::Printf(TEXT("offsets not ascending at entry %u"), I); return false; }
		if (I == 0 && O != (uint32)StringsAt) { Err = FString::Printf(TEXT("first offset %u != 16+8*count"), O); return false; }
		const uint32 End = (I + 1 < Count) ? U32(12 + 8 * (I + 1)) : (uint32)B.Num();
		if (O >= End || End > (uint32)B.Num() || B[End - 1] != 0) { Err = FString::Printf(TEXT("entry %u is not NUL-terminated at the next offset"), I); return false; }
		for (uint32 K = O; K + 1 < End; ++K) { if (B[K] == 0) { Err = FString::Printf(TEXT("entry %u has an inner NUL (padding?)"), I); return false; } }
		FUTF8ToTCHAR Conv((const ANSICHAR*)B.GetData() + O, (int32)(End - 1 - O));
		Out.Add({ H, FString(Conv.Length(), Conv.Get()) });
		PrevHash = H; PrevOff = O;
	}
	return true;
}

FString URudeToolset::ImportText(const FString& CorpusRoot, const FString& TableName, const FString& DestFolder)
{
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return RudeCfgFail(CorpusErr); }
	// "<name>[@<language>]": the corpus keys gxt2 by name only, and every language spells the same
	// names (american_rel.rpf, french_rel.rpf, ... 20+ archives), so the language is part of the ask.
	// The language is matched as a SUBSTRING of the archive folder name and the highest-slot match wins:
	// "global" has 677 copies (americandlc.rpf in 40+ DLC packs, american.rpf patch copies, american_rel.rpf),
	// so "global@american" lands the newest DLC copy while "global@american_rel" narrows to the language
	// archive proper. The verdict names the slot and file it took.
	FString Name = TableName.TrimStartAndEnd().ToLower(), Lang = TEXT("american");
	{
		FString L, R;
		if (Name.Split(TEXT("@"), &L, &R)) { Name = L; Lang = R; }
	}
	if (Name.IsEmpty()) { return RudeCfgFail(TEXT("TableName = <gxt2 name>[@<language>], e.g. 'abgail2' or 'global@french'")); }
	if (!FPackageName::IsValidLongPackageName(DestFolder / TEXT("x"))) { return RudeCfgFail(TEXT("DestFolder must be a content path like /Game/RUDE/Text")); }
	const TArray<const FRudeCorpusEntry*> Copies = Corpus->History(TEXT("gxt2"), Name);
	const FRudeCorpusEntry* Pick = nullptr;
	TSet<FString> Langs;
	for (const FRudeCorpusEntry* E : Copies)
	{
		// the language archive is the path component ending in ".rpf" right above the file
		const FString Dir = FPaths::GetPath(E->File);
		const FString Archive = FPaths::GetCleanFilename(Dir);
		Langs.Add(Archive);
		if (Archive.Contains(Lang)) { Pick = E; }   // History is lowest slot first: the last match wins
	}
	if (!Pick)
	{
		FString Avail; for (const FString& L : Langs) { Avail += (Avail.IsEmpty() ? TEXT("") : TEXT(", ")) + L; }
		return RudeCfgFail(FString::Printf(TEXT("no gxt2 '%s' for language '%s' (%d copies; archives: %s)"), *Name, *Lang, Copies.Num(), *Avail));
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Corpus->PathOf(*Pick))) { return RudeCfgFail(TEXT("cannot read ") + Corpus->PathOf(*Pick)); }
	TArray<FRudeGxt2Entry> Entries; FString Err;
	if (!RudeGxt2Parse(Bytes, Entries, Err)) { return RudeCfgFail(FString::Printf(TEXT("%s: %s"), *Pick->File, *Err)); }
	const FString PkgName = DestFolder / RudeCfgSlug(Lang) / RudeCfgSlug(Name);
	if (!FPackageName::IsValidLongPackageName(PkgName)) { return RudeCfgFail(TEXT("invalid content path ") + PkgName); }
	const FString ObjName = RudeCfgSlug(Name);
	UStringTable* T = LoadObject<UStringTable>(nullptr, *(PkgName + TEXT(".") + ObjName));
	UPackage* Pkg = T ? T->GetOutermost() : CreatePackage(*PkgName);
	const bool bNew = T == nullptr;
	if (!T) { T = NewObject<UStringTable>(Pkg, FName(*ObjName), RF_Public | RF_Standalone); }
	FStringTableRef Table = T->GetMutableStringTable();
	Table->ClearSourceStrings(Entries.Num());
	for (const FRudeGxt2Entry& E : Entries)
	{
#if WITH_EDITORONLY_DATA
		Table->SetSourceString(FTextKey(FString::Printf(TEXT("%08X"), E.Hash)), E.Text, FString());
#else
		Table->SetSourceString(FTextKey(FString::Printf(TEXT("%08X"), E.Hash)), E.Text);
#endif
	}
	Pkg->MarkPackageDirty();
	return FString::Printf(
		TEXT("{\"ok\":true,\"table\":\"%s\",\"language\":\"%s\",\"slot\":\"%s\",\"file\":\"%s\",\"copies\":%d,\"bytes\":%d,\"entries\":%d,\"created\":%s,\"asset\":\"%s\"}"),
		*RudeJsonEscape(Name), *RudeJsonEscape(Lang), *RudeJsonEscape(Pick->Slot), *RudeJsonEscape(Pick->File), Copies.Num(), Bytes.Num(), Entries.Num(),
		bNew ? TEXT("true") : TEXT("false"), *RudeJsonEscape(PkgName + TEXT(".") + ObjName));
}

FString URudeToolset::ExportText(const FString& StringTableAsset, const FString& OutGxt2Path)
{
	if (OutGxt2Path.TrimStartAndEnd().IsEmpty()) { return RudeCfgFail(TEXT("give an OutGxt2Path")); }
	UStringTable* T = LoadObject<UStringTable>(nullptr, *StringTableAsset);
	if (!T) { return RudeCfgFail(TEXT("no UStringTable at ") + StringTableAsset); }
	TArray<FRudeGxt2Entry> Entries;
	int32 KeysHashed = 0;
	T->GetStringTable()->EnumerateKeysAndSourceStrings([&](const FTextKey& Key, const FString& Str) -> bool
	{
		const FString K = Key.ToString();
		uint32 H;
		if (!RudeCfgIsHex8(K, H)) { H = RudeJoaat(K); ++KeysHashed; }   // a label key: joaat, the game's own key hash (LAWS X7: inferred)
		Entries.Add({ H, Str });
		return true;
	});
	Entries.Sort([](const FRudeGxt2Entry& A, const FRudeGxt2Entry& B) { return A.Hash < B.Hash; });
	for (int32 I = 1; I < Entries.Num(); ++I)
	{
		if (Entries[I].Hash == Entries[I - 1].Hash) { return RudeCfgFail(FString::Printf(TEXT("two keys hash to %08X; the layout cannot hold both"), Entries[I].Hash)); }
	}
	TArray<uint8> B;
	auto PutU32 = [&B](uint32 V) { B.Add((uint8)V); B.Add((uint8)(V >> 8)); B.Add((uint8)(V >> 16)); B.Add((uint8)(V >> 24)); };
	auto PutMagic = [&B]() { B.Add('2'); B.Add('T'); B.Add('X'); B.Add('G'); };
	const uint32 Count = (uint32)Entries.Num();
	PutMagic(); PutU32(Count);
	TArray<TArray<uint8>> Utf8;
	Utf8.Reserve(Entries.Num());
	uint32 Off = 16 + 8 * Count;
	for (const FRudeGxt2Entry& E : Entries)
	{
		FTCHARToUTF8 Conv(*E.Text);
		TArray<uint8> S; S.Append((const uint8*)Conv.Get(), Conv.Length()); S.Add(0);
		PutU32(E.Hash); PutU32(Off);
		Off += (uint32)S.Num();
		Utf8.Add(MoveTemp(S));
	}
	PutMagic(); PutU32(Off);
	for (const TArray<uint8>& S : Utf8) { B.Append(S); }
	if (!FFileHelper::SaveArrayToFile(B, *OutGxt2Path)) { return RudeCfgFail(TEXT("cannot write ") + OutGxt2Path); }
	return FString::Printf(TEXT("{\"ok\":true,\"entries\":%d,\"bytes\":%d,\"keysHashedFromLabels\":%d,\"file\":\"%s\"}"),
		Entries.Num(), B.Num(), KeysHashed, *RudeJsonEscape(OutGxt2Path));
}

// ================================================================================================
// BLIPS
// ================================================================================================
// Where the blips live (LAWS.md B1-B4): the sprite NAMES are the ExportAssets of minimap.gfx (a Scaleform
// SWF kept binary in the corpus: 'GFX' signature, version, u32 length, then the SWF tag stream), and the
// PIXELS are four sheets declared by minimap.ytd (blips_texturesheet 512x256, _ng 1024x1024, _ng_2
// 1024x1024, _ng_3 1024x512). x64a.rpf/textures/blips.ytd is an EMPTY dictionary. The corpus carries the
// ytd manifest but NO pixel sidecars for any cdimages ytd (0 of 1,092 scaleform_generic files have one),
// so ImportYtd reports missing pixels unless a later ROUT export writes them.
FString URudeToolset::BuildBlipCatalog(const FString& CorpusRoot, const FString& DestFolder)
{
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return RudeCfgFail(CorpusErr); }
	if (!FPackageName::IsValidLongPackageName(DestFolder / TEXT("x"))) { return RudeCfgFail(TEXT("DestFolder must be a content path like /Game/RUDE/Blips")); }
	const FRudeCorpusEntry* Gfx = Corpus->Effective(TEXT("gfx"), TEXT("minimap"));
	const FRudeCorpusEntry* Ytd = Corpus->Effective(TEXT("ytd"), TEXT("minimap"));
	if (!Gfx) { return RudeCfgFail(TEXT("the corpus has no minimap.gfx (type gfx, name minimap)")); }
	if (!Ytd) { return RudeCfgFail(TEXT("the corpus has no minimap.ytd.xml (type ytd, name minimap)")); }

	// 1) the sheets: ImportYtd on minimap.ytd with its sidecar folder (pixels when the corpus has them)
	const FString YtdVerdict = URudeToolset::ImportYtd(Corpus->PathOf(*Ytd), Corpus->SidecarDirOf(*Ytd), DestFolder);
	const int32 SheetsImported = RudeSumField(YtdVerdict, TEXT("imported"));
	const int32 SheetsDeclared = RudeSumField(YtdVerdict, TEXT("declared"));
	const int32 PixelsMissing = RudeSumField(YtdVerdict, TEXT("missingPixelCount"));
	TArray<FString> BlipSheets;
	TMap<FString, FIntPoint> Sizes;
	{
		FXmlFile Xml(Corpus->PathOf(*Ytd));
		if (!Xml.IsValid() || !Xml.GetRootNode()) { return RudeCfgFail(TEXT("minimap.ytd.xml does not parse")); }
		for (const FXmlNode* Item : Xml.GetRootNode()->GetChildrenNodes())
		{
			if (Item->GetTag() != TEXT("Item")) { continue; }
			const FXmlNode* N = Item->FindChildNode(TEXT("Name"));
			if (!N) { continue; }
			const FString TexName = N->GetContent().TrimStartAndEnd();
			const FXmlNode* W = Item->FindChildNode(TEXT("Width")); const FXmlNode* H = Item->FindChildNode(TEXT("Height"));
			Sizes.Add(TexName, FIntPoint(W ? FCString::Atoi(*W->GetAttribute(TEXT("value"))) : 0, H ? FCString::Atoi(*H->GetAttribute(TEXT("value"))) : 0));
			if (TexName.StartsWith(TEXT("blips_texturesheet"), ESearchCase::IgnoreCase)) { BlipSheets.Add(TexName); }
		}
	}
	if (BlipSheets.Num() == 0) { return RudeCfgFail(TEXT("minimap.ytd declares no blips_texturesheet* texture")); }

	// 2) the names: walk minimap.gfx's SWF tags for ExportAssets (56)
	TArray<uint8> G;
	if (!FFileHelper::LoadFileToArray(G, *Corpus->PathOf(*Gfx))) { return RudeCfgFail(TEXT("cannot read ") + Corpus->PathOf(*Gfx)); }
	if (G.Num() < 12) { return RudeCfgFail(TEXT("minimap.gfx is shorter than a SWF header")); }
	if (G[0] == 'C' && G[1] == 'F' && G[2] == 'X') { return RudeCfgFail(TEXT("minimap.gfx is zlib-compressed (CFX); this lane reads the uncompressed GFX form only")); }
	if (!(G[0] == 'G' && G[1] == 'F' && G[2] == 'X')) { return RudeCfgFail(TEXT("minimap.gfx signature is not GFX")); }
	auto U16 = [&G](int32 At) { return (uint32)G[At] | ((uint32)G[At + 1] << 8); };
	auto U32 = [&G](int32 At) { return (uint32)G[At] | ((uint32)G[At + 1] << 8) | ((uint32)G[At + 2] << 16) | ((uint32)G[At + 3] << 24); };
	const int32 Version = G[3];
	if (U32(4) != (uint32)G.Num()) { return RudeCfgFail(FString::Printf(TEXT("minimap.gfx declares %u bytes, file has %d"), U32(4), G.Num())); }
	const int32 NBits = G[8] >> 3;
	int32 Pos = 8 + (5 + 4 * NBits + 7) / 8 + 4;   // RECT (bit-packed) + u16 frame rate + u16 frame count
	int32 Tags = 0;
	TArray<TPair<int32, FString>> Exports;
	while (Pos + 2 <= G.Num())
	{
		const uint32 Hdr = U16(Pos); Pos += 2;
		const int32 Code = (int32)(Hdr >> 6);
		int64 Len = Hdr & 0x3F;
		if (Len == 0x3F) { if (Pos + 4 > G.Num()) { return RudeCfgFail(TEXT("truncated long tag header")); } Len = U32(Pos); Pos += 4; }
		if (Pos + Len > G.Num()) { return RudeCfgFail(FString::Printf(TEXT("tag %d at %d overruns the file"), Code, Pos)); }
		++Tags;
		if (Code == 56 && Len >= 2)
		{
			const int32 End = Pos + (int32)Len;
			const int32 N = (int32)U16(Pos);
			int32 Q = Pos + 2;
			for (int32 I = 0; I < N && Q + 2 < End; ++I)
			{
				const int32 Cid = (int32)U16(Q); Q += 2;
				int32 Z = Q; while (Z < End && G[Z] != 0) { ++Z; }
				FUTF8ToTCHAR Conv((const ANSICHAR*)G.GetData() + Q, Z - Q);
				Exports.Add(TPair<int32, FString>(Cid, FString(Conv.Length(), Conv.Get())));
				Q = Z + 1;
			}
		}
		Pos += (int32)Len;
		if (Code == 0) { break; }
	}
	if (Exports.Num() == 0) { return RudeCfgFail(FString::Printf(TEXT("minimap.gfx has no ExportAssets tag (%d tags walked)"), Tags)); }

	// 3) the catalog asset
	const FString PkgName = DestFolder / TEXT("BlipCatalog");
	URudeBlipCatalog* C = LoadObject<URudeBlipCatalog>(nullptr, *(PkgName + TEXT(".BlipCatalog")));
	UPackage* Pkg = C ? C->GetOutermost() : CreatePackage(*PkgName);
	if (!C) { C = NewObject<URudeBlipCatalog>(Pkg, FName(TEXT("BlipCatalog")), RF_Public | RF_Standalone); }
	C->Blips.Reset(); C->IndexByName.Reset(); C->Sheets.Reset(); C->SheetSizes.Reset(); C->OtherExports.Reset();
	int32 Dupes = 0;
	for (const TPair<int32, FString>& E : Exports)
	{
		if (!E.Value.StartsWith(TEXT("radar_"))) { C->OtherExports.Add(E.Value); continue; }
		const FName Key(*E.Value.ToLower());
		if (C->IndexByName.Contains(Key)) { ++Dupes; continue; }
		FRudeBlipEntry B; B.Name = E.Value; B.SwfCharacterId = E.Key;
		C->IndexByName.Add(Key, C->Blips.Add(B));
	}
	// ImportYtd lands assets at <DestFolder>/<TxdName>/<TexName>; TxdName = "minimap"
	int32 SheetsWithPixels = 0;
	for (const FString& S : BlipSheets)
	{
		const FString SheetPkg = DestFolder / TEXT("minimap") / S;
		// on disk OR still in memory (a -nosave gate run imports without saving)
		if (FPackageName::DoesPackageExist(SheetPkg) || FindPackage(nullptr, *SheetPkg) != nullptr) { ++SheetsWithPixels; }
		C->Sheets.Add(TSoftObjectPtr<UTexture2D>(FSoftObjectPath(SheetPkg + TEXT(".") + S)));
		C->SheetSizes.Add(S, Sizes[S]);
	}
	C->SourceGfxSlot = Gfx->Slot; C->SourceGfxFile = Gfx->File; C->GfxVersion = Version;
	C->SourceYtdSlot = Ytd->Slot; C->SourceYtdFile = Ytd->File;
	Pkg->MarkPackageDirty();
	FString SheetJson;
	for (const FString& S : BlipSheets) { SheetJson += FString::Printf(TEXT("%s\"%s\":[%d,%d]"), SheetJson.IsEmpty() ? TEXT("") : TEXT(","), *S, Sizes[S].X, Sizes[S].Y); }
	const bool bOk = C->Blips.Num() > 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"gfx\":{\"slot\":\"%s\",\"file\":\"%s\",\"version\":%d,\"bytes\":%d,\"tags\":%d,\"exports\":%d},\"blips\":%d,\"duplicateNames\":%d,\"otherExports\":%d,")
		TEXT("\"sheets\":{%s},\"sheetsWithPixels\":%d,\"ytd\":{\"slot\":\"%s\",\"file\":\"%s\",\"declared\":%d,\"imported\":%d,\"missingPixels\":%d},\"sheetPerBlip\":\"unresolved (null)\",\"catalog\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Gfx->Slot), *RudeJsonEscape(Gfx->File), Version, G.Num(), Tags, Exports.Num(), C->Blips.Num(), Dupes, C->OtherExports.Num(),
		*SheetJson, SheetsWithPixels, *RudeJsonEscape(Ytd->Slot), *RudeJsonEscape(Ytd->File), SheetsDeclared, SheetsImported, PixelsMissing, *RudeJsonEscape(PkgName + TEXT(".BlipCatalog")));
}
