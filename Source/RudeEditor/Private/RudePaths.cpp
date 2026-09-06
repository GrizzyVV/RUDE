// RUDE - RAGE <-> Unreal Development Environment
// Vehicle paths (ynd): one path cell as node actors + link splines, and the byte-safe splice back out.
// Every constant below was measured 2026-09-06 over five cells of the corpus (nodes464 = downtown, its
// neighbours 432 / 465 / 496, and the 8-node coastal cell 184): scratchpad/wp10/paths/LAWS.md.
// Helper names carry the "RudePaths" prefix on purpose: this file may share a unity-build blob with
// RudeLevelTools.cpp, whose own file-static RudeNum / RudeRawItems would otherwise collide.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeCorpus.h"
#include "RudePathNodeComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "XmlFile.h"

// A number as the corpus spells it: seven significant digits when that reads back to the same float32,
// otherwise nine (the ymap lane's rule; re-verified on paths: 18,436/18,436 node-position and
// junction-origin numbers over 5 cells re-spell identically).
static FString RudePathsNum(double V)
{
	const float F = (float)V;
	FString S = FString::Printf(TEXT("%.7g"), (double)F);
	if ((float)FCString::Atod(*S) != F) { S = FString::Printf(TEXT("%.9g"), (double)F); }
	if (S.Contains(TEXT("e")))
	{
		S = FString::Printf(TEXT("%.9f"), (double)F);
		while (S.EndsWith(TEXT("0"))) { S.LeftChopInline(1); }
		if (S.EndsWith(TEXT("."))) { S.LeftChopInline(1); }
	}
	if (S == TEXT("-0")) { S = TEXT("0"); }
	return S;
}

// ---- cell naming ----------------------------------------------------------------------------
// The grid law, measured: cell = row * 32 + col with col = floor((x + 8192) / 512), row = floor((y + 8192) / 512)
// (512 m cells over a 32 x 32 grid from -8192). nodes464 (col 16, row 14) holds x [0, 511.5], y [-1023.5,
// -513.75]; nodes184 (col 24, row 5) holds x [4386.5, 4391.75], y [-5177, -5123.25]. A node may sit exactly
// on the cell's upper edge (nodes465: y = -512.0; nodes496: y = 0.0). Downtown (120, -575) -> nodes464.
static bool RudePathsCellName(const FString& In, FString& OutCell, FString& OutWhy)
{
	FString S = In.TrimStartAndEnd().ToLower();
	if (S.StartsWith(TEXT("at:")))
	{
		TArray<FString> P;
		S.Mid(3).Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
		if (P.Num() < 2) { OutWhy = TEXT("'at:x,y' needs two GTA-metre coordinates"); return false; }
		const double X = FCString::Atod(*P[0]), Y = FCString::Atod(*P[1]);
		const int32 Col = FMath::FloorToInt((X + 8192.0) / 512.0), Row = FMath::FloorToInt((Y + 8192.0) / 512.0);
		// 32 columns; the ledger's highest cell is nodes1274 (row 39), so 40 rows: y reaches 12288 m
		if (Col < 0 || Col > 31 || Row < 0 || Row > 39) { OutWhy = FString::Printf(TEXT("(%g, %g) is outside the 32 x 40 path grid (x -8192..8192, y -8192..12288 m)"), X, Y); return false; }
		OutCell = FString::Printf(TEXT("nodes%d"), Row * 32 + Col);
		return true;
	}
	if (S.EndsWith(TEXT(".xml"))) { S.LeftChopInline(4); }
	if (S.EndsWith(TEXT(".ynd"))) { S.LeftChopInline(4); }
	if (S.IsNumeric()) { S = TEXT("nodes") + S; }
	if (!S.StartsWith(TEXT("nodes")) || S.Len() == 5 || !S.Mid(5).IsNumeric())
	{
		OutWhy = FString::Printf(TEXT("'%s' is not a path cell: give nodes<N>, <N>, or at:x,y (GTA metres; N = row*32+col over 512 m cells)"), *In);
		return false;
	}
	OutCell = S;
	return true;
}

// ---- raw slicing (the byte-safe seam) ---------------------------------------------------------
// The file's three top-level blocks sit at indent 1 with their items at indent 2; an EMPTY block is
// spelled self-closing (" <Junctions />" - nodes184). A node's nested <Links><Item> sit at indent 4, so the
// indent-2 close "\n  </Item>\n" never matches a link's close. Proven in Python before this port:
// header + " <Nodes>\n" + items + tail re-assembles all 4 non-empty measured files byte-identically.
struct FRudePathsBlock
{
	int32 Begin = -1;         // index of the newline before " <Tag>" / " <Tag />"
	int32 End = -1;           // index of the newline before " </Tag>"; self-closing: the newline ending " <Tag />"
	bool bSelfClosing = false;
	TArray<FString> Items;    // "  <Item>...\n  </Item>\n" each, byte-true, file order
};
static bool RudePathsSliceBlock(const FString& Text, const TCHAR* Tag, FRudePathsBlock& Out)
{
	const FString Open = FString::Printf(TEXT("\n <%s>\n"), Tag);
	const FString Close = FString::Printf(TEXT("\n </%s>"), Tag);
	const int32 B = Text.Find(Open, ESearchCase::CaseSensitive);
	if (B == INDEX_NONE)
	{
		const FString Self = FString::Printf(TEXT("\n <%s />\n"), Tag);
		const int32 K = Text.Find(Self, ESearchCase::CaseSensitive);
		if (K == INDEX_NONE) { return false; }
		Out.Begin = K; Out.End = K + Self.Len() - 1; Out.bSelfClosing = true;
		return true;
	}
	const int32 E = Text.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, B + Open.Len());
	if (E == INDEX_NONE) { return false; }
	const int32 InnerEnd = E + 1;   // the inner text includes the newline that ends the last item
	const FString ItemOpen = TEXT("  <Item");
	const FString ItemClose = TEXT("\n  </Item>\n");
	const FString ItemEmptyEnd = TEXT(" />\n");
	int32 Pos = B + Open.Len();
	while (Pos < InnerEnd)
	{
		if (!Text.Mid(Pos, ItemOpen.Len()).Equals(ItemOpen)) { return false; }
		const int32 Nl = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
		if (Nl == INDEX_NONE) { return false; }
		int32 ItemEnd;
		if (Text.Mid(Pos, Nl - Pos + 1).EndsWith(ItemEmptyEnd)) { ItemEnd = Nl + 1; }
		else
		{
			const int32 C = Text.Find(ItemClose, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
			if (C == INDEX_NONE || C + ItemClose.Len() > InnerEnd) { return false; }
			ItemEnd = C + ItemClose.Len();
		}
		Out.Items.Add(Text.Mid(Pos, ItemEnd - Pos));
		Pos = ItemEnd;
	}
	Out.Begin = B; Out.End = E;
	return Pos == InnerEnd;
}
// The file with one block's items replaced (an empty block stays as spelled; Wave 1 never adds to it).
static FString RudePathsReassemble(const FString& Text, const TCHAR* Tag, const FRudePathsBlock& Blk, const TArray<FString>& Items)
{
	if (Blk.bSelfClosing) { return Text; }
	FString O = Text.Left(Blk.Begin);
	O += FString::Printf(TEXT("\n <%s>\n"), Tag);
	for (const FString& I : Items) { O += I; }
	O += Text.Mid(Blk.End + 1);
	return O;
}
// Position out of a raw node item (the exporter needs every node's position, not only the placed ones).
static bool RudePathsPosOfItem(const FString& Item, FVector& Out)
{
	const int32 P = Item.Find(TEXT("<Position x=\""), ESearchCase::CaseSensitive);
	if (P == INDEX_NONE) { return false; }
	int32 I = P + 13;
	double V[3] = { 0, 0, 0 };
	for (int32 K = 0; K < 3; ++K)
	{
		const int32 Q = Item.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, I);
		if (Q == INDEX_NONE) { return false; }
		V[K] = FCString::Atod(*Item.Mid(I, Q - I));
		I = Item.Find(TEXT("=\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Q + 1);
		if (I == INDEX_NONE && K < 2) { return false; }
		I += 2;
	}
	Out = FVector(V[0], V[1], V[2]);
	return true;
}
static int32 RudePathsHeaderCount(const FString& Text, const TCHAR* Tag)
{
	const FString Key = FString::Printf(TEXT("<%s value=\""), Tag);
	const int32 P = Text.Find(Key, ESearchCase::CaseSensitive);
	if (P == INDEX_NONE) { return -1; }
	return FCString::Atoi(*Text.Mid(P + Key.Len(), 12));
}

// ---- the node as XML (edited nodes only) -------------------------------------------------------
// Field order and spelling verified against every node of 5 cells (5,988/5,988 nodes and 12,776/12,776
// links carry exactly this tag sequence). GTA metres in; the caller has already snapped to the grid.
static FString RudePathsNodeXml(const URudePathNodeComponent* R, const FVector& Gta)
{
	FString O;
	O += TEXT("  <Item>\n");
	O += FString::Printf(TEXT("   <AreaID value=\"%d\" />\n"), R->AreaID);
	O += FString::Printf(TEXT("   <NodeID value=\"%d\" />\n"), R->NodeID);
	if (R->StreetName.IsEmpty()) { O += TEXT("   <StreetName />\n"); }
	else { O += TEXT("   <StreetName>"); RudeXmlEscapeInto(O, R->StreetName); O += TEXT("</StreetName>\n"); }
	O += FString::Printf(TEXT("   <Position x=\"%s\" y=\"%s\" z=\"%s\" />\n"), *RudePathsNum(Gta.X), *RudePathsNum(Gta.Y), *RudePathsNum(Gta.Z));
	for (int32 K = 0; K < 6; ++K) { O += FString::Printf(TEXT("   <Flags%d value=\"%d\" />\n"), K, R->FlagAt(K)); }
	if (R->Links.Num() == 0)
	{
		// never observed (0 of 5,988 nodes have no links); spelled by analogy with the file's " <Junctions />"
		O += TEXT("   <Links />\n");
	}
	else
	{
		O += TEXT("   <Links>\n");
		for (const FRudePathLink& L : R->Links)
		{
			O += TEXT("    <Item>\n");
			O += FString::Printf(TEXT("     <ToAreaID value=\"%d\" />\n"), L.ToAreaID);
			O += FString::Printf(TEXT("     <ToNodeID value=\"%d\" />\n"), L.ToNodeID);
			O += FString::Printf(TEXT("     <Flags0 value=\"%d\" />\n"), L.Flags0);
			O += FString::Printf(TEXT("     <Flags1 value=\"%d\" />\n"), L.Flags1);
			O += FString::Printf(TEXT("     <Flags2 value=\"%d\" />\n"), L.Flags2);
			O += FString::Printf(TEXT("     <LinkLength value=\"%d\" />\n"), L.LinkLength);
			O += TEXT("    </Item>\n");
		}
		O += TEXT("   </Links>\n");
	}
	O += TEXT("  </Item>\n");
	return O;
}

// UE cm <-> GTA metres, Y mirrored (the entity lane's rule).
static FVector RudePathsUeToGta(const FVector& Ue) { return FVector(Ue.X / 100.0, -Ue.Y / 100.0, Ue.Z / 100.0); }
static FVector RudePathsGtaToUe(const FVector& G) { return FVector(G.X * 100.0, -G.Y * 100.0, G.Z * 100.0); }
// The file's grid: x, y on 1/4 m, z on 1/32 m (5,988/5,988 nodes and 236/236 junction origins measured).
static FVector RudePathsSnap(const FVector& G)
{
	return FVector(FMath::RoundToDouble(G.X * 4.0) / 4.0, FMath::RoundToDouble(G.Y * 4.0) / 4.0, FMath::RoundToDouble(G.Z * 32.0) / 32.0);
}

// A marker actor: scene root + one sphere. Movable, no collision, no shadow - it exists to be dragged.
static AActor* RudePathsSpawnMarker(UWorld* World, UStaticMesh* Mesh, const FString& Label, const FVector& Loc, float Scale, const FName& Folder)
{
	AActor* A = World->SpawnActor<AActor>();
	if (!A) { return nullptr; }
	USceneComponent* Root = NewObject<USceneComponent>(A, TEXT("Root"));
	A->SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);
	Root->RegisterComponent();
	A->AddInstanceComponent(Root);
	UStaticMeshComponent* SM = NewObject<UStaticMeshComponent>(A, TEXT("Marker"));
	SM->SetStaticMesh(Mesh);
	SM->SetMobility(EComponentMobility::Movable);
	SM->SetRelativeScale3D(FVector(Scale));
	SM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SM->SetCastShadow(false);
	SM->SetupAttachment(Root);
	SM->RegisterComponent();
	A->AddInstanceComponent(SM);
	A->SetActorLocation(Loc);
	A->SetActorLabel(Label);
	A->SetFolderPath(Folder);
	return A;
}

static AActor* RudePathsFindNode(UWorld* World, const FString& Cell, int32 Ordinal, URudePathNodeComponent*& OutR)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		URudePathNodeComponent* R = It->FindComponentByClass<URudePathNodeComponent>();
		if (R && R->SourceIndex == Ordinal && R->SourceYnd.Equals(Cell, ESearchCase::IgnoreCase)) { OutR = R; return *It; }
	}
	OutR = nullptr;
	return nullptr;
}

struct FRudePathsParsedNode
{
	int32 AreaID = 0, NodeID = 0;
	FString Street;
	FVector Pos = FVector::ZeroVector;
	int32 Flags[6] = { 0, 0, 0, 0, 0, 0 };
	TArray<FRudePathLink> Links;
};
static bool RudePathsParseNode(const FXmlNode* It, FRudePathsParsedNode& N, FString& Why)
{
	auto Val = [It, &Why](const TCHAR* Tag, int32& Out) -> bool
	{
		const FXmlNode* C = It->FindChildNode(Tag);
		if (!C) { Why = FString::Printf(TEXT("node lacks <%s>"), Tag); return false; }
		Out = FCString::Atoi(*C->GetAttribute(TEXT("value")));
		return true;
	};
	if (!Val(TEXT("AreaID"), N.AreaID) || !Val(TEXT("NodeID"), N.NodeID)) { return false; }
	const FXmlNode* S = It->FindChildNode(TEXT("StreetName"));
	N.Street = S ? S->GetContent().TrimStartAndEnd() : FString();
	const FXmlNode* P = It->FindChildNode(TEXT("Position"));
	if (!P) { Why = TEXT("node lacks <Position>"); return false; }
	N.Pos = FVector(FCString::Atod(*P->GetAttribute(TEXT("x"))), FCString::Atod(*P->GetAttribute(TEXT("y"))), FCString::Atod(*P->GetAttribute(TEXT("z"))));
	for (int32 K = 0; K < 6; ++K)
	{
		if (!Val(*FString::Printf(TEXT("Flags%d"), K), N.Flags[K])) { return false; }
	}
	if (const FXmlNode* L = It->FindChildNode(TEXT("Links")))
	{
		for (const FXmlNode* LI : L->GetChildrenNodes())
		{
			FRudePathLink K;
			auto LV = [LI](const TCHAR* Tag) { const FXmlNode* C = LI->FindChildNode(Tag); return C ? FCString::Atoi(*C->GetAttribute(TEXT("value"))) : 0; };
			K.ToAreaID = LV(TEXT("ToAreaID")); K.ToNodeID = LV(TEXT("ToNodeID"));
			K.Flags0 = LV(TEXT("Flags0")); K.Flags1 = LV(TEXT("Flags1")); K.Flags2 = LV(TEXT("Flags2"));
			K.LinkLength = LV(TEXT("LinkLength"));
			N.Links.Add(K);
		}
	}
	return true;
}

// ---- ImportPaths -------------------------------------------------------------------------------
// One path cell -> one actor per node (sphere: vehicle 0.6, ped 0.35, junction 1.0 of the engine's 1 m
// sphere), a URudePathNodeComponent on each, and one <cell>_Links actor holding a linear 2-point
// USplineComponent per in-cell road segment (bDrawDebug: persistent, selectable, saved - the scenario
// lane's finding). Reciprocal pairs (100% measured) draw once; a cross-cell link is counted, not drawn
// (its far node lives in another file). Re-running replaces this cell's actors by tag.
FString URudeToolset::ImportPaths(const FString& CorpusRoot, const FString& CellName, const FString& Filter)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	FString Cell, Why;
	if (!RudePathsCellName(CellName, Cell, Why)) { return Fail(Why); }
	const int32 CellNum = FCString::Atoi(*Cell.Mid(5));
	const FString F = Filter.TrimStartAndEnd().ToUpper();
	const bool bAll = F.IsEmpty() || F == TEXT("ALL");
	const bool bWantVeh = bAll || F == TEXT("VEH"), bWantPed = bAll || F == TEXT("PED"), bOnlyJunction = F == TEXT("JUNCTION");
	if (!bWantVeh && !bWantPed && !bOnlyJunction) { return Fail(TEXT("Filter must be ALL (default), VEH, PED or JUNCTION")); }

	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }
	const FRudeCorpusEntry* E = Corpus->Effective(TEXT("ynd"), Cell);
	if (!E) { return Fail(FString::Printf(TEXT("the corpus has no ynd '%s' (cells are nodes<row*32+col>; 'at:x,y' names one by GTA metres)"), *Cell)); }
	const FString Path = Corpus->PathOf(*E);
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path)) { return Fail(TEXT("cannot read ") + Path); }
	FXmlFile Xml(Raw, EConstructMethod::ConstructFromBuffer);
	if (!Xml.IsValid()) { return Fail(FString::Printf(TEXT("cannot parse %s: %s"), *Path, *Xml.GetLastError())); }
	const FXmlNode* Root = Xml.GetRootNode();
	if (!Root || Root->GetTag() != TEXT("NodeDictionary")) { return Fail(TEXT("root is not <NodeDictionary>: ") + Path); }
	const int32 VehCount = RudePathsHeaderCount(Raw, TEXT("VehicleNodeCount")), PedCount = RudePathsHeaderCount(Raw, TEXT("PedNodeCount"));
	if (VehCount < 0 || PedCount < 0) { return Fail(TEXT("no VehicleNodeCount / PedNodeCount header in ") + Path); }

	FRudePathsBlock NodesBlk, JuncBlk, RefBlk;
	if (!RudePathsSliceBlock(Raw, TEXT("Nodes"), NodesBlk)) { return Fail(TEXT("the <Nodes> block is not the measured shape (' <Nodes>' at indent 1, '  <Item>' at indent 2) in ") + Path); }
	if (!RudePathsSliceBlock(Raw, TEXT("Junctions"), JuncBlk)) { return Fail(TEXT("the <Junctions> block is not the measured shape in ") + Path); }
	if (!RudePathsSliceBlock(Raw, TEXT("JunctionRefs"), RefBlk)) { return Fail(TEXT("the <JunctionRefs> block is not the measured shape in ") + Path); }
	const FXmlNode* NodesX = Root->FindChildNode(TEXT("Nodes"));
	TArray<FXmlNode*> NodeItems; if (NodesX) { NodeItems = NodesX->GetChildrenNodes(); }
	const int32 N = NodeItems.Num();
	if (N != NodesBlk.Items.Num()) { return Fail(FString::Printf(TEXT("parsed %d nodes but sliced %d raw items - the slicer and the parser disagree; refusing"), N, NodesBlk.Items.Num())); }
	if (VehCount + PedCount != N) { return Fail(FString::Printf(TEXT("VehicleNodeCount %d + PedNodeCount %d != %d nodes (5/5 measured cells add up); refusing"), VehCount, PedCount, N)); }
	TArray<FRudePathsParsedNode> Nodes;
	Nodes.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		if (!RudePathsParseNode(NodeItems[i], Nodes[i], Why)) { return Fail(FString::Printf(TEXT("node %d: %s"), i, *Why)); }
		if (Nodes[i].NodeID != i) { return Fail(FString::Printf(TEXT("node %d spells NodeID %d - ordinals not sequential (5,988/5,988 measured are); refusing"), i, Nodes[i].NodeID)); }
		if (Nodes[i].AreaID != CellNum) { return Fail(FString::Printf(TEXT("node %d spells AreaID %d in cell %d; refusing"), i, Nodes[i].AreaID, CellNum)); }
	}
	// junctions: JunctionRefs row -> (node ordinal -> junction ordinal)
	const FXmlNode* JuncX = Root->FindChildNode(TEXT("Junctions"));
	const FXmlNode* RefsX = Root->FindChildNode(TEXT("JunctionRefs"));
	TArray<FXmlNode*> JuncItems; if (JuncX) { JuncItems = JuncX->GetChildrenNodes(); }
	TArray<FXmlNode*> RefItems; if (RefsX) { RefItems = RefsX->GetChildrenNodes(); }
	if (JuncItems.Num() != JuncBlk.Items.Num() || RefItems.Num() != RefBlk.Items.Num()) { return Fail(TEXT("junction blocks: parser and slicer disagree; refusing")); }
	struct FRef { int32 JunctionID = -1; int32 RefOrdinal = -1; int32 Unk0 = 0; };
	TMap<int32, FRef> JunctionOfNode;
	for (int32 r = 0; r < RefItems.Num(); ++r)
	{
		auto RV = [&](const TCHAR* Tag) { const FXmlNode* C = RefItems[r]->FindChildNode(Tag); return C ? FCString::Atoi(*C->GetAttribute(TEXT("value"))) : -1; };
		FRef Ref; Ref.JunctionID = RV(TEXT("JunctionID")); Ref.RefOrdinal = r; Ref.Unk0 = RV(TEXT("Unk0"));
		const int32 NodeId = RV(TEXT("NodeID"));
		if (!JuncItems.IsValidIndex(Ref.JunctionID) || !Nodes.IsValidIndex(NodeId)) { return Fail(FString::Printf(TEXT("JunctionRefs row %d points at junction %d / node %d, which do not exist; refusing"), r, Ref.JunctionID, NodeId)); }
		JunctionOfNode.Add(NodeId, Ref);
	}

	// replace a previous spawn of this cell
	const FName CellTag(*(TEXT("RUDE_PATH_CELL:") + Cell));
	int32 Replaced = 0;
	{
		TArray<AActor*> Old;
		for (TActorIterator<AActor> It(World); It; ++It) { if (It->Tags.Contains(CellTag)) { Old.Add(*It); } }
		for (AActor* A : Old) { World->EditorDestroyActor(A, true); ++Replaced; }
	}
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!Sphere) { return Fail(TEXT("engine marker mesh missing (/Engine/BasicShapes/Sphere) - cannot draw path nodes")); }
	const FName Folder(*(TEXT("RUDE_PATHS/") + Cell));

	TArray<AActor*> Actors;
	Actors.Init(nullptr, N);
	int32 Placed = 0, Skipped = 0, SpawnFailed = 0, JunctionNodes = 0;
	for (int32 i = 0; i < N; ++i)
	{
		const FRudePathsParsedNode& Nd = Nodes[i];
		const bool bPed = i >= VehCount;
		const FRef* Ref = JunctionOfNode.Find(i);
		const bool bWant = bOnlyJunction ? (Ref != nullptr) : (bPed ? bWantPed : bWantVeh);
		if (!bWant) { ++Skipped; continue; }
		AActor* A = RudePathsSpawnMarker(World, Sphere, FString::Printf(TEXT("%s:%d"), *Cell, i), RudePathsGtaToUe(Nd.Pos), Ref ? 1.0f : (bPed ? 0.35f : 0.6f), Folder);
		if (!A) { ++SpawnFailed; continue; }
		A->Tags.Add(FName(TEXT("RUDE_PATH")));
		A->Tags.Add(CellTag);
		A->Tags.Add(FName(bPed ? TEXT("RUDE_PATH_KIND:PED") : TEXT("RUDE_PATH_KIND:VEH")));
		if (Ref) { A->Tags.Add(FName(TEXT("RUDE_PATH_JUNCTION"))); }
		URudePathNodeComponent* R = NewObject<URudePathNodeComponent>(A, TEXT("RudePathNode"));
		R->AreaID = Nd.AreaID; R->NodeID = Nd.NodeID; R->bPedNode = bPed; R->StreetName = Nd.Street;
		R->Flags0 = Nd.Flags[0]; R->Flags1 = Nd.Flags[1]; R->Flags2 = Nd.Flags[2]; R->Flags3 = Nd.Flags[3]; R->Flags4 = Nd.Flags[4]; R->Flags5 = Nd.Flags[5];
		R->Links = Nd.Links;
		if (Ref)
		{
			++JunctionNodes;
			const FXmlNode* J = JuncItems[Ref->JunctionID];
			auto JV = [J](const TCHAR* Tag) { const FXmlNode* C = J->FindChildNode(Tag); return C ? C->GetAttribute(TEXT("value")) : FString(); };
			R->bJunction = true; R->JunctionID = Ref->JunctionID; R->JunctionUnk0 = Ref->Unk0;
			if (const FXmlNode* JP = J->FindChildNode(TEXT("Position"))) { R->JunctionPosition = FVector2D(FCString::Atod(*JP->GetAttribute(TEXT("x"))), FCString::Atod(*JP->GetAttribute(TEXT("y")))); }
			R->JunctionMinZ = FCString::Atof(*JV(TEXT("MinZ"))); R->JunctionMaxZ = FCString::Atof(*JV(TEXT("MaxZ")));
			R->JunctionSizeX = FCString::Atoi(*JV(TEXT("SizeX"))); R->JunctionSizeY = FCString::Atoi(*JV(TEXT("SizeY")));
			// heightmap rows from the RAW item (FXmlFile flattens multi-line text)
			const FString& JRaw = JuncBlk.Items[Ref->JunctionID];
			const int32 H0 = JRaw.Find(TEXT("<Heightmap>\n"), ESearchCase::CaseSensitive);
			const int32 H1 = H0 == INDEX_NONE ? INDEX_NONE : JRaw.Find(TEXT("\n   </Heightmap>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, H0);
			if (H0 != INDEX_NONE && H1 != INDEX_NONE)
			{
				TArray<FString> Rows;
				JRaw.Mid(H0 + 12, H1 - (H0 + 12)).ParseIntoArrayLines(Rows);
				for (FString& Row : Rows) { R->JunctionHeightmapRows.Add(Row.TrimStartAndEnd()); }
			}
			R->SourceJunctionXml = JRaw;
			R->SourceJunctionRefXml = RefBlk.Items[Ref->RefOrdinal];
		}
		R->SourceYnd = Cell; R->SourceSlot = E->Slot; R->SourceIndex = i;
		R->SourceXml = NodesBlk.Items[i];
		R->SourceTransform = A->GetActorTransform();
		R->SourceFieldsKey = R->FieldsKey();
		R->RegisterComponent();
		A->AddInstanceComponent(R);
		Actors[i] = A;
		++Placed;
	}
	if (Placed == 0) { return Fail(FString::Printf(TEXT("placed nothing: %d nodes, %d skipped by filter '%s', %d spawn failures"), N, Skipped, *F, SpawnFailed)); }

	// links: one actor, one linear spline per in-cell segment whose two nodes are both placed
	int32 LinksTotal = 0, Segments = 0, CrossCell = 0, NotDrawn = 0, Dangling = 0, OneWay = 0;
	AActor* LinksActor = World->SpawnActor<AActor>();
	if (!LinksActor) { return Fail(TEXT("links display actor spawn failed")); }
	{
		USceneComponent* CR = NewObject<USceneComponent>(LinksActor, TEXT("Root"));
		LinksActor->SetRootComponent(CR);
		CR->SetMobility(EComponentMobility::Movable);
		CR->RegisterComponent();
		LinksActor->AddInstanceComponent(CR);
		LinksActor->SetActorTransform(FTransform::Identity);   // splines below hold UE world coordinates in Local space
		LinksActor->SetActorLabel(Cell + TEXT("_Links"));
		LinksActor->SetFolderPath(Folder);
		LinksActor->Tags.Add(FName(TEXT("RUDE_PATH_LINKS")));
		LinksActor->Tags.Add(CellTag);
		TSet<uint64> Drawn;
		for (int32 i = 0; i < N; ++i)
		{
			for (const FRudePathLink& L : Nodes[i].Links)
			{
				++LinksTotal;
				if (L.ToAreaID != CellNum) { ++CrossCell; continue; }
				const int32 j = L.ToNodeID;
				if (!Nodes.IsValidIndex(j)) { ++Dangling; continue; }
				const uint64 Key = ((uint64)FMath::Min(i, j) << 32) | (uint32)FMath::Max(i, j);
				if (Drawn.Contains(Key)) { continue; }
				Drawn.Add(Key);
				bool bBack = false;
				for (const FRudePathLink& B : Nodes[j].Links) { if (B.ToAreaID == CellNum && B.ToNodeID == i) { bBack = true; break; } }
				if (!bBack) { ++OneWay; }
				if (!Actors[i] || !Actors[j]) { ++NotDrawn; continue; }
				USplineComponent* Sp = NewObject<USplineComponent>(LinksActor, FName(*FString::Printf(TEXT("Link_%d_%d"), FMath::Min(i, j), FMath::Max(i, j))));
				Sp->SetMobility(EComponentMobility::Movable);
				Sp->bDrawDebug = true;
				Sp->SetClosedLoop(false, false);
				Sp->ClearSplinePoints(false);
				Sp->AddSplinePoint(RudePathsGtaToUe(Nodes[i].Pos), ESplineCoordinateSpace::Local, false);
				Sp->AddSplinePoint(RudePathsGtaToUe(Nodes[j].Pos), ESplineCoordinateSpace::Local, false);
				Sp->SetSplinePointType(0, ESplinePointType::Linear, false);
				Sp->SetSplinePointType(1, ESplinePointType::Linear, false);
				Sp->UpdateSpline();
				Sp->SetupAttachment(CR);
				Sp->RegisterComponent();
				LinksActor->AddInstanceComponent(Sp);
				++Segments;
			}
		}
	}
	return FString::Printf(
		TEXT("{\"ok\":true,\"cell\":\"%s\",\"slot\":\"%s\",\"file\":\"%s\",\"vehicleNodes\":%d,\"pedNodes\":%d,\"nodes\":%d,\"placed\":%d,\"skippedByFilter\":%d,\"spawnFailed\":%d,")
		TEXT("\"junctions\":%d,\"junctionNodesPlaced\":%d,\"links\":%d,\"segmentsDrawn\":%d,\"crossCellLinks\":%d,\"segmentsNotDrawn\":%d,\"oneWayLinks\":%d,\"danglingLinks\":%d,\"replaced\":%d,")
		TEXT("\"labels\":\"%s:<ordinal>\",\"linksActor\":\"%s_Links\",\"note\":\"move a node with MovePathNode; ExportPaths splices it back into %s.ynd\"}"),
		*RudeJsonEscape(Cell), *RudeJsonEscape(E->Slot), *RudeJsonEscape(E->File), VehCount, PedCount, N, Placed, Skipped, SpawnFailed,
		JuncItems.Num(), JunctionNodes, LinksTotal, Segments, CrossCell, NotDrawn, OneWay, Dangling, Replaced,
		*RudeJsonEscape(Cell), *RudeJsonEscape(Cell), *RudeJsonEscape(Cell));
}

// ---- MovePathNode (agent) -----------------------------------------------------------------------
FString URudeToolset::MovePathNode(const FString& CellName, const FString& NodeID, const FString& DeltaCm)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	FString Cell, Why;
	if (!RudePathsCellName(CellName, Cell, Why)) { return Fail(Why); }
	if (!NodeID.TrimStartAndEnd().IsNumeric()) { return Fail(TEXT("NodeID must be the node's ordinal in the file")); }
	const int32 Idx = FCString::Atoi(*NodeID);
	TArray<FString> P;
	DeltaCm.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(P, TEXT(","), true);
	if (P.Num() != 3) { return Fail(TEXT("DeltaCm must be x,y,z in UE centimetres")); }
	const FVector D(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2]));
	URudePathNodeComponent* R = nullptr;
	AActor* A = RudePathsFindNode(World, Cell, Idx, R);
	if (!A) { return Fail(FString::Printf(TEXT("no path node %s:%d in the level"), *Cell, Idx)); }
	const FVector Before = A->GetActorLocation();
	A->SetActorLocation(Before + D);
	A->MarkPackageDirty();
	const FVector G0 = RudePathsUeToGta(Before), G1 = RudePathsSnap(RudePathsUeToGta(Before + D));
	return FString::Printf(TEXT("{\"ok\":true,\"node\":\"%s:%d\",\"beforeUe\":[%g,%g,%g],\"afterUe\":[%g,%g,%g],\"beforeGta\":[%s,%s,%s],\"afterGtaSnapped\":[%s,%s,%s],\"links\":%d}"),
		*RudeJsonEscape(Cell), Idx, Before.X, Before.Y, Before.Z, Before.X + D.X, Before.Y + D.Y, Before.Z + D.Z,
		*RudePathsNum(G0.X), *RudePathsNum(G0.Y), *RudePathsNum(G0.Z), *RudePathsNum(G1.X), *RudePathsNum(G1.Y), *RudePathsNum(G1.Z), R->Links.Num());
}

// ---- ExportPaths -------------------------------------------------------------------------------
// The level's path nodes back to <OutDir>/stream/<cell>.ynd (XML, the corpus spelling). The source file's
// bytes are SPLICED: only the <Nodes> block is re-emitted, and inside it an untouched node (transform and
// fields key unchanged) is its own raw slice; an edited node is rebuilt from the component + actor
// location, snapped to the file's grid. Nodes not in the level (filtered at import) go out verbatim -
// Wave 1 has no delete (ordinals are link targets in this and the neighbouring files) and no add
// (a new node would need a NodeID the neighbours cannot know); both are counted, never silent.
// Junctions / JunctionRefs / header counts are re-emitted verbatim.
FString URudeToolset::ExportPaths(const FString& OutDir, const FString& CellName, const FString& CorpusRoot)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	if (OutDir.TrimStartAndEnd().IsEmpty()) { return Fail(TEXT("give an OutDir for the FiveM resource")); }
	FString CorpusErr;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
	if (!Corpus.IsValid()) { return Fail(CorpusErr); }
	TSet<FString> Wanted;
	{
		TArray<FString> Parts;
		CellName.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& P : Parts)
		{
			if (P.TrimStartAndEnd().IsEmpty()) { continue; }
			FString Cell, Why;
			if (!RudePathsCellName(P, Cell, Why)) { return Fail(Why); }
			Wanted.Add(Cell);
		}
	}
	// 1) the level's nodes, by cell and ordinal
	struct FEnt { URudePathNodeComponent* R = nullptr; FTransform Xf; };
	TMap<FString, TMap<int32, FEnt>> ByCell;
	int32 Seen = 0, Unsourced = 0, Duplicate = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		URudePathNodeComponent* R = It->FindComponentByClass<URudePathNodeComponent>();
		if (!R) { continue; }
		++Seen;
		if (R->SourceYnd.IsEmpty() || R->SourceIndex < 0) { ++Unsourced; continue; }
		const FString Cell = R->SourceYnd.ToLower();
		if (Wanted.Num() > 0 && !Wanted.Contains(Cell)) { continue; }
		TMap<int32, FEnt>& M = ByCell.FindOrAdd(Cell);
		if (M.Contains(R->SourceIndex)) { ++Duplicate; continue; }
		FEnt En; En.R = R; En.Xf = It->GetActorTransform();
		M.Add(R->SourceIndex, En);
	}
	if (ByCell.Num() == 0) { return Fail(FString::Printf(TEXT("no RUDE path nodes to export (%d components seen, %d without a source cell)"), Seen, Unsourced)); }
	IFileManager::Get().MakeDirectory(*(OutDir / TEXT("stream")), true);

	int32 Written = 0, Refused = 0;
	int32 Kept = 0, Edited = 0, NotInLevel = 0, AddedNotWritten = 0, Snapped = 0, StaleLinkLengths = 0, OrderBroken = 0, JunctionEditsNotWritten = 0;
	FString Files, RefusedList;
	for (auto& KV : ByCell)
	{
		const FString& Cell = KV.Key;
		TMap<int32, FEnt>& M = KV.Value;
		const int32 CellNum = FCString::Atoi(*Cell.Mid(5));
		auto Refuse = [&](const FString& Why)
		{
			++Refused;
			RefusedList += FString::Printf(TEXT("%s\"%s: %s\""), RefusedList.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(Cell), *RudeJsonEscape(Why));
		};
		const FRudeCorpusEntry* E = Corpus->Effective(TEXT("ynd"), Cell);
		if (!E) { Refuse(TEXT("the corpus has no such ynd")); continue; }
		const FString SrcPath = Corpus->PathOf(*E);
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *SrcPath)) { Refuse(TEXT("cannot read ") + SrcPath); continue; }
		FRudePathsBlock Blk;
		if (!RudePathsSliceBlock(Raw, TEXT("Nodes"), Blk) || Blk.bSelfClosing) { Refuse(TEXT("the <Nodes> block is not the measured shape")); continue; }
		const int32 N = Blk.Items.Num();
		const int32 VehCount = RudePathsHeaderCount(Raw, TEXT("VehicleNodeCount"));
		TArray<FVector> Pos;
		Pos.SetNum(N);
		bool bPosOk = true;
		for (int32 i = 0; i < N; ++i) { if (!RudePathsPosOfItem(Blk.Items[i], Pos[i])) { bPosOk = false; break; } }
		if (!bPosOk || VehCount < 0) { Refuse(TEXT("a node item without a <Position>, or no VehicleNodeCount header")); continue; }

		TArray<FString> Out;
		Out.Reserve(N);
		int32 CKept = 0, CEdited = 0, CNotInLevel = 0, CAdded = 0, CSnapped = 0, CStale = 0, COrder = 0;
		for (int32 i = 0; i < N; ++i)
		{
			const FEnt* En = M.Find(i);
			if (!En) { Out.Add(Blk.Items[i]); ++CNotInLevel; continue; }
			URudePathNodeComponent* R = En->R;
			const bool bUntouched = En->Xf.GetLocation().Equals(R->SourceTransform.GetLocation(), 1e-3f) && R->FieldsKey() == R->SourceFieldsKey;
			if (bUntouched) { Out.Add(Blk.Items[i]); ++CKept; continue; }
			const FVector G = RudePathsUeToGta(En->Xf.GetLocation());
			const FVector S = RudePathsSnap(G);
			if (!S.Equals(G, 1e-6)) { ++CSnapped; }
			// LinkLength approximates 3D distance (rule unproven) - never rewritten; stale ones are counted so the
			// author knows. Reciprocal links on the far nodes are equally stale and equally untouched.
			for (const FRudePathLink& L : R->Links)
			{
				if (L.ToAreaID != CellNum || !Pos.IsValidIndex(L.ToNodeID)) { continue; }
				if (FMath::Abs(FVector::Dist(S, Pos[L.ToNodeID]) - (double)L.LinkLength) > 1.0) { ++CStale; }
			}
			// the file keeps each partition (vehicle / ped) sorted by Y ascending; a move that crosses a neighbour
			// breaks that order (whether the game relies on it is unmeasured) - counted, never re-sorted
			{
				const int32 Lo = i < VehCount ? 0 : VehCount, Hi = i < VehCount ? VehCount : N;
				if ((i > Lo && S.Y < Pos[i - 1].Y) || (i + 1 < Hi && S.Y > Pos[i + 1].Y)) { ++COrder; }
			}
			Pos[i] = S;
			Out.Add(RudePathsNodeXml(R, S));
			++CEdited;
		}
		for (auto& Pair : M)
		{
			if (Pair.Key >= N) { ++CAdded; }
			if (Pair.Value.R->bJunction && Pair.Key < N && !Pair.Value.R->SourceJunctionXml.IsEmpty())
			{
				// junction fields are VisibleAnywhere and go out verbatim; nothing to compare yet - reserved for the
				// wave that lifts the heightmap into an editable control
			}
		}
		const FString Doc = RudePathsReassemble(Raw, TEXT("Nodes"), Blk, Out);
		const FString OutPath = OutDir / TEXT("stream") / (Cell + TEXT(".ynd"));
		if (!FFileHelper::SaveStringToFile(Doc, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return Fail(TEXT("cannot write ") + OutPath); }
		++Written;
		Kept += CKept; Edited += CEdited; NotInLevel += CNotInLevel; AddedNotWritten += CAdded; Snapped += CSnapped; StaleLinkLengths += CStale; OrderBroken += COrder;
		Files += FString::Printf(TEXT("%s{\"cell\":\"%s\",\"slot\":\"%s\",\"nodes\":%d,\"kept\":%d,\"edited\":%d,\"notInLevel\":%d,\"addedNotWritten\":%d,\"snapped\":%d,\"staleLinkLengths\":%d,\"orderBroken\":%d,\"out\":\"%s\"}"),
			Files.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(Cell), *RudeJsonEscape(E->Slot), N, CKept, CEdited, CNotInLevel, CAdded, CSnapped, CStale, COrder, *RudeJsonEscape(OutPath));
	}
	if (Written > 0)
	{
		FFileHelper::SaveStringToFile(TEXT("fx_version 'cerulean'\ngame 'gta5'\nthis_is_a_map 'yes'\n"), *(OutDir / TEXT("fxmanifest.lua")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	return FString::Printf(
		TEXT("{\"ok\":%s,\"written\":%d,\"refused\":%d,\"kept\":%d,\"edited\":%d,\"notInLevel\":%d,\"addedNotWritten\":%d,\"snapped\":%d,\"staleLinkLengths\":%d,\"orderBroken\":%d,\"junctionEditsNotWritten\":%d,\"duplicateOrdinals\":%d,\"unsourced\":%d,\"files\":[%s],\"refusals\":[%s]}"),
		Written > 0 ? TEXT("true") : TEXT("false"), Written, Refused, Kept, Edited, NotInLevel, AddedNotWritten, Snapped, StaleLinkLengths, OrderBroken, JunctionEditsNotWritten, Duplicate, Unsourced, *Files, *RefusedList);
}
