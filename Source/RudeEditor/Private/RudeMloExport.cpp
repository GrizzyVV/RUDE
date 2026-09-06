// RUDE - RAGE <-> Unreal Development Environment
// MLO interior EXPORT lane (GDD Tier 1 interiors: import-author-export): the raw-slice cutter ImportMlo
// stores per entity, ExportMloYtyp (the interior back into its ytyp by splicing the file's own bytes)
// and MoveMloEntity (the scriptable edit the gate uses). Measured on the corpus 2026-09-06 -
// maintainer lane `mlo_export` (`LAWS.md`); every literal below (indents, tag spellings, the ten-per-line
// list rendering) is a measured law, not a guess.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "RudeMloEntityComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---- raw slicing ---------------------------------------------------------------------------
// The game's writer is one element per line, indented one space per depth (law 1). A block is
// "\n<Indent><Tag[ attrs]>\n ... \n<Indent></Tag>\n", or the empty form "\n<Indent><Tag[ attrs] />\n".
// Start points AT the opener's indentation; End is after the closer's newline; Inner is the bytes
// between the two lines. CaseSensitive throughout: the file's spelling is the contract.
static bool RudeMloFindBlock(const FString& Text, int32 From, const FString& Indent, const TCHAR* Tag,
                             int32& Start, int32& End, int32& InnerStart, int32& InnerEnd, bool& bEmpty)
{
	const FString Open = TEXT("\n") + Indent + TEXT("<") + Tag;
	int32 Hit = From - 1;
	while (true)
	{
		Hit = Text.Find(Open, ESearchCase::CaseSensitive, ESearchDir::FromStart, Hit + 1);
		if (Hit == INDEX_NONE) { return false; }
		const TCHAR After = Text.IsValidIndex(Hit + Open.Len()) ? Text[Hit + Open.Len()] : 0;
		if (After == '>' || After == ' ') { break; }   // "<entities>" / "<rooms itemType=...>", never "<entitySets"
	}
	const int32 LineEnd = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Hit + 1);
	if (LineEnd == INDEX_NONE) { return false; }
	Start = Hit + 1;
	if (Text.Mid(Hit, LineEnd - Hit).EndsWith(TEXT("/>")))
	{
		bEmpty = true;
		End = LineEnd + 1;
		InnerStart = InnerEnd = End;
		return true;
	}
	const FString Close = TEXT("\n") + Indent + TEXT("</") + Tag + TEXT(">\n");
	const int32 C = Text.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, LineEnd);
	if (C == INDEX_NONE) { return false; }
	bEmpty = false;
	InnerStart = LineEnd + 1;
	InnerEnd = C + 1;
	End = C + Close.Len();
	return true;
}

// Text[From..To) must be a run of "<Indent><Item...>\n ... <Indent></Item>\n" slices and NOTHING else
// (law 3: 539/539 entity blocks and 2,272/2,272 set blocks cut this way with zero leftover bytes). A
// self-closing "<Indent><Item ... />\n" is accepted as one slice (not observed for entities; the
// RudeRawItems convention). Any leftover byte = not the measured shape = refuse.
static bool RudeMloCutItems(const FString& Text, int32 From, int32 To, const FString& Indent,
                            TArray<FString>& Out, FString& Err)
{
	const FString ItemOpen = Indent + TEXT("<Item");
	const FString ItemClose = TEXT("\n") + Indent + TEXT("</Item>\n");
	int32 Pos = From;
	while (Pos < To)
	{
		if (!Text.Mid(Pos, ItemOpen.Len()).Equals(ItemOpen, ESearchCase::CaseSensitive))
		{
			Err = FString::Printf(TEXT("byte %d: expected '%s<Item', found '%s'"), Pos, *Indent, *Text.Mid(Pos, 24).ReplaceCharWithEscapedChar());
			return false;
		}
		const int32 Nl = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
		if (Nl == INDEX_NONE || Nl >= To) { Err = FString::Printf(TEXT("byte %d: item line runs past the block"), Pos); return false; }
		int32 End;
		if (Text.Mid(Pos, Nl - Pos).EndsWith(TEXT("/>"))) { End = Nl + 1; }
		else
		{
			const int32 C = Text.Find(ItemClose, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
			if (C == INDEX_NONE || C + ItemClose.Len() > To) { Err = FString::Printf(TEXT("byte %d: item has no '%s</Item>' inside the block"), Pos, *Indent); return false; }
			End = C + ItemClose.Len();
		}
		Out.Add(Text.Mid(Pos, End - Pos));
		Pos = End;
	}
	if (Pos != To) { Err = FString::Printf(TEXT("items overran the block by %d bytes"), Pos - To); return false; }
	return true;
}

// An integer scalar list element at Indent: "<Indent><Tag />\n" (empty), "<Indent><Tag>a b c</Tag>\n"
// (10 or fewer values, one line), or "<Indent><Tag>\n<Indent> v v v v v v v v v v\n ... <Indent></Tag>\n"
// (11 or more: ten per line, one deeper indent; law 8). Start/End bound the whole element incl. its
// trailing newline; Values are its numbers.
static bool RudeMloFindIntList(const FString& Text, const FString& Indent, const TCHAR* Tag,
                               int32& Start, int32& End, TArray<int32>& Values)
{
	const FString Open = TEXT("\n") + Indent + TEXT("<") + Tag;
	const int32 Hit = Text.Find(Open, ESearchCase::CaseSensitive);
	if (Hit == INDEX_NONE) { return false; }
	const TCHAR After = Text.IsValidIndex(Hit + Open.Len()) ? Text[Hit + Open.Len()] : 0;
	if (After != '>' && After != ' ') { return false; }
	Start = Hit + 1;
	const int32 LineEnd = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Hit + 1);
	if (LineEnd == INDEX_NONE) { return false; }
	Values.Reset();
	if (Text.Mid(Start, LineEnd - Start).EndsWith(TEXT("/>"))) { End = LineEnd + 1; return true; }
	const FString CloseTag = FString::Printf(TEXT("</%s>"), Tag);
	const int32 C = Text.Find(CloseTag, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
	if (C == INDEX_NONE) { return false; }
	const int32 CloseLineEnd = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, C);
	End = CloseLineEnd == INDEX_NONE ? Text.Len() : CloseLineEnd + 1;
	const int32 Gt = Text.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
	if (Gt == INDEX_NONE || Gt > C) { return false; }
	TArray<FString> Toks;
	Text.Mid(Gt + 1, C - Gt - 1).ParseIntoArrayWS(Toks);
	for (const FString& T : Toks) { Values.Add(FCString::Atoi(*T)); }
	return true;
}

// The same element re-rendered the way the game's writer spells it (law 8: 926/926 short lists inline,
// 1,193/1,193 long attachedObjects and 472/472 long locations lists in full lines of ten at Indent+1).
static FString RudeMloIntList(const FString& Indent, const TCHAR* Tag, const TArray<int32>& V)
{
	if (V.Num() == 0) { return FString::Printf(TEXT("%s<%s />\n"), *Indent, Tag); }
	FString O;
	if (V.Num() <= 10)
	{
		O = FString::Printf(TEXT("%s<%s>"), *Indent, Tag);
		for (int32 i = 0; i < V.Num(); ++i) { O += FString::Printf(TEXT("%s%d"), i ? TEXT(" ") : TEXT(""), V[i]); }
		O += FString::Printf(TEXT("</%s>\n"), Tag);
		return O;
	}
	O = FString::Printf(TEXT("%s<%s>\n"), *Indent, Tag);
	for (int32 i = 0; i < V.Num(); i += 10)
	{
		O += Indent + TEXT(" ");
		for (int32 j = i; j < FMath::Min(i + 10, V.Num()); ++j) { O += FString::Printf(TEXT("%s%d"), j > i ? TEXT(" ") : TEXT(""), V[j]); }
		O += TEXT("\n");
	}
	O += FString::Printf(TEXT("%s</%s>\n"), *Indent, Tag);
	return O;
}

bool RudeMloSliceRaw(const FString& Doc, const FString& MloName, FRudeMloRaw& Out, FString& OutError)
{
	Out = FRudeMloRaw();
	if (Doc.Contains(TEXT("\r\n")))
	{
		// 6/2,765 corpus ytyps are CRLF (law 1); the line anchors here are LF. Refused, not misread.
		OutError = TEXT("CRLF line endings: the MLO slicer is LF-only in v1");
		return false;
	}
	// 1) the ONE archetype: "  <Item type=\"CMloArchetypeDef\">" ... "  </Item>" whose "   <name>" is MloName
	//    (law 2: 37/424 MLO ytyps declare more than one MLO; the splice must target one).
	const FString ArchOpen = TEXT("\n  <Item type=\"CMloArchetypeDef\">\n");
	const FString ArchClose = TEXT("\n  </Item>\n");
	int32 Search = 0, Candidates = 0;
	while (true)
	{
		const int32 A = Doc.Find(ArchOpen, ESearchCase::CaseSensitive, ESearchDir::FromStart, Search);
		if (A == INDEX_NONE) { break; }
		const int32 C = Doc.Find(ArchClose, ESearchCase::CaseSensitive, ESearchDir::FromStart, A + ArchOpen.Len());
		if (C == INDEX_NONE) { OutError = TEXT("a CMloArchetypeDef item never closes"); return false; }
		++Candidates;
		const FString Slice = Doc.Mid(A + 1, C + ArchClose.Len() - (A + 1));
		const int32 N0 = Slice.Find(TEXT("\n   <name>"), ESearchCase::CaseSensitive);
		const int32 N1 = N0 == INDEX_NONE ? INDEX_NONE : Slice.Find(TEXT("</name>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, N0);
		if (N0 != INDEX_NONE && N1 != INDEX_NONE)
		{
			const FString Name = Slice.Mid(N0 + 10, N1 - (N0 + 10)).TrimStartAndEnd();
			if (Name.Equals(MloName, ESearchCase::IgnoreCase))
			{
				Out.ArchStart = A + 1;
				Out.ArchEnd = C + ArchClose.Len();
				Out.Arch = Slice;
				break;
			}
		}
		Search = C + 1;
	}
	if (Out.ArchStart < 0)
	{
		OutError = FString::Printf(TEXT("no CMloArchetypeDef named '%s' among %d in the file"), *MloName, Candidates);
		return false;
	}
	const FString& Arch = Out.Arch;
	// 2) the archetype's own <entities> (indent 3; the sets' are at 5 and never match the 3-space anchor)
	{
		int32 IS, IE;
		if (!RudeMloFindBlock(Arch, 0, TEXT("   "), TEXT("entities"), Out.EntStart, Out.EntEnd, IS, IE, Out.bEntitiesEmpty))
		{
			OutError = TEXT("no '   <entities>' block in the archetype");
			return false;
		}
		FString Err;
		if (!Out.bEntitiesEmpty && !RudeMloCutItems(Arch, IS, IE, TEXT("    "), Out.Items, Err))
		{
			OutError = TEXT("entities: ") + Err;
			return false;
		}
	}
	// 3) rooms: "   <rooms itemType=\"CMloRoomDef\">", bare "    <Item>" rows, "     <attachedObjects...>"
	{
		int32 IS, IE; bool bEmpty = false;
		if (RudeMloFindBlock(Arch, Out.EntEnd, TEXT("   "), TEXT("rooms"), Out.RoomsStart, Out.RoomsEnd, IS, IE, bEmpty) && !bEmpty)
		{
			TArray<FString> Items; FString Err;
			if (!RudeMloCutItems(Arch, IS, IE, TEXT("    "), Items, Err)) { OutError = TEXT("rooms: ") + Err; return false; }
			for (const FString& It : Items)
			{
				FRudeMloRawRoom R;
				R.Item = It;
				if (!RudeMloFindIntList(It, TEXT("     "), TEXT("attachedObjects"), R.AoStart, R.AoEnd, R.Attached))
				{
					OutError = FString::Printf(TEXT("room %d has no <attachedObjects> line"), Out.Rooms.Num());
					return false;
				}
				Out.Rooms.Add(MoveTemp(R));
			}
		}
	}
	// 4) entity sets: "   <entitySets itemType=\"CMloEntitySet\">", bare "    <Item>" rows carrying
	//    "     <name>", "     <locations...>", "     <entities>" with "      <Item type=\"CEntityDef\">" rows
	{
		int32 IS, IE; bool bEmpty = false;
		if (RudeMloFindBlock(Arch, Out.EntEnd, TEXT("   "), TEXT("entitySets"), Out.SetsStart, Out.SetsEnd, IS, IE, bEmpty) && !bEmpty)
		{
			TArray<FString> Items; FString Err;
			if (!RudeMloCutItems(Arch, IS, IE, TEXT("    "), Items, Err)) { OutError = TEXT("entitySets: ") + Err; return false; }
			for (const FString& It : Items)
			{
				FRudeMloRawSet S;
				S.Item = It;
				const int32 N0 = It.Find(TEXT("\n     <name>"), ESearchCase::CaseSensitive);
				const int32 N1 = N0 == INDEX_NONE ? INDEX_NONE : It.Find(TEXT("</name>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, N0);
				if (N0 == INDEX_NONE || N1 == INDEX_NONE) { OutError = FString::Printf(TEXT("entity set %d has no <name>"), Out.Sets.Num()); return false; }
				S.Name = It.Mid(N0 + 12, N1 - (N0 + 12)).TrimStartAndEnd();
				if (!RudeMloFindIntList(It, TEXT("     "), TEXT("locations"), S.LocStart, S.LocEnd, S.Locations))
				{
					OutError = FString::Printf(TEXT("entity set '%s' has no <locations> line"), *S.Name);
					return false;
				}
				int32 SIS, SIE;
				if (!RudeMloFindBlock(It, 0, TEXT("     "), TEXT("entities"), S.EntStart, S.EntEnd, SIS, SIE, S.bEntitiesEmpty))
				{
					OutError = FString::Printf(TEXT("entity set '%s' has no <entities> block"), *S.Name);
					return false;
				}
				if (!S.bEntitiesEmpty && !RudeMloCutItems(It, SIS, SIE, TEXT("      "), S.Items, Err))
				{
					OutError = FString::Printf(TEXT("entity set '%s': %s"), *S.Name, *Err);
					return false;
				}
				if (S.Locations.Num() != S.Items.Num())
				{
					// law 7: 2,272/2,272 sets have one location per entity; anything else is not the measured shape
					OutError = FString::Printf(TEXT("entity set '%s': %d locations for %d entities"), *S.Name, S.Locations.Num(), S.Items.Num());
					return false;
				}
				Out.Sets.Add(MoveTemp(S));
			}
		}
	}
	return true;
}

// ---- re-spelling inside a verbatim slice ------------------------------------------------------
// Replace the FIRST "<Tag ... />" element in the slice (the entity's own: position/rotation/guid all sit
// before <extensions> in the measured order, so the first hit is never an extension's) with NewElement;
// the line's indentation and everything else stay as spelled.
static bool RudeMloReplaceElement(FString& Slice, const TCHAR* Tag, const FString& NewElement)
{
	const FString Open = FString::Printf(TEXT("<%s "), Tag);
	const int32 A = Slice.Find(Open, ESearchCase::CaseSensitive);
	if (A == INDEX_NONE) { return false; }
	const int32 B = Slice.Find(TEXT("/>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, A);
	if (B == INDEX_NONE) { return false; }
	Slice = Slice.Left(A) + NewElement + Slice.Mid(B + 2);
	return true;
}

// UE (MLO-local) -> RAGE: position /100 with the Y mirror; the ytyp stores the entity's INVERSE
// orientation like a ymap does, so the actor quaternion goes out as (x, -y, z, w) - the same involution
// ImportMlo applied on the way in. RudeNum spelling (law 5: 468,372/472,080 corpus values are RudeNum-exact;
// the rest are exponent forms like 1E-06 that only a re-spelt tiny coordinate could hit).
static void RudeMloSpellTransform(const FTransform& Xf, FString& OutPos, FString& OutRot)
{
	const FVector P = Xf.GetLocation();
	const FQuat Q = Xf.GetRotation().GetNormalized();
	OutPos = FString::Printf(TEXT("<position x=\"%s\" y=\"%s\" z=\"%s\" />"), *RudeNumText(P.X / 100.0), *RudeNumText(-P.Y / 100.0), *RudeNumText(P.Z / 100.0));
	OutRot = FString::Printf(TEXT("<rotation x=\"%s\" y=\"%s\" z=\"%s\" w=\"%s\" />"), *RudeNumText(Q.X), *RudeNumText(-Q.Y), *RudeNumText(Q.Z), *RudeNumText(Q.W));
}

// A slot whose slice has no archetype or no position is one ImportMlo could not place (it kept the slot
// so the ordinals after it stay true); at export it re-emits verbatim and is never a "deletion".
static bool RudeMloSliceIsDead(const FString& Slice)
{
	return !Slice.Contains(TEXT("<archetypeName>")) || !Slice.Contains(TEXT("<position "));
}

// ---- the actors of one interior -----------------------------------------------------------------
struct FRudeMloActorRow
{
	AActor* Actor = nullptr;
	URudeMloEntityComponent* M = nullptr;
	FTransform Local;      // relative to the interior root
	int32 ParentRoom = -1; // RUDE_MLO_RoomIndex of the actor it is attached under (-1 = none / a set actor)
	FString ParentSet;     // RUDE_MLO_EntitySet of the actor it is attached under (empty = none)
};

static FString RudeMloTagValue(const AActor* A, const TCHAR* Prefix)
{
	const int32 L = FCString::Strlen(Prefix);
	for (const FName& T : A->Tags)
	{
		const FString S = T.ToString();
		if (S.StartsWith(Prefix, ESearchCase::IgnoreCase)) { return S.Mid(L); }
	}
	return FString();
}

// The interior's root actor for a caller's spelling: exact corpus name, or the hash_XXXXXXXX of it
// (ImportMlo's hash-tolerant convention, one direction is enough here: the root carries the corpus spelling).
static AActor* RudeMloFindRoot(UWorld* World, const FString& Wanted, FString& OutName, TArray<FString>& OutPresent)
{
	const FString WantHash = FString::Printf(TEXT("hash_%08X"), RudeJoaat(Wanted));
	AActor* Found = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(FName(TEXT("RUDE_MLO_ROOT")))) { continue; }
		const FString Name = RudeMloTagValue(*It, TEXT("RUDE_MLO:"));
		if (Name.IsEmpty()) { continue; }
		OutPresent.Add(Name);
		if (!Found && (Name.Equals(Wanted, ESearchCase::IgnoreCase) || Name.Equals(WantHash, ESearchCase::IgnoreCase)))
		{
			Found = *It;
			OutName = Name;
		}
	}
	return Found;
}

// ---- ExportMloYtyp ---------------------------------------------------------------------------------
FString URudeToolset::ExportMloYtyp(const FString& OutDir, const FString& MloArchetypeName, const FString& CorpusRoot)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("give an OutDir for the FiveM resource")); }
	const FString Wanted = MloArchetypeName.TrimStartAndEnd();
	if (Wanted.IsEmpty()) { return Fail(TEXT("MloArchetypeName is empty")); }
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }

	// 1) the interior in the level, and the file it came from
	FString Name;
	TArray<FString> Present;
	AActor* Root = RudeMloFindRoot(World, Wanted, Name, Present);
	if (!Root)
	{
		return Fail(FString::Printf(TEXT("no interior '%s' in the level (present: %s)"), *Wanted, *FString::Join(Present, TEXT(", "))));
	}
	const FString YtypAsset = RudeMloTagValue(Root, TEXT("RUDE_MLO_Ytyp:"));
	const FString ImportFile = RudeMloTagValue(Root, TEXT("RUDE_MLO_YtypFile:"));
	if (YtypAsset.IsEmpty())
	{
		return Fail(FString::Printf(TEXT("interior '%s' was built before the export lane (no RUDE_MLO_Ytyp tag) - re-run ImportMlo"), *Name));
	}
	const FRudeCorpusEntry* Entry = Corpus->Effective(TEXT("ytyp"), YtypAsset);
	if (!Entry) { return Fail(FString::Printf(TEXT("no ytyp '%s' in the corpus"), *YtypAsset)); }
	const FString SrcPath = Corpus->PathOf(*Entry);
	if (!ImportFile.IsEmpty() && !FPaths::IsSamePath(SrcPath, ImportFile))
	{
		// 86/391 MLO names live in more than one ledger row (law 2): the import and the export must read the same bytes
		return Fail(FString::Printf(TEXT("the corpus now resolves %s to %s but the interior was imported from %s - re-import, or export against that corpus"), *YtypAsset, *SrcPath, *ImportFile));
	}
	FString Doc;
	if (!FFileHelper::LoadFileToString(Doc, *SrcPath)) { return Fail(TEXT("cannot read ") + SrcPath); }
	FRudeMloRaw Raw;
	FString RawErr;
	if (!RudeMloSliceRaw(Doc, Name, Raw, RawErr)) { return Fail(FString::Printf(TEXT("%s: %s"), *FPaths::GetCleanFilename(SrcPath), *RawErr)); }

	// 2) the level's entity actors of this interior, keyed by (set, ordinal); repeats and -1 are additions
	const FTransform RootXf = Root->GetActorTransform();
	TMap<int32, FRudeMloActorRow> Top;                       // ordinal -> row
	TMap<FString, TMap<int32, FRudeMloActorRow>> BySet;      // set name (file spelling) -> ordinal -> row
	TArray<FRudeMloActorRow> AddedTop;
	TMap<FString, TArray<FRudeMloActorRow>> AddedSet;
	int32 Seen = 0, UnknownSet = 0;
	FString UnknownSetNames;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		URudeMloEntityComponent* M = It->FindComponentByClass<URudeMloEntityComponent>();
		if (!M || !M->Interior.Equals(Name, ESearchCase::IgnoreCase)) { continue; }
		++Seen;
		FRudeMloActorRow Row;
		Row.Actor = *It;
		Row.M = M;
		Row.Local = It->GetActorTransform().GetRelativeTransform(RootXf);
		if (const AActor* P = It->GetAttachParentActor())
		{
			const FString RI = RudeMloTagValue(P, TEXT("RUDE_MLO_RoomIndex:"));
			Row.ParentRoom = RI.IsEmpty() ? -1 : FCString::Atoi(*RI);
			Row.ParentSet = RudeMloTagValue(P, TEXT("RUDE_MLO_EntitySet:"));
		}
		// the set an actor belongs to: the set actor it sits under wins (a duplicate dragged into another set
		// moves with it), else the component's own name
		const FString SetName = !Row.ParentSet.IsEmpty() ? Row.ParentSet : M->SetName;
		if (SetName.IsEmpty())
		{
			if (M->SourceIndex >= 0 && !Top.Contains(M->SourceIndex)) { Top.Add(M->SourceIndex, Row); }
			else { AddedTop.Add(Row); }
		}
		else
		{
			const FRudeMloRawSet* RS = Raw.Sets.FindByPredicate([&SetName](const FRudeMloRawSet& X) { return X.Name.Equals(SetName, ESearchCase::IgnoreCase); });
			if (!RS)
			{
				++UnknownSet;
				if (UnknownSetNames.Len() < 200) { UnknownSetNames += (UnknownSetNames.IsEmpty() ? TEXT("") : TEXT(", ")) + SetName; }
				continue;
			}
			TMap<int32, FRudeMloActorRow>& Rows = BySet.FindOrAdd(RS->Name);
			if (M->SourceIndex >= 0 && !Rows.Contains(M->SourceIndex)) { Rows.Add(M->SourceIndex, Row); }
			else { AddedSet.FindOrAdd(RS->Name).Add(Row); }
		}
	}
	if (Seen == 0) { return Fail(FString::Printf(TEXT("interior '%s' has no entity actors (URudeMloEntityComponent) - re-run ImportMlo"), *Name)); }
	if (UnknownSet > 0)
	{
		return Fail(FString::Printf(TEXT("%d entity actor(s) name an entity set the file does not have (%s)"), UnknownSet, *UnknownSetNames));
	}

	auto Untouched = [](const FRudeMloActorRow& R) -> bool
	{
		// position + rotation compared, NOT scale (the ymap lane's rule; scale is 1 on 67,155/67,440 MLO entities)
		return !R.M->SourceXml.IsEmpty() && R.M->SourceIndex >= 0
			&& R.Local.GetLocation().Equals(R.M->SourceTransform.GetLocation(), 1e-3f)
			&& R.Local.GetRotation().Equals(R.M->SourceTransform.GetRotation(), 1e-4f);
	};
	auto Respell = [&](const FRudeMloActorRow& R, FString& Slice, bool bNewGuid, const FString& GuidSeed) -> bool
	{
		FString Pos, Rot;
		RudeMloSpellTransform(R.Local, Pos, Rot);
		// Only the component that CHANGED is re-spelled; the other keeps the file's own digits. UE holds a
		// rotation as Euler angles, so a float32 quaternion does not survive the round trip to its last digit
		// (the ymap lane measured w 0.9961947 -> 0.996194661): a pure translation must change ONE line.
		const bool bSamePos = R.M->SourceIndex >= 0 && R.Local.GetLocation().Equals(R.M->SourceTransform.GetLocation(), 1e-3f);
		const bool bSameRot = R.M->SourceIndex >= 0 && R.Local.GetRotation().Equals(R.M->SourceTransform.GetRotation(), 1e-4f);
		if (!bSamePos && !RudeMloReplaceElement(Slice, TEXT("position"), Pos)) { return false; }
		if (!bSameRot && !RudeMloReplaceElement(Slice, TEXT("rotation"), Rot)) { return false; }
		if (bNewGuid)
		{
			// a duplicate must not share its template's guid; unique-by-construction, like PlaceArchetype's
			const uint32 G = RudeJoaat(GuidSeed);
			if (!RudeMloReplaceElement(Slice, TEXT("guid"), FString::Printf(TEXT("<guid value=\"%u\" />"), G))) { return false; }
		}
		return true;
	};
	auto SortAdded = [](TArray<FRudeMloActorRow>& Rows)
	{
		// deterministic append order: room, then archetype, then actor name
		Rows.Sort([](const FRudeMloActorRow& A, const FRudeMloActorRow& B)
		{
			const int32 RA = A.ParentRoom >= 0 ? A.ParentRoom : A.M->RoomIndex, RB = B.ParentRoom >= 0 ? B.ParentRoom : B.M->RoomIndex;
			if (RA != RB) { return RA < RB; }
			if (A.M->ArchetypeName != B.M->ArchetypeName) { return A.M->ArchetypeName < B.M->ArchetypeName; }
			return A.Actor->GetName() < B.Actor->GetName();
		});
	};

	// 3) the top-level <entities> block: every source ordinal in order, then the additions
	int32 Kept = 0, Moved = 0, Added = 0, Deleted = 0, DeadSlots = 0, Refusals = 0;
	FString Refused, DeletedList;
	auto Refuse = [&](const FString& Why) { ++Refusals; Refused += FString::Printf(TEXT("%s\"%s\""), Refused.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(Why)); };
	FString TopBlock;
	bool bTopChanged = false;
	TArray<TArray<int32>> RoomAppend;
	RoomAppend.SetNum(Raw.Rooms.Num());
	for (int32 o = 0; o < Raw.Items.Num(); ++o)
	{
		const FRudeMloActorRow* R = Top.Find(o);
		if (!R)
		{
			if (RudeMloSliceIsDead(Raw.Items[o])) { ++DeadSlots; TopBlock += Raw.Items[o]; continue; }
			++Deleted;
			if (DeletedList.Len() < 120) { DeletedList += FString::Printf(TEXT("%s%d"), DeletedList.IsEmpty() ? TEXT("") : TEXT(","), o); }
			continue;
		}
		if (Untouched(*R)) { TopBlock += R->M->SourceXml; ++Kept; continue; }
		FString Slice = R->M->SourceXml.IsEmpty() ? Raw.Items[o] : R->M->SourceXml;
		if (!Respell(*R, Slice, false, FString())) { Refuse(FString::Printf(TEXT("entity %d: slice has no position/rotation element to re-spell"), o)); TopBlock += Raw.Items[o]; continue; }
		TopBlock += Slice;
		++Moved;
		bTopChanged = true;
	}
	SortAdded(AddedTop);
	for (const FRudeMloActorRow& R : AddedTop)
	{
		const int32 Room = R.ParentRoom >= 0 ? R.ParentRoom : R.M->RoomIndex;
		if (R.M->SourceXml.IsEmpty()) { Refuse(FString::Printf(TEXT("added entity '%s' has no template slice (duplicate an existing entity actor)"), *R.Actor->GetActorLabel())); continue; }
		if (!Raw.Rooms.IsValidIndex(Room)) { Refuse(FString::Printf(TEXT("added entity '%s' sits under no room (portal doors / unroomed additions are not expressible in v1)"), *R.Actor->GetActorLabel())); continue; }
		const int32 Ordinal = Raw.Items.Num() + Added;
		FString Slice = R.M->SourceXml;
		if (!Respell(R, Slice, true, FString::Printf(TEXT("%s:%s:%d:%s"), *Name, *R.M->ArchetypeName, Ordinal, *R.Local.GetLocation().ToString())))
		{
			Refuse(FString::Printf(TEXT("added entity '%s': template slice has no position/rotation/guid"), *R.Actor->GetActorLabel()));
			continue;
		}
		TopBlock += Slice;
		RoomAppend[Room].Add(Ordinal);
		++Added;
		bTopChanged = true;
	}

	// 4) entity sets: each set's own <entities> (its own array - law 6) and <locations>
	int32 Sets = Raw.Sets.Num(), SetEntities = 0, SetKept = 0, SetMoved = 0, SetAdded = 0, SetDeleted = 0, SetsRewritten = 0, LocationsRewritten = 0;
	TArray<FString> NewSetItems;
	for (const FRudeMloRawSet& S : Raw.Sets)
	{
		SetEntities += S.Items.Num();
		const TMap<int32, FRudeMloActorRow>* Rows = BySet.Find(S.Name);
		TArray<FRudeMloActorRow>* AddRows = AddedSet.Find(S.Name);
		FString Block;
		bool bChanged = false;
		TArray<int32> Loc = S.Locations;
		for (int32 o = 0; o < S.Items.Num(); ++o)
		{
			const FRudeMloActorRow* R = Rows ? Rows->Find(o) : nullptr;
			if (!R)
			{
				if (RudeMloSliceIsDead(S.Items[o])) { ++DeadSlots; Block += S.Items[o]; continue; }
				++SetDeleted;
				if (DeletedList.Len() < 120) { DeletedList += FString::Printf(TEXT("%s%s:%d"), DeletedList.IsEmpty() ? TEXT("") : TEXT(","), *S.Name, o); }
				continue;
			}
			if (Untouched(*R)) { Block += R->M->SourceXml; ++SetKept; continue; }
			FString Slice = R->M->SourceXml.IsEmpty() ? S.Items[o] : R->M->SourceXml;
			if (!Respell(*R, Slice, false, FString())) { Refuse(FString::Printf(TEXT("set %s entity %d: no position/rotation element"), *S.Name, o)); Block += S.Items[o]; continue; }
			Block += Slice;
			++SetMoved;
			bChanged = true;
		}
		if (AddRows)
		{
			SortAdded(*AddRows);
			for (const FRudeMloActorRow& R : *AddRows)
			{
				const int32 Room = R.M->RoomIndex;   // a set entity's room is its <locations> value, carried on the component
				if (R.M->SourceXml.IsEmpty()) { Refuse(FString::Printf(TEXT("set %s: added entity '%s' has no template slice"), *S.Name, *R.Actor->GetActorLabel())); continue; }
				if (!Raw.Rooms.IsValidIndex(Room)) { Refuse(FString::Printf(TEXT("set %s: added entity '%s' has no room for its <locations> slot"), *S.Name, *R.Actor->GetActorLabel())); continue; }
				const int32 Ordinal = S.Items.Num() + (Loc.Num() - S.Locations.Num());
				FString Slice = R.M->SourceXml;
				if (!Respell(R, Slice, true, FString::Printf(TEXT("%s:%s:%s:%d:%s"), *Name, *S.Name, *R.M->ArchetypeName, Ordinal, *R.Local.GetLocation().ToString())))
				{
					Refuse(FString::Printf(TEXT("set %s: added entity '%s' template has no position/rotation/guid"), *S.Name, *R.Actor->GetActorLabel()));
					continue;
				}
				Block += Slice;
				Loc.Add(Room);
				++SetAdded;
				bChanged = true;
			}
		}
		if (!bChanged) { NewSetItems.Add(S.Item); continue; }
		// rebuild the set's <Item>: entities block, then locations (it sits BEFORE entities - law 6 - so the
		// later span is replaced first and the earlier offsets stay valid)
		FString Item = S.Item;
		const FString EntBlock = Block.IsEmpty() ? FString(TEXT("     <entities />\n")) : (TEXT("     <entities>\n") + Block + TEXT("     </entities>\n"));
		Item = Item.Left(S.EntStart) + EntBlock + Item.Mid(S.EntEnd);
		if (Loc.Num() != S.Locations.Num())
		{
			Item = Item.Left(S.LocStart) + RudeMloIntList(TEXT("     "), TEXT("locations"), Loc) + Item.Mid(S.LocEnd);
			++LocationsRewritten;
		}
		NewSetItems.Add(Item);
		++SetsRewritten;
	}

	// 5) refusals that void the file: a deletion shifts every ordinal after it (rooms' and portals' attachedObjects
	//    are ordinals - law 4), so nothing is written. Hide the actor instead; export from a FULL import.
	if (Deleted + SetDeleted > 0)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%d entity(ies) deleted (%s) - deletions are refused in v1: rooms/portals index entities by ordinal; hide instead, and export from an ImportMlo with an empty room Filter\",\"interior\":\"%s\",\"deleted\":%d,\"setDeleted\":%d,\"entities\":%d,\"seen\":%d}"),
			Deleted + SetDeleted, *RudeJsonEscape(DeletedList), *RudeJsonEscape(Name), Deleted, SetDeleted, Raw.Items.Num(), Seen);
	}

	// 6) splice, back to front (entities < rooms < portals < entitySets in every archetype - law 1)
	FString Arch = Raw.Arch;
	if (Raw.SetsStart >= 0 && SetsRewritten > 0)
	{
		FString SetsBlock = TEXT("   <entitySets itemType=\"CMloEntitySet\">\n");
		for (const FString& It : NewSetItems) { SetsBlock += It; }
		SetsBlock += TEXT("   </entitySets>\n");
		Arch = Arch.Left(Raw.SetsStart) + SetsBlock + Arch.Mid(Raw.SetsEnd);
	}
	int32 RoomsRewritten = 0;
	{
		bool bAnyRoom = false;
		for (const TArray<int32>& A : RoomAppend) { if (A.Num() > 0) { bAnyRoom = true; break; } }
		if (bAnyRoom)
		{
			FString RoomsBlock = TEXT("   <rooms itemType=\"CMloRoomDef\">\n");
			for (int32 r = 0; r < Raw.Rooms.Num(); ++r)
			{
				if (RoomAppend[r].Num() == 0) { RoomsBlock += Raw.Rooms[r].Item; continue; }
				TArray<int32> Ao = Raw.Rooms[r].Attached;
				Ao.Append(RoomAppend[r]);
				const FRudeMloRawRoom& RR = Raw.Rooms[r];
				RoomsBlock += RR.Item.Left(RR.AoStart) + RudeMloIntList(TEXT("     "), TEXT("attachedObjects"), Ao) + RR.Item.Mid(RR.AoEnd);
				++RoomsRewritten;
			}
			RoomsBlock += TEXT("   </rooms>\n");
			Arch = Arch.Left(Raw.RoomsStart) + RoomsBlock + Arch.Mid(Raw.RoomsEnd);
		}
	}
	if (bTopChanged)
	{
		const FString EntBlock = TopBlock.IsEmpty() ? FString(TEXT("   <entities />\n")) : (TEXT("   <entities>\n") + TopBlock + TEXT("   </entities>\n"));
		Arch = Arch.Left(Raw.EntStart) + EntBlock + Arch.Mid(Raw.EntEnd);
	}
	const FString Out = Doc.Left(Raw.ArchStart) + Arch + Doc.Mid(Raw.ArchEnd);

	// 7) write the resource (UTF-8, no BOM: 0/2,765 corpus ytyps carry one)
	IFileManager::Get().MakeDirectory(*(OutDir / TEXT("stream")), true);
	const FString OutPath = OutDir / TEXT("stream") / (YtypAsset + TEXT(".ytyp"));
	if (!FFileHelper::SaveStringToFile(Out, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return Fail(TEXT("cannot write ") + OutPath); }
	FFileHelper::SaveStringToFile(TEXT("fx_version 'cerulean'\ngame 'gta5'\nthis_is_a_map 'yes'\n"), *(OutDir / TEXT("fxmanifest.lua")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool bIdentical = Out.Equals(Doc, ESearchCase::CaseSensitive);
	const bool bOk = Refusals == 0;
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"interior\":\"%s\",\"ytyp\":\"%s\",\"file\":\"%s\",\"source\":\"%s\",\"byteIdentical\":%s,"
		"\"entities\":%d,\"seen\":%d,\"kept\":%d,\"moved\":%d,\"added\":%d,\"deleted\":0,\"deadSlots\":%d,"
		"\"sets\":%d,\"setEntities\":%d,\"setKept\":%d,\"setMoved\":%d,\"setAdded\":%d,\"setDeleted\":0,\"setsRewritten\":%d,"
		"\"roomsRewritten\":%d,\"locationsRewritten\":%d,\"refused\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Name), *RudeJsonEscape(YtypAsset), *RudeJsonEscape(OutPath), *RudeJsonEscape(SrcPath),
		bIdentical ? TEXT("true") : TEXT("false"),
		Raw.Items.Num(), Seen, Kept, Moved, Added, DeadSlots,
		Sets, SetEntities, SetKept, SetMoved, SetAdded, SetsRewritten, RoomsRewritten, LocationsRewritten, *Refused);
}

// ---- MoveMloEntity (agent; the scriptable edit) -----------------------------------------------------
FString URudeToolset::MoveMloEntity(const FString& InteriorName, const FString& Index, const FString& DeltaCm)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	FString SetName, Ord = Index.TrimStartAndEnd();
	{
		int32 Colon;
		if (Ord.FindLastChar(TEXT(':'), Colon)) { SetName = Ord.Left(Colon).TrimStartAndEnd(); Ord = Ord.Mid(Colon + 1).TrimStartAndEnd(); }
	}
	if (Ord.IsEmpty() || !Ord.IsNumeric()) { return Fail(TEXT("Index must be an ordinal, or <setName>:<ordinal>")); }
	const int32 Idx = FCString::Atoi(*Ord);
	TArray<FString> P;
	DeltaCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
	if (P.Num() != 3) { return Fail(TEXT("DeltaCm must be x,y,z in UE centimetres")); }
	const FVector D(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2]));
	FString Name;
	TArray<FString> Present;
	if (!RudeMloFindRoot(World, InteriorName.TrimStartAndEnd(), Name, Present))
	{
		return Fail(FString::Printf(TEXT("no interior '%s' in the level (present: %s)"), *InteriorName, *FString::Join(Present, TEXT(", "))));
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const URudeMloEntityComponent* M = It->FindComponentByClass<URudeMloEntityComponent>();
		if (!M || M->SourceIndex != Idx || !M->Interior.Equals(Name, ESearchCase::IgnoreCase) || !M->SetName.Equals(SetName, ESearchCase::IgnoreCase)) { continue; }
		const FVector Before = It->GetActorLocation();
		It->Modify();
		It->SetActorLocation(Before + D);
		It->MarkPackageDirty();
		return FString::Printf(TEXT("{\"ok\":true,\"interior\":\"%s\",\"set\":\"%s\",\"index\":%d,\"archetype\":\"%s\",\"before\":[%f,%f,%f],\"after\":[%f,%f,%f]}"),
			*RudeJsonEscape(Name), *RudeJsonEscape(M->SetName), Idx, *RudeJsonEscape(M->ArchetypeName),
			Before.X, Before.Y, Before.Z, Before.X + D.X, Before.Y + D.Y, Before.Z + D.Z);
	}
	return Fail(FString::Printf(TEXT("no entity %s[%s%d] in interior '%s'"), *Name, SetName.IsEmpty() ? TEXT("") : *(SetName + TEXT(":")), Idx, *Name));
}
