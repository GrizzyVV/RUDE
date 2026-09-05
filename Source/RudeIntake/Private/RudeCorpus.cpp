// RUDE - RAGE <-> Unreal Development Environment
#include "RudeCorpus.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogRudeCorpus, Log, All);

namespace
{
	// The only ledger version this reader understands. A newer corpus is refused with a reason.
	constexpr int32 SupportedRoutVersion = 1;

	FString NormalizeRoot(const FString& In)
	{
		FString R = In;
		FPaths::NormalizeDirectoryName(R);
		R = FPaths::ConvertRelativePathToFull(R);
		while (R.EndsWith(TEXT("/"))) { R.LeftChopInline(1); }
		return R;
	}

	// Pull `"key":"value"` out of one JSON line. The ledger is machine-written with a fixed key
	// set and forward-slash paths, so a targeted scan beats a general parser 389k times over;
	// escapes are honoured for the one case that can occur (a backslash-escaped quote).
	bool ExtractField(const ANSICHAR* Line, int32 Len, const ANSICHAR* KeyQuoted, int32 KeyLen, FString& Out)
	{
		for (int32 i = 0; i + KeyLen <= Len; ++i)
		{
			if (FMemory::Memcmp(Line + i, KeyQuoted, KeyLen) != 0) { continue; }
			int32 p = i + KeyLen;
			while (p < Len && (Line[p] == ' ' || Line[p] == ':')) { ++p; }
			if (p >= Len || Line[p] != '"') { return false; }
			++p;
			TArray<ANSICHAR> Buf;
			Buf.Reserve(64);
			while (p < Len && Line[p] != '"')
			{
				if (Line[p] == '\\' && p + 1 < Len) { ++p; }
				Buf.Add(Line[p]);
				++p;
			}
			Buf.Add(0);
			Out = FString(UTF8_TO_TCHAR(Buf.GetData()));
			return true;
		}
		return false;
	}
}

bool FRudeCorpus::LooksLikeCorpus(const FString& Root)
{
	const FString R = NormalizeRoot(Root);
	return FPaths::FileExists(R / TEXT("_FILEBASE.json")) && FPaths::FileExists(R / TEXT("_PROVENANCE.jsonl"));
}

FString FRudeCorpus::AssetNameOf(const FString& Type, const FString& SourceName)
{
	FString N = SourceName.ToLower();
	const FString T = Type.ToLower();
	if (N.EndsWith(TEXT(".xml"))) { N.LeftChopInline(4); }
	// Gate suffixes ROUT writes for the meta family: "<name>.ymt.rbf.xml", "<name>.ymt.pso.xml",
	// "<name>.cut.pso.xml". The asset is still "<name>".
	for (int32 Guard = 0; Guard < 2; ++Guard)
	{
		if (N.EndsWith(TEXT(".rbf"))) { N.LeftChopInline(4); }
		else if (N.EndsWith(TEXT(".pso"))) { N.LeftChopInline(4); }
	}
	if (!T.IsEmpty() && N.EndsWith(TEXT(".") + T)) { N.LeftChopInline(T.Len() + 1); }
	return N;
}

TSharedPtr<FRudeCorpus> FRudeCorpus::Open(const FString& InRoot, FString& OutError)
{
	static TMap<FString, TSharedPtr<FRudeCorpus>> Cache;
	const FString Root = NormalizeRoot(InRoot);
	if (Root.IsEmpty())
	{
		OutError = TEXT("no corpus root given");
		return nullptr;
	}
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*(Root / TEXT("_PROVENANCE.jsonl")));
	if (const TSharedPtr<FRudeCorpus>* Hit = Cache.Find(Root))
	{
		if ((*Hit).IsValid() && (*Hit)->LedgerStamp == Stamp) { return *Hit; }
		Cache.Remove(Root);
	}
	TSharedPtr<FRudeCorpus> C = MakeShared<FRudeCorpus>();
	if (!C->Load(Root, OutError)) { return nullptr; }
	Cache.Add(Root, C);
	return C;
}

bool FRudeCorpus::Load(const FString& InRoot, FString& OutError)
{
	Root = InRoot;
	if (!LooksLikeCorpus(Root))
	{
		OutError = FString::Printf(TEXT("%s is not a corpus: needs _FILEBASE.json and _PROVENANCE.jsonl "
			"(a ROUT filebase root, the folder holding 00_base/)"), *Root);
		return false;
	}
	if (!LoadFilebase(OutError)) { return false; }
	LedgerStamp = IFileManager::Get().GetTimeStamp(*(Root / TEXT("_PROVENANCE.jsonl")));
	const double T0 = FPlatformTime::Seconds();
	if (!LoadLedger(OutError)) { return false; }
	UE_LOG(LogRudeCorpus, Display, TEXT("[RUDE] corpus %s: %d ledger rows, %d identities, %d types, "
		"routVersion %d, %.2fs"), *Root, Entries.Num(), Index.Num(), ByTypeIdx.Num(), RoutVersion,
		FPlatformTime::Seconds() - T0);
	return true;
}

bool FRudeCorpus::LoadFilebase(FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *(Root / TEXT("_FILEBASE.json"))))
	{
		OutError = TEXT("cannot read _FILEBASE.json");
		return false;
	}
	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		OutError = TEXT("_FILEBASE.json is not valid JSON");
		return false;
	}
	RoutVersion = (int32)Obj->GetNumberField(TEXT("routVersion"));
	if (RoutVersion != SupportedRoutVersion)
	{
		OutError = FString::Printf(TEXT("corpus routVersion %d, this RUDE reads %d - refusing rather than misread"),
			RoutVersion, SupportedRoutVersion);
		return false;
	}
	Title = Obj->GetStringField(TEXT("title"));
	Precedence.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Prec = nullptr;
	if (Obj->TryGetArrayField(TEXT("precedence"), Prec))
	{
		for (const TSharedPtr<FJsonValue>& V : *Prec) { Precedence.Add(V->AsString()); }
	}
	if (Precedence.Num() == 0)
	{
		Precedence = { TEXT("00_base"), TEXT("10_update"), TEXT("20_dlc/<order>_<name>") };
	}
	return true;
}

int32 FRudeCorpus::SlotRank(const FString& Slot) const
{
	// Precedence is a list of slot patterns; DLC packs share one pattern and rank by their own
	// ordinal ("20_dlc/017_mpheist" -> 2 + 17), which is the dlclist.xml order ROUT filed them in.
	const FString S = Slot.ToLower();
	for (int32 i = 0; i < Precedence.Num(); ++i)
	{
		const FString P = Precedence[i].ToLower();
		if (P == S) { return i * 1000; }
		const int32 Slash = P.Find(TEXT("/"));
		if (Slash != INDEX_NONE && S.StartsWith(P.Left(Slash + 1)))
		{
			const FString Tail = S.Mid(Slash + 1);
			int32 Ordinal = 0;
			for (int32 k = 0; k < Tail.Len() && FChar::IsDigit(Tail[k]); ++k)
			{
				Ordinal = Ordinal * 10 + (Tail[k] - TEXT('0'));
			}
			return i * 1000 + Ordinal;
		}
	}
	return -1;
}

bool FRudeCorpus::LoadLedger(FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *(Root / TEXT("_PROVENANCE.jsonl"))))
	{
		OutError = TEXT("cannot read _PROVENANCE.jsonl");
		return false;
	}
	Entries.Reset();
	Entries.Reserve(400000);
	Index.Reset();
	ByTypeIdx.Reset();
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	const int32 Total = Bytes.Num();
	int32 LineStart = 0;
	int32 Malformed = 0;
	while (LineStart < Total)
	{
		int32 LineEnd = LineStart;
		while (LineEnd < Total && Data[LineEnd] != '\n') { ++LineEnd; }
		int32 Len = LineEnd - LineStart;
		if (Len > 0 && Data[LineStart + Len - 1] == '\r') { --Len; }
		if (Len > 2)
		{
			const ANSICHAR* L = Data + LineStart;
			FRudeCorpusEntry E;
			const bool bOk = ExtractField(L, Len, "\"slot\"", 6, E.Slot)
				&& ExtractField(L, Len, "\"type\"", 6, E.Type)
				&& ExtractField(L, Len, "\"file\"", 6, E.File)
				&& ExtractField(L, Len, "\"sourceName\"", 12, E.SourceName);
			if (bOk)
			{
				ExtractField(L, Len, "\"sha1\"", 6, E.Sha1);
				E.Type.ToLowerInline();
				E.Name = AssetNameOf(E.Type, E.SourceName);
				E.SlotRank = SlotRank(E.Slot);
				E.bConverted = E.SourceName.EndsWith(TEXT(".xml"), ESearchCase::IgnoreCase)
					&& !(E.Type == TEXT("xml") || E.Type == TEXT("other"));
				const int32 Idx = Entries.Add(MoveTemp(E));
				Index.FindOrAdd(Key(Entries[Idx].Type, Entries[Idx].Name)).Add(Idx);
				ByTypeIdx.FindOrAdd(Entries[Idx].Type).Add(Idx);
			}
			else
			{
				++Malformed;
			}
		}
		LineStart = LineEnd + 1;
	}
	if (Entries.Num() == 0)
	{
		OutError = TEXT("_PROVENANCE.jsonl holds no readable rows");
		return false;
	}
	if (Malformed > 0)
	{
		UE_LOG(LogRudeCorpus, Warning, TEXT("[RUDE] corpus ledger: %d malformed row(s) skipped"), Malformed);
	}
	auto ByRankThenPath = [this](int32 A, int32 B)
	{
		const FRudeCorpusEntry& EA = Entries[A];
		const FRudeCorpusEntry& EB = Entries[B];
		if (EA.SlotRank != EB.SlotRank) { return EA.SlotRank < EB.SlotRank; }
		const int32 C = EA.Slot.Compare(EB.Slot, ESearchCase::IgnoreCase);
		if (C != 0) { return C < 0; }
		return EA.File.Compare(EB.File, ESearchCase::IgnoreCase) < 0;
	};
	for (auto& KV : Index) { KV.Value.Sort(ByRankThenPath); }
	for (auto& KV : ByTypeIdx) { KV.Value.Sort(ByRankThenPath); }
	return true;
}

const FRudeCorpusEntry* FRudeCorpus::Effective(const FString& Type, const FString& Name) const
{
	const TArray<int32>* L = Index.Find(Key(Type, Name));
	if (!L || L->Num() == 0) { return nullptr; }
	// Sorted ascending by rank; the last is the highest rank. Among equal top ranks the FIRST of
	// that run wins (lexicographically smallest path), so the pick is deterministic.
	const int32 TopRank = Entries[L->Last()].SlotRank;
	for (int32 i = 0; i < L->Num(); ++i)
	{
		if (Entries[(*L)[i]].SlotRank == TopRank) { return &Entries[(*L)[i]]; }
	}
	return &Entries[L->Last()];
}

TArray<const FRudeCorpusEntry*> FRudeCorpus::History(const FString& Type, const FString& Name) const
{
	TArray<const FRudeCorpusEntry*> Out;
	if (const TArray<int32>* L = Index.Find(Key(Type, Name)))
	{
		for (int32 I : *L) { Out.Add(&Entries[I]); }
	}
	return Out;
}

void FRudeCorpus::ByPrefix(const FString& Type, const FString& Prefix, TArray<const FRudeCorpusEntry*>& Out) const
{
	const FString T = Type.ToLower();
	const FString P = Prefix.ToLower();
	const TArray<int32>* L = ByTypeIdx.Find(T);
	if (!L) { return; }
	TSet<FString> Seen;
	TArray<const FRudeCorpusEntry*> Picked;
	for (int32 I : *L)
	{
		const FRudeCorpusEntry& E = Entries[I];
		if (!P.IsEmpty() && !E.Name.StartsWith(P)) { continue; }
		bool bAlready = false;
		Seen.Add(E.Name, &bAlready);
		if (bAlready) { continue; }
		if (const FRudeCorpusEntry* Eff = Effective(T, E.Name)) { Picked.Add(Eff); }
	}
	Picked.Sort([](const FRudeCorpusEntry& A, const FRudeCorpusEntry& B) { return A.Name < B.Name; });
	Out.Append(Picked);
}

void FRudeCorpus::AllOfType(const FString& Type, TArray<const FRudeCorpusEntry*>& Out) const
{
	if (const TArray<int32>* L = ByTypeIdx.Find(Type.ToLower()))
	{
		Out.Reserve(Out.Num() + L->Num());
		for (int32 I : *L) { Out.Add(&Entries[I]); }
	}
}

FString FRudeCorpus::PathOf(const FRudeCorpusEntry& E) const
{
	return Root / E.Slot / E.File;
}

FString FRudeCorpus::SidecarDirOf(const FRudeCorpusEntry& E, const TCHAR* StemSuffix) const
{
	// ROUT files a written file's sidecars under "<dir>/<stem>[suffix]/", where stem is the
	// asset name (the ytd "prop_bench_01a.ytd.xml" -> "prop_bench_01a/prop_bench_01a.dds").
	return FPaths::GetPath(PathOf(E)) / (E.Name + StemSuffix);
}

void FRudeCorpus::CountByType(TMap<FString, int32>& Out) const
{
	for (const auto& KV : ByTypeIdx) { Out.Add(KV.Key, KV.Value.Num()); }
}
