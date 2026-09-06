// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"

// One ledger row: a file the export wrote (converted XML or kept binary), where it sits and
// which build slot it came from. `Name` is the ASSET name the game uses (lower-case, no lane
// suffix): `dt1_00` for `dt1_00.ymap.xml`, `gtxd` for `gtxd.ymt.rbf.xml`, `foo` for a kept
// `foo.gfx`. `Type` is the ledger's lane word (`ymap`, `ytd`, `gfx`, `dat54`, ...).
struct RUDEINTAKE_API FRudeCorpusEntry
{
	FString Slot;        // "00_base" | "10_update" | "20_dlc/NNN_<pack>"
	FString File;        // path under the slot: "<archive chain>/<in-archive folders>/<written name>"
	FString Type;        // ledger lane word (lower-case)
	FString Name;        // asset name (lower-case), see above
	FString SourceName;  // the written file's name as the ledger spells it
	FString Sha1;        // ledger sha1 (the written file)
	int32 SlotRank = 0;  // load-order rank: higher wins
	bool bConverted = false;  // true = interchange XML; false = kept binary (game bytes as-is)
};

// A prepared corpus (a ROUT filebase). RUDE reads the manifest, never the folder tree: every
// lookup goes through the ledger index, so a layout change never reaches a reader.
//
//   slot precedence   from _FILEBASE.json ("00_base" < "10_update" < "20_dlc/<order>_<name>")
//   identity          (type, name) -> every copy across slots; Effective() = the copy the game loads
//
// Open() is cached per root and re-reads when the ledger's timestamp changes, so a --patch into
// the corpus is picked up by the next tool call without an editor restart.
class RUDEINTAKE_API FRudeCorpus
{
public:
	// Opens (or returns the cached) corpus at Root. Fails with a reason when the root has no
	// _FILEBASE.json / _PROVENANCE.jsonl, or the ledger's routVersion is one this reader does
	// not understand -- a mismatch is refused, never misread.
	static TSharedPtr<FRudeCorpus> Open(const FString& Root, FString& OutError);

	// True when Root looks like a corpus (both ledgers present) without opening it.
	static bool LooksLikeCorpus(const FString& Root);

	// The asset name a ledger row denotes: strips ".xml", then any ".rbf"/".pso" gate suffix,
	// then one trailing ".<type>". Lower-case. Exposed so callers and tests agree on one rule.
	static FString AssetNameOf(const FString& Type, const FString& SourceName);

	const FString& GetRoot() const { return Root; }
	int32 GetRoutVersion() const { return RoutVersion; }
	const FString& GetTitle() const { return Title; }
	int32 Num() const { return Entries.Num(); }
	const TArray<FString>& GetPrecedence() const { return Precedence; }

	// The copy the game would load: highest slot rank; ties broken by path so the answer is
	// machine-independent. Null when the corpus has no (type, name).
	const FRudeCorpusEntry* Effective(const FString& Type, const FString& Name) const;

	// Every copy of (type, name), lowest slot first.
	TArray<const FRudeCorpusEntry*> History(const FString& Type, const FString& Name) const;

	// The highest-ranked copy that HAS its pixel/payload sidecar folder on disk, else Effective().
	// A DLC patch copy exported without --textures shadows a base copy that has them (s_m_y_cop_01:
	// patchday9ng vs componentpeds_s_m_y, 2026-09-06) - the game's own precedence is kept, the
	// corpus's coverage gap is not.
	const FRudeCorpusEntry* EffectiveWithSidecar(const FString& Type, const FString& Name) const;

	// Effective entry of every name of Type that starts with Prefix (empty = all names).
	// Sorted by name.
	void ByPrefix(const FString& Type, const FString& Prefix, TArray<const FRudeCorpusEntry*>& Out) const;

	// Every row of Type across every slot, sorted by (slot rank, path). Iterating this in order
	// and letting later rows overwrite earlier ones yields the effective table by construction.
	void AllOfType(const FString& Type, TArray<const FRudeCorpusEntry*>& Out) const;

	// Absolute path of the written file.
	FString PathOf(const FRudeCorpusEntry& E) const;

	// Folder the file's pixel/payload sidecars live in: "<dir>/<stem>" -- the ytd's own
	// textures ("<name>/<tex>.dds") and a drawable's embedded ones ("<name>__embedded/...").
	FString SidecarDirOf(const FRudeCorpusEntry& E, const TCHAR* StemSuffix = TEXT("")) const;

	// Load-order rank of a slot name (0 = base). Unknown slots rank below base.
	int32 SlotRank(const FString& Slot) const;

	// Row counts per lane word, for boards.
	void CountByType(TMap<FString, int32>& Out) const;

private:
	bool Load(const FString& InRoot, FString& OutError);
	bool LoadFilebase(FString& OutError);
	bool LoadLedger(FString& OutError);
	static FString Key(const FString& Type, const FString& Name) { return Type.ToLower() + TEXT("/") + Name.ToLower(); }

	FString Root;
	int32 RoutVersion = 0;
	FString Title;
	TArray<FString> Precedence;
	FDateTime LedgerStamp;
	TArray<FRudeCorpusEntry> Entries;
	TMap<FString, TArray<int32>> Index;       // Key(type, name) -> entry indices
	TMap<FString, TArray<int32>> ByTypeIdx;   // type -> entry indices (sorted by rank, path)
};
