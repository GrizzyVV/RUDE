// RUDE - RAGE <-> Unreal Development Environment
// The RUDE MCP toolset: RAGE format import/export exposed as agent-callable tools.
#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "RudeToolset.generated.h"

// RUDE toolset - RAGE (GTA V) format import/export tools.
// Import RAGE XML drawables (.ydr.xml) as StaticMesh assets, with the RUDE
// GTA<->UE transform convention applied: cm scale, Y mirror, and triangle
// winding PASSED THROUGH AS-IS. (Under the Y-mirror, RAGE winding already faces
// outward in UE - reversing it renders inside-out. Only the OBJ lane reverses,
// because UE's OBJ importer adds its own handedness flip.)
UCLASS()
class URudeToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("0.1.0"); }

	// Smoke test: returns the RUDE plugin version and status.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Check that RUDE is loaded and see its version.", RudeAudience="agent"))
	static FString Ping();

	// Import a ydr XML file (.ydr.xml) as a UStaticMesh asset.
	// XmlPath: absolute path to a *.ydr.xml file on disk.
	// DestFolder: content folder for the new asset (e.g. "/Game/RUDE/Meshes/Props").
	// Returns a JSON string: {ok, assetPath, geometries, vertices, triangles, slots}
	// or {ok:false, error} on failure.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring one GTA V model into Unreal as a Static Mesh you can edit."))
	static FString ImportYdr(const FString& XmlPath, const FString& DestFolder);

	// Import ONE named entry of a ydd XML dictionary (.ydd.xml) as a UStaticMesh asset.
	// A <DrawableDictionary> holds MANY drawables; EntryName picks one, matched against each
	// entry's <Name> case-insensitively and by joaat hash BOTH ways (entries are usually named
	// hash_XXXXXXXX - the lowercase-joaat of the original name, spelled uppercase-hex; the
	// ymap<->ytyp<->dictionary joins are hash-to-hash). The imported MESH takes EntryName
	// (the archetype-facing identity), not the entry's own often-unresolvable <Name>.
	// XmlPath: absolute path to a *.ydd.xml file. DestFolder: content folder for the asset.
	// Returns ImportYdr's JSON shape; an unknown entry errors loudly, listing what IS there.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring one model out of a GTA V model dictionary file into Unreal as a Static Mesh. Give the name of the entry you want."))
	static FString ImportYddEntry(const FString& XmlPath, const FString& EntryName,
	                              const FString& DestFolder);

	// Import a ytd XML manifest (.ytd.xml) as UTexture2D assets with correct
	// semantics derived from each entry's Usage (NORMAL -> TC_Normalmap + sRGB off,
	// SPECULAR -> sRGB off, DIFFUSE -> sRGB on).
	// XmlPath: absolute path to a *.ytd.xml file.
	// PixelFolder: folder of decoded PNGs matching the entry names (BC-decode
	// happens offline until native decode lands).
	// DestFolder: content folder root; assets land in <DestFolder>/<TxdName>/.
	// Returns JSON: {ok, txd, imported, missingPixels:[...]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a GTA V texture set into Unreal, with normal and specular maps set up correctly."))
	static FString ImportYtd(const FString& XmlPath, const FString& PixelFolder,
	                         const FString& DestFolder);

	// Batch ImportYdr: ListPath = a text file of absolute *.ydr.xml paths, one per line.
	// Imports each as a UStaticMesh into DestFolder via the same path as ImportYdr,
	// SKIPPING files whose target asset already exists (idempotent re-runs; the P4
	// hash-manifest resumability model, name-level) UNLESS Mode is "FORCE" - then every
	// file reimports in place (rebinds MaterialInstances against currently-imported
	// textures; the texture-pass re-bind flow). Progress goes to the log.
	// Returns JSON: {ok, imported, skipped, failed, failedFiles:[...first 30]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring in many GTA V models at once from a list file. Skips anything already imported."))
	static FString ImportYdrBatch(const FString& ListPath, const FString& DestFolder,
	                              const FString& Mode);

	// Spawn a scene manifest (tools/ingest_ymap.py output: JSON array of
	// {ymap, entities:[{archetype, drawable, ue_location, ue_quat, scaleXY, scaleZ,
	// lodLevel, resolved}]}) into the CURRENT editor level: one actor per ymap holding
	// one InstancedStaticMeshComponent per unique drawable (the P4 ISM-first scale
	// model). Entities are filtered to HD/ORPHANHD lod levels unless Filter == "ALL".
	// Meshes resolve by lowercase drawable name under MeshFolder; unresolved archetypes
	// and missing meshes render as proxy cubes (corpus-hole policy). Transforms are the
	// manifest's UE-space values (already through the pinned GTA->UE convention).
	// Returns JSON: {ok, ymaps, entities, instances, proxies, uniqueMeshes,
	// missingMeshes, topMissing:[...first 20]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Re-place an area you already imported, into the level you have open. Reads the manifest that Build Map Area wrote, so it spawns the objects again WITHOUT re-importing any models."))
	static FString ImportScene(const FString& ManifestPath, const FString& MeshFolder,
	                           const FString& Filter);

	// Export a UStaticMesh as a ydr XML file (.ydr.xml) (the reverse
	// lane). Positions/normals/UVs inverse-transformed per the RUDE convention;
	// shader presets recovered from material slot names; texture names recovered
	// from bound RUDE MaterialInstances where present.
	// AssetPath: content path of the StaticMesh (e.g. "/Game/RUDE/Meshes/Props/prop_x").
	// OutXmlPath: absolute file path for the emitted *.ydr.xml.
	// Returns JSON: {ok, xmlPath, geometries, vertices, triangles} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save a Static Mesh back out as a GTA V model, in the editable text form."))
	static FString ExportYdr(const FString& AssetPath, const FString& OutXmlPath);

	// Export a UStaticMesh's collision as a standalone .ybn XML (physics
	// bounds) - a valid BoundsFile. NOTE (corrected 2026-07-24):
	// PROP collision actually comes from the ydr's EMBEDDED <Bounds> + archetype flag
	// bit 0x20000, NOT a standalone .ybn (props share NAMED-bound dictionaries; a
	// standalone unnamed bound never matches). ExportYdr embeds the real collider;
	// this tool remains for shared/world collision-dictionary work later. Verts
	// inverse-transformed per the RUDE convention.
	// Returns JSON: {ok, xmlPath, vertices, triangles} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save a mesh's collision on its own. Most props do not need this - their collision travels inside the model itself."))
	static FString ExportYbn(const FString& AssetPath, const FString& OutXmlPath);

	// Spawn (or move) the sea-level reference plane - GTA's ocean sits at world z=0,
	// which coastal authoring needs to see. This is a VISUAL REFERENCE, not game data:
	// FiveM water comes from water.xml, which RUDE does not yet read or write.
	// SizeMetres: half-extent of the plane in GTA metres (e.g. "4000").
	// ZMetres: sea height in GTA metres (default 0). Lands in the RUDE_ENV folder so
	// ImportScene's respawn (which clears RUDE_LS) leaves it alone.
	// Returns JSON: {ok, actor, sizeM, zM}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Add a flat ocean surface at sea level so you can see where the water sits. A visual guide only, not game data."))
	static FString SpawnSeaLevel(const FString& SizeMetres, const FString& ZMetres);

	// Position the level-editor perspective viewport and capture a screenshot -
	// the agent-vision primitive (verify imports/materials without human eyes at
	// the machine). CamSpec: "x,y,z,pitch,yaw" (UE cm/degrees; ";" also accepted, because
	// -ExecCmds splits its own command list on commas). The PNG lands at
	// OutPng on the NEXT viewport draw - poll the file. Returns {ok, requested}.
	// ⛔ Blocks on FinishAllCompilation first: a still-compiling StaticMesh renders NOTHING, and
	// compilation finishes smallest-first, so an unsynchronised shot shows the small props and
	// drops the large meshes - a convincing but FALSE "big meshes are missing" defect
	// (2026-07-28, LOG). Never remove that barrier.
	// ViewMode: "" / "LIT" (default) · "UNLIT" · "WIREFRAME". ⭐ Use UNLIT to answer "did the
	// textures bind?" - a Lit shot multiplies albedo by scene lighting, so under a dark sky an
	// untextured city and a fully textured one photograph as the same grey (2026-07-29).
	// SettleSeconds: minimum quiet time before the shot fires (default 25). ⭐ The capture is
	// DEFERRED onto the editor tick, not taken inline - the editor mounts asynchronously (sky and
	// reflection capture, mip residency, shader compilation) and NOTHING a command does from
	// inside a tick can present a frame. Three synchronous attempts each produced a different
	// false reading; see LOG 2026-07-29. Poll for the file.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Point the viewport somewhere and save a screenshot once the scene has finished loading. Use UNLIT to see texture colours without lighting.", RudeAudience="agent"))
	static FString CaptureView(const FString& CamSpec, const FString& OutPng,
	                           const FString& ViewMode, const FString& SettleSeconds);

	// File a FLAT export dump into the filebase. You export from whatever extractor you
	// already use into any folder (they dump files flat); this sorts them by type into
	// the right precedence slot, so nobody hand-sorts anything.
	// DumpFolder: the folder you exported into. SourceName: "base", "update", or a DLC
	// pack name (e.g. "mpbiker"); leave EMPTY to use the dump folder's own name.
	// FilebaseRoot: the filebase. Move: "MOVE" (default) or "COPY".
	// Returns JSON: {ok, source, dest, filed, byType:{...}, skipped}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Sort a folder of exported game files into your project. Files go to <slot>/<type>/ - the slot is which game version you say they came from (base, update, or a DLC pack name), the type is the file extension."))
	static FString IngestExport(const FString& DumpFolder, const FString& SourceName,
	                            const FString& FilebaseRoot, const FString& Move);

	// Create the RUDE FILEBASE: a seeded folder tree the user exports their own game
	// files into, shaped to their ACTUAL install so build-version-accurate assets stay
	// separable. The same asset name exists in base, update and many DLCs, so the tree
	// encodes LOAD ORDER as numeric prefixes (higher wins): 00_base < 10_update < 20_dlc/NNN_name.
	// FilebaseRoot: where to create it. GameRoot: the user's GTA V install (its
	// update/x64/dlcpacks folders are enumerated - directory names only, no archive is
	// opened or decrypted). Options: "CORE" (default; core type folders per DLC) or
	// "ALL" (every type folder everywhere).
	// Writes _FILEBASE.json (sources + order + build fingerprint) and README.md.
	// Returns JSON: {ok, root, dlcPacks, baseArchives, foldersCreated}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Create the empty project folder tree your game files go into, shaped to your own install. Does not read or unpack any game archive."))
	static FString CreateFilebase(const FString& FilebaseRoot, const FString& GameRoot,
	                              const FString& Options);

	// Save every dirty CONTENT package (assets - never level/map packages, which stay the
	// operator's call) without prompting. Exists so an agent-run import chain can persist its
	// own work: unsaved imports die with the editor, and the texture-refresh law wants the save
	// to happen AFTER texture compilation settles - ImportYtd already blocks until quiet, so a
	// chain calling this LAST is law-abiding by construction.
	// Returns JSON: {ok} (SaveDirtyPackages reports only overall success).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save every changed RUDE asset to disk. Does not touch your level - saving the map stays yours.", RudeAudience="agent"))
	static FString SaveAssets();

	// Drop streaming-level entries whose package no longer exists on disk, then save the map.
	// ⛔ WHY THIS EXISTS (2026-07-29, Matt hit it twice): deleting a sublevel from the Content
	// Browser removes the PACKAGE but leaves the persistent level's streaming-level array pointing
	// at it, so every subsequent open throws "Failed to find streamed level ..., please fix the
	// reference to it in the Level Browser". The reference is real and dangling - it is NOT fixed
	// by rescanning, and it cannot be found by grepping the .umap (a soft object path is not plain
	// text in a uasset - that assumption is what created this mess in the first place).
	// Mode="APPLY" writes; anything else reports what it WOULD remove and changes nothing.
	// Returns JSON: {ok, checked, dangling, removed, saved, names:[...]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Repair a level that complains about missing sublevels, by forgetting the ones that are gone.", RudeAudience="agent"))
	static FString FixLevelRefs(const FString& Mode);

	// Batch ImportYtd: ListPath = a text file of absolute *.ytd.xml paths, one per line.
	// Each entry's PixelFolder is DERIVED - QUARRY writes the decoded pixels to a sibling
	// "<stem>/" folder beside the XML (and resolve carries that sidecar with the winning copy),
	// so the pair is self-describing and no per-file pixel argument exists to get wrong.
	// Assets land in <DestFolder>/<TxdName>/ via the same path as ImportYtd. A txd whose content
	// folder already exists on disk is SKIPPED unless Mode is "FORCE" (re-import in place).
	// Progress goes to the log every 100. Returns JSON:
	// {ok, imported, texturesImported, skipped, failed, failedFiles:[...first 30]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring in many GTA V texture sets at once from a list file. Skips sets already imported unless you force it."))
	static FString ImportYtdBatch(const FString& ListPath, const FString& DestFolder,
	                              const FString& Mode);

	// THE THREAD-PULL: one call ingests a whole map area from the corpus.
	// 1) builds an archetype index from every .ytyp XML under <CorpusRoot>/ytyp
	// 2) parses <CorpusRoot>/ymap/<YmapPrefix>*.xml entities (import-lane transforms)
	// 3) imports every referenced drawable (skip-if-exists): plain drawables from
	//    <CorpusRoot>/ydr, fragments from yft, dictionary entries picked by name
	//    out of their <CorpusRoot>/ydd file
	// 4) writes the scene manifest and spawns it via ImportScene (ISM actors,
	//    idempotent, proxy cubes for corpus holes).
	// Filter: "HD" (default) or "ALL" lod levels, optionally + "+FORCE" to RE-IMPORT meshes that
	// already exist ("HD+FORCE", "ALL+FORCE", or bare "FORCE" meaning HD). ⭐ FORCE is the only
	// refresh path the yft/ydd lanes have - they are reachable only through this tool, so without it
	// a corpus that gains data can refresh only its ydr meshes and the project silently becomes a
	// MIX of two vintages (2026-07-30). Textures remain a separate pass
	// until native BC decode lands. Returns JSON: {ok, ymaps, entities, resolved,
	// meshesImported, meshesSkipped, meshesFailed, spawn:{...ImportScene stats}}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Build a whole area of the map in your open level - brings in the models it needs and places them. WARNING: this REPLACES any area you loaded before."))
	static FString ImportMapArea(const FString& CorpusRoot, const FString& YmapPrefix,
	                             const FString& DestMeshFolder, const FString& Filter);

	// Import a map area by its HUMAN name ("Downtown", "Vespucci Beach & Canals", ...) using the
	// measured area catalog (reports/area_aliases.json: 132 entries whose prefixes partition all
	// resolved ymaps - built and machine-verified 2026-07-28). Resolves the alias (case-insensitive,
	// also accepts a raw prefix-family name), joins the entry's prefixes + exact basenames into
	// ImportMapArea's comma-list form, and delegates - one code path, no drift.
	// AreaName: catalog alias. CatalogPath: absolute path to area_aliases.json.
	// CorpusRoot/DestMeshFolder/Filter: as ImportMapArea. Empty AreaName lists all aliases.
	// Returns ImportMapArea's own JSON, or {ok:false,error} / an alias listing.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Build a named district like 'Downtown' in your open level - no filename prefixes to know. Leave the area name empty to see every name you can pick."))
	static FString ImportArea(const FString& AreaName, const FString& CatalogPath,
	                          const FString& CorpusRoot, const FString& DestMeshFolder,
	                          const FString& Filter);

	// THE INTERIOR IMPORTER (v1) - the consumer for the MLO data QUARRY now emits
	// (ENGINEERING_LOG "MLO EMISSION" / "EXTENSIONS DECODED"). Locates the named
	// CMloArchetypeDef across <CorpusRoot>/ytyp/*.xml - name matching is hash-tolerant
	// BOTH ways, the ImportYddEntry convention (MLO names are usually hash_XXXXXXXX in
	// the corpus; joaat("ch3_01_trlr_int") == 0xCB21C865 recovers that one, e.g.) -
	// imports every mesh its entities reference (the ydr/yft/ydd lanes shared with
	// ImportMapArea), and spawns each room's entities at their MLO-LOCAL transforms
	// under ONE root actor at the WORLD ORIGIN (outliner folder RUDE_MLO).
	// V1 SCOPE, deliberate: spawn-into-current-level at origin. Placing the interior at
	// a ymap CMloInstanceDef world transform and packing it as a Level Instance belong
	// to Build Interior later - the BUILD_AREA_DESIGN section-5 swap contract is why every
	// spawned actor is TAGGED: all carry "RUDE_MLO:<archetype>" (the CORPUS spelling -
	// deterministic identity), room actors add "RUDE_MLO_Room:<roomName>", portal-attached
	// entities (doors) group under "RUDE_MLO_Portal". Re-running REPLACES this archetype's
	// actors by tag (clear-by-tag survives OFPA folder rewrites) and touches nothing else.
	// CLightAttrDef instances become light components, fields mapped honestly:
	// lightType 1 -> PointLight, 2 -> SpotLight (cone inner/outer, clamped to UE's 80-degree
	// ceiling), 4 -> PointLight + SourceLength = extents.x (capsule); posn(+offsetPosition)/
	// colour/intensity/falloff(->attenuation radius)/direction are consumed. NOT mapped, and
	// why: flags/timeFlags (bit meanings undecoded - LOG), corona*/vol*/shadow*/cullingPlane/
	// projectedTextureKey/falloffExponent (no proven UE equivalent), tangent (UE derives its
	// own light frame), boneTag (bone frames don't exist on a static-mesh import, so light
	// posn is applied ENTITY-local - a fragment whose light bone sits off-origin lands
	// slightly off). Intensity scale is one named UNCALIBRATED constant (agent's call).
	// Filter (agent's design, NOT the lod filter of the map tools - interior entities are
	// near-uniformly ORPHANHD so a lod filter would be a no-op): empty or "ALL" = the whole
	// interior; else a comma-separated ROOM-name list (case-insensitive) spawns only those
	// rooms plus their portal doors. Portals and entity sets are SUMMARIZED in the verdict,
	// not spawned (v1). Returns JSON: {ok, archetype, requested, ytyp, rooms, roomNames,
	// portals, portalRooms, entitySets, entities, spawned, proxies, unresolvedArchetypes,
	// lights, lightsSkipped[, lightProblem], otherExtensions, badAttachedRefs,
	// unroomedEntities, meshesImported, meshesSkipped, meshesFailed, meshesMissingFromCorpus}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Build a GTA V interior (MLO) in your open level - rooms, furniture and lights, standing at the world origin. Give the interior's archetype name; optionally list room names to spawn only those rooms."))
	static FString ImportMlo(const FString& CorpusRoot, const FString& MloArchetypeName,
	                         const FString& DestMeshFolder, const FString& Filter);

	// Emit a CMapTypes .ytyp (XML, FiveM-Legacy-loadable) defining one
	// CBaseArchetypeDef per drawable. YdrSpecs: comma-separated
	// "absPathTo.ydr.xml[;txd[;physDict]]". Encodes the in-game-proven collision
	// model: embedded <Bounds> children gate flag bit 17 (0x20000) AND a NON-EMPTY
	// <physicsDictionary> (the switch; no .ybn file is shipped or needed); embedded
	// ShaderGroup TextureDictionary -> empty archetype textureDictionary, else the
	// asset's own name. Returns JSON: {ok, ytypPath, archetypes}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Create the definition file that tells the game what your models are and how they behave, including whether they have collision."))
	static FString ExportYtyp(const FString& YdrSpecs, const FString& YtypName,
	                          const FString& OutYtypPath);

	// Emit a complete FiveM placement resource: stream/<name>.ymap (XML CMapData)
	// + fxmanifest.lua (with the REQUIRED this_is_a_map). EntitiesJsonPath: JSON
	// array of {archetype, ue:{x,y,z}, ue_quat:{x,y,z,w}?} in UE space; transforms
	// use the pinned EXPORT-lane conventions (cm->m Y-mirror; gta_quat =
	// (-x, y, -z, w), bench-proven). Returns JSON: {ok, ymapPath, entities}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Create a ready-to-use FiveM map resource from a list of placed objects."))
	static FString ExportYmap(const FString& EntitiesJsonPath, const FString& MapName,
	                          const FString& OutDir);

	// Export a UTexture2D's source pixels to a PNG on disk (feeds the PBR->RAGE
	// ytd pipeline). Reads the texture Source (BGRA8), writes PNG via ImageWrapper.
	// TexturePath: content path of the Texture2D. OutPngPath: absolute *.png path.
	// Returns JSON: {ok, pngPath, width, height} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save a texture out of Unreal as a PNG image file."))
	static FString ExportTexture(const FString& TexturePath, const FString& OutPngPath);

	// Export UTexture2D(s) directly to a binary FiveM .ytd (RSC7 v13) - CLEAN-ROOM,
	// no CodeWalker. The RSC7 container (pgDictionary<grcTexture> system segment +
	// page-aligned graphics segment + segment flags + raw deflate) was reversed from
	// our own CW diff pair and byte-verified (tools/write_ytd.py; ENGINEERING_LOG
	// "RSC7 binary container"). This is P5 step 1 - deleting CodeWalker for textures.
	// TextureSpecs: comma-separated entries "ContentPath;RageName[;Usage[;Format]]".
	//   Usage  = DIFFUSE|NORMAL|SPECULAR (drives grcTexture semantics).
	//   Format = AUTO|DXT1|DXT5|ATI2|RAW. AUTO (default) picks ATI2 for NORMAL, DXT5 when
	//            the source has meaningful alpha, else DXT1.
	// Emits a full mip chain, stopping at 4x4 (the min DXT block - sub-4 mips break the
	// streamer's per-mip size math), and 4MB-page-aligned graphics.
	// OutYtdPath: absolute *.ytd. MaxDim: box-downscale any texture whose W or H exceeds
	// this (power-of-two halving); "0"/empty = no cap. Mainly relevant to RAW, which is
	// uncompressed A8R8G8B8 and heavy (a 4096^2 = 64MB, and FiveM can fault the GPU on
	// oversized assets). Texture names may be any length - the name region is sized to fit.
	// Returns JSON: {ok, ytdPath, textures, bytes, sysFlags, gfxFlags} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save textures as a finished GTA V texture file the game loads directly."))
	static FString ExportYtdBinary(const FString& TextureSpecs, const FString& OutYtdPath,
	                               const FString& MaxDim);

	// Export a UStaticMesh directly to a binary FiveM .ydr (RSC7 v165) - CLEAN-ROOM,
	// no CodeWalker. P5 step 3, the LAST CodeWalker dependency. Emits a gtaDrawable:
	// GTAV1 vertex buffers (Pos/Normal/Colour0/UV, 36B stride) + u16 index buffers per
	// polygon group, ShaderGroup with the normal_spec/spec parameter template (external
	// -ytd texture stubs resolved by name), and the EMBEDDED phBoundComposite collision
	// (same serialization as ExportYbnBinary - whole-mesh GeometryBVH). All-in-system
	// (gfx=0), page-aware layout. Struct map: docs/ENGINEERING_LOG "ydr binary format".
	// AssetPath: content path of the StaticMesh. OutYdrPath: absolute *.ydr path.
	// Returns JSON: {ok, ydrPath, geometries, vertices, triangles, bytes, sysFlags}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save a Static Mesh as a finished GTA V model the game loads directly, with its collision included."))
	static FString ExportYdrBinary(const FString& AssetPath, const FString& OutYdrPath);

	// Batch ExportYdrBinary. AssetFolder = a content folder walked recursively, OR a text file of
	// content paths one per line. Filter = optional case-insensitive substring the asset NAME must
	// contain. Each mesh lands at <OutDir>/<AssetName>.ydr through the SAME path as the single-asset
	// tool, so conventions cannot drift between one prop and a district.
	// ⭐ Exists because export was per-asset while import was per-AREA: the continuity principle
	// (BENCHMARK_ADDON_CITY §5) says the difference between a trash can and a district must be batch
	// size, not a different workflow.
	// Returns JSON: {ok, considered, exported, failed, bytes, outDir, failedAssets:[...first 30]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save many Static Meshes out as GTA V models at once, into one folder.", RudeAudience="agent"))
	static FString ExportYdrBinaryBatch(const FString& AssetFolder, const FString& OutDir,
	                                    const FString& Filter);

	// Parse a BINARY FiveM/GTA V .ydr (RSC7 v165) and report its whole drawable graph as JSON.
	// This is the READ side's foundation: RUDE can write binary but until now could only READ
	// the XML interchange form, so real game binaries (e.g. from a QUARRY-extracted filebase) could not
	// reach the importer. Verifies the parse before it is wired to the mesh builder.
	// Reads untrusted files: every access is bounds-checked, malformed input returns {ok:false}.
	// A v159 drawable is reported as GTA V ENHANCED rather than silently misread.
	// BinPath: absolute path to a *.ydr. Returns JSON: {ok, name, version, sysSize, gfxSize,
	// hasEmbeddedBound, shaderCount, models, geometries, vertices, triangles,
	// indicesOutOfRange, declarations:[...], shaders:[{hash,params,textures}],
	// detail:[{model, geoCount, countAt0x2e, geoBoundsPairs, geos:[...]}]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Inspect a GTA V model file and report its internals as raw JSON, without importing. A diagnostic dump, not a plain answer.", RudeAudience="agent"))
	static FString ProbeYdrBinary(const FString& BinPath);

	// ---- lanes implemented in their own translation units (RudeScenario/BuildArea/Vehicle.cpp) --
	// They live on URudeToolset so every surface (panel/CLI/MCP/RUDE.Run) reaches them through the
	// one reflective core; the split is only to keep RudeToolset.cpp from growing without bound.
	// ⚠ Declarations added by the main session from the implementations' real signatures; the
	// authoring agents' own doc blocks supersede these when their review lands.

	// Import one scenario region (GTA's ambient life) as editable actors: every scenario POINT
	// becomes its own actor, and the chaining graph's routes are drawn so they can be seen.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring one area's ambient-life points into your level so you can see and move them."))
	static FString ImportScenarioRegion(const FString& CorpusRoot, const FString& RegionName,
	                                    const FString& Filter);

	// Pack an already-spawned area into a Level Instance at /Game/RUDE/Areas/<slug>.
	// ⛔ PivotType MUST be WorldOrigin: the default re-bases contained actors, and RAGE placements
	// are ABSOLUTE, so a re-based level mis-places the whole area silently on export.
	// Mode: "HEADLESS" builds the level with no dialog (required for a scripted/agent Build Full
	// Map - UE 5.8's own CreateLevelInstanceFrom ALWAYS opens a modal Save-As); anything else uses
	// that proven engine path. Explicit, never inferred: the two land the level in different places.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Pack the area you just built into its own level asset, so areas stay separate and editable. Mode HEADLESS avoids the save dialog.", RudeAudience="agent"))
	static FString PackAreaLevelInstance(const FString& AreaName, const FString& ActorTag,
	                                     const FString& Mode);

	// Import a vehicle: the body drawable plus its wheel drawable placed at each wheel bone.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a GTA V vehicle into Unreal with its wheels in place."))
	static FString ImportVehicle(const FString& CorpusRoot, const FString& VehicleName,
	                             const FString& DestFolder);

	// Export a UStaticMesh's collision directly to a binary FiveM .ybn (RSC7 v43) -
	// CLEAN-ROOM, no CodeWalker. P5 step 2. Emits a phBoundComposite wrapping one
	// phBoundGeometryBVH: quantized vertices, 16-byte triangles, u8 material indices,
	// and a CONSTRUCTED stackless phOptimizedBvh (escape-index tree + the mandatory
	// m_Trees subtree table: maximal <=127-node subtree ranges). Every phBound
	// struct is built from pinned field offsets (docs/ENGINEERING_LOG "ybn binary
	// format") - no template bytes, so it generalizes to any mesh.
	// FRAME CONVENTION (pinned against the real ybn): header boxes, CenterGeom and the
	// BVH header boxes are WORLD space; stored vertices are s16 quantized RELATIVE to
	// CenterGeom, so a reader recovers world = s16*Quantum + CenterGeom.
	// AssetPath: content path of the StaticMesh. OutYbnPath: absolute *.ybn path.
	// WorldOffset: optional "x,y,z" in GTA world metres, ADDED to the mesh's gta-space
	// vertices. Static world collision (map tiles) stores geometry in ABSOLUTE world
	// coordinates, so this places the bound where it belongs on the map; empty = none.
	// Returns JSON: {ok, ybnPath, vertices, triangles, bvhNodes, bytes, sysFlags}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save collision as a finished GTA V collision file, for world or shared collision."))
	static FString ExportYbnBinary(const FString& AssetPath, const FString& OutYbnPath,
	                               const FString& WorldOffset);
};
