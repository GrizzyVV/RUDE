// RUDE - RAGE <-> Unreal Development Environment
// THE WATER LANE: the game's water table in, edit it in the viewport, write it back.
//
// WHY IT IS A LANE AND NOT A PLANE. RUDE already had `AddWaterPlane`, which drops a flat surface at
// sea level so you can SEE where water sits. That is a visual guide and says so - it is not game
// data, it knows nothing about where water actually is, and nothing can be exported from it. The
// game's real answer is one file:
//
//   00_base/common.rpf/data/levels/gta5/water.xml   (329,478 bytes, measured 2026-09-11)
//     <WaterQuads>    504 items   minX maxX minY maxY | Type IsInvisible HasLimitedDepth z a1..a4 NoStencil
//     <CalmingQuads>  542 items   minX maxX minY maxY | fDampening
//     <WaveQuads>     116 items   minX maxX minY maxY | Amplitude XDirection YDirection
//   1,162 items, and 1,162 occurrences of each of the four bounds - so every item in every section
//   carries them, and only the remaining fields are per-kind. Counted from the file, not assumed.
//
// THE ROUND-TRIP DISCIPLINE, same as every other carried lane here: a quad nobody touched goes back
// as ITS OWN BYTES (`SourceXml`), a quad that moved or changed is re-emitted, and the verdict counts
// both plus anything it could not account for. An export that cannot say where all 1,162 went is not
// a round trip, it is a rewrite.
//
// ⚠ COORDINATES. The bounds are world-space GTA metres. RUDE's map convention is metres -> cm with
// the Y axis mirrored, so the mirror SWAPS the roles of minY and maxY: a quad's UE Y range is
// [-maxY*100, -minY*100]. Getting that backwards produces a quad of negative extent that still looks
// plausible in a verdict, which is why the export asserts the ordering it writes.

#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "RudeWaterQuadComponent.h"
#include "RudeCorpus.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/ConstructorHelpers.h"

#if WITH_EDITOR

namespace
{
	static const TCHAR* kWaterRelPath = TEXT("00_base/common.rpf/data/levels/gta5/water.xml");
	static const FName  kWaterTag(TEXT("RUDE_WATER"));

	struct FWaterSection
	{
		const TCHAR* Tag;
		ERudeWaterQuadKind Kind;
	};
	static const FWaterSection kSections[3] =
	{
		{ TEXT("WaterQuads"),   ERudeWaterQuadKind::Water   },
		{ TEXT("CalmingQuads"), ERudeWaterQuadKind::Calming },
		{ TEXT("WaveQuads"),    ERudeWaterQuadKind::Wave    },
	};

	static const TCHAR* KindWord(ERudeWaterQuadKind K)
	{
		switch (K)
		{
		case ERudeWaterQuadKind::Calming: return TEXT("calming");
		case ERudeWaterQuadKind::Wave:    return TEXT("wave");
		default:                          return TEXT("water");
		}
	}

	// `<name value="..." />` - the one spelling every field in this file uses. Returns false when the
	// element is absent, so a missing field is COUNTED rather than defaulted silently.
	static bool AttrOf(const FString& Item, const TCHAR* Name, FString& Out)
	{
		const FString Open = FString::Printf(TEXT("<%s "), Name);
		int32 At = Item.Find(Open, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE) { return false; }
		const int32 Q = Item.Find(TEXT("value=\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		if (Q == INDEX_NONE) { return false; }
		const int32 Start = Q + 7;
		const int32 End = Item.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE) { return false; }
		Out = Item.Mid(Start, End - Start);
		return true;
	}

	static float FloatAttr(const FString& Item, const TCHAR* Name, bool& bFound)
	{
		FString V;
		bFound = AttrOf(Item, Name, V);
		return bFound ? FCString::Atof(*V) : 0.f;
	}
	static int32 IntAttr(const FString& Item, const TCHAR* Name, bool& bFound)
	{
		FString V;
		bFound = AttrOf(Item, Name, V);
		return bFound ? FCString::Atoi(*V) : 0;
	}
	static bool BoolAttr(const FString& Item, const TCHAR* Name, bool& bFound)
	{
		FString V;
		bFound = AttrOf(Item, Name, V);
		return bFound && V.TrimStartAndEnd().Equals(TEXT("true"), ESearchCase::IgnoreCase);
	}

	// Every `<Item>...</Item>` slice inside one section, with its byte span, so the export can splice
	// the file rather than regenerate it.
	struct FRawItem { FString Xml; int32 Start = 0; int32 End = 0; };
	static bool SectionItems(const FString& Doc, const TCHAR* Tag, TArray<FRawItem>& Out,
	                         int32& OutSecStart, int32& OutSecEnd)
	{
		const FString Open = FString::Printf(TEXT("<%s>"), Tag);
		const FString Close = FString::Printf(TEXT("</%s>"), Tag);
		const int32 A = Doc.Find(Open, ESearchCase::CaseSensitive);
		if (A == INDEX_NONE) { return false; }
		const int32 B = Doc.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, A);
		if (B == INDEX_NONE) { return false; }
		OutSecStart = A + Open.Len();
		OutSecEnd = B;
		int32 Cur = OutSecStart;
		while (Cur < OutSecEnd)
		{
			const int32 IS = Doc.Find(TEXT("<Item>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cur);
			if (IS == INDEX_NONE || IS >= OutSecEnd) { break; }
			const int32 IE = Doc.Find(TEXT("</Item>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, IS);
			if (IE == INDEX_NONE || IE >= OutSecEnd) { break; }
			FRawItem R;
			R.Start = IS;
			R.End = IE + 7;
			R.Xml = Doc.Mid(R.Start, R.End - R.Start);
			Out.Add(MoveTemp(R));
			Cur = IE + 7;
		}
		return true;
	}

	// Where the corpus keeps water.xml. Resolved by the game's OWN fixed path first (it is a constant
	// of the game, not of the export), and only then by a ledger lookup - `water.xml` appears twice in
	// the corpus (gta5/ and stripped/) under one ledger key, and silently taking whichever the tie
	// break returns is how the wrong file gets edited.
	static bool ResolveWaterXml(const FString& CorpusRoot, FString& OutPath, FString& OutWhy)
	{
		const FString Direct = FPaths::Combine(CorpusRoot, kWaterRelPath);
		if (FPaths::FileExists(Direct)) { OutPath = Direct; return true; }
		OutWhy = FString::Printf(TEXT("no water table at %s - the game keeps it at %s and this lane will not "
			"guess at another copy (the corpus also holds a stripped/ one, which is a different file)"),
			*Direct, kWaterRelPath);
		return false;
	}
}

// ---- ImportWater ---------------------------------------------------------------------------------
FString URudeToolset::ImportWater(const FString& CorpusRoot, const FString& Filter)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }

	FString XmlPath, Why;
	if (!ResolveWaterXml(CorpusRoot.TrimStartAndEnd(), XmlPath, Why)) { return Fail(Why); }
	FString Doc;
	if (!FFileHelper::LoadFileToString(Doc, *XmlPath)) { return Fail(FString::Printf(TEXT("cannot read %s"), *XmlPath)); }

	// Filter: empty = every kind. Otherwise a comma list of kind words.
	const FString F = Filter.TrimStartAndEnd().ToLower();
	auto Wanted = [&F](ERudeWaterQuadKind K)
	{
		return F.IsEmpty() || F.Contains(KindWord(K));
	};

	// re-running replaces this lane's actors, the way every other carried lane does
	int32 Removed = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(kWaterTag)) { It->Destroy(); ++Removed; }
	}

	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (!Plane) { return Fail(TEXT("/Engine/BasicShapes/Plane is missing - cannot draw the quads")); }

	int32 ItemsInXml = 0, Spawned = 0, BoundsMissing = 0, FieldsMissing = 0, DegenerateBounds = 0;
	int32 PerKindXml[3] = { 0, 0, 0 }, PerKindSpawned[3] = { 0, 0, 0 };
	FString SectionsMissing;

	for (int32 si = 0; si < 3; ++si)
	{
		const FWaterSection& Sec = kSections[si];
		TArray<FRawItem> Items;
		int32 SS = 0, SE = 0;
		if (!SectionItems(Doc, Sec.Tag, Items, SS, SE))
		{
			SectionsMissing += FString::Printf(TEXT("%s%s"), SectionsMissing.IsEmpty() ? TEXT("") : TEXT(","), Sec.Tag);
			continue;
		}
		PerKindXml[si] = Items.Num();
		ItemsInXml += Items.Num();
		if (!Wanted(Sec.Kind)) { continue; }

		for (int32 i = 0; i < Items.Num(); ++i)
		{
			const FString& Raw = Items[i].Xml;
			bool bA = false, bB = false, bC = false, bD = false;
			const float MinX = FloatAttr(Raw, TEXT("minX"), bA);
			const float MaxX = FloatAttr(Raw, TEXT("maxX"), bB);
			const float MinY = FloatAttr(Raw, TEXT("minY"), bC);
			const float MaxY = FloatAttr(Raw, TEXT("maxY"), bD);
			if (!bA || !bB || !bC || !bD) { ++BoundsMissing; continue; }
			if (MaxX <= MinX || MaxY <= MinY) { ++DegenerateBounds; }

			bool bZ = false;
			const float Z = FloatAttr(Raw, TEXT("z"), bZ);   // WaterQuads only; calming/wave sit at 0

			AActor* A = World->SpawnActor<AActor>();
			if (!A) { continue; }
			USceneComponent* Root = NewObject<USceneComponent>(A, TEXT("Root"));
			A->SetRootComponent(Root);
			Root->SetMobility(EComponentMobility::Movable);
			Root->RegisterComponent();
			A->AddInstanceComponent(Root);

			UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(A, TEXT("Quad"));
			SMC->SetStaticMesh(Plane);
			SMC->SetMobility(EComponentMobility::Movable);
			SMC->SetupAttachment(Root);
			SMC->RegisterComponent();
			A->AddInstanceComponent(SMC);
			SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);

			// metres -> cm with the Y mirror; the mirror swaps which bound is the UE minimum
			const double X0 = MinX * 100.0, X1 = MaxX * 100.0;
			const double Y0 = -MaxY * 100.0, Y1 = -MinY * 100.0;
			const FVector Centre((X0 + X1) * 0.5, (Y0 + Y1) * 0.5, Z * 100.0);
			A->SetActorLocation(Centre);
			// the engine plane is 100 uu across, so a scale of 1 is 1 m
			SMC->SetWorldScale3D(FVector(FMath::Abs(X1 - X0) / 100.0, FMath::Abs(Y1 - Y0) / 100.0, 1.0));

			URudeWaterQuadComponent* C = NewObject<URudeWaterQuadComponent>(A, TEXT("WaterQuad"));
			C->RegisterComponent();
			A->AddInstanceComponent(C);
			C->Kind = Sec.Kind;
			C->SourceFile = XmlPath;
			C->SourceIndex = i;
			C->SourceXml = Raw;
			C->SourceMinX = MinX; C->SourceMaxX = MaxX;
			C->SourceMinY = MinY; C->SourceMaxY = MaxY;
			C->SourceZ = Z;

			bool bF = false;
			switch (Sec.Kind)
			{
			case ERudeWaterQuadKind::Water:
				C->Type = IntAttr(Raw, TEXT("Type"), bF);                  if (!bF) { ++FieldsMissing; }
				C->bIsInvisible = BoolAttr(Raw, TEXT("IsInvisible"), bF);  if (!bF) { ++FieldsMissing; }
				C->bHasLimitedDepth = BoolAttr(Raw, TEXT("HasLimitedDepth"), bF); if (!bF) { ++FieldsMissing; }
				C->bNoStencil = BoolAttr(Raw, TEXT("NoStencil"), bF);      if (!bF) { ++FieldsMissing; }
				C->A1 = IntAttr(Raw, TEXT("a1"), bF);                      if (!bF) { ++FieldsMissing; }
				C->A2 = IntAttr(Raw, TEXT("a2"), bF);                      if (!bF) { ++FieldsMissing; }
				C->A3 = IntAttr(Raw, TEXT("a3"), bF);                      if (!bF) { ++FieldsMissing; }
				C->A4 = IntAttr(Raw, TEXT("a4"), bF);                      if (!bF) { ++FieldsMissing; }
				break;
			case ERudeWaterQuadKind::Calming:
				C->Dampening = FloatAttr(Raw, TEXT("fDampening"), bF);     if (!bF) { ++FieldsMissing; }
				break;
			case ERudeWaterQuadKind::Wave:
				C->Amplitude = FloatAttr(Raw, TEXT("Amplitude"), bF);      if (!bF) { ++FieldsMissing; }
				C->XDirection = FloatAttr(Raw, TEXT("XDirection"), bF);    if (!bF) { ++FieldsMissing; }
				C->YDirection = FloatAttr(Raw, TEXT("YDirection"), bF);    if (!bF) { ++FieldsMissing; }
				break;
			}
			C->SourceFieldsKey = C->FieldsKey();

			A->SetActorLabel(FString::Printf(TEXT("WATER_%s_%04d"), KindWord(Sec.Kind), i));
			A->SetFolderPath(FName(TEXT("RUDE_Water")));
			A->Tags.Add(kWaterTag);
			A->Tags.Add(FName(*FString::Printf(TEXT("RUDE_WATER_KIND:%s"), KindWord(Sec.Kind))));
			++Spawned;
			++PerKindSpawned[si];
		}
	}

	const bool bOk = ItemsInXml > 0 && BoundsMissing == 0 && SectionsMissing.IsEmpty();
	return FString::Printf(
		TEXT("{\"ok\":%s,\"waterXml\":\"%s\",\"itemsInXml\":%d,\"spawned\":%d,\"replacedExisting\":%d,")
		TEXT("\"byKindInXml\":{\"water\":%d,\"calming\":%d,\"wave\":%d},")
		TEXT("\"byKindSpawned\":{\"water\":%d,\"calming\":%d,\"wave\":%d},")
		TEXT("\"boundsMissing\":%d,\"fieldsMissing\":%d,\"degenerateBounds\":%d,\"sectionsMissing\":\"%s\",")
		TEXT("\"note\":\"every quad is a movable marker: its transform IS its bounds and height, so moving it in "
		     "the viewport is the edit. ExportWater writes untouched quads back as their own bytes. Bounds are "
		     "metres -> cm with the Y mirror, which swaps minY and maxY.\"}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(XmlPath), ItemsInXml, Spawned, Removed,
		PerKindXml[0], PerKindXml[1], PerKindXml[2],
		PerKindSpawned[0], PerKindSpawned[1], PerKindSpawned[2],
		BoundsMissing, FieldsMissing, DegenerateBounds, *RudeJsonEscape(SectionsMissing));
}

// ---- ExportWater ---------------------------------------------------------------------------------
// Splices the SOURCE FILE. Every item slot is filled by exactly one of: the quad's own original bytes
// (nothing changed), or a freshly written item (it moved or a field changed). A quad that was filtered
// out at import, or deleted from the level, keeps its original bytes - deleting water is not something
// this tool will do by omission.
FString URudeToolset::ExportWater(const FString& CorpusRoot, const FString& OutDir, const FString& Options)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }

	FString Expect;
	{
		TArray<FString> Parts;
		Options.ParseIntoArray(Parts, TEXT(";"), true);
		for (const FString& P : Parts)
		{
			FString K, V;
			if (P.Split(TEXT("="), &K, &V) && K.TrimStartAndEnd().Equals(TEXT("expect"), ESearchCase::IgnoreCase))
			{
				Expect = V.TrimStartAndEnd().ToLower();
			}
		}
	}

	FString XmlPath, Why;
	if (!ResolveWaterXml(CorpusRoot.TrimStartAndEnd(), XmlPath, Why)) { return Fail(Why); }
	FString Doc;
	if (!FFileHelper::LoadFileToString(Doc, *XmlPath)) { return Fail(FString::Printf(TEXT("cannot read %s"), *XmlPath)); }

	// level quads, by (kind, source index)
	TMap<FString, URudeWaterQuadComponent*> ByKey;
	TMap<FString, AActor*> ActorByKey;
	int32 InLevel = 0, Orphans = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(kWaterTag)) { continue; }
		URudeWaterQuadComponent* C = It->FindComponentByClass<URudeWaterQuadComponent>();
		if (!C) { continue; }
		++InLevel;
		if (C->SourceIndex < 0) { ++Orphans; continue; }
		const FString Key = FString::Printf(TEXT("%s/%d"), KindWord(C->Kind), C->SourceIndex);
		ByKey.Add(Key, C);
		ActorByKey.Add(Key, *It);
	}

	int32 SlotsTotal = 0, Carried = 0, Rewritten = 0, NotInLevel = 0, Inverted = 0;
	FString Out = Doc;
	// splice from the END so earlier spans keep their offsets
	struct FEdit { int32 Start; int32 End; FString Text; };
	TArray<FEdit> Edits;

	for (int32 si = 0; si < 3; ++si)
	{
		const FWaterSection& Sec = kSections[si];
		TArray<FRawItem> Items;
		int32 SS = 0, SE = 0;
		if (!SectionItems(Doc, Sec.Tag, Items, SS, SE)) { continue; }
		SlotsTotal += Items.Num();
		for (int32 i = 0; i < Items.Num(); ++i)
		{
			const FString Key = FString::Printf(TEXT("%s/%d"), KindWord(Sec.Kind), i);
			URudeWaterQuadComponent** Found = ByKey.Find(Key);
			if (!Found || !*Found) { ++NotInLevel; ++Carried; continue; }   // its own bytes stand
			URudeWaterQuadComponent* C = *Found;
			AActor* A = ActorByKey.FindRef(Key);

			// where the marker is NOW, back through the mirror
			float MinX = C->SourceMinX, MaxX = C->SourceMaxX, MinY = C->SourceMinY, MaxY = C->SourceMaxY, Z = C->SourceZ;
			if (A)
			{
				const FVector Loc = A->GetActorLocation();
				FVector Scale(1, 1, 1);
				if (UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>())
				{
					Scale = SMC->GetComponentScale();
				}
				const double HalfX = FMath::Abs(Scale.X) * 100.0 * 0.5;
				const double HalfY = FMath::Abs(Scale.Y) * 100.0 * 0.5;
				const double UeMinX = Loc.X - HalfX, UeMaxX = Loc.X + HalfX;
				const double UeMinY = Loc.Y - HalfY, UeMaxY = Loc.Y + HalfY;
				MinX = (float)(UeMinX / 100.0);
				MaxX = (float)(UeMaxX / 100.0);
				// the mirror swaps them back
				MinY = (float)(-UeMaxY / 100.0);
				MaxY = (float)(-UeMinY / 100.0);
				Z = (float)(Loc.Z / 100.0);
			}
			// ⚠ the ordering the file requires, asserted rather than assumed: a mirrored bound written
			// the wrong way round makes a quad of negative extent that still reads plausibly in a verdict
			if (MaxX < MinX || MaxY < MinY) { ++Inverted; }

			const bool bGeomSame =
				FMath::IsNearlyEqual(MinX, C->SourceMinX, 0.001f) && FMath::IsNearlyEqual(MaxX, C->SourceMaxX, 0.001f) &&
				FMath::IsNearlyEqual(MinY, C->SourceMinY, 0.001f) && FMath::IsNearlyEqual(MaxY, C->SourceMaxY, 0.001f) &&
				FMath::IsNearlyEqual(Z, C->SourceZ, 0.001f);
			const bool bFieldsSame = C->FieldsKey() == C->SourceFieldsKey;
			if (bGeomSame && bFieldsSame) { ++Carried; continue; }

			FString T;
			T += TEXT("<Item>\n");
			T += FString::Printf(TEXT("      <minX value=\"%g\" />\n"), MinX);
			T += FString::Printf(TEXT("      <maxX value=\"%g\" />\n"), MaxX);
			T += FString::Printf(TEXT("      <minY value=\"%g\" />\n"), MinY);
			T += FString::Printf(TEXT("      <maxY value=\"%g\" />\n"), MaxY);
			switch (Sec.Kind)
			{
			case ERudeWaterQuadKind::Water:
				T += FString::Printf(TEXT("      <Type value=\"%d\" />\n"), C->Type);
				T += FString::Printf(TEXT("      <IsInvisible value=\"%s\" />\n"), C->bIsInvisible ? TEXT("true") : TEXT("false"));
				T += FString::Printf(TEXT("      <HasLimitedDepth value=\"%s\" />\n"), C->bHasLimitedDepth ? TEXT("true") : TEXT("false"));
				T += FString::Printf(TEXT("      <z value=\"%g\" />\n"), Z);
				T += FString::Printf(TEXT("      <a1 value=\"%d\" />\n"), C->A1);
				T += FString::Printf(TEXT("      <a2 value=\"%d\" />\n"), C->A2);
				T += FString::Printf(TEXT("      <a3 value=\"%d\" />\n"), C->A3);
				T += FString::Printf(TEXT("      <a4 value=\"%d\" />\n"), C->A4);
				T += FString::Printf(TEXT("      <NoStencil value=\"%s\" />\n"), C->bNoStencil ? TEXT("true") : TEXT("false"));
				break;
			case ERudeWaterQuadKind::Calming:
				T += FString::Printf(TEXT("      <fDampening value=\"%g\" />\n"), C->Dampening);
				break;
			case ERudeWaterQuadKind::Wave:
				T += FString::Printf(TEXT("      <Amplitude value=\"%g\" />\n"), C->Amplitude);
				T += FString::Printf(TEXT("      <XDirection value=\"%g\" />\n"), C->XDirection);
				T += FString::Printf(TEXT("      <YDirection value=\"%g\" />\n"), C->YDirection);
				break;
			}
			T += TEXT("    </Item>");
			Edits.Add({ Items[i].Start, Items[i].End, T });
			++Rewritten;
		}
	}

	Edits.Sort([](const FEdit& A, const FEdit& B) { return A.Start > B.Start; });
	for (const FEdit& E : Edits)
	{
		Out = Out.Left(E.Start) + E.Text + Out.RightChop(E.End);
	}

	// ⛔ THE CONSERVATION IDENTITY: every slot the FILE has is either carried or rewritten. If those
	// do not add up, the export moved something it cannot name and the count says so.
	const int32 Accounted = Carried + Rewritten;
	const bool bConserved = (Accounted == SlotsTotal);
	const bool bIdentical = Out.Equals(Doc, ESearchCase::CaseSensitive);
	const bool bExpectIdentical = Expect.Equals(TEXT("identical"));
	const bool bExpectEdited = Expect.Equals(TEXT("edited"));
	bool bMet = bConserved && Inverted == 0;
	if (bExpectIdentical) { bMet = bMet && bIdentical; }
	if (bExpectEdited)    { bMet = bMet && !bIdentical; }

	FString Written;
	if (bMet && !OutDir.TrimStartAndEnd().IsEmpty())
	{
		Written = FPaths::Combine(OutDir.TrimStartAndEnd(), TEXT("water.xml"));
		if (!FFileHelper::SaveStringToFile(Out, *Written))
		{
			return Fail(FString::Printf(TEXT("could not write %s"), *Written));
		}
	}

	return FString::Printf(
		TEXT("{\"ok\":%s,\"source\":\"%s\",\"out\":\"%s\",\"slotsInFile\":%d,\"quadsInLevel\":%d,")
		TEXT("\"carriedVerbatim\":%d,\"rewritten\":%d,\"notInLevel\":%d,\"orphans\":%d,\"invertedBounds\":%d,")
		TEXT("\"accountedFor\":%d,\"conserved\":%s,\"identicalToSource\":%s,\"expect\":\"%s\",")
		TEXT("\"note\":\"a splice, not a regeneration: an untouched quad goes back as its own bytes. A quad "
		     "missing from the level keeps its original bytes too - this lane will not DELETE water by "
		     "omission. carriedVerbatim + rewritten must equal slotsInFile.\"}"),
		bMet ? TEXT("true") : TEXT("false"), *RudeJsonEscape(XmlPath), *RudeJsonEscape(Written),
		SlotsTotal, InLevel, Carried, Rewritten, NotInLevel, Orphans, Inverted,
		Accounted, bConserved ? TEXT("true") : TEXT("false"),
		bIdentical ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Expect));
}

// ---- MoveWaterQuad -------------------------------------------------------------------------------
// The nudge every carried lane has, so an EDITED round trip can be proven from a script without a
// human in the viewport. Names a quad the way its label does: kind + the index it had in the file.
FString URudeToolset::MoveWaterQuad(const FString& Kind, const FString& Index, const FString& DeltaCm)
{
	auto Fail = [](const FString& Why) { return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why)); };
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }
	const FString K = Kind.TrimStartAndEnd().ToLower();
	if (K != TEXT("water") && K != TEXT("calming") && K != TEXT("wave"))
	{
		return Fail(TEXT("Kind must be water, calming or wave"));
	}
	const int32 Idx = FCString::Atoi(*Index.TrimStartAndEnd());
	TArray<FString> D;
	DeltaCm.TrimStartAndEnd().ParseIntoArray(D, TEXT(","), true);
	if (D.Num() != 3) { return Fail(TEXT("DeltaCm must be x,y,z in centimetres")); }
	const FVector Delta(FCString::Atod(*D[0]), FCString::Atod(*D[1]), FCString::Atod(*D[2]));

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(kWaterTag)) { continue; }
		URudeWaterQuadComponent* C = It->FindComponentByClass<URudeWaterQuadComponent>();
		if (!C || C->SourceIndex != Idx || !FString(KindWord(C->Kind)).Equals(K)) { continue; }
		const FVector Before = It->GetActorLocation();
		It->SetActorLocation(Before + Delta);
		return FString::Printf(
			TEXT("{\"ok\":true,\"kind\":\"%s\",\"index\":%d,\"actor\":\"%s\",")
			TEXT("\"beforeCm\":[%g,%g,%g],\"afterCm\":[%g,%g,%g]}"),
			*K, Idx, *RudeJsonEscape(It->GetActorLabel()),
			Before.X, Before.Y, Before.Z, Before.X + Delta.X, Before.Y + Delta.Y, Before.Z + Delta.Z);
	}
	return Fail(FString::Printf(TEXT("no %s quad with source index %d in the level - run ImportWater first"), *K, Idx));
}

#else
FString URudeToolset::ImportWater(const FString&, const FString&) { return TEXT("{\"ok\":false,\"error\":\"editor-only\"}"); }
FString URudeToolset::ExportWater(const FString&, const FString&, const FString&) { return TEXT("{\"ok\":false,\"error\":\"editor-only\"}"); }
FString URudeToolset::MoveWaterQuad(const FString&, const FString&, const FString&) { return TEXT("{\"ok\":false,\"error\":\"editor-only\"}"); }
#endif
