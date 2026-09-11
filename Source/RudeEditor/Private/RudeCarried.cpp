// RUDE - RAGE <-> Unreal Development Environment
// WP10 PASSTHROUGH tier: CatalogLane / DebugDrawNavmesh. Measured on the game's own XML (LAWS.md (c)):
// every lane's file is one root element with a handful of top-level children, most of them arrays of <Item>.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "RudeCarriedAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/LineBatchComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "XmlFile.h"

namespace RudeCarried
{
	struct FTop { FString Tag; int32 Children = 0; };

	// Linear top-level scanner: the root tag and each depth-1 child with its direct-child count. No DOM, so a
	// 42 MB core.ypt.xml costs one pass and no tree. Skips <?...?>, <!--...-->, <![CDATA[...]]>; a self-closing
	// tag counts as open+close. Returns false when the tags do not balance.
	static bool Scan(const FString& Xml, FString& Root, TArray<FTop>& Tops, int32& MaxDepth)
	{
		const int32 N = Xml.Len();
		int32 Depth = 0, i = 0;
		MaxDepth = 0;
		while (i < N)
		{
			const int32 Lt = Xml.Find(TEXT("<"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i);
			if (Lt == INDEX_NONE) { break; }
			if (Lt + 1 >= N) { break; }
			const TCHAR C1 = Xml[Lt + 1];
			if (C1 == TEXT('?')) { const int32 E = Xml.Find(TEXT("?>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Lt); if (E == INDEX_NONE) { return false; } i = E + 2; continue; }
			if (C1 == TEXT('!'))
			{
				if (Xml.Mid(Lt, 4) == TEXT("<!--")) { const int32 E = Xml.Find(TEXT("-->"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Lt); if (E == INDEX_NONE) { return false; } i = E + 3; continue; }
				if (Xml.Mid(Lt, 9) == TEXT("<![CDATA[")) { const int32 E = Xml.Find(TEXT("]]>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Lt); if (E == INDEX_NONE) { return false; } i = E + 3; continue; }
				const int32 E = Xml.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Lt); if (E == INDEX_NONE) { return false; } i = E + 1; continue;   // <!DOCTYPE ...>
			}
			const int32 Gt = Xml.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Lt);
			if (Gt == INDEX_NONE) { return false; }
			if (C1 == TEXT('/')) { --Depth; if (Depth < 0) { return false; } i = Gt + 1; continue; }
			const bool bSelf = Xml[Gt - 1] == TEXT('/');
			int32 E = Lt + 1;
			while (E < Gt && !FChar::IsWhitespace(Xml[E]) && Xml[E] != TEXT('/') && Xml[E] != TEXT('>')) { ++E; }
			const FString Tag = Xml.Mid(Lt + 1, E - Lt - 1);
			++Depth;
			MaxDepth = FMath::Max(MaxDepth, Depth);
			if (Depth == 1) { Root = Tag; }
			else if (Depth == 2) { FTop T; T.Tag = Tag; Tops.Add(T); }
			else if (Depth == 3 && Tops.Num() > 0) { Tops.Last().Children++; }
			if (bSelf) { --Depth; }
			i = Gt + 1;
		}
		return !Root.IsEmpty() && Depth == 0;
	}

	// "Polygons:2564 Portals:4 Points:1062 SectorTree:21 | Item x57" - arrays by their child count, repeated
	// same-named children collapsed to a multiplicity, scalars listed by name only (first 24 entries).
	static FString Summarise(const TArray<FTop>& Tops)
	{
		FString O; int32 Listed = 0;
		TMap<FString, int32> Repeat; TArray<FString> Order;
		for (const FTop& T : Tops) { if (!Repeat.Contains(T.Tag)) { Order.Add(T.Tag); } Repeat.FindOrAdd(T.Tag)++; }
		for (const FString& Tag : Order)
		{
			if (Listed++ >= 24) { O += TEXT(" ..."); break; }
			const int32 Rep = Repeat[Tag];
			if (Rep > 1) { O += FString::Printf(TEXT("%s%s x%d"), O.IsEmpty() ? TEXT("") : TEXT(" "), *Tag, Rep); continue; }
			const FTop* T = Tops.FindByPredicate([&](const FTop& X) { return X.Tag == Tag; });
			if (T && T->Children > 0) { O += FString::Printf(TEXT("%s%s:%d"), O.IsEmpty() ? TEXT("") : TEXT(" "), *Tag, T->Children); }
			else { O += FString::Printf(TEXT("%s%s"), O.IsEmpty() ? TEXT("") : TEXT(" "), *Tag); }
		}
		return O;
	}

	static FString SafeName(const FString& In)
	{
		FString O; O.Reserve(In.Len());
		for (TCHAR C : In) { O.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_')); }
		return O.IsEmpty() ? FString(TEXT("_")) : O;
	}

	// "x, y, z" lines (RAGE metres) -> UE cm with the house Y mirror (RudeArchetype bounds: UE = (x, -y, z) * 100).
	static int32 ParseVectors(const FString& Text, TArray<FVector>& Out)
	{
		// ⛔ DO NOT SPLIT ON LINES (fixed 2026-09-11). FXmlFile FLATTENS multi-line text content - this
		// vault already records that for MLO <attachedObjects> - so a <Vertices> block of N lines comes
		// back as ONE string. Splitting by line then demanding exactly 3 comma-separated parts therefore
		// matched nothing, every polygon counted as degenerate, and DebugDrawNavmesh drew ZERO of them:
		// measured `polygons: 0, polygonVertices: 0, degenerate: 2564` on navmesh[102][102], with the
		// portals, points and lines around it drawing fine. It had never been seen because the gate row
		// that runs this sat below a refusal and had never executed (law 60).
		// Tokenising on BOTH separators and grouping in threes is newline-agnostic: it reads the same
		// whether the flattening happens or not.
		TArray<FString> Tok;
		Text.ParseIntoArray(Tok, TEXT(","), true);
		TArray<double> Nums;
		for (const FString& T : Tok)
		{
			TArray<FString> Sub;
			T.ParseIntoArrayWS(Sub, nullptr, true);
			for (const FString& S : Sub)
			{
				const FString Trimmed = S.TrimStartAndEnd();
				if (!Trimmed.IsEmpty() && (FChar::IsDigit(Trimmed[0]) || Trimmed[0] == TEXT('-') || Trimmed[0] == TEXT('+') || Trimmed[0] == TEXT('.')))
				{
					Nums.Add(FCString::Atod(*Trimmed));
				}
			}
		}
		for (int32 i = 0; i + 2 < Nums.Num(); i += 3)
		{
			Out.Add(FVector(Nums[i] * 100.0, -Nums[i + 1] * 100.0, Nums[i + 2] * 100.0));
		}
		return Out.Num();
	}
	static FVector XyzAttr(const FXmlNode* N)
	{
		return N ? FVector(FCString::Atod(*N->GetAttribute(TEXT("x"))) * 100.0, -FCString::Atod(*N->GetAttribute(TEXT("y"))) * 100.0, FCString::Atod(*N->GetAttribute(TEXT("z"))) * 100.0) : FVector::ZeroVector;
	}
}

// ---- CatalogLane ---------------------------------------------------------------------------
// Every effective corpus row of Type whose name contains NameFilter -> one URudeCarriedAsset under DestFolder
// (default /Game/RUDE/Carried/<type>), loaded-then-filled like the palette (a package created over an unloaded
// file is re-serialised by the next LoadObject, measured 2026-09-05). The XML rides inline up to 8 MB
// (core.ypt.xml is 42 MB: over the cap it is summarised and left on disk, counted xmlTooLarge). A kept binary
// row becomes a stub with no XML (counted binaryStubs). Names that a package cannot spell are sanitised
// (navmesh[102][102] -> navmesh_102__102_); the ledger spelling stays in the asset's Name.
FString URudeToolset::CatalogLane(const FString& CorpusRoot, const FString& Type, const FString& NameFilter, const FString& DestFolder)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }
	const FString T = Type.TrimStartAndEnd().ToLower();
	if (T.IsEmpty()) { return Fail(TEXT("Type is empty (a ledger lane word: yed, yld, yfd, ypdb, ynv, mrf, ypt, ...)")); }
	TArray<const FRudeCorpusEntry*> Rows;
	Corpus->ByPrefix(T, TEXT(""), Rows);
	if (Rows.Num() == 0)
	{
		TMap<FString, int32> Counts; Corpus->CountByType(Counts);
		Counts.ValueSort([](int32 A, int32 B) { return A > B; });
		FString Have; int32 n = 0;
		for (const auto& KV : Counts) { if (n++ >= 40) { break; } Have += FString::Printf(TEXT("%s%s:%d"), Have.IsEmpty() ? TEXT("") : TEXT(" "), *KV.Key, KV.Value); }
		return Fail(FString::Printf(TEXT("the corpus has no rows of type '%s' (types present: %s)"), *T, *Have));
	}
	const FString Filter = NameFilter.TrimStartAndEnd().ToLower();
	const FString Dest = DestFolder.TrimStartAndEnd().IsEmpty() ? (TEXT("/Game/RUDE/Carried/") + RudeCarried::SafeName(T)) : DestFolder.TrimStartAndEnd();
	const int64 InlineCap = 8LL * 1024 * 1024;

	int32 Matched = 0, Created = 0, Refilled = 0, XmlInlined = 0, XmlTooLarge = 0, BinaryStubs = 0, ScanFailed = 0, Invalid = 0, ReadFailed = 0;
	int64 Bytes = 0;
	FString Sample;
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	for (const FRudeCorpusEntry* E : Rows)
	{
		if (!Filter.IsEmpty() && !E->Name.ToLower().Contains(Filter)) { continue; }
		++Matched;
		const FString AssetName = RudeCarried::SafeName(E->Name);
		const FString PkgName = Dest / AssetName;
		if (!FPackageName::IsValidLongPackageName(PkgName)) { ++Invalid; continue; }
		const FString Path = Corpus->PathOf(*E);
		FString Xml, Root, Summary; int32 Tops = 0; bool bInline = false, bScanned = false;
		int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size < 0) { ++ReadFailed; continue; }
		if (E->bConverted)
		{
			if (!FFileHelper::LoadFileToString(Xml, *Path)) { ++ReadFailed; continue; }
			TArray<RudeCarried::FTop> TopList; int32 MaxDepth = 0;
			bScanned = RudeCarried::Scan(Xml, Root, TopList, MaxDepth);
			if (!bScanned) { ++ScanFailed; Summary = TEXT("(tags do not balance - summary unavailable)"); }
			else { Tops = TopList.Num(); Summary = RudeCarried::Summarise(TopList) + FString::Printf(TEXT(" | depth %d"), MaxDepth); }
			if (Size <= InlineCap) { bInline = true; ++XmlInlined; } else { Xml.Reset(); ++XmlTooLarge; }
		}
		else { ++BinaryStubs; Summary = FString::Printf(TEXT("kept binary, %lld B (no interchange XML)"), Size); }
		Bytes += Size;

		URudeCarriedAsset* A = LoadObject<URudeCarriedAsset>(nullptr, *(PkgName + TEXT(".") + AssetName));
		UPackage* Pkg = A ? A->GetOutermost() : CreatePackage(*PkgName);
		bool bNew = false;
		if (!A) { A = NewObject<URudeCarriedAsset>(Pkg, FName(*AssetName), RF_Public | RF_Standalone); bNew = true; }
		A->LaneType = T; A->Name = E->Name; A->RootTag = Root; A->Summary = Summary; A->SizeBytes = Size; A->TopLevelChildren = Tops;
		A->bConverted = E->bConverted; A->bXmlInline = bInline;
		A->CorpusRoot = Corpus->GetRoot(); A->SourceSlot = E->Slot; A->SourceFile = E->File; A->SourceSha1 = E->Sha1;
		A->SourceXml = bInline ? MoveTemp(Xml) : FString();
		Pkg->MarkPackageDirty();
		if (bNew) { ARM.Get().AssetCreated(A); ++Created; } else { ++Refilled; }
		if (Sample.Len() < 1200) { Sample += FString::Printf(TEXT("%s{\"name\":\"%s\",\"root\":\"%s\",\"summary\":\"%s\"}"), Sample.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(E->Name), *RudeJsonEscape(Root), *RudeJsonEscape(Summary)); }
	}
	const bool bOk = (Created + Refilled) == (Matched - Invalid - ReadFailed) && Matched > 0;
	return FString::Printf(
		TEXT("{\"ok\":%s,\"type\":\"%s\",\"rows\":%d,\"matched\":%d,\"assets\":%d,\"created\":%d,\"refilled\":%d,\"xmlInlined\":%d,\"xmlTooLarge\":%d,\"binaryStubs\":%d,")
		TEXT("\"scanFailed\":%d,\"invalidNames\":%d,\"readFailed\":%d,\"bytes\":%lld,\"destFolder\":\"%s\",\"inlineCapBytes\":%lld,\"sample\":[%s],\"note\":\"passthrough tier: nothing here is edit-native; SaveAssets to persist\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(T), Rows.Num(), Matched, Created + Refilled, Created, Refilled, XmlInlined, XmlTooLarge, BinaryStubs,
		ScanFailed, Invalid, ReadFailed, Bytes, *RudeJsonEscape(Dest), InlineCap, *Sample);
}

// ---- DebugDrawNavmesh ----------------------------------------------------------------------
// One .ynv's <Polygons> as persistent lines in the editor world (the WorldPersistent line batcher: lifetime
// -1 = never expires; YnvName=CLEAR flushes it). Measured layout (navmesh[102][102]: 2,564 polygons, 4
// portals, 1,062 points): <Polygons><Item><Vertices> holds 3..11 "x, y, z" lines in RAGE metres (the polygon's
// own ring; <VertexIndices> indexes the file's shared vertex pool, <Edges> "area:poly" adjacency,
// <Flags> six bytes), <Portals><Item> PositionFrom/PositionTo, <Points><Item> Position + Angle + Type.
// Polygons cyan, portals magenta, points a 30 cm yellow tick. UE = (x, -y, z) * 100 (the house mirror).
FString URudeToolset::DebugDrawNavmesh(const FString& CorpusRoot, const FString& YnvName)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	ULineBatchComponent* LB = World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent);
	if (!LB) { return Fail(TEXT("the world has no persistent line batcher")); }
	const FString Name = YnvName.TrimStartAndEnd();
	if (Name.Equals(TEXT("CLEAR"), ESearchCase::IgnoreCase))
	{
		LB->Flush();
		if (GEditor) { GEditor->RedrawLevelEditingViewports(); }
		return TEXT("{\"ok\":true,\"cleared\":true}");
	}
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }
	const FRudeCorpusEntry* Row = Corpus->Effective(TEXT("ynv"), Name.ToLower());
	if (!Row) { return Fail(FString::Printf(TEXT("the corpus has no ynv named '%s' (e.g. navmesh[102][102], armytanker)"), *Name)); }
	if (!Row->bConverted) { return Fail(TEXT("that ynv is a kept binary in the ledger - no interchange XML to draw")); }
	const FString Path = Corpus->PathOf(*Row);
	FXmlFile Xml(Path);
	if (!Xml.IsValid() || !Xml.GetRootNode()) { return Fail(FString::Printf(TEXT("cannot parse %s: %s"), *Path, *Xml.GetLastError())); }
	const FXmlNode* RootN = Xml.GetRootNode();
	if (RootN->GetTag() != TEXT("NavMesh")) { return Fail(FString::Printf(TEXT("root is <%s>, not <NavMesh>"), *RootN->GetTag())); }

	TArray<FBatchedLine> Lines;
	int32 Polys = 0, Verts = 0, Degenerate = 0, Portals = 0, Points = 0;
	FBox Bounds(ForceInit);
	const FLinearColor PolyCol(0.1f, 0.9f, 1.0f), PortalCol(1.0f, 0.2f, 1.0f), PointCol(1.0f, 0.9f, 0.1f);
	if (const FXmlNode* PolysN = RootN->FindChildNode(TEXT("Polygons")))
	{
		for (const FXmlNode* It : PolysN->GetChildrenNodes())
		{
			const FXmlNode* VN = It->FindChildNode(TEXT("Vertices"));
			TArray<FVector> V;
			if (!VN || RudeCarried::ParseVectors(VN->GetContent(), V) < 3) { ++Degenerate; continue; }
			++Polys; Verts += V.Num();
			for (int32 i = 0; i < V.Num(); ++i)
			{
				const FVector& A = V[i]; const FVector& B = V[(i + 1) % V.Num()];
				Lines.Add(FBatchedLine(A, B, PolyCol, -1.f, 1.5f, SDPG_World));
				Bounds += A;
			}
		}
	}
	if (const FXmlNode* PN = RootN->FindChildNode(TEXT("Portals")))
	{
		for (const FXmlNode* It : PN->GetChildrenNodes())
		{
			const FVector A = RudeCarried::XyzAttr(It->FindChildNode(TEXT("PositionFrom"))), B = RudeCarried::XyzAttr(It->FindChildNode(TEXT("PositionTo")));
			Lines.Add(FBatchedLine(A, B, PortalCol, -1.f, 4.f, SDPG_World)); ++Portals;
		}
	}
	if (const FXmlNode* PN = RootN->FindChildNode(TEXT("Points")))
	{
		for (const FXmlNode* It : PN->GetChildrenNodes())
		{
			const FVector P = RudeCarried::XyzAttr(It->FindChildNode(TEXT("Position")));
			Lines.Add(FBatchedLine(P, P + FVector(0, 0, 30.0), PointCol, -1.f, 2.f, SDPG_World)); ++Points;
		}
	}
	if (Lines.Num() == 0) { return Fail(TEXT("nothing to draw: no polygon with 3+ vertices, no portals, no points")); }
	LB->DrawLines(Lines);
	if (GEditor) { GEditor->RedrawLevelEditingViewports(); }
	const FXmlNode* BbMin = RootN->FindChildNode(TEXT("BBMin")); const FXmlNode* BbMax = RootN->FindChildNode(TEXT("BBMax"));
	const FVector C = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
	return FString::Printf(
		TEXT("{\"ok\":true,\"ynv\":\"%s\",\"slot\":\"%s\",\"polygons\":%d,\"polygonVertices\":%d,\"degenerate\":%d,\"portals\":%d,\"points\":%d,\"lines\":%d,")
		TEXT("\"bbMinRage\":\"%s\",\"bbMaxRage\":\"%s\",\"centreUEcm\":[%.1f,%.1f,%.1f],\"camSpecHint\":\"%.0f,%.0f,%.0f,-89,0\",\"note\":\"persistent lines; DebugDrawNavmesh YnvName=CLEAR flushes\"}"),
		*RudeJsonEscape(Row->Name), *RudeJsonEscape(Row->Slot), Polys, Verts, Degenerate, Portals, Points, Lines.Num(),
		BbMin ? *RudeJsonEscape(FString::Printf(TEXT("%s %s %s"), *BbMin->GetAttribute(TEXT("x")), *BbMin->GetAttribute(TEXT("y")), *BbMin->GetAttribute(TEXT("z")))) : TEXT(""),
		BbMax ? *RudeJsonEscape(FString::Printf(TEXT("%s %s %s"), *BbMax->GetAttribute(TEXT("x")), *BbMax->GetAttribute(TEXT("y")), *BbMax->GetAttribute(TEXT("z")))) : TEXT(""),
		C.X, C.Y, C.Z, C.X, C.Y, C.Z + 8000.0);
}
