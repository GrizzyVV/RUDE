// RUDE - RAGE <-> Unreal Development Environment
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RudeTimecycle.generated.h"

// One <mod> of a timecycle modifier. The file spells EVERY mod as two numbers with three decimals
// ("<light_dir_col_r>0.004 0.900</light_dir_col_r>"): measured 78,389 / 78,389 mods over the corpus's
// 12 timecycle_mods_*.xml (wp10/configs LAWS.md T3). Both are carried; nothing is dropped.
USTRUCT(BlueprintType)
struct FRudeTimecycleMod
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "RUDE|Timecycle")
	float Value = 0.f;

	// The second number. INFERRED (not verified) to be a per-mod blend weight: 0.900 recurs across a
	// modifier's mods and a disabled mod spells "0.000 0.000". The game is the judge.
	UPROPERTY(EditAnywhere, Category = "RUDE|Timecycle")
	float Weight = 0.f;
};

// One timecycle modifier (a <modifier> of timecycle_mods_N.xml) as an editable palette entry.
// The byte-safe seam is the archetype's: SourceXml (the modifier's own bytes, line endings included)
// + SourceFieldsKey; ExportTimecycles re-emits an untouched modifier verbatim and rebuilds an edited one.
UCLASS(BlueprintType)
class RUDECORE_API URudeTimecycle : public UDataAsset
{
	GENERATED_BODY()

public:
	// As the file spells it (case kept: "NoAmbientmult", "DLC_mp2026_01_VAULT_ROOM"). The ymap's
	// <timeCycleModifiers> refer to it by joaat hash (spelled hash_XXXXXXXX in the interchange XML).
	UPROPERTY(EditAnywhere, Category = "RUDE|Timecycle")
	FString Name;

	// userFlags as spelled (measured values over the corpus: 0, 1, 2, 4, 6, 8, 16; meaning unknown).
	UPROPERTY(EditAnywhere, Category = "RUDE|Timecycle")
	uint32 UserFlags = 0;

	// mod name -> (value, weight). Vocabulary: 325 distinct mod names over the corpus (timecycle_vocab.json).
	UPROPERTY(EditAnywhere, Category = "RUDE|Timecycle")
	TMap<FName, FRudeTimecycleMod> Mods;

	// The file's own mod order. A rebuilt modifier follows this order, then any key added in RUDE.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Timecycle")
	TArray<FName> ModOrder;

	// numMods as the file spells it. One game modifier spells 54 over 57 children (measured:
	// DLC_mp2026_01_VAULT_ROOM, update timecycle_mods_1.xml); a rebuilt modifier writes the real count.
	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Timecycle")
	int32 NumModsAsSpelled = 0;

	// ---- provenance + the byte-safe seam ----
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceSlot;

	// The ledger's File path under the slot ("update.rpf/common/data/timecycle/timecycle_mods_1.xml").
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	FString SourceFile;

	// Ordinal among the file's modifiers; -1 = authored in RUDE (appended on export).
	UPROPERTY(VisibleAnywhere, Category = "RUDE|Source")
	int32 SourceIndex = -1;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceXml;

	UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "RUDE|Source")
	FString SourceFieldsKey;

	FString FieldsKey() const
	{
		FString K = FString::Printf(TEXT("%s|%u"), *Name, UserFlags);
		TSet<FName> Seen;
		for (const FName& M : ModOrder)
		{
			const FRudeTimecycleMod* V = Mods.Find(M);
			if (!V) { continue; }
			Seen.Add(M);
			K += FString::Printf(TEXT("|%s=%.3f %.3f"), *M.ToString(), V->Value, V->Weight);
		}
		TArray<FName> Rest;
		for (const auto& KV : Mods) { if (!Seen.Contains(KV.Key)) { Rest.Add(KV.Key); } }
		Rest.Sort(FNameLexicalLess());
		for (const FName& M : Rest)
		{
			const FRudeTimecycleMod& V = Mods[M];
			K += FString::Printf(TEXT("|%s=%.3f %.3f"), *M.ToString(), V.Value, V.Weight);
		}
		return K;
	}
};
