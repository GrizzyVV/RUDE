// RUDE - RAGE <-> Unreal Development Environment
// Map / area lane: the corpus filebase (RudeFilebase), the corpus archetype index, ImportMapArea /
// ImportArea / ImportMlo, ExportYtyp / ExportYmap, the batch importers, SaveAssets, FixLevelRefs,
// SetWorldHour and ImportScene (manifest -> actors/ISM). Split out of RudeToolset.cpp 2026-09-06.
#include "RudeToolset.h"
#include "RudeToolsetInternal.h"
#include "Interfaces/IPluginManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetCompilingManager.h"
#include "FileHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "Interfaces/Interface_CollisionDataProvider.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Framework/Application/SlateApplication.h"
#include "XmlFile.h"
#include "Misc/EngineVersion.h"
#include "RudeCorpus.h"
#include "RudeDds.h"
#include "RudeEntityComponent.h"
#include "RudeMloEntityComponent.h"
#include "RudeArchetype.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "ContentStreaming.h"
#include "Containers/Ticker.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/LevelStreaming.h"
#include "ShaderCompiler.h"
#include "MaterialShared.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "RudeToolsetInternal.h"

// ---- RUDE FILEBASE ---------------------------------------------------------------
// The user exports their own game files into this tree. Its job is to keep
// BUILD-VERSION-ACCURATE assets separable: the same name (prop_x.ydr) legitimately
// exists in the base game, in update.rpf, and in several DLC packs, and the LAST one
// in load order is the one the game actually uses. Numeric prefixes make that order
// explicit on disk, so a resolver just walks folders high-to-low.
namespace RudeFilebase
{
	// Types RUDE consumes directly, then context types worth keeping alongside.
	static const TCHAR* CORE_TYPES[] = { TEXT("ydr"), TEXT("ydd"), TEXT("ytd"), TEXT("ybn"),
	                                     TEXT("ytyp"), TEXT("ymap") };
	static const TCHAR* ALL_TYPES[] = { TEXT("ydr"), TEXT("ydd"), TEXT("ytd"), TEXT("ybn"),
	                                    TEXT("ytyp"), TEXT("ymap"), TEXT("yft"), TEXT("ycd"),
	                                    TEXT("ynv"), TEXT("ynd"), TEXT("yed"), TEXT("ymt"),
	                                    TEXT("ymf"), TEXT("ypt"), TEXT("yld"), TEXT("awc"),
	                                    TEXT("rel"), TEXT("meta"), TEXT("gxt2"), TEXT("xml") };

	static int32 MakeTypeFolders(const FString& Base, bool bAll)
	{
		int32 n = 0;
		const TCHAR* const* Types = bAll ? ALL_TYPES : CORE_TYPES;
		const int32 Count = bAll ? UE_ARRAY_COUNT(ALL_TYPES) : UE_ARRAY_COUNT(CORE_TYPES);
		for (int32 i = 0; i < Count; ++i)
		{
			if (IFileManager::Get().MakeDirectory(*(Base / Types[i]), true)) { ++n; }
		}
		return n;
	}
}

FString URudeToolset::IngestExport(const FString& DumpFolder, const FString& SourceName,
                                   const FString& FilebaseRoot, const FString& Move)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	IFileManager& FM = IFileManager::Get();
	if (!FPaths::DirectoryExists(DumpFolder)) { return Fail(TEXT("DumpFolder does not exist")); }
	if (!FPaths::DirectoryExists(FilebaseRoot)) { return Fail(TEXT("FilebaseRoot does not exist - run CreateFilebase first")); }
	const bool bMove = !Move.TrimStartAndEnd().Equals(TEXT("COPY"), ESearchCase::IgnoreCase);

	FString Src = SourceName.TrimStartAndEnd();
	if (Src.IsEmpty())
	{
		FString Trimmed = DumpFolder;
		while (Trimmed.EndsWith(TEXT("/")) || Trimmed.EndsWith(TEXT("\\")))
		{
			Trimmed.LeftChopInline(1);
		}
		Src = FPaths::GetCleanFilename(Trimmed);
	}
	const FString SrcLower = Src.ToLower();

	// resolve the destination slot: base / update / a numbered DLC folder
	FString Dest;
	if (SrcLower == TEXT("base") || SrcLower.StartsWith(TEXT("x64")) || SrcLower == TEXT("common"))
	{
		Dest = FilebaseRoot / TEXT("00_base");
	}
	else if (SrcLower.StartsWith(TEXT("update")))
	{
		Dest = FilebaseRoot / TEXT("10_update");
	}
	else
	{
		TArray<FString> DlcDirs;
		FM.FindFiles(DlcDirs, *(FilebaseRoot / TEXT("20_dlc") / TEXT("*")), false, true);
		for (const FString& D : DlcDirs)
		{
			// folders are NNN_<name>; match on the name half so callers never type numbers
			FString Name = FPaths::GetCleanFilename(D);
			int32 us;
			if (Name.FindChar(TEXT('_'), us)) { Name = Name.Mid(us + 1); }
			if (Name.Equals(SrcLower, ESearchCase::IgnoreCase))
			{
				Dest = FilebaseRoot / TEXT("20_dlc") / FPaths::GetCleanFilename(D);
				break;
			}
		}
		if (Dest.IsEmpty())
		{
			return Fail(FString::Printf(
				TEXT("unknown source '%s' - use base, update, or a DLC pack name from _FILEBASE.json"), *Src));
		}
	}

	// every file, recursively; type = the extension before any .xml
	TArray<FString> Files;
	FM.FindFilesRecursive(Files, *DumpFolder, TEXT("*.*"), true, false);
	TMap<FString, int32> ByType;
	int32 Filed = 0, Skipped = 0;
	for (const FString& F : Files)
	{
		FString Name = FPaths::GetCleanFilename(F);
		FString Type = FPaths::GetExtension(Name).ToLower();
		if (Type == TEXT("xml"))
		{
			// prop_x.ydr.xml -> ydr
			FString Base = FPaths::GetBaseFilename(Name);
			Type = FPaths::GetExtension(Base).ToLower();
		}
		if (Type.IsEmpty() || Type == TEXT("rpf")) { ++Skipped; continue; }
		const FString TypeDir = Dest / Type;
		FM.MakeDirectory(*TypeDir, true);
		const FString Target = TypeDir / Name;
		bool bOk = bMove ? FM.Move(*Target, *F, true) : (FM.Copy(*Target, *F, true) == COPY_OK);
		if (bOk) { ++Filed; ByType.FindOrAdd(Type)++; }
		else { ++Skipped; }
		if (Filed && Filed % 2000 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] IngestExport %d filed..."), Filed);
		}
	}
	FString TypeJson;
	for (const TPair<FString, int32>& P : ByType)
	{
		TypeJson += FString::Printf(TEXT("%s\"%s\":%d"), TypeJson.IsEmpty() ? TEXT("") : TEXT(","),
			*P.Key, P.Value);
	}
	return FString::Printf(TEXT(
		"{\"ok\":true,\"source\":\"%s\",\"dest\":\"%s\",\"filed\":%d,\"byType\":{%s},\"skipped\":%d}"),
		*Src, *FPaths::GetCleanFilename(Dest), Filed, *TypeJson, Skipped);
}

FString URudeToolset::CreateFilebase(const FString& FilebaseRoot, const FString& GameRoot,
                                     const FString& Options)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	IFileManager& FM = IFileManager::Get();
	if (!FPaths::DirectoryExists(GameRoot)) { return Fail(TEXT("GameRoot does not exist")); }
	const bool bAll = Options.TrimStartAndEnd().Equals(TEXT("ALL"), ESearchCase::IgnoreCase);

	// --- read the user's install: base archives + DLC pack NAMES (directory listing
	// only; nothing is opened, decrypted, copied or redistributed) ---
	TArray<FString> BaseArchives;
	FM.FindFiles(BaseArchives, *(GameRoot / TEXT("*.rpf")), true, false);
	BaseArchives.Sort();

	TArray<FString> DlcDirs;
	const FString DlcRoot = GameRoot / TEXT("update/x64/dlcpacks");
	FM.FindFiles(DlcDirs, *(DlcRoot / TEXT("*")), false, true);
	// Heuristic order: year-bearing pack names are newer, so they sort last; otherwise
	// alphabetical. NOT authoritative - the real order lives in dlclist.xml inside the
	// encrypted update.rpf. The manifest says so, and _manifest/ has a slot for it.
	DlcDirs.Sort([](const FString& A, const FString& B)
	{
		auto YearOf = [](const FString& S) -> int32
		{
			for (int32 Y = 2013; Y <= 2035; ++Y)
			{
				if (S.Contains(FString::FromInt(Y))) { return Y; }
			}
			return 0;
		};
		const int32 YA = YearOf(A), YB = YearOf(B);
		if (YA != YB) { return YA < YB; }
		return A < B;
	});

	// --- build the tree ---
	int32 Folders = 0;
	auto Mk = [&](const FString& P) { if (FM.MakeDirectory(*P, true)) { ++Folders; } };
	Mk(FilebaseRoot);
	Mk(FilebaseRoot / TEXT("_manifest"));
	Mk(FilebaseRoot / TEXT("_incoming"));
	const FString BaseDir = FilebaseRoot / TEXT("00_base");
	const FString UpdDir = FilebaseRoot / TEXT("10_update");
	Mk(BaseDir); Mk(UpdDir);
	// Type folders are created ON DEMAND by IngestExport - pre-seeding hundreds of empty
	// ones only made the tree look like work the user has to do. "ALL" restores them.
	if (bAll)
	{
		Folders += RudeFilebase::MakeTypeFolders(BaseDir, true);
		Folders += RudeFilebase::MakeTypeFolders(UpdDir, true);
	}
	const FString DlcOut = FilebaseRoot / TEXT("20_dlc");
	Mk(DlcOut);
	FString DlcJson;
	for (int32 i = 0; i < DlcDirs.Num(); ++i)
	{
		const FString Name = FPaths::GetCleanFilename(DlcDirs[i]);
		const FString Dir = DlcOut / FString::Printf(TEXT("%03d_%s"), i + 1, *Name);
		Mk(Dir);
		if (bAll) { Folders += RudeFilebase::MakeTypeFolders(Dir, false); }
		DlcJson += FString::Printf(TEXT("%s\n  {\"order\": %d, \"name\": \"%s\", \"folder\": \"%s\"}"),
			i ? TEXT(",") : TEXT(""), i + 1, *Name, *FPaths::GetCleanFilename(Dir));
	}

	// --- build fingerprint: identifies WHICH game build this filebase was cut for ---
	FString ExeName = TEXT("GTA5.exe");
	int64 ExeSize = FM.FileSize(*(GameRoot / ExeName));
	if (ExeSize <= 0) { ExeName = TEXT("GTA5_Enhanced.exe"); ExeSize = FM.FileSize(*(GameRoot / ExeName)); }
	const FDateTime ExeStamp = FM.GetTimeStamp(*(GameRoot / ExeName));

	FString BaseJson;
	for (int32 i = 0; i < BaseArchives.Num(); ++i)
	{
		BaseJson += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(", ") : TEXT(""), *BaseArchives[i]);
	}
	// ⭐ SAME SHAPE AS ROUT'S _FILEBASE.json, deliberately (Matt's call, 2026-07-27): the extractor
	// owns the project tree, and this is the hand-assembled fallback. Two tools writing the same
	// contract in two shapes is how a contract drifts - which already cost us once when the exporter
	// emitted binary and the importer read XML with nothing to announce the mismatch. Keys mirror
	// ROUT's manifest (`routVersion`, the key FRudeCorpus reads); `createdBy` is the only addition.
	const FString Manifest = FString::Printf(TEXT(
		"{\n"
		" \"routVersion\": 1,\n"
		" \"createdBy\": \"RUDE CreateFilebase\",\n"
		" \"title\": \"%s\",\n"
		" \"gameRoot\": \"%s\",\n"
		" \"build\": { \"exe\": \"%s\", \"bytes\": %lld, \"modified\": \"%s\" },\n"
		" \"precedence\": [\"00_base\", \"10_update\", \"20_dlc/<order>_<name>\"],\n"
		" \"precedenceNote\": \"Later wins. A name in several sources resolves to the "
		"highest-ordered copy - that is what keeps a project build-accurate.\",\n"
		" \"dlcOrderAuthoritative\": false,\n"
		" \"dlcOrderNote\": \"HEURISTIC (year-bearing names last, else alphabetical). RUDE cannot "
		"open update.rpf to read the real dlclist.xml - it ships no archive or crypto code by "
		"design. Run ROUT's export for an authoritative order; do not author a DLC "
		"override against this one.\",\n"
		" \"baseArchives\": [%s],\n"
		" \"dlcPacks\": [%s\n ]\n}\n"),
		ExeName.Equals(TEXT("GTA5_Enhanced.exe")) ? TEXT("gtav-enhanced") : TEXT("gtav-legacy"),
		*GameRoot.ReplaceCharWithEscapedChar(), *ExeName, ExeSize, *ExeStamp.ToString(),
		*BaseJson, *DlcJson);
	FFileHelper::SaveStringToFile(Manifest, *(FilebaseRoot / TEXT("_FILEBASE.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	const FString Readme = FString::Printf(TEXT(
		"# RUDE Filebase\n\n"
		"This is an EMPTY folder tree, shaped to your own game install. Nothing here was read out\n"
		"of your game - RUDE only listed directory names. Filling it is a separate step.\n\n"
		"## The easy path: ROUT\n\n"
		"ROUT (the maintainer's public extractor, github.com/GrizzyVV/ROUT---RAGE-Exporter-App) reads\n"
		"your own archives and writes this tree for you, in the right order, with the two ledgers RUDE\n"
		"reads (_FILEBASE.json + _PROVENANCE.jsonl). See ROUT's README for the export command.\n\n"
		"It also reads the real DLC load order out of your update.rpf, which RUDE cannot - RUDE\n"
		"ships no archive or crypto code by design, so the order below is only a guess.\n\n"
		"## The other path: an extractor you already have\n\n"
		"Export ONE source at a time (e.g. `x64a.rpf`, or `update.rpf`, or a single DLC) with\n"
		"whatever extraction tool you already use. Dump it anywhere - a flat folder is fine, do\n"
		"NOT sort it. Then tell RUDE to file it:\n\n"
		"    IngestExport(DumpFolder, SourceName, FilebaseRoot)\n"
		"      SourceName = \"base\", \"update\", or the DLC pack name (e.g. \"mpbiker\")\n\n"
		"RUDE sorts every file by type into the correct precedence slot. You never create a\n"
		"folder, never type a number, never sort anything by hand.\n\n"
		"Repeat per source. Start with `base` and `update` - that is the city; DLC packs only\n"
		"matter when you want their content.\n\n"
		"## What the numbers mean (you can ignore them)\n"
		"The same asset name exists in the base game, in update.rpf, and in several DLC packs;\n"
		"the game uses the LAST one in load order. The folders encode that order so RUDE always\n"
		"resolves the build-accurate copy:\n\n"
		"    00_base/             the base x64*.rpf / common.rpf archives\n"
		"    10_update/           update.rpf - overrides base\n"
		"    20_dlc/NNN_<name>/   DLC packs, higher NNN wins\n\n"
		"Type folders (`ydr/ ytd/ ybn/ ...`) are created for you as files arrive.\n"
		"Keep sources in their own slots - not merging them is what makes this work.\n\n"
		"## Slots\n"
		"    _manifest/    scratch space for anything that pins this build\n"
		"    _incoming/    somewhere to dump before ingesting, if you want it\n\n"
		"WARNING: the DLC order below is a GUESS (year-bearing names last, else alphabetical).\n"
		"The real order lives in dlclist.xml inside the encrypted update.rpf. If you intend to\n"
		"author an override that must land above a particular DLC, use ROUT - guessing wrong\n"
		"means your override loses silently.\n\n"
		"## This filebase was cut for\n"
		"    %s  (%s, %lld bytes, modified %s)\n"
		"    %d base archives, %d DLC packs\n\n"
		"Re-run CreateFilebase after a game patch: the build fingerprint in _FILEBASE.json is how\n"
		"a mismatch gets caught before it corrupts a project.\n"),
		*GameRoot, *ExeName, ExeSize, *ExeStamp.ToString(), BaseArchives.Num(), DlcDirs.Num());
	FFileHelper::SaveStringToFile(Readme, *(FilebaseRoot / TEXT("README.md")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	return FString::Printf(TEXT(
		"{\"ok\":true,\"root\":\"%s\",\"dlcPacks\":%d,\"baseArchives\":%d,\"foldersCreated\":%d,"
		"\"typeMode\":\"%s\"}"),
		*FilebaseRoot, DlcDirs.Num(), BaseArchives.Num(), Folders, bAll ? TEXT("ALL") : TEXT("CORE"));
}

// ---- shared corpus resolution (ImportMapArea + ImportMlo) --------------------------------
// The archetype index and the per-drawable ydr/yft/ydd import lane started life inside
// ImportMapArea (in-game-proven: downtown 13,135 instances, LOG "FRAGMENT LANE"). ImportMlo
// needs the SAME resolution for an MLO's interior entities - factored out rather than
// copied, so the two tools cannot drift. Behavior-preserving extraction, not a rewrite.
// ---- gtxd: RAGE's PARENT-TEXTURE-DICTIONARY chain ---------------------------------------
// ⭐ 2026-08-05 (#43 tier 3). When an asset's own txd does not hold a texture, the engine walks
// the parent chain declared in CMapParentTxds. That table ships as `gtxd.ymt` - an RBF0 binary,
// which the exporter's ymt stage once refused (35 RBF0 refusals on record), so the plugin reads it directly.
// ⛔ THE FORMAT WAS DERIVED FROM THE BYTES, not from any third-party implementation, and the
// derivation is checkable: both files in the corpus parse to EXACTLY their own length with a
// balanced element stack (gtxd.ymt 210,467 bytes -> 2,983 relationships; mph4_gtxd.ymt 3,848 ->
// 45), every `item` carries both a <parent> and a <child>, and every child has exactly one
// parent (2,983 distinct children of 2,983 items - a tree, not a graph).
//   header : "RBF0"
//   token  : uint16 id.  0xFFFF closes the current element.  0xFFFD introduces a VALUE:
//            uint32 byte-length then that many bytes, NUL-terminated ASCII.
//            anything else OPENS an element with that id, followed by uint16 nameLength; when
//            nameLength > 0 the name follows and DEFINES id (plus two pad bytes), when it is 0
//            the id was defined earlier. Four more bytes follow in both cases.
// ⛔ REFUSE, DO NOT GUESS: any deviation (short read, undefined id, unbalanced stack, non-zero
// pad/tail) abandons THAT FILE and is counted. A desynchronised parse would emit plausible-looking
// parent relationships, and a wrong parent is a wrong texture - strictly worse than no chain.
static bool RudeReadParentTxdFile(const TArray<uint8>& B, TMap<FString, FString>& Out, int32& Added)
{
	if (B.Num() < 8 || B[0] != 'R' || B[1] != 'B' || B[2] != 'F' || B[3] != '0') { return false; }
	auto U16 = [&B](int32 P) { return (uint16)(B[P] | ((uint16)B[P + 1] << 8)); };
	auto U32 = [&B](int32 P)
	{
		return (uint32)B[P] | ((uint32)B[P + 1] << 8) | ((uint32)B[P + 2] << 16) | ((uint32)B[P + 3] << 24);
	};
	TMap<uint16, FString> Names;
	TArray<FString> Stack;
	FString Parent, Child, Pending;
	int32 P = 4;
	while (P < B.Num())
	{
		if (P + 2 > B.Num()) { return false; }
		const uint16 Tag = U16(P); P += 2;
		if (Tag == 0xFFFF)
		{
			if (Stack.Num() == 0) { return false; }
			const FString Elem = Stack.Pop();
			if (Elem == TEXT("parent")) { Parent = Pending; }
			else if (Elem == TEXT("child")) { Child = Pending; }
			else if (Elem == TEXT("item"))
			{
				if (Parent.IsEmpty() || Child.IsEmpty()) { return false; }
				const FString C = Child.ToLower();
				if (!Out.Contains(C)) { Out.Add(C, Parent.ToLower()); ++Added; }
				Parent.Empty(); Child.Empty();
			}
			Pending.Empty();
			continue;
		}
		if (Tag == 0xFFFD)
		{
			if (P + 4 > B.Num()) { return false; }
			const int32 Len = (int32)U32(P); P += 4;
			if (Len < 0 || P + Len > B.Num()) { return false; }
			FString S;
			for (int32 i = 0; i < Len; ++i)
			{
				const uint8 C = B[P + i];
				if (C == 0) { break; }
				if (C > 0x7F) { return false; }   // non-ASCII in a dictionary name = not this shape
				S.AppendChar((TCHAR)C);
			}
			P += Len;
			Pending = S;
			continue;
		}
		if (P + 2 > B.Num()) { return false; }
		const uint16 NameLen = U16(P); P += 2;
		if (NameLen > 0)
		{
			if (P + NameLen + 2 > B.Num()) { return false; }
			FString Nm;
			for (int32 i = 0; i < NameLen; ++i)
			{
				const uint8 C = B[P + i];
				if (C == 0 || C > 0x7F) { return false; }
				Nm.AppendChar((TCHAR)C);
			}
			P += NameLen;
			if (B[P] != 0 || B[P + 1] != 0) { return false; }   // pad must be zero, or we are desynced
			P += 2;
			Names.Add(Tag, Nm);
		}
		if (P + 4 > B.Num()) { return false; }
		if (B[P] != 0 || B[P + 1] != 0 || B[P + 2] != 0 || B[P + 3] != 0) { return false; }
		P += 4;
		const FString* Known = Names.Find(Tag);
		if (!Known) { return false; }            // an id used before it was ever defined
		Stack.Add(*Known);
		Pending.Empty();
	}
	return Stack.Num() == 0;
}

// Read every CMapParentTxds table under <CorpusRoot>/ymt. Selection is by CONTENT (RBF0 header
// naming CMapParentTxds), never by filename - the corpus holds gtxd.ymt and mph4_gtxd.ymt today
// and a census is a lower bound, so a DLC whose table is named differently must still be found.
static void RudeReadParentTxds(const FRudeCorpus& Corpus, TMap<FString, FString>& Out,
                               int32& Files, int32& Relationships, int32& Refused)
{
	// The corpus converts CMapParentTxds (gtxd.ymt, RBF0) to "<name>.ymt.rbf.xml". Every copy across
	// slots is read lowest-slot first so a DLC's table overrides the base's for the same child -
	// the game's own load order. A copy still kept binary goes through the RBF0 reader.
	TArray<const FRudeCorpusEntry*> Rows;
	Corpus.AllOfType(TEXT("ymt"), Rows);
	for (const FRudeCorpusEntry* E : Rows)
	{
		if (E->Name != TEXT("gtxd")) { continue; }
		const FString Path = Corpus.PathOf(*E);
		if (E->bConverted)
		{
			FXmlFile Xml(Path);
			if (!Xml.IsValid()) { ++Refused; continue; }
			const FXmlNode* Root = Xml.GetRootNode();
			const FXmlNode* Rel = Root ? Root->FindChildNode(TEXT("txdRelationships")) : nullptr;
			if (!Rel) { ++Refused; continue; }
			++Files;
			for (const FXmlNode* Item : Rel->GetChildrenNodes())
			{
				const FXmlNode* P = Item->FindChildNode(TEXT("parent"));
				const FXmlNode* C = Item->FindChildNode(TEXT("child"));
				if (!P || !C) { continue; }
				const FString Child = C->GetContent().TrimStartAndEnd().ToLower();
				const FString Parent = P->GetContent().TrimStartAndEnd().ToLower();
				if (Child.IsEmpty() || Parent.IsEmpty()) { continue; }
				Out.Add(Child, Parent);
				++Relationships;
			}
			continue;
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) { ++Refused; continue; }
		if (Bytes.Num() < 32 || Bytes[0] != 'R' || Bytes[1] != 'B' || Bytes[2] != 'F' || Bytes[3] != '0') { continue; }
		++Files;
		int32 Added = 0;
		if (RudeReadParentTxdFile(Bytes, Out, Added)) { Relationships += Added; }
		else { ++Refused; }
	}
}

// ---- _RESOLVED.json: which build SLOT each file was won from ------------------------------
// ⭐ 2026-08-05 (#43 tier 5). `winners` is a FLAT object of "<type>/<name>.<ext>": "<slot>", so
// this reads it with a small purpose-built string scanner rather than a general JSON parse: the
// file is ~15 MB / 262,124 entries and only four of its eight lanes are wanted here. Only the
// two escapes JSON can legally put in these strings are handled (\\ and \"), and anything else
// aborts the read rather than inventing a value.
static void RudeReadCorpusSlots(const FRudeCorpus& Corpus, TMap<FString, FString>& DictSlot,
                                TMap<FString, FString>& AssetSlot, int32& Entries)
{
	// Tier 5 of the texture scope: which build slot an asset resolves from. The ledger carries
	// every copy; Effective() is the copy the game loads. (v1 read an agent-built _RESOLVED.json
	// for this; that file was a derived view Matt killed - the ledger is the source.)
	static const TCHAR* DictTypes[] = { TEXT("ytd") };
	static const TCHAR* AssetTypes[] = { TEXT("ydr"), TEXT("yft"), TEXT("ydd") };
	TArray<const FRudeCorpusEntry*> Rows;
	for (const TCHAR* T : DictTypes)
	{
		Rows.Reset();
		Corpus.ByPrefix(T, TEXT(""), Rows);
		for (const FRudeCorpusEntry* E : Rows) { DictSlot.Add(E->Name, E->Slot); ++Entries; }
	}
	for (const TCHAR* T : AssetTypes)
	{
		Rows.Reset();
		Corpus.ByPrefix(T, TEXT(""), Rows);
		for (const FRudeCorpusEntry* E : Rows)
		{
			AssetSlot.Add(E->Name, E->Slot);
			DictSlot.Add(E->Name + TEXT("__embedded"), E->Slot);
			++Entries;
		}
	}
}

struct FRudeArchetypeIndex
{
	TMap<FString, FString> ArchToAsset;   // lowercase archetype name -> lowercase drawable asset
	TSet<FString> FragmentAssets;         // assets that resolve under yft/ instead of ydr/
	TMap<FString, FString> DictEntries;   // entry mesh name -> ydd dictionary stem (under ydd/)
	// ⭐ timeFlags per archetype: a 24-bit hour mask on CTimeArchetypeDef (bit N = visible during
	// hour N). 3,936 archetypes carry one; the common masks are night windows (0-5 + 20-23). THIS
	// is how the game shows lit windows after dusk - by swapping which archetype is visible, not by
	// changing a material. Captured so the behaviour can be driven in UE and still round-trip.
	TMap<FString, uint32> ArchTimeFlags;  // lowercase archetype name -> hour mask
	TMap<FString, float> ArchRadius;      // lowercase archetype name -> bsRadius (m), the size the definition claims
	// ⭐ 2026-08-05 (#43): the archetype's declared <textureDictionary>, keyed by the DRAWABLE ASSET
	// the archetype resolves to - which is the key ImportIndexedDrawable and ImportYdrBatch have in
	// hand. This is the ONLY place the shared-txd scope exists: a .ydr.xml declares its EMBEDDED
	// dictionary and nothing else, so without walking the ytyp there is no way for the importer to
	// know which of 8,791 multi-dictionary names it is supposed to want. The walk already parses
	// every archetype once; this is one more FindChildNode on data already in memory.
	TMap<FString, FString> AssetTxd;      // lowercase drawable asset -> lowercase texture dictionary
	// ⭐ 2026-08-05 (#43/#21b): the three scoping signals BEYOND the archetype's own dictionary.
	TMap<FString, FString> ParentTxd;     // gtxd CMapParentTxds: child dictionary -> parent
	TArray<TArray<FString>> YtypTxdSets;  // per ytyp FILE, its declared dictionaries (sorted, unique)
	TMap<FString, int32> AssetYtyp;       // asset -> index into YtypTxdSets (first file, files sorted)
	TMap<FString, FString> DictSlot;      // dictionary -> the build slot it was won from
	TMap<FString, FString> AssetSlot;     // drawable asset -> the build slot it was won from
	int32 GtxdFiles = 0, GtxdRelationships = 0, GtxdRefusals = 0, ResolvedEntries = 0;
	TSharedPtr<FRudeCorpus> Corpus;      // the ledger index every lookup below goes through

	// Assemble everything provable about ONE asset. Nothing is inferred from a path or a filename.
	FRudeTextureScope MakeScope(const FString& AssetLower) const
	{
		FRudeTextureScope S;
		if (const FString* T = AssetTxd.Find(AssetLower)) { S.ArchetypeTxd = *T; }
		// Walk the parent chain nearest-ancestor-first. The corpus tables are a tree (every child
		// has exactly one parent, measured), but a cycle in DLC data would hang the import, so the
		// visited set and the depth cap are load-bearing, not decoration.
		FString Cur = S.ArchetypeTxd;
		TSet<FString> Seen;
		if (!Cur.IsEmpty()) { Seen.Add(Cur); }
		while (!Cur.IsEmpty() && S.ParentTxdChain.Num() < 16)
		{
			const FString* P = ParentTxd.Find(Cur);
			if (!P || Seen.Contains(*P)) { break; }
			S.ParentTxdChain.Add(*P);
			Seen.Add(*P);
			Cur = *P;
		}
		if (const int32* Y = AssetYtyp.Find(AssetLower))
		{
			if (YtypTxdSets.IsValidIndex(*Y)) { S.YtypNeighbours = &YtypTxdSets[*Y]; }
		}
		if (const FString* Sl = AssetSlot.Find(AssetLower)) { S.AssetSlot = *Sl; }
		S.DictSlots = &DictSlot;
		return S;
	}
};

// Optional MLO lookup riding the index walk: the walk already parses every ytyp once, and a
// second whole-corpus scan for one archetype would double the tool's dominant XML cost.
// Matching is hash-tolerant BOTH ways, the ImportYddEntry convention - MLO archetype names
// are hash_XXXXXXXX in the corpus whenever the reverse table lacks them (e.g. the trailer
// interior stores as hash_CB21C865 == joaat("ch3_01_trlr_int")).
struct FRudeMloSearch
{
	FString Wanted;                // caller's spelling
	FString WantHashName;          // "hash_%08X" of joaat(Wanted)
	uint32 WantHash = 0;           // parsed hash when Wanted is itself hash_XXXXXXXX
	bool bWantedIsHashName = false;
	FString FoundFile;             // absolute path of the declaring ytyp XML
	FString FoundName;             // the archetype <name> as the corpus stores it
	int32 MloSeen = 0;             // CMloArchetypeDef items encountered corpus-wide
	FString Sample;                // leading MLO names for the loud not-found error
};

static bool BuildCorpusArchetypeIndex(const FString& CorpusRoot, FRudeArchetypeIndex& Out,
                                      FString& Error, FRudeMloSearch* MloSearch)
{
	// One archetype's texture dictionary, recorded against the asset it resolves to. Two archetypes
	// CAN name the same asset with different dictionaries (a DLC re-texturing a base prop is the
	// common case), so the tie is broken lexicographically rather than by which ytyp the file
	// enumerator happened to reach first - the same determinism law the sorted candidate list in
	// ImportDrawableNode exists for. A machine-dependent scope would be worse than no scope: it
	// would make a material screenshot non-reproducible again.
	auto NoteTxd = [&Out](const FString& Key, const FXmlNode* Item)
	{
		const FXmlNode* TxdN = Item->FindChildNode(TEXT("textureDictionary"));
		if (!TxdN) { return; }
		const FString Txd = TxdN->GetContent().TrimStartAndEnd().ToLower();
		if (Txd.IsEmpty()) { return; }
		if (FString* Existing = Out.AssetTxd.Find(Key))
		{
			if (Txd < *Existing) { *Existing = Txd; }
			return;
		}
		Out.AssetTxd.Add(Key, Txd);
	};
	// ⭐ 2026-08-05 (#43 tier 4): the dictionaries declared inside ONE ytyp file are a real
	// neighbourhood - those archetypes were authored and shipped together. Recorded per file so
	// FindTexture can prefer a sibling's dictionary over an unrelated DLC's. FindFiles answers in
	// filesystem order, which is not a contract, so the list is SORTED: "the first file wins"
	// then means "lexicographically first", the same determinism rule AssetTxd's tie-break exists
	// for. A machine-dependent scope would make a material screenshot non-reproducible again.
	// The corpus index answers "every ytyp, every slot" in load order (base first); later rows
	// overwrite earlier ones below, which yields the game's own effective archetype table.
	{
		FString CorpusErr;
		Out.Corpus = FRudeCorpus::Open(CorpusRoot, CorpusErr);
		if (!Out.Corpus.IsValid()) { Error = CorpusErr; return false; }
	}
	TArray<FString> YtypFiles;
	{
		TArray<const FRudeCorpusEntry*> Rows;
		Out.Corpus->AllOfType(TEXT("ytyp"), Rows);
		for (const FRudeCorpusEntry* E : Rows) { YtypFiles.Add(Out.Corpus->PathOf(*E)); }
	}
	TArray<FString> PendingAssets;   // reused per file
	TSet<FString> PendingTxds;
	for (const FString& F : YtypFiles)
	{
		FXmlFile Xml(F);
		if (!Xml.IsValid()) { continue; }
		const FXmlNode* Root = Xml.GetRootNode();
		const FXmlNode* Arche = Root ? Root->FindChildNode(TEXT("archetypes")) : nullptr;
		if (!Arche) { continue; }
		PendingAssets.Reset();
		PendingTxds.Reset();
		for (const FXmlNode* Item : Arche->GetChildrenNodes())
		{
			// MLO lookup first: MLO archetypes are ASSET_TYPE_ASSETLESS, so the index
			// gate below skips them and their name must be read here.
			if (MloSearch && Item->GetAttribute(TEXT("type")) == TEXT("CMloArchetypeDef"))
			{
				const FXmlNode* MloN = Item->FindChildNode(TEXT("name"));
				const FString MloName = MloN ? MloN->GetContent().TrimStartAndEnd() : FString();
				if (!MloName.IsEmpty())
				{
					++MloSearch->MloSeen;
					if (MloSearch->Sample.Len() < 400)
					{
						MloSearch->Sample += FString::Printf(TEXT("%s%s"),
							MloSearch->Sample.IsEmpty() ? TEXT("") : TEXT(", "), *MloName);
					}
					if (MloSearch->FoundFile.IsEmpty() &&
					    (MloName.Equals(MloSearch->Wanted, ESearchCase::IgnoreCase) ||
					     MloName.Equals(MloSearch->WantHashName, ESearchCase::IgnoreCase) ||
					     (MloSearch->bWantedIsHashName && RudeJoaat(MloName) == MloSearch->WantHash)))
					{
						MloSearch->FoundFile = F;
						MloSearch->FoundName = MloName;
					}
				}
			}
			const FXmlNode* NameN = Item->FindChildNode(TEXT("name"));
			const FXmlNode* AssetN = Item->FindChildNode(TEXT("assetName"));
			const FXmlNode* TypeN = Item->FindChildNode(TEXT("assetType"));
			if (!NameN || !AssetN) { continue; }
			// drawable + fragment + drawable-dictionary archetypes resolve (fragments via
			// the extractor's yft.xml, visual drawable v1; dictionary archetypes via the ydd
			// entry-selection lane). A dictionary archetype's mesh is ONE entry inside
			// <drawableDictionary>'s ydd, and the ARCHETYPE name names that entry
			// (measured corpus-wide 2026-07-28: 72,074/72,074 dict archetypes carry a
			// plain dict name and name==assetName; the join to the entry is hash-to-hash).
			const FString AType = TypeN ? TypeN->GetContent().TrimStartAndEnd() : FString();
			const bool bDrawableArch = AType.IsEmpty() || AType == TEXT("ASSET_TYPE_DRAWABLE");
			const bool bFragmentArch = AType == TEXT("ASSET_TYPE_FRAGMENT");
			const bool bDictArch = AType == TEXT("ASSET_TYPE_DRAWABLEDICTIONARY");
			if (!bDrawableArch && !bFragmentArch && !bDictArch) { continue; }
			const FString ArchLower = NameN->GetContent().TrimStartAndEnd().ToLower();
			// ⭐ CAPTURE timeFlags. Only CTimeArchetypeDef carries it - a 24-bit hour mask where
			// bit N means "visible during hour N". This is the dataset that makes lit windows
			// appear after dusk (the common masks are hours 0-5 + 20-23), and losing it here is
			// what forced an earlier attempt to fake the behaviour in a shader.
			if (const FXmlNode* TimeN = Item->FindChildNode(TEXT("timeFlags")))
			{
				const uint32 Mask = (uint32)FCString::Strtoui64(
					*TimeN->GetAttribute(TEXT("value")), nullptr, 10);
				if (Mask != 0) { Out.ArchTimeFlags.Add(ArchLower, Mask); }
			}
			if (const FXmlNode* RadN = Item->FindChildNode(TEXT("bsRadius")))
			{
				Out.ArchRadius.Add(ArchLower, (float)FCString::Atod(*RadN->GetAttribute(TEXT("value"))));
			}
			if (bDictArch)
			{
				const FXmlNode* DictN = Item->FindChildNode(TEXT("drawableDictionary"));
				const FString Dict = DictN ? DictN->GetContent().TrimStartAndEnd().ToLower() : FString();
				if (Dict.IsEmpty()) { continue; }   // nothing to resolve against -> proxy cube
				// the manifest "drawable" stays the ENTRY (=archetype) name, so ImportScene's
				// name-based mesh lookup works unchanged
				Out.ArchToAsset.Add(ArchLower, ArchLower);
				Out.DictEntries.Add(ArchLower, Dict);
				NoteTxd(ArchLower, Item);   // dictionary lane keys on the ENTRY name
				PendingAssets.Add(ArchLower);
				if (const FXmlNode* TxdN2 = Item->FindChildNode(TEXT("textureDictionary")))
				{
					const FString T2 = TxdN2->GetContent().TrimStartAndEnd().ToLower();
					if (!T2.IsEmpty()) { PendingTxds.Add(T2); }
				}
				continue;
			}
			const FString AssetLower = AssetN->GetContent().TrimStartAndEnd().ToLower();
			Out.ArchToAsset.Add(ArchLower, AssetLower);
			NoteTxd(AssetLower, Item);
			PendingAssets.Add(AssetLower);
			if (const FXmlNode* TxdN2 = Item->FindChildNode(TEXT("textureDictionary")))
			{
				const FString T2 = TxdN2->GetContent().TrimStartAndEnd().ToLower();
				if (!T2.IsEmpty()) { PendingTxds.Add(T2); }
			}
			if (bFragmentArch) { Out.FragmentAssets.Add(AssetLower); }
		}
		// One neighbourhood per ytyp FILE. Empty sets are not stored: a file that declares no
		// dictionary can only produce an empty scope, and an empty scope must never be mistaken
		// for a scope that was consulted and missed.
		if (PendingTxds.Num() > 0 && PendingAssets.Num() > 0)
		{
			TArray<FString> Sorted = PendingTxds.Array();
			Sorted.Sort();
			const int32 SetIdx = Out.YtypTxdSets.Add(MoveTemp(Sorted));
			for (const FString& A : PendingAssets)
			{
				if (!Out.AssetYtyp.Contains(A)) { Out.AssetYtyp.Add(A, SetIdx); }
			}
		}
	}
	if (Out.ArchToAsset.Num() == 0)
	{
		Error = FString::Printf(TEXT("no archetypes indexed - the corpus at %s lists %d ytyp rows"), *CorpusRoot, YtypFiles.Num());
		return false;
	}
	// Tiers 3 and 5 come from outside the ytyp walk and are loaded here so every lane that scopes
	// gets all five signals from one call - two index builders would be two ways to disagree.
	RudeReadParentTxds(*Out.Corpus, Out.ParentTxd, Out.GtxdFiles, Out.GtxdRelationships, Out.GtxdRefusals);
	RudeReadCorpusSlots(*Out.Corpus, Out.DictSlot, Out.AssetSlot, Out.ResolvedEntries);
	// Every scope source reports its own SIZE, because an index that silently carried ZERO of any
	// of them would make the scoping a no-op that still looks wired - the exact shape of a gate
	// that cannot fail. These numbers are what tell the next reader the scope actually arrived,
	// and gtxdRefused is what tells them a table was met and REFUSED rather than quietly ignored.
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] archetype index: %d (fragment %d, dictionary-entry %d, with textureDictionary %d) "
		     "| gtxd parent chain: %d files, %d relationships, %d refused | ytyp neighbourhoods: %d "
		     "sets covering %d assets | slots: %d entries read, %d dictionaries, %d assets"),
		Out.ArchToAsset.Num(), Out.FragmentAssets.Num(), Out.DictEntries.Num(), Out.AssetTxd.Num(),
		Out.GtxdFiles, Out.GtxdRelationships, Out.GtxdRefusals,
		Out.YtypTxdSets.Num(), Out.AssetYtyp.Num(),
		Out.ResolvedEntries, Out.DictSlot.Num(), Out.AssetSlot.Num());
	return true;
}

// ⛔⛔ THE MAP LANE USED TO REDUCE A WHOLE UNIT VERDICT TO ONE BOOLEAN. This function called
// ImportYdr/ImportYddEntry, tested `R.Contains("\"ok\":true")`, and dropped R on the floor -
// so geometriesDropped, boundTextures, missingTextures, unsupportedByMaster, unmappedSamplers and
// every value-param field were discarded on the PRIMARY path. ImportMapArea, ImportArea and
// ImportMlo are the tools an operator and an agent actually run, and this is the ONLY path that
// serves the yft (302,847 entity placements) and ydd (165,552) lanes - there is no per-lane batch
// for them the way ImportYdrBatch serves ydr. A whole-city import that bound zero textures and
// dropped hundreds of geometries printed the same verdict as a perfect one. That is the exact
// failure ImportYdrBatch was fixed for on 2026-07-29, still live one level up.
struct FRudeImportTally
{
	int32 GeosDropped = 0, GeosNoUV = 0, TrisOutOfRange = 0, TrisDegenerate = 0;
	int32 Bound = 0, FromEmbedded = 0, Ambiguous = 0, Unsupported = 0, MissingTex = 0;
	int32 Scoped = 0, TieBroken = 0;   // #43 split: provenance vs lexicographic guess
	// #43/#21b per-tier split, 2026-08-05. A single "scoped" number can be inflated by loosening
	// what counts; five numbers that must still add up to it cannot.
	int32 AmbTotal = 0, FromArchTxd = 0, FromParentTxd = 0, FromYtyp = 0, FromSlot = 0;
	int32 ScopedAuth = 0, ScopedProv = 0;
	int32 TbEmbedded = 0, TbNoScope = 0, TbSlotAmb = 0, TbYtypAmb = 0, TbDictAbsent = 0, TbNotInScope = 0;
	int32 UnmappedSamp = 0, NoShaderDef = 0, NoMaterial = 0;
	int32 ValSeen = 0, ValBound = 0, ValUnsupported = 0, ValDeduped = 0;
	// #40 collision, 2026-08-05: the map/MLO lane must surface these too, or a whole-city import
	// would once again report the geometry it moved and stay silent about the collision it did not.
	int32 ColSeen = 0, ColPrims = 0, ColMeshes = 0, ColUnmapped = 0, ColMalformed = 0;
	int32 ColPolysDropped = 0, ColTris = 0;

	void Accumulate(const FString& R)
	{
		GeosDropped    += RudeSumField(R, TEXT("geometriesDropped"));
		GeosNoUV       += RudeSumField(R, TEXT("geometriesWithoutUV"));
		TrisOutOfRange += RudeSumField(R, TEXT("trianglesOutOfRange"));
		TrisDegenerate += RudeSumField(R, TEXT("trianglesDegenerate"));
		Bound          += RudeSumField(R, TEXT("boundTextures"));
		FromEmbedded   += RudeSumField(R, TEXT("texturesFromEmbedded"));
		Ambiguous      += RudeSumField(R, TEXT("ambiguousTextures"));
		Scoped         += RudeSumField(R, TEXT("texturesResolvedScoped"));
		TieBroken      += RudeSumField(R, TEXT("texturesTieBroken"));
		AmbTotal       += RudeSumField(R, TEXT("texturesAmbiguousTotal"));
		FromArchTxd    += RudeSumField(R, TEXT("texturesFromArchetypeTxd"));
		FromParentTxd  += RudeSumField(R, TEXT("texturesFromParentTxd"));
		FromYtyp       += RudeSumField(R, TEXT("texturesFromYtypNeighbour"));
		FromSlot       += RudeSumField(R, TEXT("texturesFromSameSlot"));
		ScopedAuth     += RudeSumField(R, TEXT("texturesScopedAuthoritative"));
		ScopedProv     += RudeSumField(R, TEXT("texturesScopedProvenance"));
		TbEmbedded     += RudeSumField(R, TEXT("tieBreakEmbeddedNotImported"));
		TbNoScope      += RudeSumField(R, TEXT("tieBreakNoScope"));
		TbSlotAmb      += RudeSumField(R, TEXT("tieBreakSlotAmbiguous"));
		TbYtypAmb      += RudeSumField(R, TEXT("tieBreakYtypAmbiguous"));
		TbDictAbsent   += RudeSumField(R, TEXT("tieBreakScopeDictAbsent"));
		TbNotInScope   += RudeSumField(R, TEXT("tieBreakNameNotInScope"));
		Unsupported    += RudeSumField(R, TEXT("unsupportedByMaster"));
		MissingTex     += RudeSumField(R, TEXT("missingTextures"));
		UnmappedSamp   += RudeSumField(R, TEXT("unmappedSamplers"));
		NoShaderDef    += RudeSumField(R, TEXT("slotsWithoutShaderDef"));
		NoMaterial     += RudeSumField(R, TEXT("slotsWithoutMaterial"));
		ValSeen        += RudeSumField(R, TEXT("valueParamsSeen"));
		ValBound       += RudeSumField(R, TEXT("valueParamsBound"));
		ValUnsupported += RudeSumField(R, TEXT("valueParamsUnsupported"));
		ValDeduped     += RudeSumField(R, TEXT("valueParamsDeduped"));
		ColSeen        += RudeSumField(R, TEXT("collisionBoundsSeen"));
		ColPrims       += RudeSumField(R, TEXT("collisionPrimitivesImported"));
		ColMeshes      += RudeSumField(R, TEXT("collisionMeshesImported"));
		ColUnmapped    += RudeSumField(R, TEXT("collisionBoundsUnmapped"));
		ColMalformed   += RudeSumField(R, TEXT("collisionBoundsMalformed"));
		ColPolysDropped+= RudeSumField(R, TEXT("collisionPolysDropped"));
		ColTris        += RudeSumField(R, TEXT("collisionTriangles"));
	}

	FString ToJson() const
	{
		return FString::Printf(
			TEXT("\"geometriesDropped\":%d,\"geometriesWithoutUV\":%d,\"trianglesOutOfRange\":%d,")
			TEXT("\"trianglesDegenerate\":%d,\"boundTextures\":%d,\"texturesFromEmbedded\":%d,")
			TEXT("\"texturesResolvedScoped\":%d,\"texturesTieBroken\":%d,")
			TEXT("\"ambiguousTextures\":%d,\"texturesAmbiguousTotal\":%d,")
			TEXT("\"texturesFromArchetypeTxd\":%d,\"texturesFromParentTxd\":%d,")
			TEXT("\"texturesFromYtypNeighbour\":%d,\"texturesFromSameSlot\":%d,")
			TEXT("\"texturesScopedAuthoritative\":%d,\"texturesScopedProvenance\":%d,")
			TEXT("\"tieBreakEmbeddedNotImported\":%d,\"tieBreakNoScope\":%d,")
			TEXT("\"tieBreakSlotAmbiguous\":%d,\"tieBreakYtypAmbiguous\":%d,")
			TEXT("\"tieBreakScopeDictAbsent\":%d,\"tieBreakNameNotInScope\":%d,")
			TEXT("\"unsupportedByMaster\":%d,\"missingTextures\":%d,")
			TEXT("\"unmappedSamplers\":%d,\"slotsWithoutShaderDef\":%d,\"slotsWithoutMaterial\":%d,")
			TEXT("\"valueParamsSeen\":%d,\"valueParamsBound\":%d,\"valueParamsUnsupported\":%d,")
			TEXT("\"valueParamsDeduped\":%d,\"collisionBoundsSeen\":%d,")
			TEXT("\"collisionPrimitivesImported\":%d,\"collisionMeshesImported\":%d,")
			TEXT("\"collisionBoundsUnmapped\":%d,\"collisionBoundsMalformed\":%d,")
			TEXT("\"collisionPolysDropped\":%d,\"collisionTriangles\":%d"),
			GeosDropped, GeosNoUV, TrisOutOfRange, TrisDegenerate, Bound, FromEmbedded,
			Scoped, TieBroken, Ambiguous, AmbTotal,
			FromArchTxd, FromParentTxd, FromYtyp, FromSlot, ScopedAuth, ScopedProv,
			TbEmbedded, TbNoScope, TbSlotAmb, TbYtypAmb, TbDictAbsent, TbNotInScope,
			Unsupported, MissingTex, UnmappedSamp, NoShaderDef, NoMaterial,
			ValSeen, ValBound, ValUnsupported, ValDeduped,
			ColSeen, ColPrims, ColMeshes, ColUnmapped, ColMalformed, ColPolysDropped, ColTris);
	}
};

// Resolve ONE indexed drawable to its corpus XML and import it (skip-if-exists) - dictionary
// entries live INSIDE their dict's ydd XML (one file, many drawables); plain drawables and
// fragments stay one-file-per-asset under ydr/ and yft/. The skip check runs on the ENTRY
// mesh name for all three lanes.
static void ImportIndexedDrawable(const FString& CorpusRoot, const FRudeArchetypeIndex& Index,
                                  const FString& Drawable, const FString& DestMeshFolder,
                                  int32& MeshOk, int32& MeshSkip, int32& MeshFail, int32& MeshMissing,
                                  FRudeImportTally& Tally, bool bForce = false)
{
	const FString* Dict = Index.DictEntries.Find(Drawable);
	const bool bFrag = !Dict && Index.FragmentAssets.Contains(Drawable);
	// Located through the ledger: the copy the game loads, whichever slot it sits in.
	const FRudeCorpusEntry* Row = Index.Corpus.IsValid()
		? Index.Corpus->Effective(Dict ? TEXT("ydd") : (bFrag ? TEXT("yft") : TEXT("ydr")), Dict ? *Dict : Drawable)
		: nullptr;
	if (!Row) { ++MeshMissing; return; }
	const FString XmlPath = Index.Corpus->PathOf(*Row);
	if (!FPaths::FileExists(XmlPath)) { ++MeshMissing; return; }
	// ⛔ WHY bForce EXISTS (2026-07-30). This skip is the ONLY gate on the fragment and dictionary
	// lanes, and those lanes are reachable ONLY through ImportArea/ImportMapArea - there is no
	// per-lane batch tool for them the way ImportYdrBatch serves ydr. So after the corpus gained
	// value params and embedded textures, `ImportYdrBatch ... FORCE` refreshed the ydr meshes while
	// every yft and ydd mesh stayed at its pre-fix vintage, and the project became a MIX of two
	// generations that no counter could distinguish. A refresh path is not optional once the corpus
	// can change underneath the project.
	if (!bForce && FPackageName::DoesPackageExist(DestMeshFolder / Drawable))
	{
		++MeshSkip;
		return;
	}
	// ✅ #43/#21b: hand the unit EVERY scoping signal the index can prove for this asset - the
	// archetype's declared <textureDictionary>, its gtxd parent chain, its ytyp neighbourhood and
	// the build slot it was won from. All five arrive together or the two lanes would disagree
	// about which dictionary a mesh belongs to. An asset the index knows nothing about produces an
	// empty scope, which is exactly the old behaviour - and the tieBreak* counters say how often
	// that happens instead of leaving it to be assumed.
	const FRudeTextureScope Scope = Index.MakeScope(Drawable);
	const FString R = Dict ? RudeImportYddEntryScoped(XmlPath, Drawable, DestMeshFolder, &Scope)
	                       : RudeImportYdrScoped(XmlPath, DestMeshFolder, &Scope);
	// Accumulate on BOTH paths: a mesh that came back ok:false because every slot fell to
	// WorldGridMaterial still reports the counters that say WHY, and throwing them away because
	// of the boolean is how this got lost the first time.
	Tally.Accumulate(R);
	if (R.Contains(TEXT("\"ok\":true"))) { ++MeshOk; }
	else
	{
		++MeshFail;
		// A failed mesh is a NAMED failure: the batch sums it, but only the log can say which
		// drawable and why (2026-09-05: 165/3,183 downtown meshes failed and nothing said why).
		UE_LOG(LogTemp, Warning, TEXT("[RUDE] mesh import FAILED '%s' (%s): %s"), *Drawable,
			Dict ? TEXT("ydd") : (bFrag ? TEXT("yft") : TEXT("ydr")), *R.Left(400));
	}
}

FString URudeToolset::ImportMapArea(const FString& CorpusRoot, const FString& YmapPrefix,
                                    const FString& DestMeshFolder, const FString& Filter,
                                    const FString& Mode)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	// ⭐ Mode="FORCE" re-imports meshes that already exist. This is the ONLY refresh path the yft
	// (fragment) and ydd (dictionary) lanes have - they are reachable solely through this tool, with
	// no per-lane batch equivalent to ImportYdrBatch. Without it, a corpus that gains data (value
	// params, embedded textures) can refresh only its ydr meshes, leaving the project a MIX of two
	// vintages that no counter can tell apart (2026-07-30).
	//
	// ⚠ THIS WAS FIRST BUILT AS A "+FORCE" TOKEN ON Filter, on the stated grounds that adding a
	// parameter would break existing RUDE.Run calls. Matt challenged that and it was WRONG:
	// FRudeInvoke::Call binds arguments with `if (Values.IsValidIndex(ValueIdx))` and performs NO
	// arity check, so a missing trailing argument simply stays an empty FString. Old 5-argument calls
	// therefore keep working with Mode empty, which means "not FORCE" - the previous behaviour.
	// A separate Mode is the right shape anyway: it matches ImportYdrBatch, and `Filter` means LOD
	// LEVELS - a mode flag riding in it makes the parameter mean two things.
	// The "+FORCE" spelling is still ACCEPTED, because silently reinterpreting it as an unknown lod
	// filter would turn a deliberate FORCE into a no-op, and a silent no-op is worse than an alias.
	FString LodFilter = Filter;
	bool bForceMeshes = Mode.TrimStartAndEnd().Equals(TEXT("FORCE"), ESearchCase::IgnoreCase);
	{
		TArray<FString> Parts;
		Filter.ParseIntoArray(Parts, TEXT("+"), true);
		LodFilter.Empty();
		for (const FString& Part : Parts)
		{
			const FString T = Part.TrimStartAndEnd();
			if (T.Equals(TEXT("FORCE"), ESearchCase::IgnoreCase)) { bForceMeshes = true; }
			else if (!T.IsEmpty()) { LodFilter = T; }
		}
		if (LodFilter.IsEmpty()) { LodFilter = TEXT("HD"); }
	}
	// ---- 1) archetype index from every ytyp XML (name -> drawable assetName) ----
	// (factored to BuildCorpusArchetypeIndex, shared with ImportMlo - behavior unchanged)
	FRudeArchetypeIndex Index;
	{
		FString IndexErr;
		if (!BuildCorpusArchetypeIndex(CorpusRoot, Index, IndexErr, /*MloSearch*/ nullptr))
		{
			return Fail(IndexErr);
		}
	}
	// ---- 2) parse ymaps -> manifest scenes (IMPORT-lane transforms: pos Y-mirror*100,
	// quat = (x,-y,z,w) - the boardwalk-anchored map, NOT the export involution) ----
	// YmapPrefix accepts a COMMA-SEPARATED list of prefixes ("dt1_,dt_additions") so a named
	// area spanning several families imports as ONE scene (ImportArea builds such lists from the
	// catalog). An exact basename rides along as "<name>.ymap" - the glob "<name>.ymap*.xml"
	// matches only that file. Duplicates are unioned.
	TArray<FString> Prefixes;
	YmapPrefix.ParseIntoArray(Prefixes, TEXT(","), true);
	TSet<FString> SeenYmap;
	TArray<FString> YmapFiles;
	TMap<FString, FString> YmapSlotByPath;   // provenance: which build slot each ymap copy came from
	for (FString P : Prefixes)
	{
		P.TrimStartAndEndInline();
		if (P.IsEmpty()) { continue; }
		// Every ymap whose NAME starts with the prefix, the effective copy of each (a patchday
		// re-issue of dt1_00 wins over the base's), as full paths.
		TArray<const FRudeCorpusEntry*> Found;
		Index.Corpus->ByPrefix(TEXT("ymap"), P, Found);
		for (const FRudeCorpusEntry* E : Found)
		{
			// TSet::Add's out-param is bIsAlreadyInSet - true for DUPLICATES, not new adds
			bool bAlready = false;
			SeenYmap.Add(E->Name, &bAlready);
			if (!bAlready)
			{
				const FString Path = Index.Corpus->PathOf(*E);
				YmapFiles.Add(Path);
				YmapSlotByPath.Add(Path, E->Slot);
			}
		}
	}
	YmapFiles.Sort();
	if (YmapFiles.Num() == 0) { return Fail(TEXT("no ymaps match the prefix")); }
	int32 TotalEnts = 0, Resolved = 0;
	// ⛔ "ymaps" REPORTED THE GLOB MATCH COUNT. YmapFiles.Num() is how many FILENAMES matched the
	// prefix, and it was emitted verbatim as the number of ymaps imported - while THREE separate
	// `continue`s could eliminate a file with no counter at all: an FXmlFile parse failure, a
	// missing <entities> node, and a file that contributed zero entities. MEASURED over 1,500
	// resolved ymap XML: 0 parse failures and 0 missing <entities> today, but 219 files (14.6%)
	// carry an EMPTY <entities> element and so produce no scene while still counting toward
	// "ymaps" - i.e. the number overstates coverage by ~15% on a typical prefix, and a wholesale
	// XML-corruption event would be invisible because the glob would still match.
	// ⚠ YmapsUnreadable is also the only place UE's FXmlFile can disagree with the offline
	// analysis: the corpus join rate was measured with lxml, and FXmlFile is a different,
	// hand-rolled tokenizer. Until this field exists and reads 0 on a full run, that rate is an
	// upper bound.
	int32 YmapsParsed = 0, YmapsUnreadable = 0, YmapsNoEntitiesNode = 0, YmapsWithEntities = 0;
	int32 EntitiesSkipped = 0;   // entity missing <archetypeName> or <position> - was a bare continue
	TSet<FString> NeededDrawables;
	FString ScenesJson;
	for (const FString& F : YmapFiles)
	{
		FXmlFile Xml(F);
		if (!Xml.IsValid()) { ++YmapsUnreadable; continue; }
		++YmapsParsed;
		const FXmlNode* Root = Xml.GetRootNode();
		const FXmlNode* Ents = Root ? Root->FindChildNode(TEXT("entities")) : nullptr;
		if (!Ents) { ++YmapsNoEntitiesNode; continue; }
		FString EntJson;
		int32 SceneEnts = 0;
		// Provenance for every entity: the ymap's asset name, its build slot, and the entity's
		// ordinal in the file's list (parentIndex values refer to ordinals, so it is identity).
		FString SrcYmapName = FPaths::GetBaseFilename(F);
		SrcYmapName.RemoveFromEnd(TEXT(".ymap"));
		const FString* SrcSlotPtr = YmapSlotByPath.Find(F);
		const FString SrcSlot = SrcSlotPtr ? *SrcSlotPtr : FString();
		int32 EntOrdinal = -1;
		for (const FXmlNode* E : Ents->GetChildrenNodes())
		{
			++EntOrdinal;
			const FXmlNode* AN = E->FindChildNode(TEXT("archetypeName"));
			const FXmlNode* Pos = E->FindChildNode(TEXT("position"));
			// Counted, not silent. No index consequence on THIS lane (a ymap entity is not
			// referenced by ordinal the way an MLO's attachedObjects references one), but an
			// entity that vanishes between the file and the manifest must be sayable.
			if (!AN || !Pos) { ++EntitiesSkipped; continue; }
			const FString Arch = AN->GetContent().TrimStartAndEnd().ToLower();
			const double Px = FCString::Atod(*Pos->GetAttribute(TEXT("x")));
			const double Py = FCString::Atod(*Pos->GetAttribute(TEXT("y")));
			const double Pz = FCString::Atod(*Pos->GetAttribute(TEXT("z")));
			double Qx = 0, Qy = 0, Qz = 0, Qw = 1;
			if (const FXmlNode* Rot = E->FindChildNode(TEXT("rotation")))
			{
				Qx = FCString::Atod(*Rot->GetAttribute(TEXT("x")));
				Qy = FCString::Atod(*Rot->GetAttribute(TEXT("y")));
				Qz = FCString::Atod(*Rot->GetAttribute(TEXT("z")));
				Qw = FCString::Atod(*Rot->GetAttribute(TEXT("w")));
			}
			auto Val = [&](const TCHAR* Tag, double Def) -> double
			{
				const FXmlNode* N = E->FindChildNode(Tag);
				return N ? FCString::Atod(*N->GetAttribute(TEXT("value"))) : Def;
			};
			const FXmlNode* LodN = E->FindChildNode(TEXT("lodLevel"));
			const FString Lod = LodN ? LodN->GetContent().TrimStartAndEnd() : FString();
			const FString* Asset = Index.ArchToAsset.Find(Arch);
			++TotalEnts; ++SceneEnts;
			if (Asset) { ++Resolved; NeededDrawables.Add(*Asset); }
			// ⭐ timeFlags travels WITH the entity. It belongs to the archetype, but the spawn works
			// per entity, and carrying it here means the hour mask survives into the manifest that
			// ImportScene re-spawns from - so a respawn keeps the day/night behaviour without
			// re-reading every ytyp. 0 = no mask = always visible.
			const uint32* TFlags = Index.ArchTimeFlags.Find(Arch);
			// Every CEntityDef field rides in the manifest (the ymap spec's 17: 11 T1 + 6 T2), so the
			// per-entity actor's component can be filled without re-reading the ymap, and so the
			// manifest is a faithful record of what the file said. Text fields as spelled; the
			// <extensions> subtree verbatim.
			auto Text = [&](const TCHAR* Tag) -> FString
			{
				const FXmlNode* N = E->FindChildNode(Tag);
				return N ? N->GetContent().TrimStartAndEnd() : FString();
			};
			FString ExtXml;
			if (const FXmlNode* Ext = E->FindChildNode(TEXT("extensions")))
			{
				if (Ext->GetChildrenNodes().Num() > 0) { RudeXmlNodeToString(Ext, ExtXml, 0); }
			}
			FString ItemXml;   // the whole <Item type="CEntityDef"> as spelled - the byte-safe seam
			RudeXmlNodeToString(E, ItemXml, 2);
			EntJson += FString::Printf(TEXT(
				"%s{\"archetype\":\"%s\",\"drawable\":%s,\"lodLevel\":\"%s\","
				"\"ue_location\":[%.10g,%.10g,%.10g],\"ue_quat\":[%.9g,%.9g,%.9g,%.9g],\"scaleXY\":%.9g,"
				"\"scaleZ\":%.9g,\"timeFlags\":%u,"
				"\"flags\":%.0f,\"guid\":%.0f,\"lodDist\":%f,\"childLodDist\":%f,"
				"\"numChildren\":%.0f,\"parentIndex\":%.0f,\"priorityLevel\":\"%s\","
				"\"aoMultiplier\":%f,\"artificialAo\":%f,\"tintValue\":%.0f,"
				"\"extensions\":\"%s\",\"srcYmap\":\"%s\",\"srcSlot\":\"%s\",\"srcIndex\":%d,"
				"\"xml\":\"%s\",\"itemType\":\"%s\",\"bsRadius\":%g}"),
				SceneEnts > 1 ? TEXT(",") : TEXT(""), *Arch,
				Asset ? *FString::Printf(TEXT("\"%s\""), **Asset) : TEXT("null"), *Lod,
				Px * 100.0, -Py * 100.0, Pz * 100.0,
				Qx, -Qy, Qz, Qw,
				Val(TEXT("scaleXY"), 1.0), Val(TEXT("scaleZ"), 1.0),
				TFlags ? *TFlags : 0u,
				Val(TEXT("flags"), 0.0), Val(TEXT("guid"), 0.0), Val(TEXT("lodDist"), 0.0), Val(TEXT("childLodDist"), 0.0),
				Val(TEXT("numChildren"), 0.0), Val(TEXT("parentIndex"), -1.0), *RudeJsonEscape(Text(TEXT("priorityLevel"))),
				Val(TEXT("ambientOcclusionMultiplier"), 255.0), Val(TEXT("artificialAmbientOcclusion"), 255.0), Val(TEXT("tintValue"), 0.0),
				*RudeJsonEscape(ExtXml), *RudeJsonEscape(SrcYmapName), *RudeJsonEscape(SrcSlot), EntOrdinal,
				*RudeJsonEscape(ItemXml), *RudeJsonEscape(E->GetAttribute(TEXT("type"))),
				Index.ArchRadius.Contains(Arch) ? Index.ArchRadius[Arch] : 0.f);
		}
		if (SceneEnts == 0) { continue; }
		++YmapsWithEntities;
		FString YmapName = FPaths::GetBaseFilename(F);
		YmapName.RemoveFromEnd(TEXT(".ymap"));
		// CMapData/flags: bit 0 = script-controlled (mission/variant content the game loads on
		// demand), bit 1 = LOD container - measured over downtown's 158 ymaps 2026-09-05.
		uint32 YmapFlags = 0;
		if (const FXmlNode* FN = Root->FindChildNode(TEXT("flags"))) { YmapFlags = (uint32)FCString::Strtoui64(*FN->GetAttribute(TEXT("value")), nullptr, 10); }
		// CMapData/parent: the ymap whose entities this file's parentIndex values may point into
		// (ENGINEERING_LOG law 24). Empty for a top-level LOD container.
		FString YmapParent;
		if (const FXmlNode* PN = Root->FindChildNode(TEXT("parent"))) { YmapParent = PN->GetContent().TrimStartAndEnd(); }
		ScenesJson += FString::Printf(TEXT("%s{\"ymap\":\"%s\",\"ymapFlags\":%u,\"ymapParent\":\"%s\",\"entities\":[%s]}"),
			ScenesJson.IsEmpty() ? TEXT("") : TEXT(","), *YmapName, YmapFlags, *RudeJsonEscape(YmapParent), *EntJson);
	}
	const FString ManifestPath = FPaths::ProjectSavedDir() / TEXT("RUDE") /
		FString::Printf(TEXT("area_%s_manifest.json"),
			*YmapPrefix.Replace(TEXT("*"), TEXT("")).Replace(TEXT(","), TEXT("+")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManifestPath), true);
	if (!FFileHelper::SaveStringToFile(TEXT("[") + ScenesJson + TEXT("]"), *ManifestPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write area manifest"));
	}
	// ---- 3) import every referenced drawable present in the corpus (skip-if-exists) ----
	// (per-drawable lane selection factored to ImportIndexedDrawable, shared with ImportMlo)
	int32 MeshOk = 0, MeshSkip = 0, MeshFail = 0, MeshMissing = 0, Done = 0, DictNeeded = 0;
	FRudeImportTally Tally;
	for (const FString& D : NeededDrawables)
	{
		++Done;
		if (Index.DictEntries.Contains(D)) { ++DictNeeded; }
		ImportIndexedDrawable(CorpusRoot, Index, D, DestMeshFolder, MeshOk, MeshSkip, MeshFail,
		                      MeshMissing, Tally, bForceMeshes);
		if (Done % 100 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ImportMapArea meshes %d/%d (ok %d, skip %d, fail %d)"),
				Done, NeededDrawables.Num(), MeshOk, MeshSkip, MeshFail);
			// KEEPFLAGS (= RF_Standalone in editor), NEVER RF_NoFlags: the meshes just imported are
			// unsaved and unreferenced until the spawn phase, so a no-keep GC deletes them. RF_NoFlags
			// here swept 1,600 of 1,667 downtown meshes; only imports after the last GC survived.
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ImportMapArea dictionary-entry drawables: %d of %d needed (ok %d, skip %d, fail %d, missing %d overall)"),
		DictNeeded, NeededDrawables.Num(), MeshOk, MeshSkip, MeshFail, MeshMissing);
	// ---- 4) spawn through the proven ImportScene path ----
	// The spawn only understands lod levels - never hand it the FORCE token.
	// ACTORS in Mode = one actor per entity carrying its URudeEntityComponent (the editable
	// scene); otherwise the ISM display path. FORCE never reaches the spawn.
	const bool bSpawnActors = Mode.Contains(TEXT("ACTORS"), ESearchCase::IgnoreCase);
	const FString Spawn = ImportScene(ManifestPath, DestMeshFolder, LodFilter, bSpawnActors ? TEXT("ACTORS") : TEXT(""));
	// ⛔⛔ ok WAS A LITERAL, AND THE SPAWN'S OWN VERDICT WAS NESTED UNDERNEATH IT. This function
	// calls ImportScene and then opens its format string with a hardcoded "ok":true - so
	// "no editor world", the failure that produces an import with nothing placed in it, was
	// reported as a success by the only field the CLI exit code, the RUDE.Run console path and the
	// Slate status line actually read (all three go through FRudeInvoke::ReportedFailure, a
	// substring search for "ok":false). Worse, that substring search scans the WHOLE string, so a
	// failed nested spawn turned the PANEL red while the JSON an agent parses still said
	// ok:true - the two observers disagreed about the same run.
	// THE GATE, EXACTLY: forward the spawn's ok; fail if NO ymap contributed a scene; fail if ANY
	// ymap failed to parse. The last one is deliberately zero-tolerance and it is the cheap answer
	// to an open question - the 99.4% corpus join rate was measured with lxml, while UE's FXmlFile
	// is a different hand-rolled tokenizer whose failures were dropped by an uncounted `continue`.
	// MEASURED: 0 of 1,500 resolved ymaps fail to parse offline, so a non-zero ymapsUnreadable
	// means the two parsers disagree, and that is exactly what should stop a run.
	// meshesFailed and meshesMissingFromCorpus stay NUMBERS - a city always has some of both, and
	// a gate that fires on every run is a gate nobody reads.
	const bool bSpawnOk = !Spawn.Contains(TEXT("\"ok\":false"));
	const bool bAreaOk = bSpawnOk && (YmapsWithEntities > 0) && (YmapsUnreadable == 0);
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"ymapsMatched\":%d,\"ymapsParsed\":%d,\"ymapsUnreadable\":%d,"
		"\"ymapsWithoutEntitiesNode\":%d,\"ymapsWithEntities\":%d,\"ymaps\":%d,"
		"\"entities\":%d,\"entitiesSkipped\":%d,\"resolved\":%d,\"meshesImported\":%d,"
		"\"meshesSkipped\":%d,\"meshesFailed\":%d,\"meshesMissingFromCorpus\":%d,%s,"
		"\"manifest\":\"%s\",\"spawn\":%s}"),
		bAreaOk ? TEXT("true") : TEXT("false"),
		YmapFiles.Num(), YmapsParsed, YmapsUnreadable, YmapsNoEntitiesNode, YmapsWithEntities,
		// "ymaps" is KEPT and now means what its name says - ymaps that contributed a scene.
		// Renaming it outright would silently change every existing caller's reading; the four
		// new fields say where the difference went.
		YmapsWithEntities,
		TotalEnts, EntitiesSkipped, Resolved, MeshOk, MeshSkip, MeshFail, MeshMissing,
		*Tally.ToJson(), *ManifestPath, *Spawn);
}

FString URudeToolset::ImportArea(const FString& AreaName, const FString& CatalogPath,
                                 const FString& CorpusRoot, const FString& DestMeshFolder,
                                 const FString& Filter, const FString& Mode)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	// An empty CatalogPath means the catalog beside the plugin (<plugin>/Catalogs/area_aliases.json).
	// ⛔ RUDE SHIPS NO CATALOG AND NEVER WILL: it is derived from the game's own files, and this
	// repository's promise is that no game data lives in it. So an absent catalog is not a broken
	// tool - it is a tool whose data has not been generated yet, on this machine, from this user's
	// own install. The refusal therefore names the ONE command that makes it: a missing catalog must
	// read as "run this", never as "this is broken".
	FString Catalog = CatalogPath.TrimStartAndEnd();
	bool bBundledPath = false;
	if (Catalog.IsEmpty())
	{
		const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("RUDE"));
		if (Self.IsValid()) { Catalog = Self->GetBaseDir() / TEXT("Catalogs") / TEXT("area_aliases.json"); bBundledPath = true; }
	}
	FString Raw;
	if (Catalog.IsEmpty() || !FFileHelper::LoadFileToString(Raw, *Catalog))
	{
		return Fail(bBundledPath
			? FString::Printf(TEXT("no area catalog at %s - RUDE ships none (it is derived from your own game files): run BuildAreaCatalog once, with your CorpusRoot and an empty OutJsonPath, to write it there - or use ImportMapArea with a ymap prefix"), *Catalog)
			: FString::Printf(TEXT("cannot read the area catalog at %s - run BuildAreaCatalog to write one, give a different CatalogPath, or use ImportMapArea with a ymap prefix"), *Catalog));
	}
	TArray<TSharedPtr<FJsonValue>> Entries;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Entries))
		{
			return Fail(TEXT("area catalog is not a JSON array"));
		}
	}
	// Underscores count as spaces so multi-word names survive console arg splitting
	// (RUDE.Run ImportArea Downtown_Los_Santos ...), and a unique case-insensitive substring
	// is accepted ("downtown") - a human should not have to type a catalog string exactly.
	const FString Want = AreaName.TrimStartAndEnd().Replace(TEXT("_"), TEXT(" "));
	FString Aliases;
	const TSharedPtr<FJsonObject>* Match = nullptr;
	FString MatchAlias;
	int32 Substrings = 0;
	for (const TSharedPtr<FJsonValue>& V : Entries)
	{
		const TSharedPtr<FJsonObject>* E;
		if (!V.IsValid() || !V->TryGetObject(E)) { continue; }
		const FString Alias = (*E)->GetStringField(TEXT("alias"));
		if (Want.IsEmpty())
		{
			// No name given: answer with the menu instead of an error - the panel user's
			// discovery path ("what can I type here?").
			Aliases += FString::Printf(TEXT("%s\"%s\""), Aliases.IsEmpty() ? TEXT("") : TEXT(","), *Alias);
			continue;
		}
		if (Alias.Equals(Want, ESearchCase::IgnoreCase))
		{
			Match = E;
			MatchAlias = Alias;
			Substrings = 1;
			break;
		}
		if (Alias.Contains(Want, ESearchCase::IgnoreCase))
		{
			Match = E;
			MatchAlias = Alias;
			++Substrings;
		}
	}
	if (Match && Substrings == 1)
	{
		const TSharedPtr<FJsonObject>* E = Match;
		TArray<FString> Parts;
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if ((*E)->TryGetArrayField(TEXT("prefixes"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& P : *Arr) { Parts.Add(P->AsString()); }
		}
		// exact basenames ride as "<name>.ymap" prefixes - "<name>.ymap*.xml" matches only
		// that file (see ImportMapArea's comma-list note)
		if ((*E)->TryGetArrayField(TEXT("exact"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& P : *Arr) { Parts.Add(P->AsString() + TEXT(".ymap")); }
		}
		if (Parts.Num() == 0)
		{
			return Fail(TEXT("catalog entry has no prefixes"));
		}
		return ImportMapArea(CorpusRoot, FString::Join(Parts, TEXT(",")), DestMeshFolder, Filter, Mode);
	}
	if (Want.IsEmpty())
	{
		return FString::Printf(TEXT("{\"ok\":true,\"areas\":[%s]}"), *Aliases);
	}
	if (Substrings > 1)
	{
		return Fail(FString::Printf(TEXT("'%s' matches %d areas - be more specific (empty AreaName lists them)"),
			*Want, Substrings));
	}
	return Fail(FString::Printf(TEXT("unknown area '%s' - run with an empty AreaName to list them"), *Want));
}

// One tunable, deliberately named: no measured RAGE->UE photometric law exists (LOG
// "EXTENSIONS DECODED" carries the LIGHT FIELDS, not their units). RAGE MLO intensities
// cluster ~1-20; read directly as candela those are invisible, so v1 scales them into a
// plausible domestic range (a "5" bulb -> 500 cd). 🧠 AGENT CALL, UNCALIBRATED - Matt's
// eyes tune this one constant; nothing else in the importer encodes brightness.
static const float RudeMloLightCandelaScale = 100.f;

FString URudeToolset::ImportMlo(const FString& CorpusRoot, const FString& MloArchetypeName,
                                const FString& DestMeshFolder, const FString& Filter)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	const FString Wanted = MloArchetypeName.TrimStartAndEnd();
	if (Wanted.IsEmpty()) { return Fail(TEXT("MloArchetypeName is empty")); }
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return Fail(TEXT("no editor world")); }

	// ---- 1) ONE corpus walk: archetype index (resolves the interior's entities) + MLO
	// lookup riding it (hash-tolerant both ways - see FRudeMloSearch) ----
	FRudeMloSearch Search;
	Search.Wanted = Wanted;
	Search.WantHashName = FString::Printf(TEXT("hash_%08X"), RudeJoaat(Wanted));
	if (Wanted.Len() == 13 && Wanted.StartsWith(TEXT("hash_"), ESearchCase::IgnoreCase))
	{
		Search.bWantedIsHashName = true;
		Search.WantHash = static_cast<uint32>(FCString::Strtoui64(*Wanted.Mid(5), nullptr, 16));
	}
	FRudeArchetypeIndex Index;
	{
		FString IndexErr;
		if (!BuildCorpusArchetypeIndex(CorpusRoot, Index, IndexErr, &Search))
		{
			return Fail(IndexErr);
		}
	}
	if (Search.FoundFile.IsEmpty())
	{
		return Fail(FString::Printf(
			TEXT("MLO archetype '%s' not found (also tried %s) among %d MLO archetypes under %s. Leading names: %s"),
			*Wanted, *Search.WantHashName, Search.MloSeen, *CorpusRoot, *Search.Sample));
	}

	// ---- 2) parse the declaring ytyp's MLO node ----
	FXmlFile Xml(Search.FoundFile);
	if (!Xml.IsValid())
	{
		return Fail(FString::Printf(TEXT("XML load failed: %s"), *Xml.GetLastError()));
	}
	const FXmlNode* Root = Xml.GetRootNode();
	const FXmlNode* Arche = Root ? Root->FindChildNode(TEXT("archetypes")) : nullptr;
	const FXmlNode* Mlo = nullptr;
	if (Arche)
	{
		for (const FXmlNode* Item : Arche->GetChildrenNodes())
		{
			if (Item->GetAttribute(TEXT("type")) != TEXT("CMloArchetypeDef")) { continue; }
			const FXmlNode* N = Item->FindChildNode(TEXT("name"));
			if (N && N->GetContent().TrimStartAndEnd().Equals(Search.FoundName, ESearchCase::IgnoreCase))
			{
				Mlo = Item;
				break;
			}
		}
	}
	if (!Mlo)
	{
		return Fail(FString::Printf(TEXT("re-parse lost archetype '%s' in %s - file changed mid-run?"),
			*Search.FoundName, *FPaths::GetCleanFilename(Search.FoundFile)));
	}

	// "x y z" space-separated element CONTENT (posn/direction/extents/attachedObjects use the
	// scalar-list rendering: <=10 values inline, 11+ wrapped ten-per-line - so ALWAYS
	// whitespace-parse the whole content, never split on newlines; the FXmlFile line-structure
	// lesson from the vertex parser applies here too).
	auto Vec3Content = [](const FXmlNode* N, FVector& Out) -> bool
	{
		if (!N) { return false; }
		TArray<FString> T;
		N->GetContent().ParseIntoArrayWS(T);
		if (T.Num() < 3) { return false; }
		Out = FVector(FCString::Atod(*T[0]), FCString::Atod(*T[1]), FCString::Atod(*T[2]));
		return true;
	};
	auto Val = [](const FXmlNode* P, const TCHAR* Tag, double Def) -> double
	{
		const FXmlNode* N = P->FindChildNode(Tag);
		return N ? FCString::Atod(*N->GetAttribute(TEXT("value"))) : Def;
	};

	// CLightAttrDef fields the importer consumes; everything else the emitter carries
	// (flags/timeFlags/corona*/vol*/shadow*/cullingPlane/projectedTextureKey/tangent/
	// falloffExponent) is deliberately NOT mapped - see the header comment for why each.
	struct FMloLight
	{
		FVector LocalPos = FVector::ZeroVector;   // UE cm, entity-local (posn + offsetPosition)
		FVector LocalDir = FVector(0, 0, -1);     // UE, entity-local
		FLinearColor Color = FLinearColor::White;
		float Intensity = 0.f;
		float Falloff = 0.f;                      // GTA metres
		float ConeInner = 0.f, ConeOuter = 0.f;   // degrees (half-angles)
		int32 Type = -1;                          // 1 point / 2 spot / 4 capsule (observed set)
		float ExtentX = 0.f;                      // capsule length (metres)
	};
	struct FMloEntity
	{
		FString ArchLower;
		FTransform Xf;          // MLO-LOCAL, UE space (import-lane transform)
		int32 Room = -1;        // index into Rooms (from rooms' attachedObjects)
		int32 Portal = -1;      // index into Portals (doors attach to portals, not rooms)
		TArray<FMloLight> Lights;
		// ⛔⛔ A MALFORMED ENTITY KEEPS ITS SLOT. Rooms and portals reference their contents by
		// ORDINAL - "0-based into <entities>" is this file's own stated contract - so dropping an
		// entity from the array re-bases every attachedObjects index after it and silently attaches
		// the wrong props to the wrong rooms. badAttachedRefs cannot catch it: after a drop the
		// shifted indices are all still IN RANGE, so every check passes and the interior is quietly
		// wrong. RudeScenario.cpp:139-157 already solves exactly this for points and nodes with the
		// same reasoning ("Compacting the array renumbers everything after it - a corruption that no
		// in-range check can catch"); the pattern existed and was simply not applied here.
		// MEASURED over 105 MLO archetypes / 11,451 entities: 0 occurrences today. Latent, and
		// latent is the point - the trigger is a hand-edited, truncated or re-emitted ytyp.
		bool bValid = true;
	};
	TArray<FMloEntity> Ents;
	int32 OtherExtensions = 0, LightsSkipped = 0, EntitiesMissingTransform = 0;
	FString LightProblem;

	if (const FXmlNode* EntsN = Mlo->FindChildNode(TEXT("entities")))
	{
		for (const FXmlNode* E : EntsN->GetChildrenNodes())
		{
			const FXmlNode* AN = E->FindChildNode(TEXT("archetypeName"));
			const FXmlNode* Pos = E->FindChildNode(TEXT("position"));
			if (!AN || !Pos)
			{
				// Slot preserved, entity marked dead - see FMloEntity::bValid.
				++EntitiesMissingTransform;
				FMloEntity Dead;
				Dead.bValid = false;
				Ents.Add(MoveTemp(Dead));
				continue;
			}
			FMloEntity Ent;
			Ent.ArchLower = AN->GetContent().TrimStartAndEnd().ToLower();
			// MLO-LOCAL transform through the pinned IMPORT-lane convention (pos Y-mirror*100,
			// quat = (x,-y,z,w)) - identical to ImportMapArea's manifest transforms, so a later
			// Build Interior can place the whole root at a CMloInstanceDef world transform
			// without touching the entities.
			const double Px = FCString::Atod(*Pos->GetAttribute(TEXT("x")));
			const double Py = FCString::Atod(*Pos->GetAttribute(TEXT("y")));
			const double Pz = FCString::Atod(*Pos->GetAttribute(TEXT("z")));
			double Qx = 0, Qy = 0, Qz = 0, Qw = 1;
			if (const FXmlNode* Rot = E->FindChildNode(TEXT("rotation")))
			{
				Qx = FCString::Atod(*Rot->GetAttribute(TEXT("x")));
				Qy = FCString::Atod(*Rot->GetAttribute(TEXT("y")));
				Qz = FCString::Atod(*Rot->GetAttribute(TEXT("z")));
				Qw = FCString::Atod(*Rot->GetAttribute(TEXT("w")));
			}
			FQuat Q(Qx, -Qy, Qz, Qw);
			// not normalised: the source quaternion is unit within float32 and re-normalising in double moves its last digit (measured 2026-09-06: 0.9961947 -> 0.996194661 on an untouched entity)
			Ent.Xf = FTransform(Q, FVector(Px * 100.0, -Py * 100.0, Pz * 100.0),
				FVector(Val(E, TEXT("scaleXY"), 1.0), Val(E, TEXT("scaleXY"), 1.0), Val(E, TEXT("scaleZ"), 1.0)));

			// per-entity extensions: consume CExtensionDefLightEffect instances; COUNT the
			// rest (doors/spawn points/particles...) so the verdict says what v1 left behind.
			if (const FXmlNode* Ext = E->FindChildNode(TEXT("extensions")))
			{
				for (const FXmlNode* X : Ext->GetChildrenNodes())
				{
					if (X->GetAttribute(TEXT("type")) != TEXT("CExtensionDefLightEffect"))
					{
						++OtherExtensions;
						continue;
					}
					FVector Off(0, 0, 0);
					if (const FXmlNode* O = X->FindChildNode(TEXT("offsetPosition")))
					{
						Off = FVector(FCString::Atod(*O->GetAttribute(TEXT("x"))),
						              FCString::Atod(*O->GetAttribute(TEXT("y"))),
						              FCString::Atod(*O->GetAttribute(TEXT("z"))));
					}
					const FXmlNode* Inst = X->FindChildNode(TEXT("instances"));
					if (!Inst) { continue; }
					for (const FXmlNode* L : Inst->GetChildrenNodes())
					{
						FVector P, D;
						TArray<FString> ColT;
						if (const FXmlNode* C = L->FindChildNode(TEXT("colour")))
						{
							C->GetContent().ParseIntoArrayWS(ColT);
						}
						if (!Vec3Content(L->FindChildNode(TEXT("posn")), P) ||
						    !Vec3Content(L->FindChildNode(TEXT("direction")), D) || ColT.Num() < 3)
						{
							// The emitter has NO silent defaults (LOG "MLO EMISSION"), so a
							// missing field here means corrupt input - count it, name the first.
							++LightsSkipped;
							if (LightProblem.IsEmpty())
							{
								LightProblem = FString::Printf(
									TEXT("entity %d: light instance missing posn/colour/direction"), Ents.Num());
							}
							continue;
						}
						FMloLight ML;
						ML.LocalPos = FVector((P.X + Off.X) * 100.0, -(P.Y + Off.Y) * 100.0, (P.Z + Off.Z) * 100.0);
						ML.LocalDir = FVector(D.X, -D.Y, D.Z);   // same Y-mirror as every import-lane vector
						ML.Color = FLinearColor(
							FCString::Atof(*ColT[0]) / 255.f,
							FCString::Atof(*ColT[1]) / 255.f,
							FCString::Atof(*ColT[2]) / 255.f);
						ML.Intensity = (float)Val(L, TEXT("intensity"), 0.0);
						ML.Falloff = (float)Val(L, TEXT("falloff"), 0.0);
						ML.ConeInner = (float)Val(L, TEXT("coneInnerAngle"), 0.0);
						ML.ConeOuter = (float)Val(L, TEXT("coneOuterAngle"), 0.0);
						ML.Type = (int32)Val(L, TEXT("lightType"), -1.0);
						FVector Ex(0, 0, 0);
						Vec3Content(L->FindChildNode(TEXT("extents")), Ex);
						ML.ExtentX = (float)Ex.X;
						Ent.Lights.Add(ML);
					}
				}
			}
			Ents.Add(MoveTemp(Ent));
		}
	}

	// rooms + portals: membership comes from their attachedObjects index lists (0-based into
	// <entities>; oracle-proven in-range corpus-wide, so an out-of-range index is corrupt
	// input - counted loudly, never clamped).
	struct FMloRoom { FString Name; FString NameLower; };
	struct FMloPortal { int32 From = -1; int32 To = -1; };
	TArray<FMloRoom> Rooms;
	TArray<FMloPortal> Portals;
	int32 BadRefs = 0;
	if (const FXmlNode* RoomsN = Mlo->FindChildNode(TEXT("rooms")))
	{
		for (const FXmlNode* R : RoomsN->GetChildrenNodes())
		{
			FMloRoom Room;
			if (const FXmlNode* N = R->FindChildNode(TEXT("name")))
			{
				Room.Name = N->GetContent().TrimStartAndEnd();
			}
			Room.NameLower = Room.Name.ToLower();
			const int32 RoomIdx = Rooms.Add(Room);
			if (const FXmlNode* AO = R->FindChildNode(TEXT("attachedObjects")))
			{
				TArray<FString> T;
				AO->GetContent().ParseIntoArrayWS(T);
				for (const FString& S : T)
				{
					const int32 EIdx = FCString::Atoi(*S);
					if (Ents.IsValidIndex(EIdx)) { Ents[EIdx].Room = RoomIdx; }
					else { ++BadRefs; }
				}
			}
		}
	}
	if (const FXmlNode* PortalsN = Mlo->FindChildNode(TEXT("portals")))
	{
		for (const FXmlNode* P : PortalsN->GetChildrenNodes())
		{
			FMloPortal Portal;
			Portal.From = (int32)Val(P, TEXT("roomFrom"), -1.0);
			Portal.To = (int32)Val(P, TEXT("roomTo"), -1.0);
			const int32 PortalIdx = Portals.Add(Portal);
			if (const FXmlNode* AO = P->FindChildNode(TEXT("attachedObjects")))
			{
				TArray<FString> T;
				AO->GetContent().ParseIntoArrayWS(T);
				for (const FString& S : T)
				{
					const int32 EIdx = FCString::Atoi(*S);
					if (Ents.IsValidIndex(EIdx)) { Ents[EIdx].Portal = PortalIdx; }
					else { ++BadRefs; }
				}
			}
		}
	}
	// entity sets: SUMMARIZED, not spawned (v1) - they are optional overlays the game toggles
	// at runtime (LOG "MLO INTERIORS"), so spawning them all would misrepresent the interior.
	FString SetsJson;
	int32 NumSets = 0;
	if (const FXmlNode* Sets = Mlo->FindChildNode(TEXT("entitySets")))
	{
		for (const FXmlNode* S : Sets->GetChildrenNodes())
		{
			const FXmlNode* SN = S->FindChildNode(TEXT("name"));
			const FXmlNode* SE = S->FindChildNode(TEXT("entities"));
			SetsJson += FString::Printf(TEXT("%s{\"name\":\"%s\",\"entities\":%d}"),
				NumSets ? TEXT(",") : TEXT(""),
				SN ? *SN->GetContent().TrimStartAndEnd() : TEXT(""),
				SE ? SE->GetChildrenNodes().Num() : 0);
			++NumSets;
		}
	}

	// ---- Filter = ROOM-name list (🧠 agent's design; see header comment) ----
	TSet<FString> RoomFilter;
	{
		const FString F = Filter.TrimStartAndEnd();
		if (!F.IsEmpty() && !F.Equals(TEXT("ALL"), ESearchCase::IgnoreCase))
		{
			TArray<FString> Toks;
			F.ParseIntoArray(Toks, TEXT(","), true);
			for (FString T : Toks)
			{
				T.TrimStartAndEndInline();
				if (!T.IsEmpty()) { RoomFilter.Add(T.ToLower()); }
			}
		}
	}
	FString RoomNamesJson, RoomNamesPlain;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		RoomNamesJson += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(",") : TEXT(""), *Rooms[i].Name);
		RoomNamesPlain += FString::Printf(TEXT("%s%s"), i ? TEXT(", ") : TEXT(""), *Rooms[i].Name);
	}
	for (const FString& Tok : RoomFilter)
	{
		bool bKnown = false;
		for (const FMloRoom& R : Rooms)
		{
			if (R.NameLower == Tok) { bKnown = true; break; }
		}
		if (!bKnown)
		{
			return Fail(FString::Printf(TEXT("Filter room '%s' is not a room of %s - rooms here: %s"),
				*Tok, *Search.FoundName, *RoomNamesPlain));
		}
	}
	auto RoomPasses = [&](int32 RoomIdx) -> bool
	{
		return RoomFilter.IsEmpty() ||
			(Rooms.IsValidIndex(RoomIdx) && RoomFilter.Contains(Rooms[RoomIdx].NameLower));
	};
	auto Passes = [&](const FMloEntity& E) -> bool
	{
		// A slot-preserving placeholder is not a thing to import or spawn - it exists only so the
		// entities after it keep their ordinals.
		if (!E.bValid) { return false; }
		if (E.Room >= 0) { return RoomPasses(E.Room); }
		if (Portals.IsValidIndex(E.Portal))
		{
			// a door belongs to BOTH sides of its portal - it spawns when either room does
			return RoomPasses(Portals[E.Portal].From) || RoomPasses(Portals[E.Portal].To);
		}
		return RoomFilter.IsEmpty();
	};
	int32 Unroomed = 0;
	for (const FMloEntity& E : Ents)
	{
		if (E.bValid && E.Room < 0 && E.Portal < 0) { ++Unroomed; }
	}

	// ---- 2b) the file's OWN BYTES, cut into per-entity slices - the export (ExportMloYtyp) splices these
	// back verbatim. A slicing that disagrees with the parse is refused HERE, never carried into the level
	// (measured 2026-09-06: 539/539 non-empty MLO entity blocks and 2,272/2,272 set blocks cut with zero
	// leftover bytes - maintainer lane `mlo_export` (`LAWS.md`) law 3). RUDE_MLO_RAW_SLICES
	FRudeMloRaw Raw;
	{
		FString RawText, RawErr;
		if (!FFileHelper::LoadFileToString(RawText, *Search.FoundFile))
		{
			return Fail(FString::Printf(TEXT("cannot read the ytyp's bytes: %s"), *RudeJsonEscape(Search.FoundFile)));
		}
		if (!RudeMloSliceRaw(RawText, Search.FoundName, Raw, RawErr))
		{
			return Fail(FString::Printf(TEXT("ytyp slices refused (%s): %s"), *FPaths::GetCleanFilename(Search.FoundFile), *RudeJsonEscape(RawErr)));
		}
		if (Raw.Items.Num() != Ents.Num())
		{
			return Fail(FString::Printf(TEXT("ytyp slices %d != parsed entities %d in %s - not the measured shape"),
				Raw.Items.Num(), Ents.Num(), *FPaths::GetCleanFilename(Search.FoundFile)));
		}
	}
	// the ytyp ASSET name (ledger identity): the export resolves the same row through the corpus
	const FString YtypAsset = FRudeCorpus::AssetNameOf(TEXT("ytyp"), FPaths::GetCleanFilename(Search.FoundFile));

	// ---- 3) import every referenced drawable present in the corpus (skip-if-exists;
	// the exact ydr/yft/ydd lane ImportMapArea proved, via the shared helper) ----
	TSet<FString> Needed;
	for (const FMloEntity& E : Ents)
	{
		if (!Passes(E)) { continue; }
		if (const FString* Asset = Index.ArchToAsset.Find(E.ArchLower)) { Needed.Add(*Asset); }
	}
	int32 MeshOk = 0, MeshSkip = 0, MeshFail = 0, MeshMissing = 0, Done = 0;
	FRudeImportTally Tally;
	// the entity SETS join the mesh pass (2026-09-06): without this every set entity was a proxy cube
	if (const FXmlNode* SetsN = Mlo->FindChildNode(TEXT("entitySets")))
	{
		for (const FXmlNode* S : SetsN->GetChildrenNodes())
		{
			const FXmlNode* SE = S->FindChildNode(TEXT("entities"));
			if (!SE) { continue; }
			for (const FXmlNode* E : SE->GetChildrenNodes())
			{
				const FXmlNode* AN = E->FindChildNode(TEXT("archetypeName"));
				if (!AN) { continue; }
				if (const FString* Asset = Index.ArchToAsset.Find(AN->GetContent().TrimStartAndEnd().ToLower())) { Needed.Add(*Asset); }
			}
		}
	}
	for (const FString& D : Needed)
	{
		++Done;
		ImportIndexedDrawable(CorpusRoot, Index, D, DestMeshFolder, MeshOk, MeshSkip, MeshFail,
		                      MeshMissing, Tally);
		if (Done % 100 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("[RUDE] ImportMlo meshes %d/%d (ok %d, skip %d, fail %d)"),
				Done, Needed.Num(), MeshOk, MeshSkip, MeshFail);
			// KEEPFLAGS (= RF_Standalone in editor), NEVER RF_NoFlags - a no-keep GC deletes
			// the unsaved meshes this very run imported (the GC-sweep law).
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	// ---- 4) spawn: rooms' entities at MLO-LOCAL transforms, root at the WORLD ORIGIN ----
	// Idempotent respawn is per-ARCHETYPE and clear-by-TAG (🧠 agent's call): OFPA can rewrite
	// folder paths (BUILD_AREA_DESIGN R12), and a folder clear would also kill OTHER imported
	// interiors. Every actor of this interior carries IdTag, so root + room actors all die
	// here even though DestroyActor does not cascade to attached children.
	const FName IdTag(*(TEXT("RUDE_MLO:") + Search.FoundName));
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(IdTag)) { Stale.Add(*It); }
		}
		for (AActor* A : Stale) { World->DestroyActor(A); }
	}
	// Outliner label prefers the caller's REAL spelling when the corpus stores only the hash
	// (labels are cosmetic; TAGS carry the corpus spelling as the deterministic identity).
	const FString Label = (Search.FoundName.StartsWith(TEXT("hash_")) && !Search.bWantedIsHashName)
		? Wanted : Search.FoundName;

	AActor* RootActor = World->SpawnActor<AActor>();
	if (!RootActor) { return Fail(TEXT("root actor spawn failed")); }
	{
		USceneComponent* RootComp = NewObject<USceneComponent>(RootActor, TEXT("Root"));
		RootActor->SetRootComponent(RootComp);
		RootComp->SetMobility(EComponentMobility::Static);
		RootComp->RegisterComponent();
		RootActor->AddInstanceComponent(RootComp);
		RootActor->SetActorLabel(TEXT("MLO_") + Label);
		RootActor->SetFolderPath(FName(TEXT("RUDE_MLO")));
		RootActor->Tags.Add(IdTag);
		RootActor->Tags.Add(FName(TEXT("RUDE_MLO_ROOT")));
		// the ytyp the interior was read from: ExportMloYtyp splices THAT file (86/391 MLO names live in more
		// than one ledger row, LAWS.md law 2) and refuses a different copy
		RootActor->Tags.Add(FName(*(TEXT("RUDE_MLO_Ytyp:") + YtypAsset)));
		RootActor->Tags.Add(FName(*(TEXT("RUDE_MLO_YtypFile:") + Search.FoundFile)));
	}

	// One actor per room (plus a portal-doors bucket and an unroomed bucket when needed),
	// attached under the root; inside each, the proven ImportScene ISM pattern - one
	// InstancedStaticMeshComponent per unique drawable, proxy cubes for corpus holes.
	struct FBucket
	{
		AActor* Actor = nullptr;
		USceneComponent* Root = nullptr;
		TMap<FString, UInstancedStaticMeshComponent*> IsmByMesh;
		int32 NumLights = 0;
	};
	TMap<int32, FBucket> Buckets;   // room index; -2 = portal-attached, -3 = unroomed
	auto GetBucket = [&](int32 Key) -> FBucket*
	{
		if (FBucket* B = Buckets.Find(Key)) { return B; }
		AActor* A = World->SpawnActor<AActor>();
		if (!A) { return nullptr; }
		USceneComponent* R = NewObject<USceneComponent>(A, TEXT("Root"));
		A->SetRootComponent(R);
		R->SetMobility(EComponentMobility::Static);
		R->RegisterComponent();
		A->AddInstanceComponent(R);
		const FString Suffix = (Key >= 0) ? Rooms[Key].Name
			: FString(Key == -2 ? TEXT("portalDoors") : TEXT("unroomed"));
		A->SetActorLabel(Label + TEXT("_") + Suffix);
		A->SetFolderPath(FName(TEXT("RUDE_MLO")));
		A->Tags.Add(IdTag);
		A->Tags.Add((Key >= 0) ? FName(*(TEXT("RUDE_MLO_Room:") + Rooms[Key].Name))
			: FName(Key == -2 ? TEXT("RUDE_MLO_Portal") : TEXT("RUDE_MLO_Room:(none)")));
		if (Key >= 0) { A->Tags.Add(FName(*FString::Printf(TEXT("RUDE_MLO_RoomIndex:%d"), Key))); }   // the export appends an added entity to the room it sits under
		A->AttachToActor(RootActor, FAttachmentTransformRules::KeepWorldTransform);
		return &Buckets.Add(Key, FBucket{ A, R });
	};
#if 0   // RUDE 2026-09-06 (mlo_export): ISM path retired - entities are actors now (see SpawnMloEntity); kept for the record
	auto GetBucketIsm = [&](int32 Key, const FString& MeshKey, UStaticMesh* Mesh)
		-> UInstancedStaticMeshComponent*
	{
		FBucket* B = GetBucket(Key);
		if (!B) { return nullptr; }
		if (UInstancedStaticMeshComponent** Found = B->IsmByMesh.Find(MeshKey)) { return *Found; }
		UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(
			B->Actor, FName(*FString::Printf(TEXT("ISM_%d"), B->IsmByMesh.Num())));
		Ism->SetStaticMesh(Mesh);
		Ism->SetMobility(EComponentMobility::Static);
		Ism->SetupAttachment(B->Root);
		Ism->RegisterComponent();
		B->Actor->AddInstanceComponent(Ism);
		B->IsmByMesh.Add(MeshKey, Ism);
		return Ism;
	};
#endif

	UStaticMesh* ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TMap<FString, UStaticMesh*> MeshCache;   // lowercase drawable -> mesh (nullptr = known-missing)
	int32 Spawned = 0, Proxies = 0, NumLights = 0, Unresolved = 0;
	// ---- one ACTOR per entity (GDD Tier 1: import-author-EXPORT) - RUDE_MLO_ENTITY_ACTORS ----
	// Identity rides on a URudeMloEntityComponent (interior, set, ordinal, the raw <Item> slice, the source
	// transform). The ISM path is retired: an instance had no identity, so nothing could be moved and written
	// back. Cost, counted in the verdict: one actor per entity (v_franklinshouse: 157 room + 133 set entities).
	int32 EntityActors = 0, SetEntityActors = 0, RawSetMismatch = 0;
	auto SpawnMloEntity = [&](AActor* Parent, const FString& SetName, int32 Ordinal, int32 RoomIdx, int32 PortalIdx,
	                          const FString& ArchLower, const FTransform& Xf, UStaticMesh* Mesh, const FString& Slice,
	                          bool bHidden) -> AActor*
	{
		AActor* EA = World->SpawnActor<AActor>();
		if (!EA) { return nullptr; }
		UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(EA, TEXT("Mesh"));
		SMC->SetStaticMesh(Mesh ? Mesh : ProxyCube);
		SMC->SetMobility(EComponentMobility::Static);
		EA->SetRootComponent(SMC);
		SMC->RegisterComponent();
		EA->AddInstanceComponent(SMC);
		EA->SetActorTransform(Xf);
		EA->SetActorLabel(ArchLower);
		EA->SetFolderPath(FName(*(TEXT("RUDE_MLO/") + Label)));
		EA->Tags.Add(IdTag);
		EA->Tags.Add(FName(TEXT("RUDE_MLO_Entity")));
		if (!Mesh) { EA->Tags.Add(FName(TEXT("RUDE_PROXY"))); }
		if (Parent) { EA->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform); }
		URudeMloEntityComponent* M = NewObject<URudeMloEntityComponent>(EA, TEXT("RudeMloEntity"));
		M->Interior = Search.FoundName;
		M->SetName = SetName;
		M->SourceIndex = Ordinal;
		M->RoomIndex = RoomIdx;
		M->PortalIndex = PortalIdx;
		M->ArchetypeName = ArchLower;
		M->SourceYtyp = YtypAsset;
		M->SourceFile = Search.FoundFile;
		M->SourceXml = Slice;
		M->SourceTransform = Xf;
		M->RegisterComponent();
		EA->AddInstanceComponent(M);
		if (bHidden)
		{
			EA->SetActorHiddenInGame(true);
			SMC->SetVisibility(false, true);
		}
		return EA;
	};
	// ---- entity SETS (GDD: "entity sets -> variants; activation is per instance"): every set's entities
	// spawn under their own actor, HIDDEN, tagged RUDE_MLO_EntitySet:<set> - SetEntitySet (editor) and the
	// sandbox shim's ActivateInteriorEntitySet (PIE) toggle them. Rough: lights of set entities are skipped.
	int32 SetEntitiesSpawned = 0, SetEntitiesProxied = 0, SetActors = 0;
	if (const FXmlNode* SetsN = Mlo->FindChildNode(TEXT("entitySets")))
	{
		for (const FXmlNode* S : SetsN->GetChildrenNodes())
		{
			const FXmlNode* SN = S->FindChildNode(TEXT("name"));
			const FXmlNode* SE = S->FindChildNode(TEXT("entities"));
			if (!SN || !SE || SE->GetChildrenNodes().Num() == 0) { continue; }
			const FString SetName = SN->GetContent().TrimStartAndEnd();
			AActor* SA = World->SpawnActor<AActor>();
			if (!SA) { continue; }
			USceneComponent* SR = NewObject<USceneComponent>(SA, TEXT("Root"));
			SA->SetRootComponent(SR);
			SR->SetMobility(EComponentMobility::Static);
			SR->RegisterComponent();
			SA->AddInstanceComponent(SR);
			SA->SetActorLabel(Label + TEXT("_set_") + SetName);
			SA->SetFolderPath(FName(TEXT("RUDE_MLO")));
			SA->Tags.Add(IdTag);
			SA->Tags.Add(FName(*(TEXT("RUDE_MLO_EntitySet:") + SetName)));
			SA->AttachToActor(RootActor, FAttachmentTransformRules::KeepWorldTransform);
			// slices for this set (the export re-emits them verbatim); a set whose slices disagree with the parse is
			// counted and skipped, never spawned half-right. RUDE_MLO_SET_ACTORS
			const FRudeMloRawSet* RawSet = Raw.Sets.FindByPredicate([&SetName](const FRudeMloRawSet& X) { return X.Name.Equals(SetName, ESearchCase::IgnoreCase); });
			if (!RawSet || RawSet->Items.Num() != SE->GetChildrenNodes().Num())
			{
				++RawSetMismatch;
				UE_LOG(LogTemp, Warning, TEXT("[RUDE] ImportMlo %s: entity set '%s' raw slices %d != parsed %d - set skipped"),
					*Search.FoundName, *SetName, RawSet ? RawSet->Items.Num() : -1, SE->GetChildrenNodes().Num());
				World->DestroyActor(SA);
				continue;
			}
			int32 SetOrdinal = -1;
			for (const FXmlNode* E : SE->GetChildrenNodes())
			{
				++SetOrdinal;
				const FXmlNode* AN = E->FindChildNode(TEXT("archetypeName"));
				const FXmlNode* Pos = E->FindChildNode(TEXT("position"));
				if (!AN || !Pos) { continue; }   // a dead slot keeps its ordinal; its slice re-emits verbatim at export
				const FString ArchLower = AN->GetContent().TrimStartAndEnd().ToLower();
				const double Px = FCString::Atod(*Pos->GetAttribute(TEXT("x"))), Py = FCString::Atod(*Pos->GetAttribute(TEXT("y"))), Pz = FCString::Atod(*Pos->GetAttribute(TEXT("z")));
				double Qx = 0, Qy = 0, Qz = 0, Qw = 1;
				if (const FXmlNode* Rot = E->FindChildNode(TEXT("rotation")))
				{
					Qx = FCString::Atod(*Rot->GetAttribute(TEXT("x"))); Qy = FCString::Atod(*Rot->GetAttribute(TEXT("y")));
					Qz = FCString::Atod(*Rot->GetAttribute(TEXT("z"))); Qw = FCString::Atod(*Rot->GetAttribute(TEXT("w")));
				}
				// scale read like the room entities' (the ISM path dropped it); never compared at export
				const FTransform Xf(FQuat(Qx, -Qy, Qz, Qw), FVector(Px * 100.0, -Py * 100.0, Pz * 100.0),
					FVector(Val(E, TEXT("scaleXY"), 1.0), Val(E, TEXT("scaleXY"), 1.0), Val(E, TEXT("scaleZ"), 1.0)));
				const FString* Asset = Index.ArchToAsset.Find(ArchLower);
				UStaticMesh* Mesh = nullptr;
				if (Asset)
				{
					if (UStaticMesh** Cached = MeshCache.Find(*Asset)) { Mesh = *Cached; }
					else { Mesh = LoadObject<UStaticMesh>(nullptr, *(DestMeshFolder / *Asset)); MeshCache.Add(*Asset, Mesh); }
				}
				if (!Mesh && !ProxyCube) { continue; }
				// the set's <locations> slot = this entity's room (one per entity: 2,272/2,272 sets measured)
				const int32 Loc = RawSet->Locations.IsValidIndex(SetOrdinal) ? RawSet->Locations[SetOrdinal] : -1;
				if (SpawnMloEntity(SA, SetName, SetOrdinal, Loc, -1, ArchLower, Xf, Mesh, RawSet->Items[SetOrdinal], /*bHidden*/ true))
				{
					++SetEntityActors;
					if (Mesh) { ++SetEntitiesSpawned; } else { ++SetEntitiesProxied; }
				}
			}
			// hidden until activated (the game's own default: a set is off unless the instance lists it);
			// the entity actors were spawned hidden above
			SA->SetActorHiddenInGame(true);
			++SetActors;
		}
	}
	for (int32 i = 0; i < Ents.Num(); ++i)
	{
		const FMloEntity& E = Ents[i];
		if (!Passes(E)) { continue; }
		const int32 Key = (E.Room >= 0) ? E.Room : (E.Portal >= 0 ? -2 : -3);
		FBucket* B = GetBucket(Key);
		if (!B) { continue; }

		const FString* Asset = Index.ArchToAsset.Find(E.ArchLower);
		UStaticMesh* Mesh = nullptr;
		if (Asset)
		{
			if (UStaticMesh** Cached = MeshCache.Find(*Asset)) { Mesh = *Cached; }
			else
			{
				Mesh = LoadObject<UStaticMesh>(nullptr, *(DestMeshFolder / *Asset));
				MeshCache.Add(*Asset, Mesh);
			}
		}
		else { ++Unresolved; }
		if (!Mesh && !ProxyCube) { continue; }
		// one ACTOR per entity under its room actor, carrying its ordinal + raw slice (the export's identity).
		// RUDE_MLO_MAIN_ACTORS
		AActor* EA = SpawnMloEntity(B->Actor, FString(), i, E.Room, E.Portal, E.ArchLower, E.Xf, Mesh, Raw.Items[i], /*bHidden*/ false);
		if (!EA) { continue; }
		++EntityActors;
		if (Mesh) { ++Spawned; } else { ++Proxies; }

		// lights: one component per CLightAttrDef instance, ON THE ENTITY ACTOR (a moved entity carries them;
		// the placement math is the v1 math, unchanged - world position from the entity transform)
		for (const FMloLight& L : E.Lights)
		{
			ULocalLightComponent* LC = nullptr;
			if (L.Type == 2)
			{
				USpotLightComponent* Spot = NewObject<USpotLightComponent>(EA,
					FName(*FString::Printf(TEXT("Light_%d_%d"), i, B->NumLights)));
				// RAGE cone angles are half-angle degrees like UE's; UE's outer cone tops out
				// at 80, so RAGE's 90-degree hemisphere washes clamp (documented narrowing).
				Spot->SetOuterConeAngle(FMath::Clamp(L.ConeOuter, 1.f, 80.f));
				Spot->SetInnerConeAngle(FMath::Clamp(L.ConeInner, 0.f, Spot->OuterConeAngle));
				LC = Spot;
			}
			else if (L.Type == 1 || L.Type == 4)
			{
				UPointLightComponent* Pt = NewObject<UPointLightComponent>(EA,
					FName(*FString::Printf(TEXT("Light_%d_%d"), i, B->NumLights)));
				if (L.Type == 4)
				{
					// capsule: a line emitter along `direction` - UE's point light expresses
					// exactly that as SourceLength (extents.x carries the length, measured on
					// the corpus tube lights).
					Pt->SetSourceLength(FMath::Max(0.f, L.ExtentX) * 100.f);
				}
				LC = Pt;
			}
			else
			{
				// only 1/2/4 are observed in the resolved corpus - an unknown type is refused
				// loudly per light, never guessed into some default shape
				++LightsSkipped;
				if (LightProblem.IsEmpty())
				{
					LightProblem = FString::Printf(
						TEXT("entity %d: lightType %d has no derived mapping (observed set: 1 point / 2 spot / 4 capsule)"),
						i, L.Type);
				}
				continue;
			}
			// Movable, not Static: the imported content has no lightmap-UV story, so the whole
			// RUDE lighting model is dynamic-only (BUILD_AREA_DESIGN section 4) - a Static light
			// here would render as unbuilt preview forever.
			LC->SetMobility(EComponentMobility::Movable);
			LC->SetupAttachment(EA->GetRootComponent());
			LC->RegisterComponent();
			EA->AddInstanceComponent(LC);
			LC->SetLightColor(L.Color);
			LC->SetIntensityUnits(ELightUnits::Candelas);
			LC->SetIntensity(L.Intensity * RudeMloLightCandelaScale);
			LC->SetAttenuationRadius(FMath::Max(10.f, L.Falloff * 100.f));   // falloff metres -> cm
			const FVector WPos = E.Xf.TransformPosition(L.LocalPos);
			FRotator WRot = FRotator::ZeroRotator;
			const FVector WDir = E.Xf.TransformVectorNoScale(L.LocalDir);
			if (!WDir.IsNearlyZero())
			{
				WRot = FRotationMatrix::MakeFromX(WDir.GetSafeNormal()).Rotator();
			}
			LC->SetWorldLocationAndRotation(WPos, WRot);
			++B->NumLights;
			++NumLights;
		}
	}
	World->MarkPackageDirty();

	// portal summary: room names when the indices resolve, raw indices otherwise
	auto RoomLabel = [&Rooms](int32 Idx) -> FString
	{
		return Rooms.IsValidIndex(Idx) ? Rooms[Idx].Name : FString::FromInt(Idx);
	};
	FString PortalsJson;
	for (int32 i = 0; i < Portals.Num(); ++i)
	{
		PortalsJson += FString::Printf(TEXT("%s\"%s->%s\""), i ? TEXT(",") : TEXT(""),
			*RoomLabel(Portals[i].From), *RoomLabel(Portals[i].To));
	}
	const FString LightProblemJson = LightProblem.IsEmpty()
		? FString()
		: FString::Printf(TEXT("\"lightProblem\":\"%s\","), *LightProblem);
	// "entities" stays the SLOT COUNT (that is what rooms and portals index into); the placeholders
	// added for malformed records are broken out separately so the two can never be confused.
	// ok is computed: an interior that indexed nothing or placed nothing is not a success, and the
	// per-mesh tally is forwarded so this lane - which together with ImportMapArea is the ONLY
	// consumer of the yft and ydd import paths - finally reports the quality of what it imported.
	// ⚠ DELIBERATELY WEAK. I have no measurement of how often a legitimate room filter leaves an
	// interior with nothing to place, so the gate fires only on shapes that cannot be legitimate:
	// an MLO with neither a room nor a portal (it did not parse as an interior at all), or one
	// that placed NOTHING - not even a proxy cube - while having entity slots to place. A gate
	// that fires on a normal run is a gate that gets ignored; tightening this one needs an
	// in-editor run across several interiors, which is on the handoff list.
	const bool bMloOk = (Rooms.Num() > 0 || Portals.Num() > 0)
		&& (Spawned + Proxies > 0 || Ents.Num() == 0) && RawSetMismatch == 0;
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"archetype\":\"%s\",\"requested\":\"%s\",\"ytyp\":\"%s\","
		"\"rooms\":%d,\"roomNames\":[%s],\"portals\":%d,\"portalRooms\":[%s],"
		"\"entitySets\":[%s],\"entitySetActors\":%d,\"entitySetEntitiesSpawned\":%d,\"entitySetEntitiesProxied\":%d,\"entities\":%d,\"entitiesMissingTransform\":%d,"
		"\"spawned\":%d,\"proxies\":%d,\"entityActors\":%d,\"setEntityActors\":%d,\"rawSetMismatch\":%d,"
		"\"unresolvedArchetypes\":%d,\"lights\":%d,\"lightsSkipped\":%d,%s"
		"\"otherExtensions\":%d,\"badAttachedRefs\":%d,\"unroomedEntities\":%d,"
		"\"meshesImported\":%d,\"meshesSkipped\":%d,\"meshesFailed\":%d,"
		"\"meshesMissingFromCorpus\":%d,%s}"),
		bMloOk ? TEXT("true") : TEXT("false"),
		*Search.FoundName, *Wanted, *FPaths::GetCleanFilename(Search.FoundFile),
		Rooms.Num(), *RoomNamesJson, Portals.Num(), *PortalsJson,
		*SetsJson, SetActors, SetEntitiesSpawned, SetEntitiesProxied, Ents.Num(), EntitiesMissingTransform, Spawned, Proxies, EntityActors, SetEntityActors, RawSetMismatch,
		Unresolved, NumLights, LightsSkipped, *LightProblemJson,
		OtherExtensions, BadRefs, Unroomed,
		MeshOk, MeshSkip, MeshFail, MeshMissing, *Tally.ToJson());
}

// Ported from the in-game-proven tools/emit_ytyp.py - the archetype flag +
// physicsDictionary laws are load-bearing (FULL COLLISION MODEL, 2026-07-24).
FString URudeToolset::ExportYtyp(const FString& YdrSpecs, const FString& YtypName,
                                 const FString& OutYtypPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	TArray<FString> Specs;
	YdrSpecs.ParseIntoArray(Specs, TEXT(","), true);
	if (Specs.Num() == 0) { return Fail(TEXT("no ydr specs (want absPath[;txd[;physDict]], ...)")); }

	FString Archetypes;
	// ADDED 2026-08-03 - two specs whose drawables share a <Name> used to emit two same-named
	// <Item>s and report archetypes:2, while the game keeps exactly one. Silent halving of the
	// archetype set, with a count that says otherwise.
	TSet<FString> SeenNames;
	for (const FString& SpecStr : Specs)
	{
		TArray<FString> F;
		SpecStr.TrimStartAndEnd().ParseIntoArray(F, TEXT(";"), false);
		FXmlFile Xml(F[0]);
		if (!Xml.IsValid()) { return Fail(FString::Printf(TEXT("XML load failed: %s"), *F[0])); }
		const FXmlNode* Root = Xml.GetRootNode();
		if (!Root || Root->GetTag() != TEXT("Drawable")) { return Fail(FString::Printf(TEXT("not a Drawable: %s"), *F[0])); }

		FString Name = FPaths::GetBaseFilename(F[0]);
		Name.RemoveFromEnd(TEXT(".ydr"));
		if (const FXmlNode* N = Root->FindChildNode(TEXT("Name")))
		{
			FString S = N->GetContent().TrimStartAndEnd();
			int32 Dot; if (S.FindChar(TEXT('.'), Dot)) { S.LeftInline(Dot); }
			if (!S.IsEmpty()) { Name = S; }
		}
		Name.ToLowerInline();
		if (SeenNames.Contains(Name))
		{
			return Fail(FString::Printf(TEXT("duplicate archetype name '%s' (from %s); the game keeps only one"),
			                            *Name, *F[0]));
		}
		SeenNames.Add(Name);
		auto Vec = [&](const TCHAR* Tag, float V[3]) -> bool
		{
			const FXmlNode* E = Root->FindChildNode(Tag);
			if (!E) { return false; }
			V[0] = FCString::Atof(*E->GetAttribute(TEXT("x")));
			V[1] = FCString::Atof(*E->GetAttribute(TEXT("y")));
			V[2] = FCString::Atof(*E->GetAttribute(TEXT("z")));
			return true;
		};
		float BbMin[3], BbMax[3], Bsc[3];
		if (!Vec(TEXT("BoundingBoxMin"), BbMin) || !Vec(TEXT("BoundingBoxMax"), BbMax) ||
		    !Vec(TEXT("BoundingSphereCenter"), Bsc))
		{
			return Fail(FString::Printf(TEXT("missing bounds fields: %s"), *F[0]));
		}
		// FIXED 2026-08-03 - bsRadius was OPTIONAL while its three sibling bounds fields above are
		// hard requirements: a drawable with no <BoundingSphereRadius> shipped bsRadius 0 with
		// ok:true. bsRadius is the archetype's cull sphere, so radius 0 is an entity the engine can
		// cull immediately - an invisible prop reported as a successful export.
		const FXmlNode* RNode = Root->FindChildNode(TEXT("BoundingSphereRadius"));
		if (!RNode) { return Fail(FString::Printf(TEXT("missing bounds fields: %s"), *F[0])); }
		const float Bsr = FCString::Atof(*RNode->GetAttribute(TEXT("value")));
		// Collidable iff the drawable embeds a <Bounds> that actually describes collision.
		// ⛔ THIS USED TO REQUIRE <Children>, WHICH IS ONLY TRUE OF A *COMPOSITE* ROOT (fixed
		// 2026-07-31). A phBound root may legitimately be a primitive - Box, Sphere, Cylinder -
		// and those carry no <Children> at all: measured 220 of 1,012 bound-bearing base-game
		// ydr (21.7%; Box 160 / Sphere 53 / Cylinder 7), a figure the converter's own docstring
		// records. Every one of them was exported with the collidable bit CLEAR, so a fifth of
		// all collidable props shipped as pass-through geometry - invisible in the editor,
		// visible only by walking through a crate in game. Presence of a <Bounds> with a known
		// type is the real signal; <Children> is one shape of it.
		// ⚠ The type is an ATTRIBUTE - `<Bounds type="Composite">` - NOT a <Type> child element.
		// Checked against real emitted corpus XML before trusting it: a FindChildNode("Type")
		// test would have compiled, run, and never once fired.
		bool bCollidable = false;
		if (const FXmlNode* B = Root->FindChildNode(TEXT("Bounds")))
		{
			const FString BoundType = B->GetAttribute(TEXT("type")).TrimStartAndEnd();
			if (const FXmlNode* C = B->FindChildNode(TEXT("Children")))
			{
				bCollidable = C->GetChildrenNodes().Num() > 0;
			}
			// A primitive (or BVH/Geometry) root IS collision, with no children to count.
			if (!bCollidable && !BoundType.IsEmpty()
				&& !BoundType.Equals(TEXT("Composite"), ESearchCase::IgnoreCase))
			{
				bCollidable = true;
			}
		}
		// embedded ShaderGroup TextureDictionary -> empty archetype txd
		bool bEmbeddedTex = false;
		if (const FXmlNode* SG = Root->FindChildNode(TEXT("ShaderGroup")))
		{
			if (const FXmlNode* TD = SG->FindChildNode(TEXT("TextureDictionary")))
			{
				bEmbeddedTex = TD->GetChildrenNodes().Num() > 0;
			}
		}
		const FString Txd = (F.Num() > 1 && !F[1].IsEmpty()) ? F[1] : (bEmbeddedTex ? TEXT("") : Name);
		const FString PhysDict = (F.Num() > 2 && !F[2].IsEmpty()) ? F[2] : (bCollidable ? Name : TEXT(""));
		const uint32 Flags = (bCollidable || !PhysDict.IsEmpty()) ? 537001984u : 536870912u;
		const int32 LodDist = FMath::Max(100, (int32)(Bsr * 4.f));
		// FIXED 2026-08-03 - hdTextureDist was emitted as lodDist. It is the HD-texture streaming
		// radius, NOT the draw distance, and the two are unrelated in the game's own data: measured
		// over 12,582 ASSET_TYPE_DRAWABLE archetypes from a 300-file sample of the resolved base-game
		// ytyp corpus, hdTextureDist == lodDist in 1.61% of archetypes. The distributions barely
		// overlap - modal hdTextureDist 5.0 (3,078), then 50.0 (1,015) and 149.5 (910); modal
		// lodDist 100 (3,020), then 299 (1,096). RUDE emitted >= 100 for EVERY archetype, i.e. at
		// least 20x the game's single most common value, so the engine resident-loaded HD textures
		// for every RUDE prop far earlier than for a base-game prop and any streaming/memory
		// comparison between a RUDE area and a Rockstar area was measuring this default.
		// The 3.0 factor is the measured median hdTextureDist/bsRadius ratio (p25 1.19, median 2.94,
		// p75 8.70) and the 5..150 clamp brackets the observed value band (p10 5, p50 50, p90 168).
		// HONEST LIMIT: the real distribution is multi-modal and per-archetype intent is NOT
		// recoverable from bsRadius alone - this is a defensible default, not a recovered value.
		const int32 HdDist = FMath::Clamp((int32)(Bsr * 3.f), 5, 150);
		Archetypes += FString::Printf(TEXT(
			"  <Item type=\"CBaseArchetypeDef\">\n"
			"   <lodDist value=\"%d\" />\n   <flags value=\"%u\" />\n"
			"   <specialAttribute value=\"0\" />\n"
			"   <bbMin x=\"%f\" y=\"%f\" z=\"%f\" />\n   <bbMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"
			"   <bsCentre x=\"%f\" y=\"%f\" z=\"%f\" />\n   <bsRadius value=\"%f\" />\n"
			"   <hdTextureDist value=\"%d\" />\n   <name>%s</name>\n"
			"   <textureDictionary>%s</textureDictionary>\n   <clipDictionary />\n"
			"   <drawableDictionary />\n   <physicsDictionary>%s</physicsDictionary>\n"
			"   <assetType>ASSET_TYPE_DRAWABLE</assetType>\n   <assetName>%s</assetName>\n"
			"   <extensions />\n  </Item>\n"),
			LodDist, Flags, BbMin[0], BbMin[1], BbMin[2], BbMax[0], BbMax[1], BbMax[2],
			Bsc[0], Bsc[1], Bsc[2], Bsr, HdDist, *Name, *Txd, *PhysDict, *Name);
	}
	const FString Ytyp = FString::Printf(TEXT(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapTypes>\n <extensions />\n <archetypes>\n%s"
		" </archetypes>\n <name>%s</name>\n <dependencies />\n"
		" <compositeEntityTypes itemType=\"CCompositeEntityType\" />\n</CMapTypes>\n"),
		*Archetypes, *YtypName);
	if (!FFileHelper::SaveStringToFile(Ytyp, *OutYtypPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write ytyp"));
	}
	return FString::Printf(TEXT("{\"ok\":true,\"ytypPath\":\"%s\",\"archetypes\":%d}"),
		*OutYtypPath, Specs.Num());
}

// Ported from the in-game-proven tools/emit_ymap.py (P0-validated placement lane;
// EXPORT-side transform + quat conventions, bench-pinned).
FString URudeToolset::ExportYmap(const FString& EntitiesJsonPath, const FString& MapName,
                                 const FString& OutDir)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *EntitiesJsonPath)) { return Fail(TEXT("cannot read entities JSON")); }
	TArray<TSharedPtr<FJsonValue>> Ents;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Ents) || Ents.Num() == 0)
		{
			return Fail(TEXT("entities JSON must be a non-empty array"));
		}
	}
	FString Rows;
	double MinX = 1e18, MinY = 1e18, MinZ = 1e18, MaxX = -1e18, MaxY = -1e18, MaxZ = -1e18;
	int32 Count = 0;
	// ⛔ A FIELD-LESS ENTITY USED TO EXPORT AS A SUCCESS (fixed 2026-08-03).
	// FJsonObject::GetStringField / GetNumberField DO NOT fail on a missing field - they log a
	// LogJson warning and hand back ""/0.0 (UE 5.8 JsonObject.cpp:474/404 -> GetField:338 returns
	// FJsonValueNull; JsonValue.cpp:26/13). So {"ue":{}} became <archetypeName></archetypeName> at
	// (0,0,0), that origin was folded into streamingExtents, and the call returned ok:true with the
	// bogus entity COUNTED. Measured on the 12-entity showcase scene: ONE such entity inflates the
	// streaming box from 636x666 m to 828x1570 m - 3.1x the area - which changes when the whole
	// resource streams in. <archetypeName> is never empty in the game's own data (0 of 136,786
	// across 900 base-game ymaps). Three further paths dropped an entity with an uncounted
	// `continue`, so an upstream producer bug silently shrank the map with nothing in the JSON
	// saying so.
	// An entity the caller asked us to place that cannot exist in game is a REFUSAL, not a default,
	// and it is refused BEFORE it touches the extents fold.
	for (int32 EntIdx = 0; EntIdx < Ents.Num(); ++EntIdx)
	{
		const TSharedPtr<FJsonValue>& V = Ents[EntIdx];
		const TSharedPtr<FJsonObject>* E;
		if (!V.IsValid() || !V->TryGetObject(E))
		{
			return Fail(FString::Printf(TEXT("entity %d is not a JSON object"), EntIdx));
		}
		const TSharedPtr<FJsonObject>* Ue;
		if (!(*E)->TryGetObjectField(TEXT("ue"), Ue))
		{
			return Fail(FString::Printf(TEXT("entity %d has no 'ue' object (want {x,y,z} in UE cm)"), EntIdx));
		}
		FString Arch;
		if (!(*E)->TryGetStringField(TEXT("archetype"), Arch) || Arch.TrimStartAndEnd().IsEmpty())
		{
			return Fail(FString::Printf(
				TEXT("entity %d has no non-empty 'archetype'; an empty archetypeName resolves to no archetype in game"),
				EntIdx));
		}
		// ExportYtyp lowercases the archetype name it emits (:3892) while this lane used to pass the
		// caller's string through verbatim - the two halves of RUDE's own round trip could disagree
		// on case, and the symptom would be "the prop just doesn't appear" with a ytyp and a ymap
		// that both read correctly by eye. Every real archetype name in the sampled corpus is
		// lowercase, so normalising here makes the question moot rather than betting on the
		// downstream converter hashing case-insensitively.
		Arch = Arch.TrimStartAndEnd().ToLower();
		if (Arch.Contains(TEXT("<")) || Arch.Contains(TEXT(">")) || Arch.Contains(TEXT("&"))
			|| Arch.Contains(TEXT("\"")) || Arch.Contains(TEXT("'")))
		{
			// this value is interpolated straight into XML with no escaping
			return Fail(FString::Printf(
				TEXT("entity %d archetype '%s' contains an XML-special character"), EntIdx, *Arch));
		}
		double Ux = 0, Uy = 0, Uz = 0;
		if (!(*Ue)->TryGetNumberField(TEXT("x"), Ux) || !(*Ue)->TryGetNumberField(TEXT("y"), Uy)
			|| !(*Ue)->TryGetNumberField(TEXT("z"), Uz))
		{
			return Fail(FString::Printf(TEXT("entity %d ('%s') is missing ue.x / ue.y / ue.z"), EntIdx, *Arch));
		}
		const double X = Ux / 100.0;
		const double Y = -Uy / 100.0;
		const double Z = Uz / 100.0;
		double Qx = 0, Qy = 0, Qz = 0, Qw = 1;
		const TSharedPtr<FJsonObject>* Q;
		if ((*E)->TryGetObjectField(TEXT("ue_quat"), Q))
		{
			// ⛔⛔ THIS WAS THE INVERSE OF THE CORRECT ROTATION, AND IT SHIPPED (fixed 2026-08-03).
			// The import lane applies g(q) = (x, -y, z, w) (:3146, :3427); this wrote
			// m(q) = (-x, y, -z, w). Both are involutions, but they are DIFFERENT ones, so
			// g(m(q)) = conj(q): a UE -> GTA -> UE round trip inverted every rotation. Every
			// entity authored in Unreal shipped to FiveM facing the wrong way, and the old
			// comment called that "bench-pinned".
			// WHICH ONE IS RIGHT WAS PROVEN AGAINST ROCKSTAR'S OWN DATA, not reasoned: a ymap
			// declares <entitiesExtentsMin/Max>, so transforming each archetype's bbMin/bbMax by
			// the entity transform and unioning must reproduce it. On the single-entity ymap
			// facelobbyfake_lod (one entity, rot z=-0.2377 w=0.9713) the INVERSE reproduces the
			// declared extents EXACTLY (max error 0.0000 m) while the forward quaternion is
			// 22.77 m out in Y - re-verified in this session, independently of the audit that
			// found it. Corpus-wide, 81.25% of 1,690,098 entity rotations have forward != inverse
			// and 79.54% differ by more than 5 degrees: unmistakable by eye, had anyone looked at
			// an exported placement in game.
			// ⇒ A ymap <rotation> stores the entity's INVERSE orientation, so ue->gta is
			// conj(mirror(q_ue)) == (x, -y, z, w) - THE SAME involution the import lane uses. The
			// two lanes must share this formula; it is its own inverse.
			// ⚠ NOT a global rule: a phBound CompositeTransform stores a FORWARD matrix and keeps
			// the pure mirror at :1480 (verified 4,031/4,031 real composite children). The
			// distinction is "ymap entity = inverse-stored, phBound = forward-stored".
			// A PARTIAL ue_quat used to default silently: a missing "w" became 0, giving a
			// zero-norm quaternion that no rotation can be recovered from, with ok:true.
			double Rx = 0, Ry = 0, Rz = 0, Rw = 0;
			if (!(*Q)->TryGetNumberField(TEXT("x"), Rx) || !(*Q)->TryGetNumberField(TEXT("y"), Ry)
				|| !(*Q)->TryGetNumberField(TEXT("z"), Rz) || !(*Q)->TryGetNumberField(TEXT("w"), Rw))
			{
				return Fail(FString::Printf(
					TEXT("entity %d ('%s') has a partial 'ue_quat'; x,y,z,w are all required "
					     "(a missing w defaults to 0 = a zero-norm quaternion)"), EntIdx, *Arch));
			}
			const double N2 = Rx * Rx + Ry * Ry + Rz * Rz + Rw * Rw;
			if (FMath::Abs(N2 - 1.0) > 1e-3)
			{
				return Fail(FString::Printf(
					TEXT("entity %d ('%s') ue_quat is not unit-length (|q|^2 = %f)"), EntIdx, *Arch, N2));
			}
			// ⚠ the involution below is the conductor's 2026-08-03 rotation fix - (x, -y, z, w),
			// the SAME one the import lane uses. Do not "restore" (-x, y, -z, w): that is the
			// inverse, and it is what shipped every UE-authored entity facing the wrong way.
			Qx = Rx;
			Qy = -Ry;
			Qz = Rz;
			Qw = Rw;
		}
		MinX = FMath::Min(MinX, X); MinY = FMath::Min(MinY, Y); MinZ = FMath::Min(MinZ, Z);
		MaxX = FMath::Max(MaxX, X); MaxY = FMath::Max(MaxY, Y); MaxZ = FMath::Max(MaxZ, Z);
		// EntIdx folded in 2026-08-03: the guid was a CRC over map:arch:x:y:z, so two entities of the
		// same archetype within 1e-6 m collided, and an entity-dedup pass downstream would fold them
		// to one. The index makes it unconditionally unique.
		const uint32 Guid = FCrc::StrCrc32(*FString::Printf(TEXT("%s:%d:%s:%f:%f:%f"), *MapName, EntIdx, *Arch, X, Y, Z));
		Rows += FString::Printf(TEXT(
			"  <Item type=\"CEntityDef\">\n   <archetypeName>%s</archetypeName>\n"
			"   <flags value=\"1572864\" />\n   <guid value=\"%u\" />\n"
			"   <position x=\"%f\" y=\"%f\" z=\"%f\" />\n"
			"   <rotation x=\"%f\" y=\"%f\" z=\"%f\" w=\"%f\" />\n"
			"   <scaleXY value=\"1\" />\n   <scaleZ value=\"1\" />\n   <parentIndex value=\"-1\" />\n"
			"   <lodDist value=\"500\" />\n   <childLodDist value=\"0\" />\n"
			"   <lodLevel>LODTYPES_DEPTH_ORPHANHD</lodLevel>\n   <numChildren value=\"0\" />\n"
			"   <priorityLevel>PRI_REQUIRED</priorityLevel>\n   <extensions />\n"
			"   <ambientOcclusionMultiplier value=\"255\" />\n"
			"   <artificialAmbientOcclusion value=\"255\" />\n   <tintValue value=\"0\" />\n  </Item>\n"),
			*Arch, Guid, X, Y, Z, Qx, Qy, Qz, Qw);
		++Count;
	}
	if (Count == 0) { return Fail(TEXT("no valid entities")); }
	const double M = 10.0, S = 300.0;
	const FString Ymap = FString::Printf(TEXT(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapData>\n <name>%s</name>\n <parent />\n"
		" <flags value=\"0\" />\n <contentFlags value=\"1\" />\n"
		" <streamingExtentsMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"
		" <streamingExtentsMax x=\"%f\" y=\"%f\" z=\"%f\" />\n"
		" <entitiesExtentsMin x=\"%f\" y=\"%f\" z=\"%f\" />\n"
		" <entitiesExtentsMax x=\"%f\" y=\"%f\" z=\"%f\" />\n <entities>\n%s </entities>\n"
		" <containerLods itemType=\"rage__fwContainerLodDef\" />\n <boxOccluders itemType=\"BoxOccluder\" />\n"
		" <occludeModels itemType=\"OccludeModel\" />\n <physicsDictionaries />\n <instancedData>\n"
		"  <ImapLink />\n  <PropInstanceList itemType=\"rage__fwPropInstanceListDef\" />\n"
		"  <GrassInstanceList itemType=\"rage__fwGrassInstanceListDef\" />\n </instancedData>\n"
		" <timeCycleModifiers itemType=\"CTimeCycleModifier\" />\n <carGenerators itemType=\"CCarGen\" />\n"
		" <LODLightsSOA>\n  <direction itemType=\"FloatXYZ\" />\n  <falloff />\n  <falloffExponent />\n"
		"  <timeAndStateFlags />\n  <hash />\n  <coneInnerAngle />\n  <coneOuterAngleOrCapExt />\n"
		"  <coronaIntensity />\n </LODLightsSOA>\n <DistantLODLightsSOA>\n"
		"  <position itemType=\"FloatXYZ\" />\n  <RGBI />\n  <numStreetLights value=\"0\" />\n"
		"  <category value=\"0\" />\n </DistantLODLightsSOA>\n <block>\n  <version value=\"0\" />\n"
		"  <flags value=\"0\" />\n  <name>%s</name>\n  <exportedBy>RUDE</exportedBy>\n  <owner></owner>\n"
		"  <time></time>\n </block>\n</CMapData>\n"),
		*MapName,
		MinX - S, MinY - S, MinZ - S, MaxX + S, MaxY + S, MaxZ + S,
		MinX - M, MinY - M, MinZ - M, MaxX + M, MaxY + M, MaxZ + M,
		*Rows, *MapName);
	const FString StreamDir = OutDir / TEXT("stream");
	IFileManager::Get().MakeDirectory(*StreamDir, true);
	const FString YmapPath = StreamDir / (MapName + TEXT(".ymap"));
	if (!FFileHelper::SaveStringToFile(Ymap, *YmapPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Fail(TEXT("failed to write ymap"));
	}
	// ⛔ MERGE, NEVER OVERWRITE (AGENTS §9, fixed in maintainer lane `product_debt`). This write was
	// unconditional: a second export into a resource folder someone had added `client_script` or
	// `files` to deleted those lines, silently, and the write result was not even tested. A resource
	// folder is the user's. RudeMergeManifest keeps every byte the file already has and appends only
	// the directives it does not already declare - so a hand-added line survives any number of
	// exports, and the second export over a complete manifest writes nothing at all.
	const TArray<FString> RequiredManifest = {
		TEXT("fx_version 'cerulean'"),
		TEXT("game 'gta5'"),
		TEXT(""),
		TEXT("author 'RUDE - RAGE <-> Unreal Development Environment'"),
		TEXT("description 'RUDE-authored placement resource'"),
		TEXT(""),
		TEXT("-- Required for streamed ymaps to take effect (reloads map storage on load)."),
		TEXT("this_is_a_map 'yes'")
	};
	int32 ManifestPreserved = 0, ManifestAlready = 0, ManifestAdded = 0;
	FString ManifestError;
	const bool bManifest = RudeMergeManifest(OutDir / TEXT("fxmanifest.lua"), RequiredManifest,
		ManifestPreserved, ManifestAlready, ManifestAdded, ManifestError);
	// ok is COMPUTED over BOTH writes: a ymap whose resource has no manifest does not load, so a
	// verdict that said ok:true on the strength of the ymap alone would be lying about the product.
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"ymapPath\":\"%s\",\"entities\":%d,\"manifestLinesPreserved\":%d,")
		TEXT("\"manifestDirectivesAlreadyPresent\":%d,\"manifestDirectivesAdded\":%d,\"manifestError\":\"%s\"}"),
		bManifest ? TEXT("true") : TEXT("false"), *YmapPath, Count,
		ManifestPreserved, ManifestAlready, ManifestAdded, *RudeJsonEscape(ManifestError));
}

FString URudeToolset::ImportYdrBatch(const FString& ListPath, const FString& DestFolder,
                                     const FString& Mode, const FString& CorpusRoot)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *ListPath))
	{
		return Fail(TEXT("cannot read list file"));
	}
	const bool bForce = Mode.TrimStartAndEnd().Equals(TEXT("FORCE"), ESearchCase::IgnoreCase);
	// ⭐ CorpusRoot is OPTIONAL and NEW (2026-08-05, #43). This batch takes a list of absolute XML
	// paths and has no archetype - which is exactly why the 08-05 400-drawable run tie-broke 95.8%
	// of its texture binds. Given a corpus root it builds the SAME archetype index the map lane
	// builds and scopes each file's textures by its archetype's declared <textureDictionary>.
	// Left empty it behaves exactly as before, so no existing 3-argument caller changes meaning:
	// FRudeInvoke::Call performs no arity check and pads a missing trailing argument with an empty
	// FString (the same property that let ImportMapArea gain Mode).
	// ⛔ Deliberately NOT derived from the list paths. Guessing "..\..\" off the first line would
	// silently point at whatever happened to be two directories up and produce a scope nobody
	// asked for - and a wrong scope is a wrong texture, which is the defect this fixes.
	FRudeArchetypeIndex ScopeIndex;
	bool bHaveScope = false;
	if (!CorpusRoot.TrimStartAndEnd().IsEmpty())
	{
		FString IndexErr;
		if (!BuildCorpusArchetypeIndex(CorpusRoot, ScopeIndex, IndexErr, /*MloSearch*/ nullptr))
		{
			// REFUSE, do not carry on unscoped. A CorpusRoot was passed on purpose; silently
			// ignoring it would report the pre-fix numbers under a post-fix command line.
			return Fail(FString::Printf(TEXT("CorpusRoot given but the archetype index failed: %s"),
				*IndexErr));
		}
		bHaveScope = true;
	}
	int32 Imported = 0, Skipped = 0, Failed = 0;
	// ⛔ THE BATCH USED TO THROW THESE AWAY, and that is why "did the rebind work?" was
	// unanswerable after 4,956 files ran with ok:true (2026-07-29). Every file reported its own
	// texture verdict; the batch summed only ok/skip/fail, so a run that bound ZERO textures and a
	// run that bound all of them printed the identical line. A batch must aggregate the counters
	// its unit reports - a silent contributor has to be as loud as a failing one.
	int32 Bound = 0, Unsupported = 0, MissingTex = 0, UnmappedSamp = 0;
	int32 ValSeen = 0, ValBound = 0, ValUnsupported = 0, ValDeduped = 0;
	// ⛔ THE BATCH SUMMED SEVEN FIELDS AND STOPPED, and the two it left out were the ones that
	// report LOST GEOMETRY. ImportYdr has emitted geometriesDropped + geometryErrors since
	// 2026-07-31; the batch never read them, so a run in which a fraction of every mesh failed to
	// parse printed imported=N with nothing else moving - the identical failure this batch was
	// fixed for on 2026-07-29, one level down. MEASURED drop rate on today's corpus is 0 (900
	// resolved ydr / 3,444 geometries: no unknown semantics, no misalignment), which is exactly
	// when a blind spot goes unnoticed. Everything ImportYdr counts is now summed here.
	int32 GeosDropped = 0, GeoErrors = 0, GeosNoUV = 0;
	int32 TrisOOR = 0, TrisDegen = 0;
	int32 FromEmbedded = 0, Ambiguous = 0, NoShaderDef = 0, NoMaterial = 0;
	int32 Scoped = 0, TieBroken = 0;      // #43 split
	int32 FilesWithScope = 0;             // files whose archetype named a textureDictionary
	// #43/#21b per-tier split + the reason a tie-break was still needed.
	int32 AmbTotal = 0, FromArchTxd = 0, FromParentTxd = 0, FromYtyp = 0, FromSlot = 0;
	int32 ScopedAuth = 0, ScopedProv = 0;
	int32 TbEmbedded = 0, TbNoScope = 0, TbSlotAmb = 0, TbYtypAmb = 0, TbDictAbsent = 0, TbNotInScope = 0;
	// Per-FILE scope availability. Separated because "no chain" and "an unresolvable hash_ scope"
	// are different gaps with different fixes, and one number would hide both.
	int32 FilesWithParentChain = 0;       // archetype txd has at least one gtxd ancestor
	int32 FilesWithHashTxd = 0;           // <textureDictionary> is an unresolved joaat (hash_XXXXXXXX)
	int32 FilesWithSlot = 0;              // _RESOLVED.json knows which slot this asset was won from
	int32 FilesWithYtypSet = 0;           // the asset's ytyp declares at least one dictionary
	// #40: the batch sums EVERY counter its unit reports, including the new collision ones. A batch
	// that summed geometry but not collision would be the same blind spot this batch has now been
	// fixed for twice (textures 2026-07-29, geometry 2026-07-31).
	int32 ColSeen = 0, ColPrims = 0, ColMeshes = 0, ColUnmapped = 0, ColMalformed = 0;
	int32 ColPolysDropped = 0, ColTris = 0, FilesWithCollision = 0;
	FString FailedFiles;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString Path = Lines[i].TrimStartAndEnd();
		if (Path.IsEmpty()) { continue; }
		// skip-if-exists on the FILENAME base (corpus files are named <drawable>.ydr.xml,
		// matching the drawable <Name> ImportYdr derives) - idempotent re-runs.
		// FORCE mode reimports in place (MI re-bind after a texture pass).
		FString Base = FPaths::GetBaseFilename(Path);
		Base.RemoveFromEnd(TEXT(".ydr"));
		if (!bForce && FPackageName::DoesPackageExist(DestFolder / Base))
		{
			++Skipped;
			continue;
		}
		// The list names <drawable>.ydr.xml, and the archetype index is keyed on the drawable
		// assetName - the same join ImportIndexedDrawable makes, so the two lanes cannot disagree
		// about which dictionary a mesh belongs to.
		FRudeTextureScope Scope;
		if (bHaveScope)
		{
			Scope = ScopeIndex.MakeScope(Base.ToLower());
			if (!Scope.ArchetypeTxd.IsEmpty())
			{
				++FilesWithScope;
				if (Scope.ArchetypeTxd.StartsWith(TEXT("hash_"))) { ++FilesWithHashTxd; }
			}
			if (Scope.ParentTxdChain.Num() > 0) { ++FilesWithParentChain; }
			if (Scope.YtypNeighbours && Scope.YtypNeighbours->Num() > 0) { ++FilesWithYtypSet; }
			if (!Scope.AssetSlot.IsEmpty()) { ++FilesWithSlot; }
		}
		const FString R = RudeImportYdrScoped(Path, DestFolder, bHaveScope ? &Scope : nullptr);
		// ⚠ Counters are read on BOTH paths, matching FRudeImportTally. Since ImportYdr's ok is now
		// computed (slotsWithoutMaterial), a mesh can come back ok:false while still carrying the
		// counters that explain WHY - and a hard failure carries none of them, so they read 0.
		// Dropping them because of the boolean is the same blindness one level down.
		Bound          += RudeSumField(R, TEXT("boundTextures"));
		Unsupported    += RudeSumField(R, TEXT("unsupportedByMaster"));
		MissingTex     += RudeSumField(R, TEXT("missingTextures"));
		UnmappedSamp   += RudeSumField(R, TEXT("unmappedSamplers"));
		ValSeen        += RudeSumField(R, TEXT("valueParamsSeen"));
		ValBound       += RudeSumField(R, TEXT("valueParamsBound"));
		ValUnsupported += RudeSumField(R, TEXT("valueParamsUnsupported"));
		ValDeduped     += RudeSumField(R, TEXT("valueParamsDeduped"));
		GeosDropped    += RudeSumField(R, TEXT("geometriesDropped"));
		GeosNoUV       += RudeSumField(R, TEXT("geometriesWithoutUV"));
		TrisOOR        += RudeSumField(R, TEXT("trianglesOutOfRange"));
		TrisDegen      += RudeSumField(R, TEXT("trianglesDegenerate"));
		FromEmbedded   += RudeSumField(R, TEXT("texturesFromEmbedded"));
		Ambiguous      += RudeSumField(R, TEXT("ambiguousTextures"));
		Scoped         += RudeSumField(R, TEXT("texturesResolvedScoped"));
		TieBroken      += RudeSumField(R, TEXT("texturesTieBroken"));
		AmbTotal       += RudeSumField(R, TEXT("texturesAmbiguousTotal"));
		FromArchTxd    += RudeSumField(R, TEXT("texturesFromArchetypeTxd"));
		FromParentTxd  += RudeSumField(R, TEXT("texturesFromParentTxd"));
		FromYtyp       += RudeSumField(R, TEXT("texturesFromYtypNeighbour"));
		FromSlot       += RudeSumField(R, TEXT("texturesFromSameSlot"));
		ScopedAuth     += RudeSumField(R, TEXT("texturesScopedAuthoritative"));
		ScopedProv     += RudeSumField(R, TEXT("texturesScopedProvenance"));
		TbEmbedded     += RudeSumField(R, TEXT("tieBreakEmbeddedNotImported"));
		TbNoScope      += RudeSumField(R, TEXT("tieBreakNoScope"));
		TbSlotAmb      += RudeSumField(R, TEXT("tieBreakSlotAmbiguous"));
		TbYtypAmb      += RudeSumField(R, TEXT("tieBreakYtypAmbiguous"));
		TbDictAbsent   += RudeSumField(R, TEXT("tieBreakScopeDictAbsent"));
		TbNotInScope   += RudeSumField(R, TEXT("tieBreakNameNotInScope"));
		NoShaderDef    += RudeSumField(R, TEXT("slotsWithoutShaderDef"));
		NoMaterial     += RudeSumField(R, TEXT("slotsWithoutMaterial"));
		const int32 SeenHere = RudeSumField(R, TEXT("collisionBoundsSeen"));
		ColSeen        += SeenHere;
		ColPrims       += RudeSumField(R, TEXT("collisionPrimitivesImported"));
		ColMeshes      += RudeSumField(R, TEXT("collisionMeshesImported"));
		ColUnmapped    += RudeSumField(R, TEXT("collisionBoundsUnmapped"));
		ColMalformed   += RudeSumField(R, TEXT("collisionBoundsMalformed"));
		ColPolysDropped+= RudeSumField(R, TEXT("collisionPolysDropped"));
		ColTris        += RudeSumField(R, TEXT("collisionTriangles"));
		if (SeenHere > 0) { ++FilesWithCollision; }
		// geometryErrors is a JSON ARRAY, so it cannot be summed - count the FILES that carry a
		// non-empty one. Zero here alongside a non-zero geometriesDropped would mean every dropped
		// geometry is unexplained, which is itself a defect worth seeing.
		if (R.Contains(TEXT("\"geometryErrors\":[\""))) { ++GeoErrors; }
		if (R.Contains(TEXT("\"ok\":true")))
		{
			++Imported;
		}
		else
		{
			++Failed;
			if (Failed <= 30)
			{
				FailedFiles += FString::Printf(TEXT("%s\"%s\""), FailedFiles.IsEmpty() ? TEXT("") : TEXT(","), *Base);
			}
		}
		if ((i + 1) % 50 == 0)
		{
			UE_LOG(LogTemp, Display,
				TEXT("[RUDE] ImportYdrBatch %d/%d (ok %d, skip %d, fail %d | tex bound %d, "
				     "unsupported %d, missing %d, unmapped %d)"),
				i + 1, Lines.Num(), Imported, Skipped, Failed,
				Bound, Unsupported, MissingTex, UnmappedSamp);
		}
		if ((i + 1) % 250 == 0)
		{
			// Keep editor memory flat on long batches - but with KEEPFLAGS (= RF_Standalone in
			// editor), NEVER RF_NoFlags, which deletes the unsaved meshes this very batch imported.
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ImportYdrBatch DONE: %d imported, %d skipped, %d failed | geometries dropped %d "
		     "(%d files explained), no-UV %d | triangles out-of-range %d, degenerate %d | textures "
		     "bound %d, unsupportedByMaster %d, missing %d, unmappedSamplers %d | ambiguous %d = "
		     "scoped %d (embedded %d + archetypeTxd %d + parentTxd %d | ytypNeighbour %d + sameSlot "
		     "%d) + tieBroken %d [embeddedNotImported %d, noScope %d, slotAmbiguous %d, "
		     "ytypAmbiguous %d, scopeDictAbsent %d, nameNotInScope %d] | files: %d archetype txd "
		     "(%d unresolved hash_), %d parent chain, %d ytyp set, %d slot | slots without shader "
		     "def %d, without material %d | value params seen %d = bound %d + unsupported %d + "
		     "deduped %d | collision: %d files carried bounds, %d seen = %d primitives + %d meshes "
		     "+ %d unmapped + %d malformed, %d tris, %d polys dropped"),
		Imported, Skipped, Failed, GeosDropped, GeoErrors, GeosNoUV, TrisOOR, TrisDegen,
		Bound, Unsupported, MissingTex, UnmappedSamp,
		AmbTotal, Scoped, FromEmbedded, FromArchTxd, FromParentTxd, FromYtyp, FromSlot, TieBroken,
		TbEmbedded, TbNoScope, TbSlotAmb, TbYtypAmb, TbDictAbsent, TbNotInScope,
		FilesWithScope, FilesWithHashTxd, FilesWithParentChain, FilesWithYtypSet, FilesWithSlot,
		NoShaderDef, NoMaterial, ValSeen, ValBound, ValUnsupported, ValDeduped,
		FilesWithCollision, ColSeen, ColPrims, ColMeshes, ColUnmapped, ColMalformed, ColTris,
		ColPolysDropped);
	// ⛔ `ok` IS COMPUTED, NEVER HARDCODED (fixed 2026-08-05). It used to be the literal `true`, so a
	// batch in which EVERY file failed returned `"ok":true,"failed":400` and the commandlet exited 0.
	// Measured, not theorised: a shell-quoting bug produced 400 non-existent paths, and no automated
	// check could tell that run from a clean one - the CLI exit code is derived from `ok`, so the one
	// gate a headless caller has was blind to total failure. Two conditions, both load-bearing:
	//   Failed == 0            - any failure is a failure; `failedFiles` already names them.
	//   Imported + Skipped > 0 - a run that did NOTHING is not a success. An all-skipped batch
	//                            (nothing to re-import outside Mode=FORCE) IS legitimate, which is
	//                            why Skipped counts as work done and Imported alone does not.
	// Same law as ROUT's strict regression gate: a gate that cannot fail is worse than no gate.
	// See ENGINEERING_LOG "MEASUREMENT LAWS".
	const bool bOk = (Failed == 0) && (Imported + Skipped > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"imported\":%d,\"skipped\":%d,\"failed\":%d,")
		TEXT("\"geometriesDropped\":%d,\"filesWithGeometryErrors\":%d,\"geometriesWithoutUV\":%d,")
		TEXT("\"trianglesOutOfRange\":%d,\"trianglesDegenerate\":%d,\"boundTextures\":%d,")
		TEXT("\"texturesFromEmbedded\":%d,\"texturesResolvedScoped\":%d,\"texturesTieBroken\":%d,")
		TEXT("\"filesWithArchetypeTxd\":%d,\"ambiguousTextures\":%d,\"texturesAmbiguousTotal\":%d,")
		TEXT("\"texturesFromArchetypeTxd\":%d,\"texturesFromParentTxd\":%d,")
		TEXT("\"texturesFromYtypNeighbour\":%d,\"texturesFromSameSlot\":%d,")
		TEXT("\"texturesScopedAuthoritative\":%d,\"texturesScopedProvenance\":%d,")
		TEXT("\"tieBreakEmbeddedNotImported\":%d,\"tieBreakNoScope\":%d,")
		TEXT("\"tieBreakSlotAmbiguous\":%d,\"tieBreakYtypAmbiguous\":%d,")
		TEXT("\"tieBreakScopeDictAbsent\":%d,\"tieBreakNameNotInScope\":%d,")
		TEXT("\"filesWithParentChain\":%d,\"filesWithHashTxd\":%d,\"filesWithYtypSet\":%d,")
		TEXT("\"filesWithSlot\":%d,\"gtxdFiles\":%d,\"gtxdRelationships\":%d,\"gtxdRefused\":%d,")
		TEXT("\"resolvedEntries\":%d,")
		TEXT("\"unsupportedByMaster\":%d,\"missingTextures\":%d,\"unmappedSamplers\":%d,")
		TEXT("\"slotsWithoutShaderDef\":%d,\"slotsWithoutMaterial\":%d,")
		TEXT("\"valueParamsSeen\":%d,\"valueParamsBound\":%d,\"valueParamsUnsupported\":%d,")
		TEXT("\"valueParamsDeduped\":%d,\"filesWithCollision\":%d,\"collisionBoundsSeen\":%d,")
		TEXT("\"collisionPrimitivesImported\":%d,\"collisionMeshesImported\":%d,")
		TEXT("\"collisionBoundsUnmapped\":%d,\"collisionBoundsMalformed\":%d,")
		TEXT("\"collisionPolysDropped\":%d,\"collisionTriangles\":%d,\"failedFiles\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"),
		Imported, Skipped, Failed, GeosDropped, GeoErrors, GeosNoUV, TrisOOR, TrisDegen,
		Bound, FromEmbedded, Scoped, TieBroken, FilesWithScope, Ambiguous, AmbTotal,
		FromArchTxd, FromParentTxd, FromYtyp, FromSlot, ScopedAuth, ScopedProv,
		TbEmbedded, TbNoScope, TbSlotAmb, TbYtypAmb, TbDictAbsent, TbNotInScope,
		FilesWithParentChain, FilesWithHashTxd, FilesWithYtypSet, FilesWithSlot,
		ScopeIndex.GtxdFiles, ScopeIndex.GtxdRelationships, ScopeIndex.GtxdRefusals,
		ScopeIndex.ResolvedEntries,
		Unsupported, MissingTex, UnmappedSamp,
		NoShaderDef, NoMaterial, ValSeen, ValBound, ValUnsupported, ValDeduped,
		FilesWithCollision, ColSeen, ColPrims, ColMeshes, ColUnmapped, ColMalformed,
		ColPolysDropped, ColTris, *FailedFiles);
}

// ⛔⛔ WHY THIS WRAPPER EXISTS — `-unattended` SILENTLY CANCELS EVERY SAVE (measured 2026-07-31,
// root cause read out of the engine source, not guessed).
// `FEditorFileUtils::SaveDirtyPackages` → `InternalSavePackages` → `PromptForCheckoutAndSave`,
// which begins (FileHelpers.cpp:4659-4667):
//     if (GIsRunningUnattendedScript) { return UEditorLoadingAndSavingUtils::SavePackages(...); }
//     if (FApp::IsUnattended() && !bAlreadyCheckedOut) { return PR_Cancelled; }
// A commandlet/`-ExecCmds` run sets `FApp::IsUnattended()` but NOT `GIsRunningUnattendedScript`
// (that flag belongs to scripted automation), so the save fell into the SECOND branch: cancelled,
// nothing written, `ok:false`, and the whole chain's work lost with a false-looking summary.
// The engine's own escape hatch is the first branch — it guards with exactly this TGuardValue when
// it needs a modal-free save (FileHelpers.cpp:5919). Setting it ONLY while unattended keeps the
// interactive path (checkout prompts, source control) untouched for a human at the editor.
// ⛔ AND IN A COMMANDLET THERE IS NO SLATE AT ALL (2026-09-05): `SaveDirtyPackages` reaches into
// the Slate application for its notifications and asserted `CurrentBaseApplication.IsValid()`
// the moment the CLI tried to persist 1,954 freshly imported dictionaries. Headless, every dirty
// content package is saved directly through UPackage::SavePackage - no prompt, no notification,
// no Slate - and the count of what was written is what the caller gets.
// The names behind the counts. A verdict that says "saved 3032" and cannot say WHICH is not
// checkable by the caller, and this is the one choke point every agent chain persists through.
// Only the headless leg can fill these: FEditorFileUtils::SaveDirtyPackages returns one bool and
// no names at all, which SaveAssets reports as namesKnown:false rather than as an empty list.
static TArray<FString> GRudeSaveWroteNames, GRudeSaveFailedNames;
int32 GRudeLastSaved = 0, GRudeLastSaveFailed = 0;
bool RudeSaveDirty(bool bMaps, bool bContent)   // declared in RudeToolsetInternal.h
{
	GRudeLastSaved = 0; GRudeLastSaveFailed = 0;
	GRudeSaveWroteNames.Reset(); GRudeSaveFailedNames.Reset();
	if (!FSlateApplication::IsInitialized())
	{
		TArray<UPackage*> Dirty;
		if (bContent) { FEditorFileUtils::GetDirtyContentPackages(Dirty); }
		if (bMaps) { FEditorFileUtils::GetDirtyWorldPackages(Dirty); }
		for (UPackage* Pkg : Dirty)
		{
			if (!Pkg) { continue; }
			const bool bIsMap = UWorld::FindWorldInPackage(Pkg) != nullptr;
			const FString Ext = bIsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Pkg->GetName(), Filename, Ext))
			{
				++GRudeLastSaveFailed; GRudeSaveFailedNames.Add(Pkg->GetName()); continue;
			}
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			Args.Error = GWarn;
			// A FORCED re-import builds a NEW in-memory package over a file that already exists on
			// disk, and the raw save answers Canceled (measured 2026-09-05: 6,888 canceled, every
			// one an existing file; 3,032 new files saved). Set the old file aside, save, then
			// drop the old copy - and put it back if the save fails, so a failure costs nothing.
			IFileManager& FM = IFileManager::Get();
			const FString Aside = Filename + TEXT(".rude_prev");
			const bool bExisted = FM.FileExists(*Filename);
			if (bExisted) { FM.Delete(*Aside, false, true, true); FM.Move(*Aside, *Filename, true, true, true, true); }
			const FSavePackageResultStruct R = UPackage::Save(Pkg, nullptr, *Filename, Args);
			if (R == ESavePackageResult::Success)
			{
				++GRudeLastSaved;
				GRudeSaveWroteNames.Add(Filename);
				if (bExisted) { FM.Delete(*Aside, false, true, true); }
			}
			else
			{
				++GRudeLastSaveFailed;
				GRudeSaveFailedNames.Add(Pkg->GetName() + FString::Printf(TEXT(" (result %d)"), (int32)R.Result));
				if (bExisted) { FM.Move(*Filename, *Aside, true, true, true, true); }
				UE_LOG(LogTemp, Warning, TEXT("[RUDE] save FAILED %s -> %s (result %d%s)"), *Pkg->GetName(), *Filename,
					(int32)R.Result, bExisted ? TEXT(", existing file restored") : TEXT(""));
			}
		}
		return GRudeLastSaveFailed == 0;
	}
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript,
		FApp::IsUnattended() ? true : GIsRunningUnattendedScript);
	return FEditorFileUtils::SaveDirtyPackages(
		/*bPromptUserToSave*/ false, bMaps, bContent, /*bFastSave*/ false,
		/*bNotifyNoPackagesSaved*/ false, /*bCanBeDeclined*/ false);
}

FString URudeToolset::SaveAssets()
{
	// ⛔ THE COMPILE-BEFORE-SAVE LAW, ENFORCED HERE (it was documented but wired NOWHERE - the
	// BUILD_AREA_DESIGN grounded catch): saving while async texture/mesh builds are in flight is
	// exactly the 381-asset bulkdata corruption incident. Block until every compilation settles,
	// THEN save. This is the single choke point every agent chain saves through.
	FAssetCompilingManager::Get().FinishAllCompilation();
	// Content packages only (bSaveMapPackages=false) - an agent persisting its imports must not
	// silently commit the operator's level edits.
	const bool bOk = RudeSaveDirty(/*bMaps*/ false, /*bContent*/ true);
	// ⭐ WHAT IT WROTE, NOT JUST HOW MANY (maintainer lane `product_debt`). The headless leg saves
	// each package by name, so it can name them; the interactive leg goes through
	// FEditorFileUtils::SaveDirtyPackages, which reports one overall bool and no names at all -
	// that is reported as namesKnown:false rather than as an empty list, because an empty list and
	// "this path cannot tell you" are different facts and only one of them is a pass.
	const bool bNames = !FSlateApplication::IsInitialized();
	FString WroteJson, FailedJson;
	for (int32 Ni = 0; Ni < GRudeSaveWroteNames.Num() && Ni < 40; ++Ni)
	{
		WroteJson += FString::Printf(TEXT("%s\"%s\""), WroteJson.IsEmpty() ? TEXT("") : TEXT(","),
			*RudeJsonEscape(GRudeSaveWroteNames[Ni]));
	}
	for (int32 Ni = 0; Ni < GRudeSaveFailedNames.Num() && Ni < 40; ++Ni)
	{
		FailedJson += FString::Printf(TEXT("%s\"%s\""), FailedJson.IsEmpty() ? TEXT("") : TEXT(","),
			*RudeJsonEscape(GRudeSaveFailedNames[Ni]));
	}
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"saved\":%d,\"saveFailed\":%d,\"headless\":%s,\"unattended\":%s,")
		TEXT("\"namesKnown\":%s,\"wrote\":[%s],\"couldNotWrite\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"), GRudeLastSaved, GRudeLastSaveFailed,
		FSlateApplication::IsInitialized() ? TEXT("false") : TEXT("true"),
		FApp::IsUnattended() ? TEXT("true") : TEXT("false"),
		bNames ? TEXT("true") : TEXT("false"), *WroteJson, *FailedJson);
}

FString URudeToolset::SetWorldHour(const FString& Hour)
{
	// ⭐ THE DAY/NIGHT DATASET, DRIVEN (2026-07-30, Matt corrected the model that produced this).
	// GTA does not fade lit windows in a shader - it ships 3,936 CTimeArchetypeDef whose `timeFlags`
	// is a 24-bit mask, bit N meaning "visible during hour N". The common masks are night windows
	// (hours 0-5 + 20-23). ImportScene groups every gated archetype into its own ISM component
	// tagged RUDE_TIME:<mask>, so setting the hour is a visibility sweep over exactly those
	// components and nothing else.
	//
	// ⛔ WHY NOT A SHADER GATE: I first multiplied emissive by a global NightFactor. It looked
	// right and was wrong - a UE-only invention that cannot round-trip to GTA, and round-trip is
	// one of the only two places fidelity actually matters here. The mask is the game's own data;
	// driving it keeps import and export talking about the same thing.
	const FString H = Hour.TrimStartAndEnd();
	if (H.IsEmpty() || !H.IsNumeric())
	{
		return TEXT("{\"ok\":false,\"error\":\"Hour must be 0-23\"}");
	}
	const int32 Hr = FCString::Atoi(*H);
	if (Hr < 0 || Hr > 23)
	{
		return TEXT("{\"ok\":false,\"error\":\"Hour must be 0-23\"}");
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { return TEXT("{\"ok\":false,\"error\":\"no editor world\"}"); }

	const uint32 Bit = 1u << Hr;
	int32 Gated = 0, Shown = 0, Hidden = 0;
	// The sun follows the hour too (2026-09-06): elevation = 90 sin(pi (h-6)/12) - up at 06:00, noon at
	// 12:00, down at 18:00, below the horizon at night - on the RUDE_SKY directional light. A rough
	// time-of-day for captures and editing; the sandbox shim runs its own clock in PIE.
	int32 SunMoved = 0; (void)SunMoved;
	{
		const double Elev = 90.0 * FMath::Sin(PI * (Hr - 6.0) / 12.0);
		for (TActorIterator<ADirectionalLight> SIt(World); SIt; ++SIt)
		{
			if (!SIt->ActorHasTag(FName(TEXT("RUDE_SKY")))) { continue; }
			FRotator R = SIt->GetActorRotation();
			R.Pitch = (float)-Elev;
			SIt->SetActorRotation(R);
			SIt->MarkPackageDirty();
			++SunMoved;
		}
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		// the district's entity actors carry the mask on their UStaticMeshComponent (RudeSpawnEntityActor);
		// ImportScene's ISM path carries it on the instanced component - both are swept
		TArray<UStaticMeshComponent*> Comps;
		It->GetComponents<UStaticMeshComponent>(Comps);
		for (UStaticMeshComponent* C : Comps)
		{
			for (const FName& Tag : C->ComponentTags)
			{
				FString T = Tag.ToString();
				if (!T.StartsWith(TEXT("RUDE_TIME:"))) { continue; }
				T.RightChopInline(10);
				const uint32 Mask = (uint32)FCString::Strtoui64(*T, nullptr, 10);
				const bool bVisible = (Mask & Bit) != 0;
				C->SetVisibility(bVisible, /*bPropagateToChildren*/ true);
				C->SetHiddenInGame(!bVisible);
				++Gated;
				bVisible ? ++Shown : ++Hidden;
				break;
			}
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] SetWorldHour %02d:00 - %d gated components, %d shown, %d hidden"),
		Hr, Gated, Shown, Hidden);
	return FString::Printf(
		TEXT("{\"ok\":true,\"hour\":%d,\"gatedComponents\":%d,\"shown\":%d,\"hidden\":%d}"),
		Hr, Gated, Shown, Hidden);
}

FString URudeToolset::FixLevelRefs(const FString& Mode)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return TEXT("{\"ok\":false,\"error\":\"no editor world\"}");
	}
	const bool bApply = Mode.TrimStartAndEnd().Equals(TEXT("APPLY"), ESearchCase::IgnoreCase);

	// Collect first, mutate second - RemoveStreamingLevel edits the array we would be walking.
	TArray<ULevelStreaming*> Dangling;
	FString Names;
	const TArray<ULevelStreaming*>& Streaming = World->GetStreamingLevels();
	const int32 Checked = Streaming.Num();
	for (ULevelStreaming* Level : Streaming)
	{
		if (!Level)
		{
			continue;
		}
		const FString PackageName = Level->GetWorldAssetPackageName();
		// DoesPackageExist is the authority here, not the asset registry: a package deleted while
		// the editor was open can still sit in the registry's cache, which is exactly the state
		// that produces the load error.
		if (PackageName.IsEmpty() || !FPackageName::DoesPackageExist(PackageName))
		{
			Dangling.Add(Level);
			Names += FString::Printf(TEXT("%s\"%s\""), Names.IsEmpty() ? TEXT("") : TEXT(","),
			                         *PackageName);
		}
	}

	// ⭐ AND THE OTHER KIND, which is the one that actually bit (2026-07-29): a Level Instance is
	// an ACTOR holding a soft world-asset pointer, not an entry in the streaming array. Delete the
	// level package and the persistent map still spawns an ALevelInstance pointing nowhere - it
	// reports the same "Failed to find streamed level ..." text, so the message alone does not
	// tell you which of the two you have. Checking only the streaming array reported
	// "checked:0, dangling:0" on a map that was visibly broken. Check both, always.
	TArray<ALevelInstance*> DanglingLI;
	for (TActorIterator<ALevelInstance> It(World); It; ++It)
	{
		ALevelInstance* LI = *It;
		if (!LI) { continue; }
		const FString Pkg = LI->GetWorldAssetPackage();
		if (Pkg.IsEmpty() || !FPackageName::DoesPackageExist(Pkg))
		{
			DanglingLI.Add(LI);
			Names += FString::Printf(TEXT("%s\"%s (LevelInstance)\""),
			                         Names.IsEmpty() ? TEXT("") : TEXT(","), *Pkg);
		}
	}

	// ⭐⭐ AND THE THIRD KIND, which is the one that was ACTUALLY broken (2026-07-29). On a WORLD
	// PARTITION map every actor is its own external package and is NOT LOADED at startup, so
	// TActorIterator sees none of them: both checks above returned a confident "0 dangling" for a
	// map that threw "Failed to find streamed level" on every open. A check that cannot see the
	// broken thing is worse than no check - it reports healthy.
	// The asset registry knows the dependency graph WITHOUT loading anything, so ask it: does any
	// external actor package of this world depend on a /Game package that no longer exists? That
	// is the dangling reference, found headlessly and by name.
	// ⛔ Do NOT try to answer this by grepping the .umap - an object path is not stored as plain
	// text there, and that assumption is what produced this broken state to begin with.
	TArray<FString> DanglingActorPkgs;
	{
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		const FString ExtPath = ULevel::GetExternalActorsPath(World->GetPackage()->GetName());
		if (!ExtPath.IsEmpty())
		{
			AR.ScanPathsSynchronous({ ExtPath }, /*bForceRescan*/ true);
			if (AR.IsLoadingAssets()) { AR.WaitForCompletion(); }
			TArray<FAssetData> ActorAssets;
			AR.GetAssetsByPath(FName(*ExtPath), ActorAssets, /*bRecursive*/ true);
			for (const FAssetData& AD : ActorAssets)
			{
				TArray<FName> Deps;
				AR.GetDependencies(AD.PackageName, Deps,
				                   UE::AssetRegistry::EDependencyCategory::Package);
				for (const FName& Dep : Deps)
				{
					const FString DepStr = Dep.ToString();
					if (!DepStr.StartsWith(TEXT("/Game/"))) { continue; }
					if (FPackageName::DoesPackageExist(DepStr)) { continue; }
					DanglingActorPkgs.AddUnique(AD.PackageName.ToString());
					Names += FString::Printf(TEXT("%s\"%s -> MISSING %s\""),
					                         Names.IsEmpty() ? TEXT("") : TEXT(","),
					                         *AD.PackageName.ToString(), *DepStr);
				}
			}
		}
	}

	// ⭐⭐ AND THE PLACE I NEVER LOOKED - which is where it actually was (2026-07-30, reproduced by
	// Matt on Lvl_ThirdPerson while all three checks above reported clean).
	// The MAP PACKAGE ITSELF depends on the missing levels. Asking the registry
	// GetDependencies(<world package>) listed /Game/RUDE/Areas/DowntownHL3, HL4 and HL5 directly -
	// not via any external actor. So the reference lives in the world's own saved package, which is
	// why the streaming array was empty, no LevelInstance actor was loaded, and the external-actor
	// sweep found nothing. Three checks, all looking past the obvious one.
	// ⚠ A stale import like this is dropped by RE-SAVING the map, because nothing live holds it.
	// That is the repair, and APPLY verifies it afterwards rather than assuming.
	TArray<FString> DanglingMapDeps;
	{
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		const FName WorldPkg(*World->GetPackage()->GetName());
		TArray<FName> Deps;
		AR.GetDependencies(WorldPkg, Deps, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& Dep : Deps)
		{
			const FString D = Dep.ToString();
			if (!D.StartsWith(TEXT("/Game/"))) { continue; }
			if (FPackageName::DoesPackageExist(D)) { continue; }
			DanglingMapDeps.Add(D);
			Names += FString::Printf(TEXT("%s\"MAP DEPENDS ON MISSING %s\""),
			                         Names.IsEmpty() ? TEXT("") : TEXT(","), *D);
		}
	}

	int32 Removed = 0;
	bool bSaved = false;
	bool bMapDepsCleared = false;
	if (bApply && DanglingMapDeps.Num() > 0)
	{
		// Re-save the map so the stale imports are rewritten away, then RE-ASK the registry. The
		// verification is the point: if the dependency survives, something live still holds it and
		// this repair does not apply - say so instead of reporting success.
		// ⛔ THROUGH RudeSaveDirty (2026-08-01). This was a RAW SaveDirtyPackages call and it is the
		// third save site the headless-cancel bug hid in: under `-unattended` the save silently did
		// nothing, so the re-save that IS the repair never happened and the tool honestly reported
		// mapDepsCleared=false. The verification was doing its job - the repair was not.
		World->MarkPackageDirty();
		const bool bRepairSaved = RudeSaveDirty(/*bMaps*/ true, /*bContent*/ false);
		UE_LOG(LogTemp, Display, TEXT("[RUDE] FixLevelRefs: repair re-save %s"),
			bRepairSaved ? TEXT("OK") : TEXT("FAILED"));
		FAssetRegistryModule& ARM =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		const FString PkgName = World->GetPackage()->GetName();
		AR.ScanModifiedAssetFiles({ PkgName });
		TArray<FName> After;
		AR.GetDependencies(FName(*PkgName), After, UE::AssetRegistry::EDependencyCategory::Package);
		bMapDepsCleared = true;
		for (const FName& Dep : After)
		{
			const FString D = Dep.ToString();
			if (D.StartsWith(TEXT("/Game/")) && !FPackageName::DoesPackageExist(D))
			{
				bMapDepsCleared = false;
				break;
			}
		}
		UE_LOG(LogTemp, Display, TEXT("[RUDE] FixLevelRefs: map had %d dangling dependencies; "
			"after re-save cleared=%s"), DanglingMapDeps.Num(),
			bMapDepsCleared ? TEXT("true") : TEXT("false"));
	}
	if (bApply && DanglingActorPkgs.Num() > 0)
	{
		// The external actor package IS the actor. Its target is gone and cannot be restored, so
		// deleting the package is the repair - and it works while the actor is UNLOADED, which is
		// the whole reason this goes through the registry instead of the actor iterator.
		for (const FString& Pkg : DanglingActorPkgs)
		{
			FString Filename;
			if (FPackageName::DoesPackageExist(Pkg, &Filename)
				&& IFileManager::Get().Delete(*Filename))
			{
				++Removed;
			}
		}
	}
	if (bApply && (Dangling.Num() > 0 || DanglingLI.Num() > 0))
	{
		for (ULevelStreaming* Level : Dangling)
		{
			World->RemoveStreamingLevel(Level);
			++Removed;
		}
		for (ALevelInstance* LI : DanglingLI)
		{
			// The actor is the only thing holding the broken pointer - with its target gone there
			// is nothing to repair it to, so removing it IS the repair.
			World->EditorDestroyActor(LI, /*bShouldModifyLevel*/ true);
			++Removed;
		}
		World->MarkPackageDirty();
		// Maps ONLY here - repairing the map package is this tool's entire purpose, and it is the
		// one thing SaveAssets deliberately refuses to touch. Through RudeSaveDirty, so an
		// unattended APPLY actually writes instead of being cancelled (see its comment).
		bSaved = RudeSaveDirty(/*bMaps*/ true, /*bContent*/ false);
	}
	UE_LOG(LogTemp, Display,
	       TEXT("[RUDE] FixLevelRefs: %d streaming levels (%d dangling), %d dangling level "
	            "instances, %d removed"),
	       Checked, Dangling.Num(), DanglingLI.Num(), Removed);
	return FString::Printf(
		TEXT("{\"ok\":true,\"checked\":%d,\"dangling\":%d,\"danglingLevelInstances\":%d,")
		TEXT("\"danglingMapDependencies\":%d,\"mapDepsCleared\":%s,")
		TEXT("\"removed\":%d,\"saved\":%s,\"names\":[%s]}"),
		Checked, Dangling.Num(), DanglingLI.Num(), DanglingMapDeps.Num(),
		bMapDepsCleared ? TEXT("true") : TEXT("false"), Removed,
		bSaved ? TEXT("true") : TEXT("false"), *Names);
}

FString URudeToolset::ImportYtdBatch(const FString& ListPath, const FString& DestFolder,
                                     const FString& Mode)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *ListPath))
	{
		return Fail(TEXT("cannot read list file"));
	}
	const bool bForce = Mode.TrimStartAndEnd().Equals(TEXT("FORCE"), ESearchCase::IgnoreCase);
	int32 Imported = 0, Textures = 0, Skipped = 0, Failed = 0;
	// ⛔ THIS BATCH PARSED EXACTLY ONE FIELD OUT OF THE UNIT VERDICT ("imported") AND DROPPED THE
	// REST - including invalidNames and missingPixels, the two counters ImportYtd was given
	// precisely so a silent texture loss could never recur. On 2026-07-30 the unit reported
	// texturesImported=0 across 1,943 dictionaries and the batch said ok:true; the fix put the
	// counter on the UNIT and left the aggregator blind, so the same run today would still print
	// only a total that had gone to zero. MEASURED over the whole resolved corpus: 45 of 82,241
	// dictionaries carry a texture whose name is not a legal package segment (78 items), so
	// invalidNames is a small, sharp signal that must not be averaged into silence.
	int32 Declared = 0, InvalidNames = 0, MissingPixels = 0;
	int32 UsageDefaulted = 0, UsageUnknown = 0, ItemsWithoutName = 0;
	FString FailedFiles;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString Path = Lines[i].TrimStartAndEnd();
		if (Path.IsEmpty()) { continue; }
		FString Txd = FPaths::GetBaseFilename(Path);
		Txd.RemoveFromEnd(TEXT(".ytd"));
		// The pixel folder is DERIVED: the extractor writes "<stem>/" beside the XML and resolve
		// carries the sidecar with the winning copy - the pair is self-describing.
		const FString PixelFolder = FPaths::GetPath(Path) / Txd;
		// Skip-if-exists on the txd's CONTENT FOLDER on disk (assets inside are named per
		// texture, unknowable here). FORCE re-imports in place - the texture-refresh law says
		// fresh packages, and ImportYtd's own edit-in-place handling owns that concern.
		const FString ContentDir = FPackageName::LongPackageNameToFilename(DestFolder / Txd, TEXT(""));
		if (!bForce && IFileManager::Get().DirectoryExists(*ContentDir))
		{
			++Skipped;
			continue;
		}
		const FString R = ImportYtd(Path, PixelFolder, DestFolder);
		// ⚠ The counters are read on BOTH paths. A dictionary that failed still declared textures
		// and still rejected names, and dropping its numbers because its ok flipped is the same
		// class of blindness this block is fixing.
		Declared         += RudeSumField(R, TEXT("declared"));
		InvalidNames     += RudeSumField(R, TEXT("invalidNames"));
		MissingPixels    += RudeSumField(R, TEXT("missingPixelCount"));
		UsageDefaulted   += RudeSumField(R, TEXT("usageDefaulted"));
		UsageUnknown     += RudeSumField(R, TEXT("usageUnknown"));
		ItemsWithoutName += RudeSumField(R, TEXT("itemsWithoutName"));
		if (R.Contains(TEXT("\"ok\":true")))
		{
			++Imported;
			// accumulate the per-txd texture count from the tool's own verdict
			Textures += RudeSumField(R, TEXT("imported"));
		}
		else
		{
			++Failed;
			if (Failed <= 30)
			{
				FailedFiles += FString::Printf(TEXT("%s\"%s\""), FailedFiles.IsEmpty() ? TEXT("") : TEXT(","), *Txd);
			}
		}
		if ((i + 1) % 25 == 0)
		{
			UE_LOG(LogTemp, Display,
				TEXT("[RUDE] ImportYtdBatch %d/%d (ok %d, skip %d, fail %d | declared %d, tex %d, "
				     "invalidNames %d, missingPixels %d)"),
				i + 1, Lines.Num(), Imported, Skipped, Failed, Declared, Textures,
				InvalidNames, MissingPixels);
		}
		if ((i + 1) % 100 == 0)
		{
			// Textures are heavy; keep memory flat - KEEPFLAGS, never RF_NoFlags (the GC-sweep law)
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	UE_LOG(LogTemp, Display,
		TEXT("[RUDE] ImportYtdBatch DONE: %d dictionaries ok, %d skipped, %d failed | declared %d, "
		     "textures imported %d, invalidNames %d, missingPixels %d, usage defaulted %d, "
		     "usage outside DIFFUSE/NORMAL/SPECULAR %d, nameless items %d"),
		Imported, Skipped, Failed, Declared, Textures, InvalidNames, MissingPixels,
		UsageDefaulted, UsageUnknown, ItemsWithoutName);
	// ⛔ Computed, not hardcoded - same class as ImportYdrBatch (fixed 2026-08-05).
	// `missingPixels` deliberately does NOT gate `ok`: it is a CORPUS gap (a manifest whose PNGs were
	// pruned), not a tool failure, and #37 measures it as 41.4% availability corpus-wide - failing on
	// it would make every honest run red. `Failed` is the tool's own failure and does gate.
	const bool bOk = (Failed == 0) && (Imported + Skipped > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"imported\":%d,\"texturesImported\":%d,\"texturesDeclared\":%d,")
		TEXT("\"skipped\":%d,\"failed\":%d,\"invalidNames\":%d,\"missingPixels\":%d,")
		TEXT("\"usageDefaulted\":%d,\"usageUnknown\":%d,\"itemsWithoutName\":%d,")
		TEXT("\"failedFiles\":[%s]}"),
		bOk ? TEXT("true") : TEXT("false"),
		Imported, Textures, Declared, Skipped, Failed, InvalidNames, MissingPixels,
		UsageDefaulted, UsageUnknown, ItemsWithoutName, *FailedFiles);
}

// One actor per entity: a StaticMeshComponent root (the drawable, or the proxy cube when the
// mesh is absent) plus a URudeEntityComponent filled from the manifest row. Folder RUDE_LS/<ymap>
// so the idempotent clear and the per-ymap grouping both work; label = archetype name.
AActor* RudeSpawnEntityActor(UWorld* World, const FString& YmapName, const TSharedPtr<FJsonObject>& Ent,
                             const FTransform& Xf, UStaticMesh* Mesh, bool bProxy, uint32 TimeMask)
{
	AActor* A = World->SpawnActor<AActor>();
	if (!A) { return nullptr; }
	UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(A, TEXT("Mesh"));
	SMC->SetStaticMesh(Mesh);
	SMC->SetMobility(EComponentMobility::Static);
	A->SetRootComponent(SMC);
	SMC->RegisterComponent();
	A->AddInstanceComponent(SMC);
	A->SetActorTransform(Xf);
	if (TimeMask != 0 && TimeMask != 0xFFFFFFu)
	{
		SMC->ComponentTags.Add(FName(*FString::Printf(TEXT("RUDE_TIME:%u"), TimeMask)));
	}
	if (bProxy) { A->Tags.Add(FName(TEXT("RUDE_PROXY"))); }
	// LOD accounting: the game shows ONE level of a lineage at a time. Every level is PLACED (the
	// export needs them all) but only HD / ORPHANHD is VISIBLE by default; LOD and SLOD shells stay
	// hidden and tagged RUDE_LOD:<level> so SetLodView can switch the view (Matt, 2026-09-05: the
	// blue "glass tower" was an SLOD shell stacked on its HD building).
	{
		FString LodLv;
		Ent->TryGetStringField(TEXT("lodLevel"), LodLv);
		if (LodLv.IsEmpty()) { LodLv = TEXT("LODTYPES_DEPTH_HD"); }
		A->Tags.Add(FName(*(TEXT("RUDE_LOD:") + LodLv)));
		const bool bHd = LodLv == TEXT("LODTYPES_DEPTH_HD") || LodLv == TEXT("LODTYPES_DEPTH_ORPHANHD");
		double FlagsD = 0.0;
		Ent->TryGetNumberField(TEXT("flags"), FlagsD);
		const bool bReflectionOnly = (((uint32)FlagsD) & 0x02000000u) != 0;   // ONLY_RENDER_IN_REFLECTIONS
		if (bReflectionOnly) { A->Tags.Add(FName(TEXT("RUDE_REFLECTION_ONLY"))); }
		// Sanity: a mesh far bigger than its own definition claims is a mis-import (2026-09-05: a
		// 7 m cloth tarp came in as a 260 m sheet). Placed, tagged, hidden, counted - never shown as fact.
		bool bSuspect = false;
		{
			double BsR = 0.0;
			Ent->TryGetNumberField(TEXT("bsRadius"), BsR);
			if (!bProxy && BsR > 0.0 && Mesh)
			{
				const double MeshR = Mesh->GetBoundingBox().GetExtent().Size() / 100.0;   // metres
				if (MeshR > 3.0 * BsR + 5.0) { bSuspect = true; A->Tags.Add(FName(TEXT("RUDE_SUSPECT_BOUNDS"))); }
			}
		}
		if (!bHd || bReflectionOnly || bSuspect)
		{
			SMC->SetVisibility(false, true);
			SMC->SetHiddenInGame(true, true);
		}
	}
	URudeEntityComponent* R = NewObject<URudeEntityComponent>(A, TEXT("RudeEntity"));
	auto Str = [&Ent](const TCHAR* K) { FString V; Ent->TryGetStringField(K, V); return V; };
	auto Num = [&Ent](const TCHAR* K, double Def) { double V = Def; Ent->TryGetNumberField(K, V); return V; };
	R->ArchetypeName = Str(TEXT("archetype"));
	R->SourceYmap = Str(TEXT("srcYmap"));
	R->SourceSlot = Str(TEXT("srcSlot"));
	R->SourceIndex = (int32)Num(TEXT("srcIndex"), -1.0);
	R->LodDist = (float)Num(TEXT("lodDist"), 0.0);
	R->ChildLodDist = (float)Num(TEXT("childLodDist"), 0.0);
	{
		const FString Lod = Str(TEXT("lodLevel"));
		if (!Lod.IsEmpty()) { R->LodLevel = Lod; }
	}
	R->ParentIndex = (int32)Num(TEXT("parentIndex"), -1.0);
	{
		const FString Pri = Str(TEXT("priorityLevel"));
		if (!Pri.IsEmpty()) { R->PriorityLevel = Pri; }
	}
	R->ExtensionsXml = Str(TEXT("extensions"));
	R->Flags = (uint32)Num(TEXT("flags"), 0.0);
	R->Guid = (uint32)Num(TEXT("guid"), 0.0);
	R->NumChildren = (int32)Num(TEXT("numChildren"), 0.0);
	R->AmbientOcclusionMultiplier = (float)Num(TEXT("aoMultiplier"), 255.0);
	R->ArtificialAmbientOcclusion = (float)Num(TEXT("artificialAo"), 255.0);
	R->TintValue = (uint32)Num(TEXT("tintValue"), 0.0);
	R->SourceXml = Str(TEXT("xml"));
	{
		const FString T = Str(TEXT("itemType"));
		if (!T.IsEmpty()) { R->ItemType = T; }
	}
	R->SourceTransform = Xf;
	R->SourceFieldsKey = R->FieldsKey();
	R->RegisterComponent();
	A->AddInstanceComponent(R);
	RudeAttachEntityLights(A, R);   // its light extensions as UE lights (RUDE_LIGHT:<i> components)
	A->SetActorLabel(R->ArchetypeName.IsEmpty() ? YmapName : R->ArchetypeName);
	A->SetFolderPath(FName(*(TEXT("RUDE_LS/") + YmapName)));
	return A;
}

FString URudeToolset::ImportScene(const FString& ManifestPath, const FString& MeshFolder,
                                  const FString& Filter, const FString& Mode)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why);
	};
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *ManifestPath))
	{
		return Fail(TEXT("cannot read manifest"));
	}
	TArray<TSharedPtr<FJsonValue>> Scenes;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Scenes))
		{
			return Fail(TEXT("manifest is not a JSON array"));
		}
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return Fail(TEXT("no editor world"));
	}
	const bool bAll = Filter.TrimStartAndEnd().Equals(TEXT("ALL"), ESearchCase::IgnoreCase);
	// ACTORS = one actor per entity with its RUDE entity component (editable, exportable);
	// empty = the ISM display path (fast, not per-entity addressable).
	const bool bActors = Mode.TrimStartAndEnd().Equals(TEXT("ACTORS"), ESearchCase::IgnoreCase);
	int32 NumActors = 0;

	// Idempotent respawn: clear any previous RUDE_LS spawn first (re-running the tool
	// REPLACES the scene instead of stacking duplicates).
	{
		TArray<AActor*> Stale;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			// RUDE_LS itself (ISM mode) and RUDE_LS/<ymap> (ACTORS mode) are both this tool's.
			const FString Folder = It->GetFolderPath().ToString();
			if (Folder == TEXT("RUDE_LS") || Folder.StartsWith(TEXT("RUDE_LS/"))) { Stale.Add(*It); }
		}
		for (AActor* A : Stale) { World->DestroyActor(A); }
	}

	UStaticMesh* ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TMap<FString, UStaticMesh*> MeshCache;      // lowercase drawable -> mesh (nullptr = known-missing)
	TMap<FString, int32> Missing;               // drawable/archetype -> proxy instance count
	int32 NumYmaps = 0, NumEntities = 0, NumInstances = 0, NumProxies = 0;
	// ⛔ "entities" WAS THE POST-FILTER NUMBER, AND THE FILTER WAS SILENT. ++NumEntities fires
	// AFTER both `continue`s below, so a caller could not tell a manifest that is 12% LOD content
	// from a manifest whose entities failed to parse - and "entities" reads as "the entities in
	// this manifest". MEASURED over 1,500 resolved ymap / 239,662 entities: 25,185 (10.51%) are
	// dropped by the default HD filter (LOD 22,745, SLOD1 2,274, SLOD2 112, SLOD3 37), and 17
	// carry the unresolvable string hash_6F5D45B3 as their lodLevel - neither HD nor a known LOD
	// tier, so they get their own bucket rather than being folded into either.
	// ⚠ An EMPTY lodLevel is KEPT by the predicate below (treated as HD). 0 corpus entities lack
	// the element today; the count is here so that if one ever does, the assumption is visible
	// instead of silently spawning something the game would not have drawn.
	int32 EntitiesInManifest = 0, FilteredByLod = 0, UnknownLodLevel = 0, EmptyLodLevel = 0;
	int32 MalformedEntities = 0, UniqueMeshLookups = 0;
	TMap<FString, FString> YmapParentMap;   // ymap (lower) -> CMapData/parent, for the lineage resolve

	for (const TSharedPtr<FJsonValue>& SceneVal : Scenes)
	{
		const TSharedPtr<FJsonObject>* SceneObj;
		if (!SceneVal.IsValid() || !SceneVal->TryGetObject(SceneObj)) { continue; }
		const TArray<TSharedPtr<FJsonValue>>* Entities;
		if (!(*SceneObj)->TryGetArrayField(TEXT("entities"), Entities)) { continue; }
		const FString YmapName = (*SceneObj)->GetStringField(TEXT("ymap"));
		{
			FString YP;
			(*SceneObj)->TryGetStringField(TEXT("ymapParent"), YP);
			YmapParentMap.Add(YmapName.ToLower(), YP);
		}

		AActor* Actor = nullptr;
		USceneComponent* Root = nullptr;
		TMap<FString, UInstancedStaticMeshComponent*> IsmByMesh;   // key: mesh name or "proxy:<name>"
		if (bActors && Entities->Num() > 0) { ++NumYmaps; }   // ACTORS mode has no per-ymap parent actor

		// ⭐ The key carries the HOUR MASK as well as the mesh, so entities that appear only at
		// certain hours land in their OWN component. Visibility is a per-component switch in UE,
		// so grouping by mask is what makes the game's dataset drivable at all - mixing a
		// night-only archetype into a shared component would force per-instance work for something
		// the data expresses per archetype.
		auto GetIsm = [&](const FString& Key, UStaticMesh* Mesh, uint32 TimeMask = 0)
			-> UInstancedStaticMeshComponent*
		{
			if (UInstancedStaticMeshComponent** Found = IsmByMesh.Find(Key)) { return *Found; }
			if (!Actor)
			{
				Actor = World->SpawnActor<AActor>();
				if (!Actor) { return nullptr; }
				Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
				Actor->SetRootComponent(Root);
				Root->SetMobility(EComponentMobility::Static);
				Root->RegisterComponent();
				Actor->AddInstanceComponent(Root);
				Actor->SetActorLabel(YmapName);
				Actor->SetFolderPath(FName(TEXT("RUDE_LS")));
				++NumYmaps;
			}
			UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(
				Actor, FName(*FString::Printf(TEXT("ISM_%d"), IsmByMesh.Num())));
			Ism->SetStaticMesh(Mesh);
			Ism->SetMobility(EComponentMobility::Static);
			Ism->SetupAttachment(Root);
			Ism->RegisterComponent();
			// ALWAYS_VISIBLE (0xFFFFFF) and "no mask" need no tag - tagging only what is genuinely
			// gated keeps SetWorldHour's sweep proportional to the gated set, not the whole city.
			if (TimeMask != 0 && TimeMask != 0xFFFFFFu)
			{
				Ism->ComponentTags.Add(FName(*FString::Printf(TEXT("RUDE_TIME:%u"), TimeMask)));
			}
			Actor->AddInstanceComponent(Ism);
			IsmByMesh.Add(Key, Ism);
			return Ism;
		};

		for (const TSharedPtr<FJsonValue>& EntVal : *Entities)
		{
			const TSharedPtr<FJsonObject>* Ent;
			if (!EntVal.IsValid() || !EntVal->TryGetObject(Ent)) { ++MalformedEntities; continue; }
			++EntitiesInManifest;
			{
				FString Lod;
				(*Ent)->TryGetStringField(TEXT("lodLevel"), Lod);
				const bool bHd = Lod.IsEmpty()
					|| Lod == TEXT("LODTYPES_DEPTH_HD") || Lod == TEXT("LODTYPES_DEPTH_ORPHANHD");
				if (Lod.IsEmpty()) { ++EmptyLodLevel; }
				else if (!bHd && !Lod.StartsWith(TEXT("LODTYPES_DEPTH_"))) { ++UnknownLodLevel; }
				if (!bAll && !bHd)
				{
					++FilteredByLod;
					continue;
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Loc;
			const TArray<TSharedPtr<FJsonValue>>* Quat;
			if (!(*Ent)->TryGetArrayField(TEXT("ue_location"), Loc) || Loc->Num() != 3 ||
			    !(*Ent)->TryGetArrayField(TEXT("ue_quat"), Quat) || Quat->Num() != 4)
			{
				++MalformedEntities;
				continue;
			}
			++NumEntities;
			const double SXY = (*Ent)->HasField(TEXT("scaleXY")) ? (*Ent)->GetNumberField(TEXT("scaleXY")) : 1.0;
			const double SZ = (*Ent)->HasField(TEXT("scaleZ")) ? (*Ent)->GetNumberField(TEXT("scaleZ")) : 1.0;
			FQuat Q((*Quat)[0]->AsNumber(), (*Quat)[1]->AsNumber(), (*Quat)[2]->AsNumber(), (*Quat)[3]->AsNumber());
			// not normalised: the source quaternion is unit within float32 and re-normalising in double moves its last digit (measured 2026-09-06: 0.9961947 -> 0.996194661 on an untouched entity)
			const FTransform Xf(Q,
				FVector((*Loc)[0]->AsNumber(), (*Loc)[1]->AsNumber(), (*Loc)[2]->AsNumber()),
				FVector(SXY, SXY, SZ));

			FString Drawable;
			(*Ent)->TryGetStringField(TEXT("drawable"), Drawable);   // null for unresolved archetypes
			Drawable.ToLowerInline();
			UStaticMesh* Mesh = nullptr;
			if (!Drawable.IsEmpty())
			{
				if (UStaticMesh** Cached = MeshCache.Find(Drawable)) { Mesh = *Cached; }
				else
				{
					Mesh = LoadObject<UStaticMesh>(nullptr, *(MeshFolder / Drawable));
					MeshCache.Add(Drawable, Mesh);
					// ⛔ MeshCache.Num() was reported as "uniqueMeshes". The cache DELIBERATELY
					// stores nullptr for a known-missing drawable (its own comment says so), so
					// every drawable whose asset failed to load inflated the figure that reads as
					// "how many distinct meshes this scene placed". Count the ones that LOADED;
					// keep the lookup total too, because their difference is exactly the missing
					// set and that is worth having as a number rather than a subtraction.
					++UniqueMeshLookups;
				}
			}
			if (bActors)
			{
				uint32 TimeMaskA = 0;
				{
					int32 TF = 0;
					if ((*Ent)->TryGetNumberField(TEXT("timeFlags"), TF) && TF > 0) { TimeMaskA = (uint32)TF; }
				}
				if (!Mesh)
				{
					const FString Tag = Drawable.IsEmpty() ? (*Ent)->GetStringField(TEXT("archetype")) : Drawable;
					Missing.FindOrAdd(Tag)++;
				}
				if (Mesh || ProxyCube)
				{
					if (RudeSpawnEntityActor(World, YmapName, *Ent, Xf, Mesh ? Mesh : ProxyCube, Mesh == nullptr, TimeMaskA))
					{
						++NumActors;
						if (Mesh) { ++NumInstances; } else { ++NumProxies; }
					}
				}
				continue;
			}
			if (Mesh)
			{
				uint32 TimeMask = 0;
				{
					int32 TF = 0;
					if ((*Ent)->TryGetNumberField(TEXT("timeFlags"), TF) && TF > 0)
					{
						TimeMask = (uint32)TF;
					}
				}
				const FString IsmKey = TimeMask ? FString::Printf(TEXT("%s#t%u"), *Drawable, TimeMask)
				                                : Drawable;
				if (UInstancedStaticMeshComponent* Ism = GetIsm(IsmKey, Mesh, TimeMask))
				{
					Ism->AddInstance(Xf, /*bWorldSpace*/ true);
					++NumInstances;
				}
			}
			else
			{
				// ⛔ THE TALLY MOVED OUT OF THE `if (ProxyCube)` BRANCH. An entity with no mesh was
				// only recorded as missing when the /Engine/BasicShapes/Cube proxy happened to
				// load; if that LoadObject ever failed, every unresolved entity in the scene
				// vanished from missingMeshes and topMissing as well as from the viewport, and the
				// verdict read like a clean import. What is missing is a property of the DATA, not
				// of whether the placeholder was available.
				const FString Tag = Drawable.IsEmpty() ? (*Ent)->GetStringField(TEXT("archetype")) : Drawable;
				Missing.FindOrAdd(Tag)++;
				if (ProxyCube)
				{
					if (UInstancedStaticMeshComponent* Ism = GetIsm(TEXT("proxy"), ProxyCube))
					{
						Ism->AddInstance(Xf, /*bWorldSpace*/ true);
						++NumProxies;
					}
				}
			}
		}
	}
	World->MarkPackageDirty();

	Missing.ValueSort(TGreater<int32>());
	FString TopMissing;
	int32 Shown = 0;
	for (const TPair<FString, int32>& M : Missing)
	{
		if (++Shown > 20) { break; }
		TopMissing += FString::Printf(TEXT("%s\"%s x%d\""), Shown > 1 ? TEXT(",") : TEXT(""), *M.Key, M.Value);
	}
	int32 UniqueMeshes = 0;
	for (const TPair<FString, UStaticMesh*>& M : MeshCache)
	{
		if (M.Value) { ++UniqueMeshes; }
	}
	// unknownLodLevel is a FLAG, not a partition: an entity with an unrecognisable tier is also
	// counted in filteredByLod (it is not HD, so the default filter drops it). The partition that
	// closes is entitiesInManifest == entities + filteredByLod + malformedEntities.
	// ⛔ `ok` IS COMPUTED, NEVER HARDCODED (fixed 2026-08-05, open item #44 - the same class as
	// ImportYdrBatch/ImportYtdBatch/ExportYdrBinaryBatch in 65248c2, but this one needed a SEMANTICS
	// decision first because it counts two failures that mean opposite things. #42's rule was
	// deliberately NOT copied here; what follows is that decision, and it is MINE - the agent's -
	// not Matt's. He was never asked and never ruled on it.
	//
	// This function is the SPAWN. ImportMapArea forwards its verdict through
	// `bSpawnOk = !Spawn.Contains("\"ok\":false")`, so whatever is decided here is also the map
	// lane's gate; getting it wrong reddens or greens the whole import tool.
	//
	//   malformedEntities > 0  ->  FALSE. An entity the tool could not parse - not a JSON object at
	//     all, or missing/short ue_location or ue_quat - is the tool meeting input it does not
	//     understand, and this codebase REFUSES rather than defaults (the same rule that made
	//     ExportYmap reject an empty <archetypeName> instead of placing it at the origin, #23).
	//     The entity is DROPPED: it is in entitiesInManifest and in nothing else, so the scene is
	//     silently short by exactly that many placements. MEASURED shape of the risk: 0 of 33,973
	//     entities in the emitted corpus are malformed today (INTERCHANGE_CONTRACT re-census,
	//     2026-08-05), so this gate costs nothing on healthy data and fires only when the ymap
	//     emitter or the manifest writer has actually broken - which is precisely when a headless
	//     run must stop instead of scoring 0.
	//
	//   missingMeshes  ->  DOES NOT GATE. It is a CORPUS gap, not a tool failure: the drawable was
	//     never converted, or its lane was never extracted. #37 measures corpus texture/mesh
	//     availability at 41.4% overall and wildly per lane (ydr 14.2% unavailable, ydd 63.3%,
	//     yft 74.6%), so gating on it would make EVERY honest run red, and a gate that fires on
	//     every run is a gate nobody reads - the same reasoning that keeps missingPixels out of
	//     ImportYtdBatch's ok and meshesMissingFromCorpus out of ImportMapArea's. The number stays
	//     loud (missingMeshes + topMissing), which is where a corpus gap belongs.
	//
	//   ZERO WORK  ->  FALSE. `ymaps` counts ymaps that actually produced an actor and `entities`
	//     the placements that survived the LOD filter; if either is 0 nothing was placed, and a
	//     manifest that spawned an empty level must not report success. PRESENCE IS NOT COVERAGE -
	//     a gate that cannot fail is worse than no gate (ENGINEERING_LOG "MEASUREMENT LAWS").
	//     Note this also catches the case a filter typo produces: every entity filtered out by LOD,
	//     nothing spawned, previously ok:true.
	// ACTORS mode: resolve parentIndex links into LodParent / LodChildren (ENGINEERING_LOG law 24)
	int32 LodLinks = 0, LodUnresolved = 0, LodPartial = 0;
	if (bActors) { RudeResolveLodLineage(World, YmapParentMap, LodLinks, LodUnresolved, LodPartial); }
	const bool bOk = (MalformedEntities == 0) && (NumYmaps > 0) && (NumEntities > 0);
	return FString::Printf(
		TEXT("{\"ok\":%s,\"ymaps\":%d,\"entitiesInManifest\":%d,\"entities\":%d,")
		TEXT("\"filteredByLod\":%d,\"unknownLodLevel\":%d,\"emptyLodLevel\":%d,")
		TEXT("\"malformedEntities\":%d,\"instances\":%d,\"proxies\":%d,")
		TEXT("\"uniqueMeshes\":%d,\"uniqueMeshLookups\":%d,\"missingMeshes\":%d,\"topMissing\":[%s],")
		TEXT("\"mode\":\"%s\",\"actors\":%d,\"lodLinks\":%d,\"lodUnresolved\":%d,\"lodPartial\":%d}"),
		bOk ? TEXT("true") : TEXT("false"),
		NumYmaps, EntitiesInManifest, NumEntities, FilteredByLod, UnknownLodLevel, EmptyLodLevel,
		MalformedEntities, NumInstances, NumProxies,
		UniqueMeshes, UniqueMeshLookups, Missing.Num(), *TopMissing,
		bActors ? TEXT("ACTORS") : TEXT("ISM"), NumActors, LodLinks, LodUnresolved, LodPartial);
}

// ============================================================================================
// PRODUCT DEBT (maintainer lane `product_debt`): the fxmanifest merge, the area catalog
// generator, and the environment doctor. Appended to the map/area lane because every one of
// them already needs what this translation unit includes - the corpus reader, the plugin
// manager, the asset registry - and a NEW .cpp is not compiled until the module's file list is
// invalidated, whose only symptom is a link error.
// ============================================================================================

// ⛔ WHY THIS EXISTS - `ExportYmap` OVERWROTE `fxmanifest.lua` UNCONDITIONALLY (AGENTS §9).
// A FiveM resource folder belongs to a person, not to a tool: the moment someone adds
// `client_script 'main.lua'` or a `files { }` block beside the exported ymap, the next export
// deleted it with no prompt and no record. Merging is the only honest write - keep the file's
// bytes verbatim, append only the directives it does not already declare, and COUNT what was
// preserved so a caller reads it in the verdict instead of taking it on trust.
//
// "Already declares" is tested on the DIRECTIVE KEY (the first token of the line), never on the
// whole line: `this_is_a_map "yes"` in double quotes is the same directive as ours, and appending
// a second copy would silently override the person's value (an fxmanifest is last-wins). Their
// spelling stands; only what is absent gets added.
//
// A blank or `--` line in RequiredLines is DECORATION that rides with the next directive - it is
// emitted only when that directive is. So a second export over an already-complete manifest adds
// nothing, writes nothing, and leaves the file byte-identical.
bool RudeMergeManifest(const FString& Path, const TArray<FString>& RequiredLines,
                       int32& OutPreserved, int32& OutAlready, int32& OutAdded, FString& OutError)
{
	OutPreserved = 0;
	OutAlready = 0;
	OutAdded = 0;
	OutError.Empty();

	auto KeyOf = [](const FString& Line) -> FString
	{
		const FString T = Line.TrimStartAndEnd();
		if (T.IsEmpty() || T.StartsWith(TEXT("--"))) { return FString(); }
		int32 Cut = INDEX_NONE;
		for (int32 Ci = 0; Ci < T.Len(); ++Ci)
		{
			const TCHAR C = T[Ci];
			if (FChar::IsWhitespace(C) || C == TEXT('(') || C == TEXT('{') || C == TEXT('\'')
				|| C == TEXT('"'))
			{
				Cut = Ci;
				break;
			}
		}
		return (Cut == INDEX_NONE ? T : T.Left(Cut)).ToLower();
	};

	FString Existing;
	const bool bHad = FFileHelper::LoadFileToString(Existing, *Path);
	TSet<FString> Present;
	if (bHad)
	{
		TArray<FString> Lines;
		Existing.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
		OutPreserved = Lines.Num();
		for (const FString& L : Lines)
		{
			const FString K = KeyOf(L);
			if (!K.IsEmpty()) { Present.Add(K); }
		}
	}

	FString Additions;
	FString PendingDecoration;
	for (const FString& Req : RequiredLines)
	{
		const FString K = KeyOf(Req);
		if (K.IsEmpty())
		{
			PendingDecoration += Req + TEXT("\n");
			continue;
		}
		if (Present.Contains(K))
		{
			++OutAlready;
			PendingDecoration.Empty();
			continue;
		}
		Additions += PendingDecoration + Req + TEXT("\n");
		PendingDecoration.Empty();
		Present.Add(K);
		++OutAdded;
	}
	// Nothing missing and the file is already there: do not touch it. This is what makes a second
	// export byte-identical to the first, which is the gate this merge is measured by.
	if (OutAdded == 0 && bHad) { return true; }

	FString Out = bHad ? Existing : FString();
	if (!Out.IsEmpty() && !Out.EndsWith(TEXT("\n"))) { Out += TEXT("\n"); }
	Out += Additions;
	if (!FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("cannot write %s"), *Path);
		return false;
	}
	return true;
}

// ---- BuildAreaCatalog (agent) ---------------------------------------------------------------
// One popzone row: an axis-aligned box and the region word the game files under it.
struct FRudeAreaZoneBox
{
	FString Region;
	double MinX = 0.0, MinY = 0.0, MinZ = 0.0, MaxX = 0.0, MaxY = 0.0, MaxZ = 0.0;
	double FootprintArea() const { return (MaxX - MinX) * (MaxY - MinY); }
};

// One ymap prefix family, accumulated over the ymaps that belong to it.
struct FRudeAreaFamily
{
	int32 Ymaps = 0, Entities = 0, WithExtents = 0;
	bool bHasExtent = false;
	double MinX = 0.0, MinY = 0.0, MinZ = 0.0, MaxX = 0.0, MaxY = 0.0, MaxZ = 0.0;
	TMap<FString, int32> Zones;   // region word -> ymaps whose centre landed in it
};

// THE FAMILY RULE, stated so it can be argued with: the first underscore-separated token of the
// ymap's name, and the first TWO when that token is shorter than three characters (`v_michael`,
// `id2_28` - a one- or two-letter head is a namespace letter, not a district). Measured over the
// corpus in maintainer lane `product_debt` (`LAWS.md`); it is a NAMING rule, not a spatial one,
// which is exactly why the catalog also carries a zone-named entry derived from the game's data.
static FString RudeAreaFamilyOf(const FString& Name)
{
	TArray<FString> Toks;
	Name.ParseIntoArray(Toks, TEXT("_"), /*bCullEmpty*/ false);
	if (Toks.Num() >= 2 && Toks[0].Len() < 3) { return Toks[0] + TEXT("_") + Toks[1]; }
	return Toks.Num() > 0 ? Toks[0] : Name;
}

// Read `<Tag ... x="" y="" z="" />` out of an XML document. Deliberately a token scan and not
// FXmlFile: a 29 GB corpus of ymaps must not be DOM-parsed to read six numbers off the header.
static bool RudeAreaReadVec(const FString& Doc, const TCHAR* Tag, double& OutX, double& OutY, double& OutZ)
{
	const int32 At = Doc.Find(Tag, ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
	if (At == INDEX_NONE) { return false; }
	const int32 Close = Doc.Find(TEXT("/>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
	if (Close == INDEX_NONE) { return false; }
	const FString Seg = Doc.Mid(At, Close - At);
	auto Attr = [&Seg](const TCHAR* Key, double& Out) -> bool
	{
		const int32 K = Seg.Find(Key, ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
		if (K == INDEX_NONE) { return false; }
		const int32 Q1 = Seg.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, K);
		if (Q1 == INDEX_NONE) { return false; }
		const int32 Q2 = Seg.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Q1 + 1);
		if (Q2 == INDEX_NONE) { return false; }
		Out = FCString::Atod(*Seg.Mid(Q1 + 1, Q2 - Q1 - 1));
		return true;
	};
	return Attr(TEXT("x=\""), OutX) && Attr(TEXT("y=\""), OutY) && Attr(TEXT("z=\""), OutZ);
}

static FString RudeAreaVecJson(bool bHas, double X, double Y, double Z)
{
	if (!bHas) { return TEXT("null"); }
	return FString::Printf(TEXT("[%.6f,%.6f,%.6f]"), X, Y, Z);
}

FString URudeToolset::BuildAreaCatalog(const FString& CorpusRoot, const FString& OutJsonPath)
{
	auto Fail = [](const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *RudeJsonEscape(Why));
	};
	const FString Root = CorpusRoot.TrimStartAndEnd();
	if (Root.IsEmpty()) { return Fail(TEXT("CorpusRoot is empty")); }

	FString OpenError;
	const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(Root, OpenError);
	if (!Corpus.IsValid()) { return Fail(OpenError); }

	// ---- 1) the game's own region data: popzone.ipl -----------------------------------------
	// `zone` section, one comma-separated row per zone:
	//     id, minX, minY, minZ, maxX, maxY, maxZ, regionWord, flag
	// The region word is the game's OWN name for the district, which is the whole reason the
	// catalog can carry a human alias at all. Absent (a corpus exported without common.rpf), the
	// catalog still builds - every entry is then a prefix entry, marked as such.
	TArray<FRudeAreaZoneBox> Zones;
	FString ZonePath;
	if (const FRudeCorpusEntry* ZoneEntry = Corpus->Effective(TEXT("ipl"), TEXT("popzone")))
	{
		ZonePath = Corpus->PathOf(*ZoneEntry);
		TArray<FString> ZoneLines;
		FFileHelper::LoadFileToStringArray(ZoneLines, *ZonePath);
		for (const FString& RawLine : ZoneLines)
		{
			const FString L = RawLine.TrimStartAndEnd();
			if (L.IsEmpty() || L.StartsWith(TEXT("#"))) { continue; }
			TArray<FString> Fields;
			L.ParseIntoArray(Fields, TEXT(","), /*bCullEmpty*/ false);
			if (Fields.Num() < 8) { continue; }
			double V[6] = { 0, 0, 0, 0, 0, 0 };
			bool bNumeric = true;
			for (int32 Fi = 0; Fi < 6; ++Fi)
			{
				const FString S = Fields[Fi + 1].TrimStartAndEnd();
				// ⛔ NOT FString::IsNumeric(): it takes a sign, digits and at most one '.' and has NO
				// exponent branch - and popzone.ipl spells four of its OWN z values as exponents (FrW47
				// 2.19345e-005, FrW42 -2.28882e-005, ZVCan7 -1.52588e-005, FrW117 2.28882e-005). Replaying
				// that predicate over the file parses 1,317 of 1,321 rows and drops four named regions -
				// Paleto, Mount Chiliad, Vespucci Canals, Braddock Pass - on the floor, silently.
				// LexTryParseString routes to FCString::Atod, so it takes the exponent, and it still
				// refuses a non-numeric token (replayed over the real file in compare_product_debt.py).
				if (S.IsEmpty() || !LexTryParseString(V[Fi], *S)) { bNumeric = false; break; }
				V[Fi] = FCString::Atod(*S);
			}
			if (!bNumeric) { continue; }
			FRudeAreaZoneBox Box;
			Box.Region = Fields[7].TrimStartAndEnd();
			Box.MinX = V[0]; Box.MinY = V[1]; Box.MinZ = V[2];
			Box.MaxX = V[3]; Box.MaxY = V[4]; Box.MaxZ = V[5];
			if (Box.Region.IsEmpty()) { continue; }
			Zones.Add(Box);
		}
	}

	// ---- 2) every ymap the game would load, once ---------------------------------------------
	TArray<const FRudeCorpusEntry*> Ymaps;
	Corpus->ByPrefix(TEXT("ymap"), FString(), Ymaps);
	if (Ymaps.Num() == 0) { return Fail(TEXT("the corpus ledger lists no ymap rows")); }

	TMap<FString, FRudeAreaFamily> Families;
	int32 Read = 0, Missing = 0, NoExtents = 0, Degenerate = 0, Zoned = 0, Unzoned = 0;
	int64 TotalEntities = 0, TotalBytes = 0;
	for (const FRudeCorpusEntry* E : Ymaps)
	{
		const FString Path = Corpus->PathOf(*E);
		FString Doc;
		if (!FFileHelper::LoadFileToString(Doc, *Path)) { ++Missing; continue; }
		++Read;
		// The FILE's byte length, not Doc.Len(): LoadFileToString DECODES, so Doc.Len() is a TCHAR
		// count that equals the byte count only while every ymap is pure ASCII. The Python twin's
		// denominator (measure_product_debt.py) is raw bytes, and a field named `bytes` must mean bytes.
		const int64 OnDisk = IFileManager::Get().FileSize(*Path);
		if (OnDisk > 0) { TotalBytes += OnDisk; }

		int32 Ents = 0;
		int32 Hit = Doc.Find(TEXT("type=\"CEntityDef\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
		while (Hit != INDEX_NONE)
		{
			++Ents;
			Hit = Doc.Find(TEXT("type=\"CEntityDef\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Hit + 1);
		}
		TotalEntities += Ents;

		FRudeAreaFamily& Fam = Families.FindOrAdd(RudeAreaFamilyOf(E->Name));
		++Fam.Ymaps;
		Fam.Entities += Ents;

		double Ax = 0, Ay = 0, Az = 0, Bx = 0, By = 0, Bz = 0;
		if (!RudeAreaReadVec(Doc, TEXT("<entitiesExtentsMin"), Ax, Ay, Az)
			|| !RudeAreaReadVec(Doc, TEXT("<entitiesExtentsMax"), Bx, By, Bz))
		{
			++NoExtents;
			continue;
		}
		// The game's own "this ymap places nothing" sentinel: an inverted box. Folding it into a
		// family's extent would drag the family to the float limits, so it is COUNTED, not used.
		if (Bx < Ax || By < Ay || Bz < Az) { ++Degenerate; continue; }
		++Fam.WithExtents;
		if (!Fam.bHasExtent)
		{
			Fam.bHasExtent = true;
			Fam.MinX = Ax; Fam.MinY = Ay; Fam.MinZ = Az;
			Fam.MaxX = Bx; Fam.MaxY = By; Fam.MaxZ = Bz;
		}
		else
		{
			Fam.MinX = FMath::Min(Fam.MinX, Ax); Fam.MinY = FMath::Min(Fam.MinY, Ay); Fam.MinZ = FMath::Min(Fam.MinZ, Az);
			Fam.MaxX = FMath::Max(Fam.MaxX, Bx); Fam.MaxY = FMath::Max(Fam.MaxY, By); Fam.MaxZ = FMath::Max(Fam.MaxZ, Bz);
		}
		// Smallest containing box wins: popzone nests a district inside a larger catch-all, and the
		// tighter box is the more specific name.
		const double Cx = (Ax + Bx) * 0.5, Cy = (Ay + By) * 0.5, Cz = (Az + Bz) * 0.5;
		const FRudeAreaZoneBox* Best = nullptr;
		for (const FRudeAreaZoneBox& Box : Zones)
		{
			if (Cx < Box.MinX || Cx > Box.MaxX || Cy < Box.MinY || Cy > Box.MaxY) { continue; }
			if (Cz < Box.MinZ - 1.0 || Cz > Box.MaxZ + 1.0) { continue; }
			if (!Best || Box.FootprintArea() < Best->FootprintArea()) { Best = &Box; }
		}
		if (Best) { ++Fam.Zones.FindOrAdd(Best->Region); ++Zoned; }
		else { ++Unzoned; }
	}

	// ---- 3) the catalog ----------------------------------------------------------------------
	// TWO kinds of entry, both in the schema `ImportArea` already reads ({alias, prefixes[],
	// exact[]}); every other field it ignores, so the catalog can carry its own provenance.
	//   prefix entry - one per ymap family, alias = the prefix. ALWAYS resolvable, never a guess.
	//   zone entry   - one per popzone region word, prefixes = every family whose ymaps mostly
	//                  land in it. This is the human name, and it comes from the game's data.
	TArray<FString> FamilyNames;
	Families.GetKeys(FamilyNames);
	FamilyNames.Sort();

	auto TopZoneOf = [](const FRudeAreaFamily& F, int32& OutCount) -> FString
	{
		FString BestName;
		OutCount = 0;
		TArray<FString> Keys;
		F.Zones.GetKeys(Keys);
		Keys.Sort();                                   // deterministic on a tie
		for (const FString& K : Keys)
		{
			const int32 C = F.Zones[K];
			if (C > OutCount) { OutCount = C; BestName = K; }
		}
		return BestName;
	};

	FString Json = TEXT("[\n");
	int32 PrefixEntries = 0, ZoneEntries = 0, FamiliesWithZone = 0, AliasCollisions = 0;
	TSet<FString> AllAliasLower;
	for (const FString& FN : FamilyNames)
	{
		const FRudeAreaFamily& F = Families[FN];
		int32 ZCount = 0;
		const FString Zone = TopZoneOf(F, ZCount);
		if (!Zone.IsEmpty()) { ++FamiliesWithZone; }
		// ⛔ `ImportArea` REPLACES '_' WITH ' ' IN THE QUERY BEFORE IT MATCHES, on every surface -
		// so an alias that CONTAINS an underscore can never be matched, exactly or by substring.
		// Measured 2026-09-07: 170 of 375 entries would have been unreachable by name. The alias is
		// spelled with spaces; `prefixes` keeps the real prefix, which is what ImportMapArea eats.
		FString Alias = FN.Replace(TEXT("_"), TEXT(" "));
		// The FALLBACKS keep the space-substituted form too. Using the raw family name here would put
		// the '_' straight back into an alias and make it unmatchable again (law 9) - latent, not live:
		// 0 prefix-vs-prefix collisions in the 375-entry catalog, so it fires only on another corpus,
		// which is exactly when nobody is watching.
		if (AllAliasLower.Contains(Alias.ToLower())) { Alias += TEXT(" (prefix)"); ++AliasCollisions; }
		for (int32 Bump = 2; AllAliasLower.Contains(Alias.ToLower()); ++Bump)
		{
			Alias = FString::Printf(TEXT("%s (prefix %d)"), *FN.Replace(TEXT("_"), TEXT(" ")), Bump);
		}
		Json += FString::Printf(TEXT(
			" {\"alias\":\"%s\",\"prefixes\":[\"%s\"],\"source\":\"prefix\",\"named\":false,")
			TEXT("\"ymaps\":%d,\"entities\":%d,\"ymapsWithExtents\":%d,\"zone\":\"%s\",\"zoneYmaps\":%d,")
			TEXT("\"extentMin\":%s,\"extentMax\":%s,\"note\":\"%s\"},\n"),
			*RudeJsonEscape(Alias), *RudeJsonEscape(FN), F.Ymaps, F.Entities, F.WithExtents,
			*RudeJsonEscape(Zone), ZCount,
			*RudeAreaVecJson(F.bHasExtent, F.MinX, F.MinY, F.MinZ),
			*RudeAreaVecJson(F.bHasExtent, F.MaxX, F.MaxY, F.MaxZ),
			Zone.IsEmpty()
				? TEXT("no human name derivable from the game's region data - the ymap prefix IS the alias")
				: TEXT(""));
		AllAliasLower.Add(Alias.ToLower());
		++PrefixEntries;
	}

	TMap<FString, TArray<FString>> ByZone;
	for (const FString& FN : FamilyNames)
	{
		int32 ZCount = 0;
		const FString Zone = TopZoneOf(Families[FN], ZCount);
		if (!Zone.IsEmpty()) { ByZone.FindOrAdd(Zone).Add(FN); }
	}
	TArray<FString> ZoneNames;
	ByZone.GetKeys(ZoneNames);
	ZoneNames.Sort();
	for (const FString& ZN : ZoneNames)
	{
		TArray<FString>& Pre = ByZone[ZN];
		Pre.Sort();
		int32 ZY = 0, ZE = 0, ZW = 0, ZHits = 0;
		bool bHasExt = false;
		double MnX = 0, MnY = 0, MnZ = 0, MxX = 0, MxY = 0, MxZ = 0;
		FString PreJson;
		for (const FString& P : Pre)
		{
			const FRudeAreaFamily& F = Families[P];
			ZY += F.Ymaps; ZE += F.Entities; ZW += F.WithExtents;
			if (const int32* Hits = F.Zones.Find(ZN)) { ZHits += *Hits; }
			if (F.bHasExtent)
			{
				if (!bHasExt)
				{
					bHasExt = true;
					MnX = F.MinX; MnY = F.MinY; MnZ = F.MinZ;
					MxX = F.MaxX; MxY = F.MaxY; MxZ = F.MaxZ;
				}
				else
				{
					MnX = FMath::Min(MnX, F.MinX); MnY = FMath::Min(MnY, F.MinY); MnZ = FMath::Min(MnZ, F.MinZ);
					MxX = FMath::Max(MxX, F.MaxX); MxY = FMath::Max(MxY, F.MaxY); MxZ = FMath::Max(MxZ, F.MaxZ);
				}
			}
			PreJson += FString::Printf(TEXT("%s\"%s\""), PreJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(P));
		}
		// ⛔ ALIAS COLLISION. `ImportArea` matches an alias EXACTLY first and breaks on the first
		// hit, so two entries sharing a name case-insensitively make the second unreachable by
		// name - silently. Measured 2026-09-07: 1 collision in 375 entries (a `oceana` ymap family
		// under an `Oceana` popzone region). The prefix entry keeps the bare name (it is the exact
		// thing the game calls that family); the zone entry is suffixed so BOTH stay reachable.
		FString ZoneAlias = ZN.Replace(TEXT("_"), TEXT(" "));
		if (AllAliasLower.Contains(ZoneAlias.ToLower()))
		{
			ZoneAlias = ZN.Replace(TEXT("_"), TEXT(" ")) + TEXT(" (zone)");
			++AliasCollisions;
		}
		for (int32 Bump = 2; AllAliasLower.Contains(ZoneAlias.ToLower()); ++Bump)
		{
			ZoneAlias = FString::Printf(TEXT("%s (zone %d)"), *ZN.Replace(TEXT("_"), TEXT(" ")), Bump);
		}
		AllAliasLower.Add(ZoneAlias.ToLower());
		Json += FString::Printf(TEXT(
			" {\"alias\":\"%s\",\"prefixes\":[%s],\"source\":\"zone\",\"named\":true,")
			TEXT("\"ymaps\":%d,\"entities\":%d,\"ymapsWithExtents\":%d,\"zone\":\"%s\",\"zoneYmaps\":%d,")
			TEXT("\"extentMin\":%s,\"extentMax\":%s,\"note\":\"named from the game's own popzone region field\"},\n"),
			*RudeJsonEscape(ZoneAlias), *PreJson, ZY, ZE, ZW, *RudeJsonEscape(ZN), ZHits,
			*RudeAreaVecJson(bHasExt, MnX, MnY, MnZ), *RudeAreaVecJson(bHasExt, MxX, MxY, MxZ));
		++ZoneEntries;
	}
	if (Json.EndsWith(TEXT(",\n"))) { Json.LeftChopInline(2); Json += TEXT("\n"); }
	Json += TEXT("]\n");

	// ---- 4) write it where ImportArea looks ---------------------------------------------------
	FString Out = OutJsonPath.TrimStartAndEnd();
	if (Out.IsEmpty())
	{
		const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("RUDE"));
		if (!Self.IsValid()) { return Fail(TEXT("OutJsonPath is empty and the RUDE plugin is not mounted")); }
		Out = Self->GetBaseDir() / TEXT("Catalogs") / TEXT("area_aliases.json");
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Out), true);
	const bool bWrote = FFileHelper::SaveStringToFile(Json, *Out, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool bOk = bWrote && (PrefixEntries + ZoneEntries) > 0;
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"outPath\":\"%s\",\"wrote\":%s,\"entries\":%d,\"prefixEntries\":%d,\"zoneEntries\":%d,")
		TEXT("\"aliasCollisionsSuffixed\":%d,")
		TEXT("\"families\":%d,\"familiesWithZone\":%d,\"familiesWithoutZone\":%d,")
		TEXT("\"zonesParsed\":%d,\"popzone\":\"%s\",\"ymapsListed\":%d,\"ymapsRead\":%d,\"ymapsMissing\":%d,")
		TEXT("\"ymapsNoExtents\":%d,\"ymapsDegenerateExtents\":%d,\"ymapsZoned\":%d,\"ymapsUnzoned\":%d,")
		TEXT("\"entities\":%lld,\"xmlBytesRead\":%lld}"),
		bOk ? TEXT("true") : TEXT("false"), *RudeJsonEscape(Out), bWrote ? TEXT("true") : TEXT("false"),
		PrefixEntries + ZoneEntries, PrefixEntries, ZoneEntries, AliasCollisions,
		FamilyNames.Num(), FamiliesWithZone, FamilyNames.Num() - FamiliesWithZone,
		Zones.Num(), *RudeJsonEscape(ZonePath), Ymaps.Num(), Read, Missing,
		NoExtents, Degenerate, Zoned, Unzoned, TotalEntities, TotalBytes);
}

// ---- RudeDoctor (agent) ----------------------------------------------------------------------
FString URudeToolset::RudeDoctor(const FString& CorpusRoot)
{
	TArray<FString> Problems;
	auto Note = [&Problems](const FString& S) { Problems.Add(S); };

	// ---- engine ------------------------------------------------------------------------------
	const FEngineVersion& Ver = FEngineVersion::Current();
	const FString EngineText = Ver.ToString(EVersionComponent::Patch);
	const bool bEngineExpected = (Ver.GetMajor() == 5 && Ver.GetMinor() == 8);
	if (!bEngineExpected)
	{
		Note(FString::Printf(TEXT("engine is %s - RUDE is developed and measured against 5.8; other versions are untested"), *EngineText));
	}

	// ---- plugins -----------------------------------------------------------------------------
	// REQUIRED: RUDE itself, and ToolsetRegistry - what makes the tools reachable at all. Without
	// it there is no tool surface and nothing in this plugin can be called.
	// OPTIONAL: ModelContextProtocol is ONE of the four surfaces (AGENTS §2) - the one an AGENT
	// drives RUDE through. A person working in the Slate panel or from the CLI has a completely
	// healthy install without it, and RUDE.uplugin does not depend on it. It is REPORTED with
	// present/enabled and is deliberately NOT a problem: a doctor that cries wolf on its headline
	// signal teaches people to ignore the headline.
	IPluginManager& PM = IPluginManager::Get();
	struct FRudeWantedPlugin { const TCHAR* Name; bool bRequired; };
	const FRudeWantedPlugin Wanted[] = {
		{ TEXT("RUDE"), true }, { TEXT("ToolsetRegistry"), true }, { TEXT("ModelContextProtocol"), false } };
	FString PluginJson;
	for (const FRudeWantedPlugin& W : Wanted)
	{
		const TSharedPtr<IPlugin> P = PM.FindPlugin(W.Name);
		const bool bFound = P.IsValid();
		const bool bOn = bFound && P->IsEnabled();
		PluginJson += FString::Printf(TEXT("%s{\"name\":\"%s\",\"required\":%s,\"present\":%s,\"enabled\":%s}"),
			PluginJson.IsEmpty() ? TEXT("") : TEXT(","), W.Name, W.bRequired ? TEXT("true") : TEXT("false"),
			bFound ? TEXT("true") : TEXT("false"), bOn ? TEXT("true") : TEXT("false"));
		if (!bOn && W.bRequired)
		{
			Note(FString::Printf(TEXT("plugin %s is %s - enable it in the project's plugin settings and restart"),
				W.Name, bFound ? TEXT("present but disabled") : TEXT("not installed")));
		}
	}
	const int32 EnabledPlugins = PM.GetEnabledPlugins().Num();

	// ---- the plugin's own content ------------------------------------------------------------
	// A clone whose Content never mounted fails deep inside an import with a material error; say it
	// here instead.
	const bool bMastersMounted = FPackageName::DoesPackageExist(TEXT("/RUDE/Masters/M_RUDE_Opaque"));
	if (!bMastersMounted)
	{
		Note(TEXT("/RUDE/Masters is not mounted - the plugin's Content folder is missing from this build, and every import will fail to find a master material"));
	}

	// Masters: count what is on disk and ASK the staleness rule - RudeGeneratedMasterHealth, the
	// one the generator itself calls (RudeToolset.cpp). The doctor deliberately does not restate
	// the rule: the restated copy said only 'bucket-1 without OpacityScale' and would have gone on
	// reporting every tint master healthy from the day the tint condition was added. Read-only:
	// nothing is regenerated here.
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	ARM.Get().ScanPathsSynchronous({ TEXT("/RUDE/Masters") }, true);
	TArray<FAssetData> MasterAssets;
	ARM.Get().GetAssetsByPath(FName(TEXT("/RUDE/Masters")), MasterAssets, /*bRecursive*/ true);
	int32 MastersTotal = 0, MastersGen = 0, MastersStale = 0, MastersUnparsed = 0;
	int32 StaleNamedCount = 0;        // the NAMED masters, which RegenerateMasters does NOT cover
	FString StaleNamed;
	FString StaleNames;
	for (const FAssetData& AD : MasterAssets)
	{
		const FString AssetName = AD.AssetName.ToString();
		if (!AssetName.StartsWith(TEXT("M_RUDE_"))) { continue; }
		++MastersTotal;
		const bool bGen = AD.PackageName.ToString().Contains(TEXT("/Masters/Gen/"));
		if (bGen) { ++MastersGen; }
		// The generated masters, plus the TWO named masters whose generators also upgrade them in
		// place (M_RUDE_Detail, M_RUDE_Cutout). The other four named masters have no upgrade rule at
		// all, so they are counted and deliberately not judged - inventing a condition for them would
		// be the second definition this lane just removed.
		const bool bNamedWithRule = (AssetName == TEXT("M_RUDE_Detail") || AssetName == TEXT("M_RUDE_Cutout"));
		if (!bGen && !bNamedWithRule) { continue; }
		UMaterial* M = Cast<UMaterial>(AD.GetAsset());
		FString Why;
		const ERudeMasterHealth Health = RudeGeneratedMasterHealth(M, AssetName, Why);
		if (Health == ERudeMasterHealth::Unreadable) { ++MastersUnparsed; continue; }
		if (Health != ERudeMasterHealth::Stale) { continue; }
		++MastersStale;
		if (!bGen)
		{
			++StaleNamedCount;
			StaleNamed += FString::Printf(TEXT("%s%s"), StaleNamed.IsEmpty() ? TEXT("") : TEXT(", "), *AssetName);
		}
		StaleNames += FString::Printf(TEXT("%s\"%s (%s)\""), StaleNames.IsEmpty() ? TEXT("") : TEXT(","),
			*RudeJsonEscape(AssetName), *RudeJsonEscape(Why));
	}
	const int32 StaleGenMasters = MastersStale - StaleNamedCount;
	if (StaleGenMasters > 0)
	{
		Note(FString::Printf(TEXT("%d generated master(s) under /RUDE/Masters/Gen are stale by the generator's own rule - run RegenerateMasters"), StaleGenMasters));
	}
	if (!StaleNamed.IsEmpty())
	{
		// Said separately because RegenerateMasters walks /RUDE/Masters/Gen ONLY and never touches the
		// named masters: pointing a user at that tool for this fault would be advice that cannot work.
		Note(FString::Printf(TEXT("named master(s) %s are stale by the generator's own rule - RegenerateMasters does NOT cover them (it walks /RUDE/Masters/Gen only); the next import that needs one regenerates it in place"), *StaleNamed));
	}

	// ---- DID THE MASTERS ACTUALLY COMPILE? (law 52) ------------------------------------------
	// A material that fails to compile does not error and does not disappear - the engine logs one
	// warning and substitutes the DEFAULT material, which reads NO parameters. Measured 2026-09-08:
	// all 6 generated masters with a detail texture were in that state, and 990 of 12,374 material
	// instances (8.0%) were parented to one of them - including the downtown GROUND, the plaza and
	// the tower. Nothing asked, so nothing knew. The doctor asks.
	// Reported as UNKNOWN (-1), never as clean, when the run has no rendering: a question that could
	// not be asked must not read as one that passed.
	int32 MastersCompileFailed = -1;
	FString CompileFailedNames;
	if (FApp::CanEverRender())
	{
		if (GShaderCompilingManager) { GShaderCompilingManager->FinishAllCompilation(); }
		MastersCompileFailed = 0;
		for (const FAssetData& AD : MasterAssets)
		{
			const FString AssetName = AD.AssetName.ToString();
			if (!AssetName.StartsWith(TEXT("M_RUDE_"))) { continue; }
			UMaterial* Mat = LoadObject<UMaterial>(nullptr, *(AD.PackageName.ToString() + TEXT(".") + AssetName));
			if (!Mat) { continue; }
			const FMaterialResource* Res = Mat->GetMaterialResource(GMaxRHIShaderPlatform, EMaterialQualityLevel::Num);
			if (!Res) { continue; }
			const TArray<FString>& Errors = Res->GetCompileErrors();
			if (Errors.Num() > 0)
			{
				++MastersCompileFailed;
				CompileFailedNames += FString::Printf(TEXT("%s\"%s: %s\""),
					CompileFailedNames.IsEmpty() ? TEXT("") : TEXT(","),
					*RudeJsonEscape(AssetName), *RudeJsonEscape(Errors[0]));
			}
		}
		if (MastersCompileFailed > 0)
		{
			Note(FString::Printf(TEXT("%d master(s) FAIL TO COMPILE - every material instance parented to one of them is rendering as Unreal's default material, which ignores every setting RUDE puts on it. Fix the graph, then RegenerateMasters."), MastersCompileFailed));
		}
	}

	// ---- the area catalog --------------------------------------------------------------------
	const TSharedPtr<IPlugin> Self = PM.FindPlugin(TEXT("RUDE"));
	FString CatalogPath;
	int32 CatalogEntries = 0;
	bool bCatalog = false;
	if (Self.IsValid())
	{
		CatalogPath = Self->GetBaseDir() / TEXT("Catalogs") / TEXT("area_aliases.json");
		FString Raw;
		if (FFileHelper::LoadFileToString(Raw, *CatalogPath))
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
			if (FJsonSerializer::Deserialize(Reader, Arr))
			{
				bCatalog = true;
				CatalogEntries = Arr.Num();
			}
			else
			{
				Note(FString::Printf(TEXT("the area catalog at %s is not a JSON array - ImportArea will refuse it"), *CatalogPath));
			}
		}
		else
		{
			Note(FString::Printf(TEXT("no area catalog at %s - ImportArea cannot resolve a district name until BuildAreaCatalog writes one"), *CatalogPath));
		}
	}

	// ---- the corpus --------------------------------------------------------------------------
	// The single most common new-user mistake: pointing CorpusRoot at a folder of loose XML instead
	// of a ledgered filebase. Both are folders; only one has the manifest every lookup goes through.
	const FString Root = CorpusRoot.TrimStartAndEnd();
	FString CorpusJson = TEXT("{\"checked\":false}");
	if (!Root.IsEmpty())
	{
		const bool bDir = FPaths::DirectoryExists(Root);
		const bool bLedgered = bDir && FRudeCorpus::LooksLikeCorpus(Root);
		if (!bDir)
		{
			Note(FString::Printf(TEXT("CorpusRoot %s does not exist"), *Root));
			CorpusJson = FString::Printf(TEXT("{\"checked\":true,\"exists\":false,\"ledgered\":false,\"root\":\"%s\"}"), *RudeJsonEscape(Root));
		}
		else if (!bLedgered)
		{
			// Count what IS there, so the message can say what the folder looks like instead of
			// only what it is not.
			TArray<FString> Xml;
			IFileManager::Get().FindFilesRecursive(Xml, *Root, TEXT("*.xml"), true, false, false);
			Note(FString::Printf(TEXT("CorpusRoot %s is a FLAT folder, not a ledgered filebase (no _FILEBASE.json / _PROVENANCE.jsonl): the single-file tools work, the corpus tools (ImportMapArea, ImportArea, ImportMlo, BuildAreaCatalog) do not"), *Root));
			CorpusJson = FString::Printf(TEXT("{\"checked\":true,\"exists\":true,\"ledgered\":false,\"root\":\"%s\",\"xmlFiles\":%d}"),
				*RudeJsonEscape(Root), Xml.Num());
		}
		else
		{
			FString OpenError;
			const TSharedPtr<FRudeCorpus> Corpus = FRudeCorpus::Open(Root, OpenError);
			if (!Corpus.IsValid())
			{
				Note(FString::Printf(TEXT("CorpusRoot %s has the ledgers but will not open: %s"), *Root, *OpenError));
				CorpusJson = FString::Printf(TEXT("{\"checked\":true,\"exists\":true,\"ledgered\":true,\"opened\":false,\"root\":\"%s\",\"error\":\"%s\"}"),
					*RudeJsonEscape(Root), *RudeJsonEscape(OpenError));
			}
			else
			{
				TMap<FString, int32> ByType;
				Corpus->CountByType(ByType);
				const TCHAR* Lanes[] = { TEXT("ymap"), TEXT("ytyp"), TEXT("ydr"), TEXT("ydd"), TEXT("yft"), TEXT("ytd"), TEXT("ybn") };
				FString LaneJson;
				for (const TCHAR* Lane : Lanes)
				{
					const int32* N = ByType.Find(FString(Lane));
					LaneJson += FString::Printf(TEXT("%s\"%s\":%d"), LaneJson.IsEmpty() ? TEXT("") : TEXT(","), Lane, N ? *N : 0);
					if (!N || *N == 0)
					{
						Note(FString::Printf(TEXT("the corpus ledger has no %s rows - that lane's tools have nothing to read"), Lane));
					}
				}
				CorpusJson = FString::Printf(TEXT(
					"{\"checked\":true,\"exists\":true,\"ledgered\":true,\"opened\":true,\"root\":\"%s\",")
					TEXT("\"title\":\"%s\",\"routVersion\":%d,\"rows\":%d,\"lanes\":{%s}}"),
					*RudeJsonEscape(Root), *RudeJsonEscape(Corpus->GetTitle()), Corpus->GetRoutVersion(),
					Corpus->Num(), *LaneJson);
			}
		}
	}

	// ---- headless / world state --------------------------------------------------------------
	const bool bSlate = FSlateApplication::IsInitialized();
	const bool bWorld = (GEditor && GEditor->GetEditorWorldContext().World() != nullptr);
	if (!bWorld)
	{
		Note(TEXT("there is no editor world - the tools that spawn actors need one (open or create a level first; the CLI opens one with NewLevel)"));
	}

	FString ProblemJson;
	for (const FString& P : Problems)
	{
		ProblemJson += FString::Printf(TEXT("%s\"%s\""), ProblemJson.IsEmpty() ? TEXT("") : TEXT(","), *RudeJsonEscape(P));
	}
	return FString::Printf(TEXT(
		"{\"ok\":%s,\"problems\":%d,\"problemList\":[%s],")
		TEXT("\"engine\":\"%s\",\"engineExpected\":\"5.8\",\"engineMatches\":%s,")
		TEXT("\"plugins\":[%s],\"pluginsEnabled\":%d,")
		TEXT("\"mastersMounted\":%s,\"masters\":%d,\"generatedMasters\":%d,\"staleMasters\":%d,")
		TEXT("\"unparsedMasters\":%d,\"staleMasterNames\":[%s],")
		TEXT("\"mastersCompileFailed\":%d,\"mastersCompileChecked\":%s,\"compileFailedMasters\":[%s],")
		TEXT("\"catalogPath\":\"%s\",\"catalogPresent\":%s,\"catalogEntries\":%d,")
		TEXT("\"corpus\":%s,\"headless\":%s,\"unattended\":%s,\"editorWorld\":%s}"),
		Problems.Num() == 0 ? TEXT("true") : TEXT("false"), Problems.Num(), *ProblemJson,
		*RudeJsonEscape(EngineText), bEngineExpected ? TEXT("true") : TEXT("false"),
		*PluginJson, EnabledPlugins,
		bMastersMounted ? TEXT("true") : TEXT("false"), MastersTotal, MastersGen, MastersStale,
		MastersUnparsed, *StaleNames,
		MastersCompileFailed, MastersCompileFailed >= 0 ? TEXT("true") : TEXT("false"), *CompileFailedNames,
		*RudeJsonEscape(CatalogPath), bCatalog ? TEXT("true") : TEXT("false"), CatalogEntries,
		*CorpusJson, bSlate ? TEXT("false") : TEXT("true"),
		FApp::IsUnattended() ? TEXT("true") : TEXT("false"), bWorld ? TEXT("true") : TEXT("false"));
}
