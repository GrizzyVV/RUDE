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
	// Returns a JSON string: {ok, assetPath, geometries, geometriesDropped, geometryErrors,
	// geometriesWithoutUV, vertices, triangles, trianglesOutOfRange, trianglesDegenerate,
	// boundTextures, texturesFromEmbedded, ambiguousTextures, unsupportedByMaster,
	// missingTextures, unmappedSamplers, slotsWithoutShaderDef, slotsWithoutMaterial,
	// valueParamsSeen, valueParamsBound, valueParamsUnsupported, valueParamsDeduped, slots}
	// or {ok:false, error} on failure.
	// ⛔ 2026-08-04: ok is COMPUTED - false when any slot silently kept WorldGridMaterial
	// (slotsWithoutMaterial > 0). If /RUDE/Masters cannot be created or loaded, EVERY slot of
	// EVERY mesh took the engine default while every counter read 0 and ok said true - a whole
	// untextured city and a clean verdict were the same output.
	// valueParamsSeen == valueParamsBound + valueParamsUnsupported + valueParamsDeduped holds
	// exactly whenever slotsWithoutMaterial == 0. It counts bind DECISIONS (per geometry),
	// not distinct params in the file; the old spelling summed the whole shader table, which
	// included 4.4% of params no geometry could ever reach.
	// TEXTURE PROVENANCE, split 2026-08-05 (open items #43 + #21b) because 95.8% of a 400-drawable
	// run's binds were an unproven lexicographic tie-break that missingTextures cannot see (a wrong
	// pick still binds *a* texture). The precedence and its rationale live on FRudeTextureScope in
	// RudeToolset.cpp; the counters it produces are:
	//   texturesAmbiguousTotal - every bind whose name had more than one candidate dictionary.
	//     texturesResolvedScoped + texturesTieBroken == this, exactly. A checkable identity.
	//   texturesFromEmbedded      (tier 1) the drawable's own __embedded dictionary - exact by
	//     construction, that dictionary was carved out of this very drawable.
	//   texturesFromArchetypeTxd  (tier 2) the archetype's declared <textureDictionary> (+ its
	//     +hi/+hidr/+hidd siblings).
	//   texturesFromParentTxd     (tier 3) RAGE's own fallback - the gtxd CMapParentTxds chain,
	//     nearest ancestor first.
	//   texturesFromYtypNeighbour (tier 4) a dictionary declared by another archetype in the SAME
	//     ytyp, accepted ONLY when it narrows the candidates to exactly one.
	//   texturesFromSameSlot      (tier 5) a candidate won from the SAME build slot as this asset
	//     (_RESOLVED.json), again ONLY when unique.
	//   texturesScopedAuthoritative = tiers 1-3, the engine's own lookup order. THE STRICT NUMBER.
	//   texturesScopedProvenance    = tiers 4-5, corpus provenance rather than a RAGE rule.
	//   texturesResolvedScoped      = the sum of all five. DERIVED, never a primitive.
	//   texturesTieBroken - nothing scoped it; the narrowest available candidate list was taken
	//     lexicographically. Deterministic but NOT proven correct: the residual to drive down.
	//     Split by CAUSE, and the six sum to it exactly: tieBreakEmbeddedNotImported (the drawable
	//     SHIPS this texture and its __embedded dictionary is not imported here - a corpus gap, not
	//     an ambiguity), tieBreakNoScope (the caller had nothing to scope with - the control path),
	//     tieBreakSlotAmbiguous / tieBreakYtypAmbiguous (that tier matched more than one candidate,
	//     so it narrowed without selecting), tieBreakScopeDictAbsent (every dictionary the scope
	//     names is absent from this project), tieBreakNameNotInScope (the scope is here and
	//     genuinely does not hold this name).
	//   ambiguousTextures - KEPT and unchanged in meaning (== texturesTieBroken) so the pre-fix
	//     2,390/2,495 measurement stays directly comparable to any later run.
	// A bind whose name exists in exactly ONE dictionary moves none of these: nothing had to choose.
	// ⛔ Tiers 4-5 require UNIQUENESS on purpose. On the 400-drawable list the slot matches more
	// than one candidate 350 times against 24 unique; accepting the non-unique hit would have moved
	// 350 guesses into "scoped" without resolving one of them. Do not relax that to lower the
	// tie-break count - that is scoring your own exam.
	// ⭐ COLLISION IMPORT, added 2026-08-05 (open item #40). A drawable's embedded <Bounds> is now
	// read into the asset's UBodySetup instead of being ignored:
	//   Box / Sphere / Capsule -> FKBoxElem / FKSphereElem / FKSphylElem (simple collision)
	//   Geometry / GeometryBVH -> a separate <name>_col UStaticMesh wired as ComplexCollisionMesh
	//   Cylinder               -> REFUSED and counted (UE FKAggregateGeom has no cylinder)
	//   anything else          -> counted as MALFORMED, which reddens ok
	// Verdict gains: collisionBoundsSeen, collisionPrimitivesImported, collisionMeshesImported,
	// collisionBoundsUnmapped, collisionBoundsMalformed, collisionPolysDropped, collisionTriangles,
	// collisionTrisOutOfRange, collisionTrisDegenerate, collisionMeshAsset, collisionReasons[].
	// collisionBoundsSeen == primitivesImported + meshesImported + unmapped + malformed, exactly.
	// ⛔ ONLY collisionBoundsMalformed gates ok. Unmapped is a capability gap on VALID data (240 of
	// ~3,900 sampled child bounds are cylinders) and gating it would fire on every honest run - the
	// same call that keeps missingMeshes and missingPixels out of their batches' ok.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring one GTA V model into Unreal as a Static Mesh you can edit."))
	static FString ImportYdr(const FString& XmlPath, const FString& DestFolder);

	// Read back the collision an asset ACTUALLY has, straight off its UBodySetup.
	// ⛔ This is deliberately NOT a reader of RUDE's own verdict JSON: proving the importer worked by
	// re-reading the counter the importer wrote is circular. It queries FKAggregateGeom element
	// counts and calls UStaticMesh::GetPhysicsTriMeshData (the physics cooker's own call, which
	// follows ComplexCollisionMesh) for the triangle count, and prints one representative primitive
	// with its numbers so a size or mirror error is visible, not just a non-zero count.
	// Returns JSON: {ok, assetPath, boxElems, sphereElems, sphylElems, convexElems, aggGeomTotal,
	// traceFlag, complexCollisionMesh, complexTriangles, complexVertices, firstBox, firstSphere,
	// firstCapsule}. ok:false means the QUERY failed - an empty AggGeom is a legitimate answer and
	// stays ok:true, or this instrument could not score the do-nothing control.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Show what collision an imported model actually ended up with."))
	static FString InspectCollision(const FString& AssetPath);

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
	// PixelFolder: folder of pixel sidecars matching the entry names (DDS decoded natively by
	// FRudeDds; PNG accepted). Empty = the folder named for the XML's stem beside it.
	// DestFolder: content folder root; assets land in <DestFolder>/<TxdName>/.
	// Returns JSON: {ok, txd, declared, imported, invalidNames, itemsWithoutName,
	// usageDefaulted, usageUnknown, missingPixelCount, missingPixels:[...first 30]}.
	// ok is COMPUTED (2026-08-04) and false only on the total-loss shape: the manifest
	// declared textures, every one was rejected by the package-name check, and nothing
	// imported - the exact 2026-07-30 incident. A PARTIAL rejection stays ok:true with a
	// non-zero invalidNames, because failing it would also drop the textures that DID
	// import out of ImportYtdBatch's total.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a GTA V texture set into Unreal, with normal and specular maps set up correctly."))
	static FString ImportYtd(const FString& XmlPath, const FString& PixelFolder,
	                         const FString& DestFolder);

	// Batch ImportYdr: ListPath = a text file of absolute *.ydr.xml paths, one per line.
	// Imports each as a UStaticMesh into DestFolder via the same path as ImportYdr,
	// SKIPPING files whose target asset already exists (idempotent re-runs; the P4
	// hash-manifest resumability model, name-level) UNLESS Mode is "FORCE" - then every
	// file reimports in place (rebinds MaterialInstances against currently-imported
	// textures; the texture-pass re-bind flow). Progress goes to the log.
	// CorpusRoot is OPTIONAL (added 2026-08-05, open items #43 + #21b): the _resolved folder the
	// list points into. Given one, the batch builds the archetype index - archetype
	// <textureDictionary>, the gtxd CMapParentTxds chain, per-ytyp dictionary neighbourhoods and
	// _RESOLVED.json's winning slot per file - and scopes each drawable's texture lookup with it
	// instead of tie-breaking ambiguous names lexicographically. Left empty the behaviour is
	// unchanged, which is also the do-nothing CONTROL any measurement of the scoping is scored
	// against. It is never inferred from the list paths - a guessed scope is a wrong texture.
	// Returns JSON: {ok, imported, skipped, failed, geometriesDropped,
	// filesWithGeometryErrors, geometriesWithoutUV, trianglesOutOfRange,
	// trianglesDegenerate, boundTextures, texturesFromEmbedded, texturesResolvedScoped,
	// texturesTieBroken, filesWithArchetypeTxd, ambiguousTextures, texturesAmbiguousTotal,
	// texturesFromArchetypeTxd, texturesFromParentTxd, texturesFromYtypNeighbour,
	// texturesFromSameSlot, texturesScopedAuthoritative, texturesScopedProvenance,
	// tieBreakEmbeddedNotImported, tieBreakNoScope, tieBreakSlotAmbiguous, tieBreakYtypAmbiguous,
	// tieBreakScopeDictAbsent, tieBreakNameNotInScope, filesWithParentChain, filesWithHashTxd,
	// filesWithYtypSet, filesWithSlot, gtxdFiles, gtxdRelationships, gtxdRefused, resolvedEntries,
	// unsupportedByMaster, missingTextures, unmappedSamplers, slotsWithoutShaderDef,
	// slotsWithoutMaterial, valueParamsSeen, valueParamsBound, valueParamsUnsupported,
	// valueParamsDeduped, failedFiles:[...first 30]}.
	// gtxdRefused is load-bearing: a parent table that was MET and could not be parsed must never
	// read the same as one that was never there.
	// The batch now sums EVERY counter its unit reports; it used to sum seven and drop
	// geometriesDropped/geometryErrors - the lost-geometry ones.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring in many GTA V models at once from a list file. Skips anything already imported."))
	static FString ImportYdrBatch(const FString& ListPath, const FString& DestFolder,
	                              const FString& Mode, const FString& CorpusRoot);

	// Spawn a scene manifest (tools/ingest_ymap.py output: JSON array of
	// {ymap, entities:[{archetype, drawable, ue_location, ue_quat, scaleXY, scaleZ,
	// lodLevel, resolved}]}) into the CURRENT editor level: one actor per ymap holding
	// one InstancedStaticMeshComponent per unique drawable (the P4 ISM-first scale
	// model). Entities are filtered to HD/ORPHANHD lod levels unless Filter == "ALL".
	// Meshes resolve by lowercase drawable name under MeshFolder; unresolved archetypes
	// and missing meshes render as proxy cubes (corpus-hole policy). Transforms are the
	// manifest's UE-space values (already through the pinned GTA->UE convention).
	// Returns JSON: {ok, ymaps, entitiesInManifest, entities, filteredByLod,
	// unknownLodLevel, emptyLodLevel, malformedEntities, instances, proxies,
	// uniqueMeshes, uniqueMeshLookups, missingMeshes, topMissing:[...first 20]}.
	// entitiesInManifest == entities + filteredByLod + malformedEntities holds exactly.
	// ok is COMPUTED (2026-08-05, open item #44) and false when malformedEntities > 0 (an entity
	// the tool could not parse is dropped from the scene - refuse rather than default) or when the
	// run did NO work (0 ymaps or 0 entities - presence is not coverage). missingMeshes
	// deliberately does NOT gate: it is a corpus gap (#37), and a gate that fires on every honest
	// run is a gate nobody reads. ImportMapArea forwards this ok, so it is the map lane's gate too.
	// "entities" has always been the POST-filter count; the LOD filter drops 10.51% of a
	// typical manifest (measured, 1,500 resolved ymap / 239,662 entities) and used to do
	// it with no counter. uniqueMeshes now counts meshes that LOADED - it used to report
	// MeshCache.Num(), and the cache deliberately memoises nullptr for known-missing.
	// Export the open level's RUDE entities back to ymap files, one per source ymap, as a FiveM
	// resource (<OutDir>/stream/<ymap>.ymap + fxmanifest.lua; FiveM Legacy loads the XML form).
	// An untouched entity goes out VERBATIM (its own XML from import); an edited one is rebuilt
	// from its component + actor transform. Extents only grow. A ymap with LOD lineage refuses
	// deletions (ordinals would shift). YmapFilter = comma list of ymap names (empty = every source
	// ymap in the level); NewEntitiesYmap names the file for entities authored in UE (empty = they
	// are counted and dropped).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save the placements in your open level back out as game map files, ready to stream in FiveM. Objects you did not touch go back exactly as they came in."))
	static FString ExportLevelYmaps(const FString& OutDir, const FString& YmapFilter,
	                                const FString& CorpusRoot, const FString& NewEntitiesYmap);

	// Move one placed entity by a delta (UE cm) - the scriptable edit the export gate needs so the
	// import -> edit -> export loop can run headless. Identity = source ymap + ordinal.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Nudge one placed object by x,y,z centimetres. Name it by the map file it came from and its number in that file.", RudeAudience="agent"))
	static FString MoveRudeEntity(const FString& SourceYmap, const FString& SourceIndex, const FString& DeltaCm);

	// Open a saved level in the editor (script chains: build -> reopen -> export). Verdict counts actors
	// and RUDE entities in the loaded world.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Open a level you built earlier so the tools that follow work on it.", RudeAudience="agent"))
	static FString OpenLevel(const FString& LevelPath);

	// The district as a World Partition level: one Runtime Data Layer per ymap (toggle a ymap like a
	// layer), one actor per entity carrying its URudeEntityComponent, saved headless. Reads the
	// manifest ImportMapArea wrote; meshes from MeshFolder; Filter as ImportScene (empty = HD, ALL).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Build a whole district as its own streaming level, with every map file as a layer you can switch on and off, and every object editable. Give the level a content path, and the manifest that Build Map Area wrote."))
	static FString BuildDistrictLevel(const FString& LevelPath, const FString& ManifestPath,
	                                  const FString& MeshFolder, const FString& Filter);

	// The archetype palette: one URudeArchetype DataAsset per archetype the manifest's placements refer
	// to (empty ManifestPath = every archetype in the corpus), fields by tier + provenance + own XML,
	// under DestFolder; MeshFolder links the imported drawable when it exists.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Build the palette of object definitions a district uses, as editable assets you can place from and edit (distances, flags, dictionaries)."))
	static FString BuildArchetypePalette(const FString& CorpusRoot, const FString& ManifestPath,
	                                     const FString& DestFolder, const FString& MeshFolder);

	// Export the palette back to ytyp files (FiveM resource): the source ytyp's bytes are SPLICED - an
	// untouched archetype goes out verbatim, an edited base/time archetype is rebuilt from its asset,
	// an edited MLO archetype goes out as read (counted). YtypFilter = comma list (empty = all).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save the object-definition palette back out as game definition files, ready to stream. Definitions you did not touch go back exactly as they came in."))
	static FString ExportPaletteYtyps(const FString& OutDir, const FString& PaletteFolder,
	                                  const FString& YtypFilter, const FString& CorpusRoot);

	// Set one property on a palette archetype asset by name (reflection; the value as text). The
	// scriptable edit the palette export gate uses.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Change one value on an object definition in the palette, by field name.", RudeAudience="agent"))
	static FString SetArchetypeField(const FString& PaletteFolder, const FString& ArchetypeName,
	                                 const FString& Field, const FString& Value);

	// Script plumbing: a fresh untitled level (Partitioned = true/false) and a headless-safe map save.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Start a new empty level for the tools that follow.", RudeAudience="agent"))
	static FString NewLevel(const FString& Partitioned);

	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save the open level to a content path (needed for an untitled level).", RudeAudience="agent"))
	static FString SaveLevel(const FString& LevelPath);

	// A packed interior level placed as a Level Instance at every CMloInstanceDef of that archetype in
	// the open level, attached to the placement's entity actor (the proxy cube is hidden, the
	// component stays the export's source of truth).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Put a built interior into your level everywhere the map places it. Give the interior's archetype name and the interior level asset that Pack Area Level Instance made."))
	static FString PlaceInterior(const FString& MloArchetypeName, const FString& LevelAsset);

	// LOD view: which LOD level of the placed lineage is visible (HD default, LOD, SLOD1..4, ALL).
	// Everything stays placed; only visibility changes. The first cut of the GDD's lineage view.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Choose which detail level of the map you see: HD (the real buildings, default), LOD or SLOD (the far-away shells), or ALL stacked together. Nothing is removed, only shown or hidden."))
	static FString SetLodView(const FString& Level);

	// Author a NEW placement from the palette: an entity actor at a UE location (cm) / rotation
	// (deg), destined for TargetYmap (a new ymap name, or an existing one to append to). Export it
	// with ExportLevelYmaps NewEntitiesYmap=<same name>.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Place a new object from the palette into your level, and say which map file it should be saved into."))
	static FString PlaceArchetype(const FString& PaletteFolder, const FString& ArchetypeName,
	                              const FString& LocationCm, const FString& RotationDeg, const FString& TargetYmap);

	// Regenerate stale generated masters in place (the glass fix of 2026-09-05); instances update.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Rebuild RUDE's generated master materials that are out of date, so every material using them updates.", RudeAudience="agent"))
	static FString RegenerateMasters();

	// Show/hide one ymap's placed actors - the editor's stand-in for the game's IPL toggle on a
	// script-controlled map (CMapData flags bit 0; those start hidden).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Show or hide everything one map file places - the way a script turns a map on or off in game. Mission and variant maps start hidden."))
	static FString SetYmapVisible(const FString& YmapName, const FString& Visible);

	// What is under a pixel of a CaptureView frame (same CamSpec; U,V 0..1; Aspect optional). Names the
	// first visible hit (actor, archetype, ymap, LOD level, mesh, master materials) and what the ray passed.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Ask what object is under a point of a screenshot: give the same camera as the capture and where in the frame (0-1 across, 0-1 down).", RudeAudience="agent"))
	static FString PickAt(const FString& CamSpec, const FString& U, const FString& V, const FString& Aspect);

	// The numbers behind an imported mesh: bounds, LOD0 vertex extents, collision primitive counts.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Show the size numbers of an imported model: bounds, real vertex extents, collision pieces.", RudeAudience="agent"))
	static FString InspectMesh(const FString& AssetPath);

	// Scenario region export (WP10): the point actors ImportScenarioRegion placed go back to their
	// region file as a FiveM resource (<OutDir>/stream/<region>.ymt, XML form, + fxmanifest.lua). The
	// source file's bytes are SPLICED - only the top-level <Points>/<MyPoints> block is replaced; an
	// untouched point re-emits its own XML verbatim (the slice on its URudeScenarioPointComponent), a
	// moved/rotated/edited one is rebuilt in the file's field order (heading text kept when unrotated).
	// Refuses deletions (AccelGridNodeIndices index points by ordinal). Verdict: sourcePoints, kept,
	// edited, added, sourceDrift, cellCrossings (a moved point that left its 64 m accel-grid cell - the
	// grid is written verbatim and is then stale; the in-game test judges), byteIdentical.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save the ambient-life points you moved back out as a game scenario file, ready to stream in FiveM. Points you did not touch go back exactly as they came in."))
	static FString ExportScenarioRegion(const FString& OutDir, const FString& RegionName, const FString& CorpusRoot);

	// Nudge one imported scenario point by x,y,z centimetres (optionally ",yawDeg"). Identity = region +
	// ordinal in the file's MyPoints list. The scriptable edit the scenario export gate uses.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Nudge one ambient-life point by x,y,z centimetres. Name it by its region and its number in that region's file.", RudeAudience="agent"))
	static FString MoveScenarioPoint(const FString& RegionName, const FString& PointIndex, const FString& DeltaCm);

	// ---- wp10 configs: timecycle modifiers (maintainer lane `configs` (`LAWS.md`)) ----
	// One URudeTimecycle DataAsset per <modifier> across EVERY timecycle_mods_*.xml the corpus carries (base,
	// update and each DLC copy are separate documents - the ledger keys them all as one name, the game loads
	// them additively; a later copy of the same modifier name refills the asset). Fields: mod name ->
	// (value, weight) in the file's order, userFlags, numMods as spelled, provenance (slot, file, ordinal)
	// and the modifier's own bytes. Verdict: files, modifiersSeen, assets, created, overwrittenByLaterCopy,
	// selfClosing (numMods=0 form), irregular (kept verbatim), gaps, mods, modVocabulary, refused.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring the game's timecycle modifiers (the lighting, fog and colour tweaks a map switches on by area) in as assets you can edit."))
	static FString ImportTimecycles(const FString& CorpusRoot, const FString& DestFolder);

	// The modifiers back to their source files: <OutDir>/<slot>/<filename> (game filename kept, identity in
	// folders) plus an fxmanifest.lua of data_file 'TIMECYCLEMOD_FILE' lines. The source document is SPLICED:
	// an untouched modifier re-emits its own bytes, an edited one is rebuilt in the file's spelling (CRLF,
	// "%.3f %.3f" pairs, self-closing when it has no mods), a RUDE-authored one (SourceIndex -1) is appended
	// to its source file, or to rude/timecycle_mods_rude.xml when it has none. Gate: every untouched file
	// byte-identical to the corpus copy.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save the timecycle modifiers back out as game files, ready to stream. Modifiers you did not touch go back exactly as they came in."))
	static FString ExportTimecycles(const FString& OutDir, const FString& DestFolder, const FString& CorpusRoot);

	// ---- wp10 configs: text (gxt2) ----
	// One gxt2 table -> a UStringTable at <DestFolder>/<language>/<name>, keyed by the entry hash spelled as
	// 8 upper-case hex digits. TableName = "<name>[@<language>]" (default american): the corpus keys gxt2 by
	// name only and 20+ language archives spell the same names, so the language is part of the ask. The
	// binary layout is checked on read (magic x2, size field, ascending hashes/offsets, NUL-terminated
	// contiguous UTF-8 strings) and any violation is refused with its reason.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring one of the game's text tables in as a String Table you can edit. Give the table name, optionally @language (american is the default)."))
	static FString ImportText(const FString& CorpusRoot, const FString& TableName, const FString& DestFolder);

	// A UStringTable back to a gxt2 in the measured layout: '2TXG', count, (hash, offset) pairs sorted by
	// hash, '2TXG', total size, then each string UTF-8 + NUL in that order. An 8-hex key is the hash
	// itself; any other key is joaat'd (a label authored in RUDE) and counted keysHashedFromLabels. Two keys
	// with one hash are refused. Gate: an unedited import re-exports byte-identical.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Write a String Table out as a game text file (.gxt2)."))
	static FString ExportText(const FString& StringTableAsset, const FString& OutGxt2Path);

	// ---- wp10 configs: blips ----
	// The blip catalog as one URudeBlipCatalog at <DestFolder>/BlipCatalog: every radar_* name minimap.gfx
	// exports (the SWF's ExportAssets tag, walked from the uncompressed 'GFX' container; a 'CFX' one is
	// refused) with its SWF character id, the blip sheets minimap.ytd declares (blips_texturesheet, _ng,
	// _ng_2, _ng_3) as soft refs to the textures ImportYtd lands under <DestFolder>/minimap/, and the
	// non-radar exports. The per-sprite sheet stays null (not derived in this lane). Verdict carries the
	// ytd import's declared/imported/missingPixels: the corpus has no pixel sidecars for cdimages ytds today.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Build the list of map blip icons the game knows, with the sprite sheets they are drawn from."))
	static FString BuildBlipCatalog(const FString& CorpusRoot, const FString& DestFolder);

	// ---- vehicle paths (ynd) - WP10 paths lane; laws in maintainer lane `paths` (`LAWS.md`) ----------
	// Import one path cell as editable actors: nodes<N> (N = row*32+col over 512 m cells from -8192 m;
	// "at:x,y" in GTA metres names the cell that holds a point - downtown (120,-575) is nodes464). One
	// actor per node with a URudePathNodeComponent (every node field + provenance), in-cell links as
	// linear splines on one <cell>_Links actor, junction heightmap data carried on the junction's node.
	// Filter: ALL (default) | VEH | PED | JUNCTION. Re-running replaces the cell's actors.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring one square of the game's road and footpath network into your level as movable points, with the connections drawn."))
	static FString ImportPaths(const FString& CorpusRoot, const FString& CellName, const FString& Filter);

	// Move one path node by x,y,z UE centimetres (identity = cell + ordinal). The scriptable edit the
	// export gate uses. Reports the GTA position before and after, snapped to the file's grid.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Nudge one road/footpath point by x,y,z centimetres. Name it by its square and its number in that square.", RudeAudience="agent"))
	static FString MovePathNode(const FString& CellName, const FString& NodeID, const FString& DeltaCm);

	// The path readback: every node actor goes back to its cell as <OutDir>/stream/<cell>.ynd (XML in the
	// corpus spelling) + fxmanifest.lua. The source bytes are SPLICED - only the <Nodes> block is re-emitted:
	// an untouched node verbatim from its raw slice, an edited node rebuilt from its fields with the position
	// snapped to the measured grid (x,y 1/4 m; z 1/32 m); Junctions/JunctionRefs/header verbatim. Wave 1 has
	// no add and no delete (ordinals are link targets across files) - both are counted, as are stale
	// LinkLengths (never rewritten: the rule is unproven) and Y-order breaks. CellName = comma list (empty =
	// every cell in the level).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save the road/footpath points in your level back out as game path files, ready to stream in FiveM. Points you did not touch go back exactly as they came in."))
	static FString ExportPaths(const FString& OutDir, const FString& CellName, const FString& CorpusRoot);

	// Import a ped: the fragment's skeleton -> USkeleton, every component drawable of its dictionary ->
	// a skinned USkeletalMesh (real BlendWeights / BlendIndices resolved through the geometry's <BoneIDs>
	// table), materials through the shared drawable lane, the ymt variation matrix -> a URudePedOutfit
	// DataAsset, and a preview actor wearing drawable 0 / texture a of every component. Laws + numbers:
	// maintainer lane `peds` (`LAWS.md`) (a_m_m_business_01: 106 bones, 8 drawables, 46 textures, 4 components).
	// Props (WP11, RUDE_PEDPROPS): <ped>_p.ydd entries (rigid, 977/977 measured) as static meshes under <ped>/props/,
	// <ped>_p.ytd into /Game/RUDE/Textures/<ped>_p/, the ymt propInfo matrix into the outfit's Props (anchor / propId /
	// texture letters), each prop a HIDDEN component on its anchor bone of the preview actor (SetPedProp shows one).
	// Counts: props, propsImported, propsInMatrix/Resolved, propsSkinnedRefused, propTextures, anchorsUnmapped,
	// propsAttached. Laws: maintainer lane `pedprops` (`LAWS.md.`) A streamed ped's per-prop folder layout is counted, not read.
	// LOD GROUPS (WP12, RUDE_PEDLOD): a component ped ships each part with up to three LOD groups, and every
	// one of them is imported - `DrawableModelsHigh` -> LOD0, `Medium` -> LOD1, `Low` -> LOD2 (823/1,152 corpus
	// entries carry all three, 182 carry two, 147 carry one; none carries a VeryLow group). A LOD geometry
	// re-uses the High group's material slot by ordinal and a surplus one is counted (`lodSlotsClamped`). The
	// entry's four lodDist floats ride on the outfit (`FRudePedDrawable::LodDist`) and `ExportPedReplace` hands
	// them to `ExportYddBinary` as `LODDIST=`, so the export re-emits the entry's own values. `FSkeletalMeshLODInfo`
	// ScreenSize is INFERRED (1.0 / 0.4 / 0.15, fitted to the measured vertex ratios) and editor-only: no
	// exported byte depends on it. Per mesh the verdict adds lodGroups / lodVertices / lodTriangles /
	// lodGeometries / lodUeIndex (index 0 = High). ⚠ EVERY counter that existed before stays HIGH-only -
	// vertices, triangles, geometries, geometriesDropped, unweighted, influencesOutOfRange - so the file-level
	// totals keep the meaning they had; what the LOD groups add is counted beside them in lodGeometriesDropped /
	// lodUnweighted / lodInfluencesOutOfRange / lodTrianglesOutOfRange. Laws: maintainer lane `ped_lods` (`LAWS.md`).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a GTA V character (ped) into Unreal: its skeleton, every clothing piece as a skinned mesh, and its outfit variations."))
	static FString ImportPed(const FString& CorpusRoot, const FString& PedName, const FString& DestFolder);

	// ---- WP10 audio lane (RudeAudio.cpp) ----
	// Write ONE USoundWave as a plaintext single-stream .awc (PCM16). Container ('ADAT', measured on the
	// game's own files, LAWS.md): 16-B header | per-stream [u16 chunkStart][u32 word], word =
	// (chunkCount<<29)|(joaat(name)&0x1FFFFFFF) | u64 chunk table (offset 28b | size 28b | type 8b) |
	// bodies data(0x55) format(0xFA, 24 B) peak(0x36) back-to-back from dataStart 46, no padding; flags
	// 0xFF01 (the game's 50 plaintext-PCM files). PCM from USoundWave::GetImportedSoundWaveData (16-bit
	// only; stereo downmixed to mono and reported). Self-check re-parses the file with the tiling reader.
	// StreamName empty = the output file's stem. Returns JSON {ok, frames, sampleRate, bytes, chunks, selfCheck}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save a sound as a GTA V audio container (.awc) holding one uncompressed track."))
	static FString ExportAwc(const FString& SoundWaveAssetPath, const FString& OutAwcPath, const FString& StreamName);

	// Import the PCM16 streams of one corpus .awc as USoundWave assets (AwcName = ledger name, e.g. "chicken").
	// Converted XML: kind="pcm16" .wav sidecars / kind="raw" PCM16 are imported; kind="none" (ROUT --textures
	// none - the 2026-09-04 corpus), kind="encrypted" and ADPCM streams are counted and skipped. Kept binary:
	// parsed by the tiling reader; no 'ADAT' magic = whole-file encrypted = named refusal. DestFolder empty =
	// /Game/RUDE/Audio/<name>. Returns JSON {ok, dataChunks, pcmImported, adpcmSkipped, encryptedSkipped, payloadAbsent, ...}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring the uncompressed tracks of a game audio container into the project as sounds; compressed or encrypted tracks are counted, not guessed."))
	static FString ImportAwc(const FString& CorpusRoot, const FString& AwcName, const FString& DestFolder);

	// ---- WP10 passthrough tier (RudeCarried.cpp) ----
	// One URudeCarriedAsset per effective corpus file of Type (ledger lane word: yed, yld, yfd, ypdb, ynv,
	// mrf, ypt, ...) whose name contains NameFilter (empty = all): raw XML inline up to 8 MB, root tag, a
	// summary of the top-level arrays ("Polygons:2564 Portals:4 ..."), ledger provenance (slot, file, sha1).
	// Kept-binary rows become stubs. DestFolder empty = /Game/RUDE/Carried/<type>. Nothing edit-native.
	// Returns JSON {ok, rows, matched, assets, created, refilled, xmlInlined, xmlTooLarge, binaryStubs, sample}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Catalogue one kind of game file into the project as browsable carried assets, each with a one-line count summary."))
	static FString CatalogLane(const FString& CorpusRoot, const FString& Type, const FString& NameFilter, const FString& DestFolder);

	// Draw one corpus navmesh (.ynv) as persistent lines in the editor world: every polygon ring (cyan),
	// portals (magenta), points (yellow ticks); RAGE metres -> UE cm with the house Y mirror. YnvName=CLEAR
	// flushes the persistent batch. Returns JSON {ok, polygons, polygonVertices, portals, points, lines, camSpecHint}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Draw a game navigation mesh in the level as lines, to see where characters can walk.", RudeAudience="agent"))
	static FString DebugDrawNavmesh(const FString& CorpusRoot, const FString& YnvName);

	// ---- WP10 ANIMS ----------------------------------------------------------------------------
	// Import a clip dictionary (.ycd, ROUT XML) as ONE UAnimSequence per <Animations><Item> on the given
	// USkeleton. Laws measured over 5 dictionaries / 30 animations (maintainer lane `anims` (`LAWS.md`)):
	//   * frames = <FrameCount>; frame rate = round((FrameCount-1)/Duration) - 30 fps in 27/30, 10 fps in 3/30.
	//   * <BoneIds><Item> = {BoneId = skeleton bone TAG, Track, Unk0 = kind 0 vec3 / 1 quat / 2 float}; the
	//     <SequenceData><Item>s of every sequence are parallel to it (44/44 sequences). Track 0 = bone
	//     translation (metres, parent-local), Track 1 = bone rotation (quaternion). Other tracks are counted
	//     and skipped (camera 7/8/27..., facial 24/25/26, mover extras 5/6/134..140).
	//   * consecutive sequences share one frame: sum(seq frames) - (nSeq-1) == FrameCount; a file that breaks
	//     this refuses. Global frame f -> sequence f / SequenceFrameLimit, local f - s*limit.
	//   * channels: QuantizeFloat <Values> (already dequantised, one per frame), StaticFloat, StaticVector3,
	//     StaticQuaternion, IndirectQuantizeFloat (<Values> palette indexed by <Frames>), CachedQuaternion1/2
	//     (<QuatIndex> = the OMITTED component; the 3 preceding channels fill the remaining indices in order;
	//     omitted = +sqrt(1 - sum sq) - sign UNVERIFIED in-game, see NOTES.md).
	//   * transform to UE: pos (x*100, -y*100, z*100) cm, quat (x, -y, z, w) - the plain mirror a skeleton bone
	//     takes (RudeVehicle.cpp bone note). Scale = the skeleton's reference scale. Root motion OFF.
	//   * tags: every <Tags><Item> of every clip referencing the animation becomes a plain notify named
	//     <NameHash>, at StartTime + StartPhase * (EndTime - StartTime) of that clip's reference (EndPhase dropped).
	// Bone mapping: BoneId (tag) -> NAME through the ped outfit's name->tag map (URudePedOutfit `BoneTags`,
	// read by reflection so this lane compiles without the ped lane), then the skeleton bone by NAME; a bone
	// whose skeleton name spells the tag's decimal maps by TAG as the fallback. Unmapped tags are counted and
	// listed - never silently dropped.
	// CorpusRoot: filebase root (ledger lookup, type "ycd"); any other folder = "<folder>/<name>.ycd.xml".
	// YcdName: the dictionary name as the ledger spells it (e.g. amb@bagels@male@walking@).
	// SkeletonAssetPath: "/Game/.../SK_ped" or "/Game/.../SK_ped;/Game/.../DA_outfit" (explicit outfit; else the
	// single URudePedOutfit asset in the skeleton's folder is used and named in the verdict).
	// DestFolder: assets land at <DestFolder>/<dictionary>/A_<animation>.
	// Verdict: clips, animations, per-animation {frames, frameRate, bonesMapped, bonesUnmapped, unmappedTags,
	// tracksSkipped, rotUnreadable, notifies, keys[first bones: t0/tLast cm, r0/rLast quat]}, totals, failures.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a GTA V animation dictionary into Unreal as animations on a ped skeleton. Give the skeleton (and its outfit asset) so bones line up."))
	static FString ImportClipDictionary(const FString& CorpusRoot, const FString& YcdName,
	                                    const FString& SkeletonAssetPath, const FString& DestFolder);

	// Import a cutscene (.cut, ROUT XML of rage__cutfCutsceneFile2) as a Level Sequence: one spawnable camera
	// keyed from the cutscene's own animation parts (<cut>-0.ycd, <cut>-1.ycd, ... - each carries a clip
	// "<camera cName>-<k>", bone 0 track 7 = position (metres, RAGE Z-up), track 8 = rotation; measured against
	// the .cut's own camera-cut positions: identical to 1e-5 on 2/2 cutscenes once the .cut's (x, y, z) is read
	// as (x, z_up, -y)), a camera-cut track with one cut per rage__cutfCameraCutEventArgs event (iEventId 43),
	// and a sidecar URudeCutsceneEvents DataAsset carrying EVERY event verbatim (list, time, id, object,
	// args index/type/name, the args and event XML re-spelled), every object, the concat rows.
	// Parts are laid back-to-back by their own frame counts; the verdict reports the sum against fTotalDuration
	// (the exact boundary rule is UNMEASURED - see NOTES.md). Camera axis convention (which local axis the
	// RAGE camera looks along) is UNVERIFIED: first-frame rotation is reported for Matt's eyes.
	// CorpusRoot: filebase root (ledger types "cut" and "ycd"); CutName: e.g. ah_1_int; DestFolder: assets land
	// at <DestFolder>/<cut>/LS_<cut> and DA_<cut>_events.
	// Verdict: totalDuration, objects, events, eventArgs, cameraCutEvents, parts[{name, frames, fps, seqs}],
	// cameraKeys, sumPartsSeconds, levelSequence, eventsAsset, firstCamPosUE, firstCamRotUE, fovFirst.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a GTA V cutscene into Unreal as a Level Sequence with its camera moves and cuts, plus a data asset listing every event."))
	static FString ImportCutscene(const FString& CorpusRoot, const FString& CutName, const FString& DestFolder);

	// Vehicle composite (GDD Tier 2). One actor: the body drawable as a Static Mesh whose LOD0 is the
	// _hi fragment's DrawableModelsHigh and LOD1..4 the base fragment's High/Medium/Low/VeryLow (the
	// detail toggle = LOD); a named component per <Physics><LOD1> child at its bone's model-space frame
	// (Child_NN_<group>; bone frames composed up the parent chain, GTA->UE (x,-y,z) cm + plain
	// quaternion mirror); the wheel child's mesh at every wheel_* bone (mirrored across local X for the
	// other side); collision from <Archetype><Bounds> grafted onto the LOD0 drawable; a URudeVehicle
	// DataAsset (<DestFolder>/<veh>/<veh>_vehicle) with vehicles.meta -> handlingId -> handling.meta ->
	// carvariations (meta or PSO, hash tags resolved by joaat) flattened by field name as spelled plus
	// each row's re-spelled item XML, the livery list (<veh>_sign_<n> in <veh>.ytd / <veh>+hi.ytd, flags
	// from carvariations colors/Item/liveries) and the child/bone/bound table. Every drawable goes
	// through ImportDrawableNode from a re-spelled self-contained <Drawable> buffer (a child has no
	// ShaderGroup of its own; its ShaderIndex indexes the fragment's). Measured on blista / taxi /
	// burrito 2026-09-06 (maintainer lane `vehicles` (`LAWS.md`)): doors/bonnet/boot are skinned parts of the
	// main drawable, only wheel_lf carries child geometry. Verdict: bones, children, childComponents,
	// wheelBones/wheelsPlaced, lodCount/lodSources, liveries, boundTypes, field counts, missing[] (capped,
	// total beside it); ok is COMPUTED (every child placed, wheels where the skeleton has them, no LOD failed).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a GTA V vehicle into Unreal as one assembled car: body with a detail level, every door/bonnet/boot pivot, wheels, its paint liveries and its handling numbers."))
	static FString ImportVehicleComposite(const FString& CorpusRoot, const FString& VehicleName,
	                                      const FString& DestFolder);

	// Swap the body's livery: sets the master parameter the livery sampler maps to (DiffuseSampler ->
	// Diffuse) on every body material slot whose geometry uses the livery shader (LOD0 by recorded slot,
	// LOD1.. by preset) to livery LiveryIndex's texture (the DataAsset's Liveries[].Index; +hi texture
	// preferred). Refuses: no liveries, a texture not imported (textureMissing), or a sampler the masters
	// have no parameter for (vehicle_paint3's DiffuseSampler2 - the burrito case).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Change which paint-job (livery) an imported vehicle shows. Give the vehicle actor's name and the livery number."))
	static FString SetVehicleLivery(const FString& ActorLabel, const FString& LiveryIndex);

	// ---- THE SANDBOX (GDD 1b): PIE as the rehearsal space (RudeSandboxTools.cpp; runtime classes in RudeCore) ----
	// Ready a level for Play: a PlayerStart tagged RUDE_SANDBOX_SPAWN at Location ("x,y,z" UE cm, ';' accepted,
	// empty = origin; Z snaps to the ground under it), the level's GameMode override = RudeSandboxGameMode
	// (DefaultPawn = the on-foot ARudeSandboxPawn), a RUDE_SKY rig if the level has none, then the level saved.
	// Empty LevelPath = the open level. In PIE: `Rude.Native <native> [args]` drives the mocked FiveM natives.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Make a level playable: add a spawn point, set the sandbox game mode and save, so Play walks you around in it."))
	static FString SandboxSetup(const FString& LevelPath, const FString& Location);

	// The Lua that calls the REAL FiveM native the sandbox shim mocks ("every export ships its invocation").
	// Kind = ipl | cutscene | entityset | scenario | clock (empty = list). Name = the ymap / the cut / "interior|set" /
	// the group / "HH:MM". Verdict: native, shim (the Rude.Native line for PIE), lua (the snippet), note.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Show the FiveM script lines that do in game what a sandbox command does in Play: load a map, run a cutscene, switch an interior set, enable a scenario group, set the clock."))
	static FString EmitNativeSnippet(const FString& Kind, const FString& Name);

	// The ped variation matrix as a surface: drawable D of slot S on the ped, wearing texture letter L.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Dress a character: pick which clothing piece a body slot wears and which colour variant.", RudeAudience="agent"))
	static FString SetPedOutfit(const FString& ActorLabel, const FString& Slot, const FString& DrawableIndex, const FString& TextureLetter);

	// An interior's entity set on or off in the editor (the game's per-instance activation).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Switch one of an interior's optional prop sets on or off.", RudeAudience="agent"))
	static FString SetEntitySet(const FString& InteriorName, const FString& SetName, const FString& Visible);

	// A ymap's car generators as slab markers (length, width, heading) with their fields; export rebuilds the block when one moves.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a map's parked-car spawn spots in as markers you can move; the export writes them back.", RudeAudience="agent"))
	static FString ImportCarGenerators(const FString& CorpusRoot, const FString& YmapFilter);
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Nudge one parked-car spawn spot by x,y,z centimetres.", RudeAudience="agent"))
	static FString MoveCarGenerator(const FString& YmapName, const FString& Index, const FString& DeltaCm);

	// ---- WP11 skinned drawable dictionary (RudeSkinnedWriter.cpp) ----
	// Write a BINARY .ydd (RSC7 v165 pgDictionary<gtaDrawable>) holding one SKINNED drawable per USkeletalMesh -
	// the custom-clothing container a FiveM ped streams. Container (3/3 game binaries, ROUT 400/400): 0x40
	// header {vft, blockmap*, 0, refcount 1, hashArr*, n|cap<<16, entryArr* (8B/entry), n|cap<<16}, hashes
	// ASCENDING, one 0xD0 gtaDrawable record per entry (blockmap/skeleton/bound slots raw NULL). Per drawable:
	// the ydr writer's structs with the skinned deltas measured on 2 peds / 56 geometries + 3 binaries:
	// grmModel +0x28 = rig bone count, +0x29 = 1, +0x2D = 1; grmGeometry +0x68 -> identity u16 bone-id table,
	// +0x72 = its count; GTAV1 layout mask 0x7F stride 48 (Pos f3 | BlendWeights u8x4 | BlendIndices u8x4 |
	// Normal f3 | Colour0 | Colour1 | UV0 f2); BlendWeights are bytes summing to 255 (27,808/27,808 measured),
	// BlendIndices = rig index (bone-id table identity); the `ped` 13-parameter shader template verbatim
	// (DiffuseSampler/TextureSamplerDiffPal/VolumeSampler=givemechecker/BumpSampler/SpecSampler + 8 vec4s,
	// registers 0,2,3,4,5 / 187..180; +0x14 = 336, +0x16 = 432, +0x24 = 5<<24). Materials: the Diffuse /
	// Normal / Specular parameters of each slot's material instance (the ydr writer's path). Bone order: the
	// mesh's USkeleton reference skeleton (Options "SKELETON=<path>" overrides) - ImportPed builds it in yft
	// order; mesh bones map by NAME; an influence on an unmapped bone is dropped and counted, a vertex left with
	// no weight is bound to bone 0 and counted, a 5th+ influence is truncated and counted. High LOD only.
	// Self-check before writing: single ownership across the whole dictionary + geoBounds/count + declarations.
	// SkeletalMeshAssetPaths / DrawableNames = comma lists (name default = the asset name; the entry hash is
	// joaat(name), e.g. "uppr_000_u"). Returns JSON {ok, yddPath, drawables:[{name, hash, geometries, vertices,
	// triangles, bonesReferenced, influencesUnmapped, influencesTruncated, verticesRebound, uv1Dropped,
	// texturesMissing, textures}], rigBones, rig, bytes, segSize, pages, sysFlags, selfCheck}.
	// Laws + denominators: maintainer lane `ydd_writer` (`LAWS.md.`) In-game load: NOT yet verified (Matt's test).
	// RIGID entries (WP11 ped props, RUDE_PEDPROPS): a UStaticMesh content path writes an UNSKINNED entry - grmModel
	// +0x28/+0x29/+0x2D = 0, geometry +0x68 raw NULL / +0x72 = 0, the game's own prop layout (mask 0x40F9 stride 64:
	// Position Normal Colour0 Colour1 TexCoord0 TexCoord1 Tangent, 2,674/2,677 prop geometries), `ped` or `ped_alpha`
	// (bucket 1, 12 params, registers 0/2/5/6) by the slot's preset, entry +0x80 = 0xFF00 | OR(1<<bucket) (1,763/1,763).
	// Mixed lists are fine (a rig is required only when a skinned mesh is present). maintainer lane `pedprops` (`LAWS.md.`)
	// LOD GROUPS (WP12, RUDE_PEDLOD): every LOD the skeletal mesh carries is written, up to three groups -
	// LOD0 -> `DrawableModelsHigh` (+0x50), LOD1 -> `Medium` (+0x58), LOD2 -> `Low` (+0x60); an absent group
	// leaves its pointer AND its flag word raw zero (172/172 absent Medium and 555/555 absent Low words read 0x0
	// in the game's files), and a VeryLow group is never written (0/2,125). Each present group owns its own
	// geometry blocks, geoBounds, shader map, geometry array, model and 0x10 header - the game never shares one
	// (the three header offsets ascend High < Medium < Low on 2,125/2,125 entries). A Medium/Low geometry is the
	// SAME skinned form as High (mask 0x7F stride 48 on 1,980/1,984 and 1,573/1,579 geometries; model words
	// (1,0,255,1); identity bone-id table over the same rig; weight bytes summing to 255 on 14,058/14,058 and
	// 2,459/2,459 measured vertices). One shader per geometry across the groups in group order, so every LOD
	// shader index is strictly greater than every High one (1,899/1,953 Medium, 1,525/1,570 Low); the game names
	// its LOD shader `ped_default`, whose parameter table is UNMEASURED, so the High template is written and the
	// difference is COUNTED (`lodShaderSubstituted`). `+0x80/84/88` = 0xFF00 | OR(1<<bucket) over THAT group's
	// shaders (High 0xFF08 with Medium 0xFF01 on 257 entries). `Options` gains `LODDIST=a/b/c/d,a/b/c/d,...` -
	// one four-float group per entry in the same order as the asset list, empty for an entry that has none;
	// `ExportPedReplace` fills it from the outfit, so an entry re-emits the four floats the game spelled, and
	// without the token the writer uses the measured modal 9998 x4 (2,119/2,125). `+0x98` (meaning UNKNOWN) is
	// written from the GROUP COUNT - the modal over the 4,327-entry binary draw: 1 group 0x00120000 (1,038/2,374),
	// 2 groups 0x003E0000 (276/383), 3 groups 0x005D0000 (1,140/1,570) - and the word written is reported per
	// drawable (`u98`, `u98Basis`). ⚠ Over the SKINNED one-group subset alone the mode is 0x00220000 (62/172);
	// RUDE writes the population modal, which is also the value an in-game-proven static drawable carried. The
	// record's box / sphere (+0x20..0x4c) span EVERY present group - the better-attested of the two candidates
	// (box size 280 vs 14, sphere radius 115 vs 6, where High-only and the union disagree). A mesh with ONE LOD
	// produces exactly the bytes the single-group writer produced. Verdict adds lodGroups / lodVertices /
	// lodTriangles / lodGeometries / lodShaderSubstituted / lodsSkipped / lodDist / lodDistCarried / u98 per
	// drawable and geometriesAllLods / verticesAllLods / trianglesAllLods / lodDistCarried at the top;
	// `vertices` / `triangles` / `geometries` stay the HIGH group's.
	// Laws + denominators: maintainer lane `ped_lods` (`LAWS.md`).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save clothing pieces (skinned meshes on a ped skeleton) as a finished GTA V clothing file the game loads directly."))
	static FString ExportYddBinary(const FString& SkeletalMeshAssetPaths, const FString& DrawableNames,
	                               const FString& OutYddPath, const FString& Options);

	// Parse a BINARY .ydd and report it as JSON: entries (name, hash, ascending), per entry shaders / models /
	// geometries / vertices / triangles / declarations, skin facts (vertices whose 4 weight bytes sum to 255,
	// max blend index, indices inside the bone-id table, bone-id table size and identity), skeleton / bound
	// presence, and the single-ownership audit (advisory on game files - an embedded texdict may share).
	// Reads untrusted files: every access bounds-checked; malformed input returns ok:false.
	// LOD GROUPS (WP12, RUDE_PEDLOD): per entry `lodGroups`, the four `lodDist` floats and four `lodFlags` words
	// as stored, and a `groups` row per present group (models, geometries, vertices, triangles, its bone count
	// at grmModel+0x28, its flag word, its lodDist), plus the entry's `+0x98` word as stored (`u98`) - the only
	// field besides the LOD slots and flag words that moves with the group count, so a written file can be held
	// against the game's. The entry-level and file-level totals stay HIGH-only so
	// every number that existed before keeps its meaning; the all-group figures are reported beside them as
	// geometriesAllLods / verticesAllLods / trianglesAllLods. maintainer lane `ped_lods` (`LAWS.md`).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Inspect a GTA V clothing file and report its internals as raw JSON, without importing.", RudeAudience="agent"))
	static FString ProbeYddBinary(const FString& BinPath);

	// ---- THE CHAOS TEST-DRIVE (GDD Tier 2 / WP11; RudeDrive.cpp; runtime classes RudeCore/RudeDriveablePawn.h) ----
	// From a VehicleComposite_<name> actor (ImportVehicleComposite) and its URudeVehicle asset: a USkeletalMesh with
	// the yft's bones (re-read from the asset's SourceYft; the ROOT gets a +90 deg yaw so the nose is the pawn's +X,
	// which is what Chaos drives), every body vertex weighted 1.0 to `chassis`, every placed Wheel_* component's
	// vertices (mirror folded in, winding flipped) weighted 1.0 to its wheel bone, materials borrowed per slot; a
	// PhysicsAsset with one chassis body (the body's convex collision when it has any, else its bounds box); an
	// ARudeDriveablePawn spawned 6 m beside the composite with one FChaosWheelSetup per wheel_* bone (front axle
	// steers, rear handbrakes) and the handling.meta row mapped onto Chaos - every mapped line lands in the
	// component's MappingNotes with its tag (as-read / kinematics / inferred / placeholder; the table is
	// maintainer lane `drive` (`DESIGN.md`)). Tagged RUDE_DRIVEABLE:<name>. Verdict: bones, body/wheel vertices and
	// triangles, wheels bound/mirrored, radii read off the meshes, chassis shape, ground clearance, the Chaos
	// numbers, the handling fields that were missing (defaults named), problems[]; ok is COMPUTED.
	// Then SandboxSetup + Play: `Rude.Native EnterVehicle <name>` possesses it; F or `Rude.Native ExitVehicle` returns.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Turn an imported vehicle into one you can drive in Play: builds its drivable body and wheels and parks a test-drive car next to it. Then press Play and type Rude.Native EnterVehicle <name>."))
	static FString BuildDriveable(const FString& ActorLabel, const FString& LocationCm);

	// A REPLACE resource for one ped (custom clothing without a variation-table writer): every drawable the
	// outfit knows -> stream/<ped>.ydd under the game's entry names, every imported texture -> stream/<ped>.ytd,
	// + fxmanifest.lua. OutfitAssetPath = the outfit asset or just the ped name. Options pass to ExportYddBinary.
	// Props (WP11, RUDE_PEDPROPS): when the outfit carries any, stream/<ped>_p.ydd (rigid entries through the same
	// writer) + stream/<ped>_p.ytd (every texture under /Game/RUDE/Textures/<ped>_p/) ride along; counts props,
	// propsExported, propsWithoutMesh, propTextures; ok folds the prop verdicts in.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Pack a ped's clothing and textures as a FiveM resource that replaces the game's own files for that ped.", RudeAudience="agent"))
	static FString ExportPedReplace(const FString& OutfitAssetPath, const FString& OutDir, const FString& Options);

	// One imported texture dictionary back to the game as a REPLACE resource: every texture under
	// /Game/RUDE/Textures/<dict>/ -> stream/<dict>.ytd (+ fxmanifest.lua). The livery / paint / prop-texture
	// edit path. MaxDim: downscale cap for ExportYtdBinary ("0"/empty = none).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Pack an imported texture set as a FiveM resource that replaces the game's own texture file of that name.", RudeAudience="agent"))
	static FString ExportTxdReplace(const FString& DictName, const FString& OutDir, const FString& MaxDim);

	// RUDE_PEDPROPS_BEGIN header
	// The prop matrix as a surface (WP11 ped props; mirrors SetPedOutfit): prop P of anchor A (head / eyes / ears /
	// lwrist / rwrist, an ANCHOR_* enumerant, or the id 0/1/2/6/7) on the imported ped, wearing texture index T (0 =
	// letter a; empty/-1 = leave the materials). PropIndex -1 = nothing on that anchor. One prop per anchor, like the
	// game. The prop rides its anchor bone (SKEL_Head for head/eyes/ears, SKEL_L_Hand / SKEL_R_Hand for the wrists -
	// RUDE's table) with the bone's bind rotation inverted: props are modeled in ped axes at the bone's origin
	// (maintainer lane `pedprops` (`LAWS.md`) law 7). Verdict: anchor, bone, prop, entry, mesh, texture, hidden, component.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Put a hat, glasses, earpiece or watch on a character, or take it off (-1), and pick its colour variant.", RudeAudience="agent"))
	static FString SetPedProp(const FString& ActorLabel, const FString& Anchor, const FString& PropIndex, const FString& TextureIndex);
	// RUDE_PEDPROPS_END header

	// THE INTERIOR EXPORTER (GDD Tier 1 interiors: import-author-EXPORT; maintainer lane `mlo_export`). The
	// interior's entity actors (ImportMlo since 2026-09-06 spawns one actor per entity carrying a
	// URudeMloEntityComponent: interior, set, ordinal, raw slice, source transform) go back into the ytyp
	// that declared the CMloArchetypeDef, as a FiveM resource: <OutDir>/stream/<ytyp>.ytyp (XML form,
	// Legacy loads it) + fxmanifest.lua. The source file's bytes are SPLICED: inside the ONE archetype
	// (37/424 MLO ytyps declare several - the splice targets one by name), only its top-level <entities>
	// block, the <entities>/<locations> of an entity set that changed, and the <attachedObjects> of a room
	// that gained entities are replaced. Every untouched entity re-emits its own slice VERBATIM; a moved
	// one (position + rotation compared, NOT scale) has only its <position>/<rotation> lines re-spelled
	// (RudeNum); an added one (a duplicated entity actor: SourceIndex -1 or a repeated ordinal) is appended
	// after the source items with a fresh guid and its ordinal appended to the room actor it sits under
	// (a set entity: to the set, with its room in <locations>). DELETIONS ARE REFUSED and nothing is
	// written: rooms and portals index entities by ORDINAL, a shift would re-attach every later prop
	// (LAWS.md law 4). Hide instead - and export from a FULL import: a room Filter leaves rooms unspawned,
	// which reads as deletions. Everything else in the file stays byte-identical (gate: the clean export
	// equals the source bytes). MloArchetypeName is hash-tolerant like ImportMlo's. Returns JSON:
	// {ok, interior, ytyp, file, source, byteIdentical, entities, seen, kept, moved, added, deleted,
	// deadSlots, sets, setEntities, setKept, setMoved, setAdded, setDeleted, setsRewritten, roomsRewritten,
	// locationsRewritten, refused[]}; ok is COMPUTED (any refusal = false).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Write an interior you built or edited back to its game definition file as a FiveM resource. Untouched props come out exactly as they went in, moved props are rewritten, duplicated props are added; deleting a prop is refused (hide it instead)."))
	static FString ExportMloYtyp(const FString& OutDir, const FString& MloArchetypeName, const FString& CorpusRoot);

	// Nudge one interior entity by x,y,z centimetres. Index = the ordinal in the archetype's <entities>, or
	// "<setName>:<ordinal>" for an entity-set entity. Identity = interior + set + ordinal. The scriptable edit
	// the MLO export gate uses (maintainer lane `mlo_export` (`gate.jsonl`)).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Nudge one prop of an interior by x,y,z centimetres.", RudeAudience="agent"))
	static FString MoveMloEntity(const FString& InteriorName, const FString& Index, const FString& DeltaCm);

	// ---- WP12 weapons lane (RudeWeapon.cpp) ----
	// The weapon bench (GDD Tier 2, the weapon's answer to the vehicle showroom). One actor: the weapon's
	// own drawable at the origin, and every component its meta row lists placed at the socket bone the meta
	// names - default components visible, alternates imported and HIDDEN (SetWeaponComponent swaps them).
	// WeaponName takes either spelling: the meta name (WEAPON_PISTOL) or the model (w_pi_pistol).
	// MEASURED 2026-09-06 over the whole weapon set of a corpus cut from a legally owned copy of the game
	// (875 w_* drawables, 184 CWeaponInfo rows, 474 component rows; maintainer lane `weapons` (`LAWS.md`)):
	//   * The WEAPON's skeleton carries the sockets (WAPClip on 185 drawables, WAPFlshLasr 163, WAPSupp 151,
	//     WAPScop 115, WAPGrip 76, WAPScop_2 69); the COMPONENT's drawable carries ONE AAP* bone, and it is
	//     its FIRST bone at parent -1 with identity rotation and zero translation in 516/516 - so a component
	//     mesh needs NO correction, it drops straight onto the socket's model-space frame. 198 drawables
	//     carry WAP bones, 516 carry AAP bones, NOT ONE carries both.
	//   * The pairing comes from the META, never from the bone names: 377 of 402 socket references pair by
	//     name stem (WAPSupp<-AAPSupp, WAPFlshLasr<-AAPFlsh) and 25 do not (WAPScop_2<-AAPCamo2 x22,
	//     WAPScop<-AAPFlsh x2, WAPFlshLasr<-AAPCover x1). 133 more sit at gun_root and 12 at gun_gripr.
	//   * Sockets hang mid-chain (249 of 317 on bone index 2), so bone frames are composed up the parent
	//     chain, GTA->UE (x,-y,z) cm + the plain quaternion mirror - the vehicle lane's map, same reason.
	//   * 16 of 272 attach points name a bone the weapon's own skeleton lacks (all WAPClip, on shotguns and
	//     launchers): those components ride the weapon origin and are COUNTED (componentsUnmapped).
	//   * Weapons ship NO lod groups inside a file (875/875 High only) and no lights (0/875); collision rides
	//     the drawable's own <Bounds> (Composite on 627, absent on 248). The detail toggle is a SECOND FILE:
	//     204 of the 875 drawables have a <name>_hi twin carrying the same skeleton and more vertices in
	//     194/204 (w_ar_carbinerifle 3,961 -> 20,251), so the _hi is imported as what the actor shows and the
	//     base drawable becomes its LOD1 - the vehicle lane's rule, applied to the weapon's own file pair.
	//   * Textures: own dictionary 2,806 of 5,274 sampler references, embedded in the ydr 234, another weapon
	//     dictionary 1,859, nowhere in the weapon set 375 (env_smooth_concrete2 / env_noise_heavy, which live
	//     in map dictionaries, and givemechecker, which exists nowhere). gtxd.ymt has ZERO w_ rows, so a
	//     weapon does not ride the map's texture-parent chain: the scope is the drawable's own dictionary
	//     plus the other dictionaries of this composite. ⚠ The 2026-09-04 corpus carries pixels for 0 of 804
	//     weapon dictionaries, so texturesMissing is expected non-zero and never gates ok.
	// Verdict: bones, wapBones, attachPoints, components, componentsImported, componentsMissingMesh,
	// componentsUnmapped, componentsWithoutModel, componentBonesMatched, texturesMissing, metaFilesSearched,
	// weaponMetaFields / ammoMetaFields / componentMetaFields, hiDrawable / componentHiLods / lodFailed,
	// boneNames[] and componentMeshes[] (name,
	// model, socket, component bone, vertices, triangles, default - what the offline comparator checks
	// against the corpus XML), missing[] (capped, total beside it); ok is
	// COMPUTED and refuses only on the total-loss shape (no weapon mesh, or a weapon whose meta lists
	// components and not one of them got a mesh).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Bring a GTA V weapon into Unreal as one assembled gun: the weapon itself plus every magazine, scope, suppressor, flashlight and grip it can take, each on its own mounting point."))
	static FString ImportWeapon(const FString& CorpusRoot, const FString& WeaponName,
	                            const FString& DestFolder);

	// Swap or clear one mounting point of an imported weapon: shows ComponentName on AttachPoint and hides
	// every other component of that point (one component per point, which is how the game's own data is
	// shaped - 93 of 273 attach points declare exactly one <Default value="true"/> and NOT ONE declares two).
	// AttachPoint = the socket bone as the meta spells it (WAPSupp, WAPFlshLasr, gun_root) or its ordinal;
	// ComponentName = the component's meta name (COMPONENT_AT_AR_SUPP) or its model (w_at_ar_supp); EMPTY =
	// nothing on that point. Refuses by name on an unknown point, a component that point does not list, or a
	// component whose mesh never imported. Mirrors SetVehicleLivery / SetPedProp in shape.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Put a scope, suppressor, flashlight, grip or magazine on an imported weapon, or take it off (leave the name empty).", RudeAudience="agent"))
	static FString SetWeaponComponent(const FString& ActorLabel, const FString& AttachPoint,
	                                  const FString& ComponentName);

	// ---- WP12 ycd_export lane (RudeYcdExport.cpp) ----
	// Write a clip dictionary (.ycd) back out as the XML the game's own files are spelled in, from
	// UAnimSequence tracks on a ped skeleton. The write half of ImportClipDictionary. Laws + numbers:
	// maintainer lane `ycd_export` (`LAWS.md`) - 24,844 .ycd.xml in the corpus, a seeded 303-file sample
	// (2,206 animations, 2,673 sequences, 106,005 BoneIds rows, 278,075 channels) and a 40-file draw for
	// the round-trip numbers (10,746 QuantizeFloat channels / 1,382,180 frame values).
	// XML, NOT BINARY - MEASURED (LAWS.md H): the maintainer's shipped C++ exporter has no .ycd writer
	// (37 .cpp, the one that names ycd is the reader, 0 named *_write or xml2*), and its Python write
	// direction is binary->binary donor repack - no ycd module reads XML at all. Packing this XML into a
	// .ycd image is NOT done here and is NOT claimed.
	// A TEMPLATE IS REQUIRED. An animation carries eight fields, a sequence three, and the dictionary a
	// RecordUnknown00 block, that no UAnimSequence models (Unknown10/1C/38/3C, SequenceFrameLimit,
	// Duration, StartTime/EndTime - EndTime == Duration on only 1,626/1,857 clips, so it is carried, never
	// recomputed). The template's BYTES are copied and only the channel number lines being authored are
	// replaced, so an untouched dictionary comes out byte-identical by construction and an authored one is
	// a surgical diff. Line-oriented on purpose: FXmlFile does not preserve line structure (section 6.5)
	// and "ten values per line" (131,483/131,483) and "one space per depth" (303/303) are line laws.
	// HOW A CHANNEL IS WRITTEN, and why the round trip survives it:
	//   * metres->UE cm->metres is not the float32 identity (1,199,600/1,382,180 exact, worst 3.05e-05 m)
	//     and the import's quaternion normalise loses a little too (1,259,661/1,259,820, worst 6.56e-07);
	//   * requantising against the DONOR channel's own Quantum/Offset absorbs both - 1,382,180/1,382,180
	//     frame values and 1,089,954/1,089,954 rotation labels come back byte-identical. So Quantum and
	//     Offset are PRESERVED and only the raws are re-derived; re-deriving the quantum is opt-in
	//     (Options rescale=0 refuses instead) and every rescaled channel is counted and flagged, because
	//     it needs a wider binary payload than the donor's.
	//   * a StaticFloat has no quantum to round into (53 labels moved), so it is CARRIED VERBATIM; an
	//     authored value further than Options statictol from it is a REFUSAL, not a rewrite - making a
	//     static channel vary is a size change and that is not built.
	//   * bone tag 0 / track 0 (root translation) is GATED behind Options authorroot=1: a +0.5 m root edit
	//     crashed the game natively once (maintainer lane `ycd_layer_b`, in-game 2026-08-22, cause still
	//     unknown) while every structural referee stayed clean. 646/2,206 animations carry a varying root.
	//   * tracks other than 0 and 1 (43,916 of 106,005 BoneIds rows), IndirectQuantizeFloat (7,736
	//     channels) and RawFloat (317) are CARRIED and counted - never reshaped, never dropped.
	//   * a pool-6 channel (37,104 of 131,483 QuantizeFloat) carries RiceSelector and PayloadTail, which
	//     describe the DONOR's packed payload. This writer rewrites decoded numbers only, so those two
	//     are carried unchanged and every such channel is COUNTED in channelsPool6Written - a packer has
	//     to re-derive them. A channel carrying a second value list (<RawValues>, 11 of 131,483) is
	//     REFUSED rather than half-rewritten.
	// Floats are spelled the way the corpus spells them: 7 significant digits widening to 9 when 7 does
	// not round-trip float32, ties AWAY FROM ZERO, %G fixed/scientific, uppercase E with a two-digit
	// exponent (1,414,618/1,414,618 floats exact over 40 files; plain %.7G matches only 57,737/131,483).
	// AnimSequenceAssetPaths: ";"-separated UAnimSequence paths. A bare path matches the template
	// animation whose hash spells the asset's name minus the "A_" prefix (ImportClipDictionary's own
	// naming, so a round trip needs no mapping); "<hash>=/Game/..." names one explicitly. Empty = author
	// nothing, i.e. re-emit the template (the true no-op referee: byteIdenticalToTemplate must be true).
	// ClipNames: ";"-separated animation hashes to restrict the write to; empty = every matched animation.
	// OutYcdPath: the .ycd.xml file to write (UTF-8, LF, trailing newline - the corpus's own form).
	// Options: "key=value;..." - template=<corpus ycd name or .ycd.xml path> (REQUIRED),
	// corpus=<filebase root>, outfit=<URudePedOutfit asset supplying BoneTags>, authorroot=0|1 (default 0),
	// rescale=0|1 (default 1), statictol=<metres|units> (default 1e-5).
	// Verdict: {ok, file, template, templateSource, boneMap, animationsInTemplate, animationsAuthored,
	// byteIdenticalToTemplate, channelsWritten, channelsCarried, channelsRescaled, channelsRefused,
	// framesWritten, staticCarried, staticRefused, rootChannelsGated, tracksNotAuthored, bonesUnmapped,
	// bonesWithoutTrack, inverseNotExact, channelsPool6Written, rawValuesRefused, authoredButWroteNothing,
	// firstRefusal, anims[]}; ok is COMPUTED - false on any refusal, on an unmapped bone, and on a run
	// that loaded animations and wrote no channel at all (without outfit= that no-op used to report ok
	// true and byteIdenticalToTemplate true, which reads exactly like a clean round trip).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Write animations from Unreal back out as a GTA V animation dictionary. Give the game dictionary it came from as the template; untouched animations come out exactly as they went in."))
	static FString ExportClipDictionary(const FString& AnimSequenceAssetPaths, const FString& ClipNames,
	                                    const FString& OutYcdPath, const FString& Options);

	// Re-read a clip dictionary XML and check it against the corpus's own structural laws, counted:
	// the declaration and root element (303/303 files), LF-only with a trailing newline (303/303), one
	// SequenceData item per BoneIds row (2,673/2,673), and per QuantizeFloat channel - Offset == min(Values)
	// (131,483/131,483), Quantum > 0 (131,483/131,483), len(Values) == the sequence's FrameCount
	// (131,483/131,483), the line form - at most ten numbers on one line, eleven or more as a block of
	// ceil(n/10) rows (2,198 one-line and 62,339 block <Values> measured, 0 violations), every raw
	// non-negative (131,483/131,483), no QuantizeFloat channel with an EMPTY value list (a dictionary
	// with no animation data in it fails here by name), and whether the quantiser inverse is exact
	// (131,482/131,483 in the corpus - reported, not required).
	// The comparator for an export: run it on the written file, then diff against the source with
	// `compare_ycd.py` (maintainer lane `ycd_export`). Returns JSON; ok is COMPUTED from the laws above.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Check an animation dictionary file RUDE wrote: that it is shaped and spelled the way the game's own files are.", RudeAudience="agent"))
	static FString ProbeYcdXml(const FString& XmlPath);

	// RUDE_PEDLOD_BEGIN header
	// What LOD groups an imported ped part actually carries, and what the ydd writer would do with them.
	// LOD0/1/2 map to the game's `DrawableModelsHigh` / `Medium` / `Low`; a 4th+ LOD is not exported (no
	// measured component-ped entry carries a VeryLow group: 0/1,152 corpus entries, 0/2,125 game binary
	// entries). Per LOD: vertices, triangles, polygon groups, material slots that do NOT resolve to a slot on
	// the mesh (a LOD geometry re-uses the High group's slot by ordinal - a surplus one clamps and is counted),
	// the editor ScreenSize and whether the export writes it. `ok` is COMPUTED: true when the mesh has at least
	// one LOD and every polygon group's slot resolves. ⚠ ScreenSize is INFERRED and editor-only, and no exported
	// byte reads it: the ydd's four lodDist floats travel a different road entirely - the outfit carries them
	// from the source entry and `ExportPedReplace` hands them to `ExportYddBinary` as `LODDIST=`. They are never
	// derived from ScreenSize, and ScreenSize is never derived from them (the game stores 9998 on 2,119/2,125
	// entries, so the file holds no switch distance to convert). maintainer lane `ped_lods` (`LAWS.md`).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Show how many distance versions a clothing piece has, and how big each one is.", RudeAudience="agent"))
	static FString InspectPedLods(const FString& SkeletalMeshAssetPath);
	// RUDE_PEDLOD_END header

	// ---- MLO AUTHORING: a NEW interior, made in Unreal (GDD Tier 1 interiors: import-AUTHOR-export) ----
	// The third leg beside ImportMlo (bring one in) and ExportMloYtyp (write an EDITED one back into its own
	// file). Here nothing came from the game: the rooms, the portals, the entity list, the CMloArchetypeDef
	// and its ymap instance are all authored, and ExportNewMlo writes the two files FiveM loads.
	//
	// THE SURFACE IS WORKFLOW-FREE. There is no interior mode and no registry to keep in step:
	//   * a ROOM is an ARudeMloRoomVolume box you place and scale; it carries the room's own CMloRoomDef
	//     fields (name, flags, blend, the two timecycle names, floorId, exteriorVisibiltyDepth - the game's
	//     own misspelling, kept). Its BOX is the room's bounds; portalCount is DERIVED, never authored.
	//   * a PORTAL is a thin ARudeMloPortalVolume box across a doorway. The room volumes it touches give
	//     roomFrom/roomTo; the mid-plane of its thinnest axis gives the four corners.
	//   * an ENTITY is ANY static-mesh actor standing inside a room volume whose mesh resolves to an
	//     archetype - through its URudeMloEntityComponent, its URudeEntityComponent, the palette
	//     (AddMloProp reads a URudeArchetype), or failing those the mesh's own asset name.
	// Drag a prop into another room and it changes rooms. Delete a volume and the interior loses a room.
	//
	// Measured on the corpus 2026-09-06 over 424 MLO ytyps / 541 CMloArchetypeDef / 2,143 rooms / 3,466
	// portals / 67,440 entities and the 1,745 ymaps that carry a CMloInstanceDef - maintainer lane
	// `mlo_author` (`LAWS.md`). The laws that shape the tools below:
	//   * the archetype's 22 children are ONE order in 541/541, and flags / specialAttribute /
	//     hdTextureDist / assetType / the three dictionaries / extensions are fixed in 541/541 (law 1).
	//   * bbMin/bbMax/bsCentre/bsRadius are ZERO in 541/541, so the corpus cannot teach the rule; RUDE
	//     writes the union of the rooms and props and says so (law 2 - the lane's one deviation, NOTES.md).
	//   * room 0 is `limbo` in 541/541, with flags 96 / blend 1 / an EMPTY timecycle / depth -1 (law 3).
	//   * portalCount == the portals naming that room in 2,143/2,143 - DERIVED (law 4).
	//   * membership is the attachedObjects ORDINAL list, not geometry: an entity sits inside its own
	//     room's box in only 30,779/66,332, so containment is an authoring convenience (law 6).
	//   * a portal is 4 coplanar corners spelled "x, y, z, NaN" (13,864/13,864) with roomFrom NEVER limbo
	//     (0/3,466); the L H H L corner order is the plurality (1,762/3,206) and the winding relative to
	//     the room pair is NOT recoverable (towards roomFrom 1,855 / towards roomTo 1,609 / degenerate 2
	//     of 3,466). RUDE winds towards roomTo - the 1,609 MINORITY half, chosen for determinism, not
	//     because the game leans that way (laws 8, 9).
	//   * one CMloInstanceDef per ymap (1,745/1,745), 22 fields in one order, numExitPortals = the portals
	//     touching limbo, contentFlags bit 0x40 <-> a non-empty <physicsDictionaries> (1,745/1,745) (law 10).

	// Spawn the interior's ROOT actor (tags RUDE_MLO:<name>, RUDE_MLO_ROOT, RUDE_MLO_AUTHORED - and
	// deliberately NO RUDE_MLO_Ytyp, so ExportMloYtyp never tries to splice a source file this interior does
	// not have). Its transform is where the interior stands in the world: everything else is written
	// MLO-local, so moving the root re-places the whole interior and changes nothing inside it.
	// InteriorName is the archetype name, the ytyp file name and a joaat key, so it must be lower-case
	// a-z / 0-9 / _ (540/541 corpus MLO names are). LocationCm = "x,y,z" UE centimetres (default 0,0,0).
	// Re-running on an existing interior returns {created:false} and leaves the level alone.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Start a brand-new interior in the level: give it a name and where it stands.", RudeAudience="agent"))
	static FString NewMloInterior(const FString& InteriorName, const FString& LocationCm);

	// Add one ROOM to an authored interior: an ARudeMloRoomVolume box, centred at CenterCm relative to the
	// interior root, with ExtentCm as its HALF sizes (all three positive - 1,589/1,591 finite corpus room
	// boxes are non-degenerate). Fields is "key=value;key=value" over
	// limbo | flags | blend | timecycle | secondaryTimecycle | floorId | exteriorDepth; an unknown key is
	// REFUSED rather than ignored, so a typo can never silently do nothing. Defaults are the corpus modes:
	// flags 96 (685/1,602), blend 1 (1,598/1,602), floorId 0 (1,183/1,602), exteriorDepth -1 (1,602/1,602),
	// timecycle EMPTY (the game names one in 1,599/1,602 but every name is an unrecoverable joaat hash).
	// A room named `limbo` (or limbo=true) is THE limbo room - index 0, and FOUR of its fields are forced,
	// here and again at export: flags 96, blend 1, an EMPTY timecycleName and exteriorVisibiltyDepth -1
	// (541/541 each, law 3). `floorId` is NOT forced - room 0's floorId was never binned, and an unmeasured
	// field stays the author's. Leave limbo out and ExportNewMlo synthesizes it from the union of the rooms.
	// A RoomName or timecycle name carrying whitespace or an XML special (< > & " ' tab newline) is REFUSED
	// here AND re-checked at export, because both land in element TEXT with no escaping and a volume can
	// also be placed by hand and retyped in the details panel without this tool ever running.
	// Returns {ok, interior, room, limbo, actor, flags, floorId}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Add a room to an interior you are building: a box, with the room's own name and settings.", RudeAudience="agent"))
	static FString AddMloRoom(const FString& InteriorName, const FString& RoomName,
	                          const FString& CenterCm, const FString& ExtentCm, const FString& Fields);

	// Add one PORTAL: a thin ARudeMloPortalVolume box across a doorway. ExtentCm are HALF sizes and the
	// SMALLEST of the three picks the portal plane, so a portal is a slab, not a cube. roomFrom / roomTo come
	// from the room volumes the slab overlaps - two rooms give both sides, ONE room gives an exit to limbo
	// (roomTo 0; the game spells 2,116/3,466 portals that way and roomFrom is limbo in 0/3,466). Fields is
	// "key=value;..." over flags | mirrorPriority | opacity | audioOcclusion | roomFrom | roomTo, the last two
	// being the escape hatch when a slab overlaps three rooms and the graph is genuinely ambiguous. Supplying
	// only ONE of them is fine: the other still comes from the volumes the slab touches when exactly two are
	// touched, so a half-overridden internal doorway is never quietly turned into an exit to limbo.
	// Returns {ok, interior, actor, flags, thinAxisExtentCm}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Add a doorway between two rooms of an interior you are building (or out of the interior), as a thin box.", RudeAudience="agent"))
	static FString AddMloPortal(const FString& InteriorName, const FString& CenterCm,
	                            const FString& ExtentCm, const FString& Fields);

	// Place one PROP inside an authored interior: a static-mesh actor at LocationCm / RotationDeg relative to
	// the interior root, carrying a URudeMloEntityComponent that names the archetype. PaletteFolder is where
	// BuildArchetypePalette put its URudeArchetype assets - the archetype's mesh is used when the palette has
	// it, and /Engine/BasicShapes/Cube stands in (tag RUDE_PROXY) when it does not, so an interior can be
	// blocked out before a single drawable is imported. The room the prop ends up in is NOT stored: it is
	// resolved from the room volume containing it at export, which is what makes dragging a prop between
	// rooms work. Returns {ok, interior, archetype, actor, palette, mesh}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Put a prop inside an interior you are building, by archetype name.", RudeAudience="agent"))
	static FString AddMloProp(const FString& InteriorName, const FString& ArchetypeName,
	                          const FString& LocationCm, const FString& RotationDeg, const FString& PaletteFolder);

	// WRITE the authored interior as a FiveM resource: <OutDir>/stream/<name>.ytyp (a complete new
	// CMloArchetypeDef - the 22 children in the 541/541 order, bounds from the rooms and props, room bounds
	// from the volumes, attachedObjects from containment, portal corners in MLO-LOCAL space, floats spelled
	// the way the game's own writer spells them), <OutDir>/stream/<name>.ymap (ONE CMloInstanceDef at the
	// interior root's transform, the 22 fields in the 1,745/1,745 order) and fxmanifest.lua. UTF-8 without a
	// BOM and LF: 0/2,765 corpus ytyps and 0/19,387 ymaps carry a BOM, and every MLO-bearing file is LF
	// (0/424 ytyps, 0/1,745 ymaps; corpus-wide 6 ytyps and 4 ymaps are CRLF and none of them carries an MLO).
	// Entity ordinals are assigned by (room, archetype, actor name) so two runs of the same level produce the
	// same file. CorpusRoot is optional and only reports whether each archetype resolves to a drawable there:
	// a LOWER BOUND (a drawable can live in a ydd or yft the ledger names differently), so it never fails the
	// export.
	// Returns JSON: {ok, written, interior, ytyp, ymap, rooms, portals, entities, entitiesUnmapped,
	// portalsWithoutTwoRooms, portalsAmbiguousPlane, bounds[6], bsRadius, limboSynthesized, limboFieldsForced,
	// limboAttached, attachedTotal, numExitPortals, roomsWithoutPortals, adoptedUntagged, nonUniformScaleXY,
	// hiddenIncluded, alsoYmapEntity, orphanVolumes, archetypesInCorpus, archetypesNotInCorpus, refused[]}.
	// ok is COMPUTED and an INVALID GRAPH refuses: no room, two rooms sharing a name, a degenerate box, a
	// room or timecycle name carrying whitespace or an XML special, a prop whose mesh names no archetype, or
	// a portal that does not touch exactly one or two rooms (roomFrom/roomTo are not guessable from a
	// three-way overlap, and up to 10 portals in one interior share a room pair, so uniqueness cannot break
	// the tie either).
	// THE REFUSAL SET IS COMPUTED BEFORE ANYTHING IS WRITTEN: a refused interior leaves the file system
	// untouched (`written:false`), so a broken .ytyp never sits in a stream/ folder beside an fxmanifest.lua
	// telling the game to load it. `ytyp` / `ymap` are the paths the interior WOULD occupy; a refusal does
	// not delete an earlier good export of the same name. `limboFieldsForced` counts how many of limbo's
	// five 541/541 values (the name plus those four fields) the author's volume disagreed with, so the
	// forcing is never invisible;
	// `orphanVolumes` counts volumes that name no interior in a level holding several - a dropped room is
	// exactly the failure nothing downstream can see.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Write the interior you built in Unreal out as a FiveM resource the game loads - its definition file and the map that places it."))
	static FString ExportNewMlo(const FString& InteriorName, const FString& OutDir, const FString& CorpusRoot);

	// ---- WP12 weapon_tint lane (RudeWeapon.cpp) ----
	// Paint an imported weapon in one of the game's own tints. A weapon body's diffuse is GREYSCALE by
	// design (136 of the 171 measurable diffuses behind a palette are >=90% desaturated, 119 of them
	// >=99%) and the colour arrives from a small lookup texture the drawable binds beside it: 583 shader
	// items across the 879 effective weapon drawables bind one, 351 as TextureSamplerDiffPal and 232 as
	// TintPaletteSampler, and each name always travels with its own selector (351/351 and 232/232, 0
	// carrying both). The lookup is 2-D: u = the DIFFUSE'S ALPHA, which is a material-ZONE index and not
	// a shade (a median of 10 authored values, round numbers, uncorrelated with luminance - mean |r|
	// 0.23 - and in 114 of 171 pairs every value lands on its own palette column), and v = the tint.
	// TintIndex is that row. The range is the bound palette's OWN height (94 of the 98 entries in this
	// corpus's 804 weapon dictionaries are 128x32, 4 are 4x4), and the verdict reports distinctRows and
	// rowDuplicateOf, because a 32-row palette does not carry 32 different tints: 27 have 8 distinct
	// leading rows - which is the count weapons.meta's TINT_DEFAULT declares, referenced by 91/91
	// CWeaponInfo rows in the copy the game loads - 34 have 9, and 29 have all 32.
	// REFUSES BY NAME: an actor label no weapon in the level carries (it lists the ones it found), a
	// non-numeric or negative index, an index at or past the palette's row count, and a weapon on which
	// no material carries a palette at all (with the slot census that says why).
	// ⛔ It does NOT switch the lookup on. TintAmount is set once, by the import, only when a palette
	// bound to a RenderBucket-0 shader over a diffuse whose alpha varies, on a shader preset the lookup
	// is enabled for (weapons only today - the palette samplers are shared with the ped, vehicle and
	// prop lanes and only the weapon set was measured) - so this tool cannot enable a tint the data
	// does not support. slotsTinting says how many slots will actually look different, and ok is
	// COMPUTED from it. Mirrors SetVehicleLivery / SetPedProp in shape.
	// ⛔ EDITOR STATE ONLY: the chosen index lives on the material instance. ExportYdr does not yet
	// emit shader value parameters or the palette sampler, so nothing reads it back and no tint
	// survives an export today. That exporter work is unbuilt, not assumed.
	// Returns JSON: {ok, weapon, actor, tint, paletteRows, paletteRowsMax, distinctRows, rowDuplicateOf,
	// palettes[], slots, slotsUpdated, slotsTinting, slotsWithoutInstance, slotsWithoutParameter,
	// slotsPaletteUnbound, tintSpecValues, note}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Paint an imported weapon in one of the colours the game gives it. Give the weapon actor's name and the tint number, counting from 0."))
	static FString SetWeaponTint(const FString& ActorLabel, const FString& TintIndex);

	// ---- product debt (maintainer lane `product_debt`) ----
	// THE AREA CATALOG `ImportArea` ASKS FOR, DERIVED FROM THE CORPUS ITSELF. Until this tool
	// existed the repository shipped no catalog and `ImportArea` could only answer "cannot read
	// the area catalog" - a named tool with no data behind it (AGENTS §9).
	// Walks the corpus ledger's ymap rows (the copy the game would LOAD, one per name), reads each
	// one's declared `<entitiesExtentsMin/Max>` and counts its `CEntityDef` items, groups them into
	// prefix families, and files each family under the region the game's OWN `popzone.ipl` puts its
	// centre in (smallest containing box wins - popzone nests districts inside catch-alls).
	// Writes ONE JSON array in the schema `ImportArea` already reads: {alias, prefixes[]} plus
	// provenance fields that reader ignores (source, named, ymaps, entities, zone, extentMin/Max,
	// note). Two kinds of entry:
	//   "prefix" - one per ymap family, alias = the prefix. Always resolvable, never a guess.
	//   "zone"   - one per popzone region word, prefixes = every family whose ymaps mostly land in
	//              it. This is the human name, and it is the game's own word, not an invention.
	// A family the region data cannot place keeps the prefix as its alias and SAYS SO in `note` -
	// no name is ever fabricated. An alias that would collide case-insensitively with a prefix
	// alias is suffixed " (zone)", because `ImportArea` matches exactly and breaks on the first hit,
	// so a duplicate name would silently make the second entry unreachable.
	// CorpusRoot: a ledgered filebase. OutJsonPath: absolute *.json; EMPTY writes
	// <plugin>/Catalogs/area_aliases.json, exactly where `ImportArea` looks with an empty CatalogPath.
	// ⛔ THE CATALOG IS NEVER SHIPPED WITH RUDE. It is derived from the game's own files, and this
	// repository ships no game data - so every user generates their own with this tool, from their own
	// install. `ImportArea` refuses until they do, and names this tool when it refuses.
	// ⚠ COST: it reads every effective ymap in full - 11,086 files, 9,956,359,597 bytes on the
	// maintainer's corpus (maintainer lane `product_debt`, `measure_product_debt.json`). The Python
	// twin that measured it took 19.2 s with the OS cache warm; a first run on a cold cache is
	// materially slower, and this C++ has never been run, so no timing for it is quoted here. It is a
	// once-per-corpus generator, not a per-session call.
	// Returns JSON: {ok, outPath, wrote, entries, prefixEntries, zoneEntries, aliasCollisionsSuffixed,
	// families, familiesWithZone, familiesWithoutZone, zonesParsed, popzone, ymapsListed, ymapsRead,
	// ymapsMissing, ymapsNoExtents, ymapsDegenerateExtents, ymapsZoned, ymapsUnzoned, entities,
	// xmlBytesRead} or {ok:false, error}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Build the list of district names you can type into ImportArea, worked out from your own game folder. Run it once after you point RUDE at a new folder.", RudeAudience="agent"))
	static FString BuildAreaCatalog(const FString& CorpusRoot, const FString& OutJsonPath);

	// ONE CALL THAT ANSWERS "why doesn't this work on my machine". Pure diagnosis: it reads, it
	// never writes, and it never regenerates anything. Checks, in order, the things a new user gets
	// wrong: the engine version RUDE is measured against (5.8); whether RUDE and ToolsetRegistry are
	// present AND enabled (a disabled ToolsetRegistry costs the whole tool surface) and whether the
	// OPTIONAL ModelContextProtocol surface is there - reported, never a fault, because the panel and
	// the CLI are whole surfaces without it; whether the plugin's own Content mounted (an unmounted
	// /RUDE/Masters fails deep inside an import instead of here); how many master materials exist and
	// whether any is stale - by CALLING RudeGeneratedMasterHealth, the single rule the generators
	// themselves call, so the doctor cannot drift from it (it covers the generated masters plus
	// M_RUDE_Detail and M_RUDE_Cutout; the other four named masters have no upgrade rule to check);
	// whether an area catalog is present and parses; whether CorpusRoot is a LEDGERED filebase or
	// just a flat folder of XML (both are folders - only one has the manifest every corpus lookup
	// goes through) and, when it is ledgered, its title, rout version, row count and per-lane rows;
	// and whether there is an editor world at all.
	// CorpusRoot: a filebase to check; EMPTY skips only the corpus section (it is reported as
	// checked:false, never as a pass).
	// Every fault found is one plain sentence in `problemList`, and `ok` is COMPUTED as
	// "problems == 0" - a doctor that always says ok is not a doctor. An OPTIONAL surface that is
	// absent is reported in `plugins` and is NOT a problem: a false alarm on the headline signal
	// teaches people to ignore the headline.
	// ❓ Neither this tool nor BuildAreaCatalog has been run in the editor: they were written and
	// reviewed on 2026-09-07 and the gate that exercises them (`gate.jsonl`) has not been executed.
	// Returns JSON: {ok, problems, problemList, engine, engineExpected, engineMatches, plugins,
	// pluginsEnabled, mastersMounted, masters, generatedMasters, staleMasters, unparsedMasters,
	// staleMasterNames, catalogPath, catalogPresent, catalogEntries, corpus, headless, unattended,
	// editorWorld}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Check this machine and say what is wrong: engine version, plugins, materials, the district list, and whether your game folder is the kind RUDE can read."))
	static FString RudeDoctor(const FString& CorpusRoot);

	// ---- WP13 ycd_pack lane (RudeYcdPack.cpp) ----
	// Pack a clip-dictionary XML into a loadable binary .ycd, by DONOR REPACK. This is the block that
	// stood between ExportClipDictionary's XML and the game: measured (maintainer lane `ycd_export`,
	// LAWS.md H), the maintainer's own exporter has NO xml->ycd packer in either language, so the XML
	// had nowhere to go. Container laws: maintainer lane `ycd_pack` (`LAWS.md` + `measure_bin.json`).
	// DENOMINATOR FIRST, because it limits everything: the corpus holds 24,844 .ycd as XML and 0 as
	// binaries, so every container law here was measured over the 10 .ycd binaries that exist on the
	// authoring machine - all of them PRODUCED files, none shipped by the game. 10/10 RSC7 v46,
	// 20 animations, 20 sequences, 751 BoneIds rows, 1,417 QuantizeFloat channels, 1,341 frames.
	// HOW IT PACKS: a sequence block is self-contained (reached by arithmetic, no internal pointers),
	// so a SIZE-PRESERVING channel edit is applied in place in the donor's inflated image and the
	// donor's own header re-wraps it - no page plan, no atMap rebuild, no pointer relocation. Two
	// edits are built: the 12-byte descriptor (u32 numBits, f32 Quantum, f32 Offset - Quantum/Offset
	// are an 8-byte overwrite, 1,417/1,417 descriptors tiled with no hole), and the raws in the
	// frame-major LSB-first bit block (decode-and-write-back reproduced the block byte-identically on
	// 20/20 sequences, with zero padding bits after the last channel on 1,341/1,341 frames).
	// WHAT THIS CAN AND CANNOT DO, plainly: it EDITS a .ycd binary you hand it; it cannot BUILD one.
	// Nothing in RUDE, and nothing in the maintainer's own exporter in either language, turns a
	// clip-dictionary XML into a .ycd container from scratch - so the donor is not a convenience, it
	// is the only thing that makes a loadable file possible here. Every change that would move a SIZE
	// is out of scope and refused.
	// REFUSES BY NAME, and the refusal is ATOMIC: every channel is validated and its bytes STAGED
	// before any byte is committed, so one refusal means NOTHING is written - the verdict says
	// `written:false` and no output file is produced. Refused by name: a sequence whose count table is
	// ambiguous, an animation or sequence count the donor does not have, a channel whose XML shape is
	// not the donor's, a value list whose length is not the sequence's frame count, a Quantum that is
	// not positive, a value that does not fit its channel's own numBits (widening a channel moves
	// every offset after it - a container assembly this lane did not build), and a donor whose
	// graphics segment is not empty (the re-wrap re-uses the donor's header, which would then announce
	// bytes the stream no longer holds; empty on 10/10 measured, and 10 produced files is not a
	// licence). Root translation (bone 0, track 0) is GATED behind `authorroot=1`: it is the channel a
	// prior in-game test crashed on.
	// THE MEASURE, computed in the verdict rather than asserted: a pack whose patched image differs
	// from the donor's in 0 bytes copies the donor's FILE bytes verbatim, so byte identity never
	// depends on a compressor; an edited pack re-deflates (its compressed bytes WILL differ from the
	// donor's everywhere, which is legal and why the measure is on the inflated segment) and every
	// byte that moved is attributed to a declared extent. `ok` is COMPUTED: false on any refusal, on
	// a byte that moved outside a declared extent, on an XML channel with no donor channel, and on a
	// run that matched channels and wrote none of them.
	// THE CLAIM IS GATEABLE. `Options expect=identical` makes ok false unless this pack reproduced the
	// donor's file bytes; `expect=edited` makes it false unless a channel's value actually moved -
	// without which a pack that silently wrote nothing would report noOp:true, ok:true and sail
	// through a gate. An expect= value the tool does not know is refused, never ignored.
	// Gate: maintainer lane `ycd_pack` (`gate.jsonl` + `compare_ycd_bin.py`).
	// Returns JSON: {ok, file, xml, template, version, systemSegmentBytes, fileBytes, written, expect,
	// expectationMet, animations, clips, channelsMatched, channelsAuthored, channelsChanged,
	// channelsUnchanged, descriptorsWritten, framesWritten, channelsRefused, shapeMismatch,
	// frameCountMismatch, rawsOutOfRange, xmlChannelsWithoutDonor, rootChannelsGated,
	// xmlItemsInUnknownList, carriedStatic, carriedRawFloat, carriedIndirect, carriedInlinePool6,
	// segmentBytesDiffering, bytesDifferingOutsideDeclaredExtents, firstDifferingOffset, noOp,
	// byteIdenticalToTemplate, reDeflated, authoredButWroteNothing, firstRefusal, note}.
	// `channelsAuthored` counts the channels this pack wrote bytes back for; `channelsChanged` counts
	// the ones whose value actually moved. On an untouched export the second is zero while the first
	// is not - do not read either as the other. (Neither has been observed: this has not been run.)
	// Nothing this lane writes has been loaded by the game, and this file has not been compiled.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Turn an animation dictionary RUDE wrote into the packed file the game actually loads. Give the game file it came from as the template: this EDITS that file and cannot build a packed file from nothing. Any change that would resize something is refused, and a refusal writes no file at all. An untouched animation comes out byte for byte the same."))
	static FString PackYcdBinary(const FString& XmlPath, const FString& OutYcdPath, const FString& Options);

	// Read a BINARY .ycd and report the container a packer has to write into, with the reader's own
	// self-check on it. Reports the RSC7 header and page plan, whether the blockmap's page-count
	// record agrees with the flag words (10/10 measured), the clip and animation atMap walk, and per
	// sequence: whether exactly one count table survives the four constraints that pin it (20/20, 0
	// ambiguous), the nine pool counts, the QuantizeFloat channel census, and - the check that makes
	// the bit layout evidence rather than a transcription - whether every channel's raws survive a
	// decode and a write-back byte-identically (20/20) with zero padding bits after the last channel
	// (1,341/1,341 frames). `ok` is COMPUTED from those: any ambiguous sequence, any broken packed
	// block, any non-positive Quantum, any numBits outside 1..32, any non-zero padding, or a
	// blockmap record that disagrees with the flags makes it false. The padding check is SKIPPED, and
	// counted in `sequencesPadCheckSkippedIndirect`, on a sequence carrying an IndirectQuantizeFloat
	// channel: those bits belong to a channel this reader does not decode, so checking them for zero
	// would call a good file broken. None was witnessed here (Indirect 0/20 sequences).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Read a packed animation file and report what is inside it, and whether RUDE reads it consistently.", RudeAudience="agent"))
	static FString ProbeYcdBinary(const FString& YcdPath);

	// Compare two BINARY .ycd files and say WHERE they differ, in the only place a comparison means
	// anything: the inflated system segment. A compressed .ycd is not canonical - two files holding
	// the same image can differ in every compressed byte - so a file-level diff answers the wrong
	// question. Every differing byte is attributed through the FIRST file's own container walk to a
	// channel descriptor, a sequence's packed block, or elsewhere. `ok` is COMPUTED: the images are
	// identical, or every byte that moved sits inside a channel this reader can name. A byte that
	// moved anywhere else is a structural difference and this tool says so instead of passing.
	// READ ITS `ok` FOR WHAT IT IS: an ATTRIBUTION verdict, not an identity one - it passes when the
	// two images are identical AND when they differ only inside channels it can name. For identity,
	// read `segmentIdentical`/`fileBytesIdentical`, or gate it at the source with PackYcdBinary's
	// `expect=identical`. Gate: maintainer lane `ycd_pack` (`gate.jsonl` + `compare_ycd_bin.py`).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Check two packed animation files against each other and say exactly which animation channels differ.", RudeAudience="agent"))
	static FString CompareYcdBinary(const FString& APath, const FString& BPath);

	// RUDE_PEDVAR_BEGIN header
	// THE PED VARIATION TABLE (WP13, maintainer lane `ped_variation` (`LAWS.md`)). `ExportPedReplace` ships a ped by
	// REPLACING its own dictionaries, so it never needed a variation row. Adding a garment does: the ymt is the table
	// that says which drawables a slot HAS and how many texture letters each carries. These two tools give a NEW
	// garment a row instead of overwriting one.
	//
	// `ExportPedVariationYmt` writes the ped's `CPedVariationInfo` back from the outfit asset by SPLICE, the way
	// `ExportMloYtyp` / `ExportScenarioRegion` do: every untouched row re-emits its OWN bytes and only an edited or
	// added one is rebuilt. Measured: cut -> split at the fixed `<Item>` indent -> re-assemble reproduces all
	// 1,820/1,820 corpus files byte for byte (law 9), each region occurs exactly once (1,820/1,820), and the top-level
	// order is a single order (1,820/1,820). THE MEASURE: an untouched export is byte-identical to its source -
	// pass `Expect="identical"` and the tool ASSERTS it (`ok:false` when it is not), which is the only way a
	// script that stops on ok:false can catch a row this tool rebuilt when it should not have. `Expect="changed"`
	// asserts the opposite for an export that is meant to carry an edit; empty asserts nothing.
	// ⚠ The corpus form is the PSO container RENDERED to text - it carries `<MetaSchema>` and an opaque
	// `<UnknownBlob40>` (1,820/1,820, law 1) and NO original binary survives beside it. The game reads the binary and
	// this repo has no PSO writer, so the file this tool writes is an AUTHORED variation table, not a drop-in: the
	// verdict says so itself (`format":"pso-xml"`, `gameReady":false`) and never claims otherwise.
	// A REBUILT row keeps its own `ownsCloth`: the outfit asset has no cloth field, so the flag is re-read off
	// the source row (98 of 45,533 corpus rows carry `true`) and a row that does not spell it is left verbatim
	// instead of being rebuilt - counted as `clothFlagsCarried` / `clothRowsRefused`, never dropped in silence.
	// CorpusRoot empty = the outfit's own `SourceYmt`. Verdict: ped, ymtPath, sourceYmt, bytes, sourceBytes,
	// byteIdentical, components, componentsAdded, drawables, drawablesVerbatim, drawablesRebuilt, drawablesAdded,
	// drawablesOnlyInSource, texRows, numAvailTexRewritten, compInfoRows, compInfoRowsAdded, propRowsVerbatim,
	// availCompRewritten, clothFlagsCarried, clothRowsRefused, expect, expectMet, format, gameReady, problems[].
	// ⛔ DRAFT - never compiled and no gate has been run in the editor.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Write a character's clothing-variation table back out from Unreal, keeping every row the game wrote untouched. Expect=identical makes an untouched write prove it changed nothing.", RudeAudience="agent"))
	static FString ExportPedVariationYmt(const FString& OutfitAssetPath, const FString& OutDir, const FString& CorpusRoot, const FString& Expect);

	// Give a NEW garment its own row on the outfit: one drawable appended to `Slot` (head/berd/hair/uppr/lowr/hand/
	// feet/teef/accs/task/decl/jbib, or 0..11), pointing at `MeshAssetPath` (a skinned mesh on the ped's own
	// skeleton), with one texture letter per entry in `TextureAssetPaths` (comma-separated; empty = one letter `a`
	// with no texture yet). The row's index is the slot's next free one, so the entry name it will ship under is
	// `<slot>_<ddd>_u` - the name `ExportPedReplace` / `ExportYddBinary` write into `stream/<ped>.ydd`.
	// Every value the row carries that the caller did not give is the MODE of the game's own data with its
	// denominator (law 5): `numAlternatives` 0 (41,258/45,533), `distribution` 255 (228,687/228,790), `texId` 0
	// (176,569/228,790), `ownsCloth` false (45,435/45,533), and `propMask` = the modal mask OF THAT SLOT
	// (e.g. uppr 17 on 5207/6863). Nothing is invented; a value that has no measured mode is left at zero, and the
	// three propMask arrays are read out of `measure_variation5.json`, not transcribed.
	// It REFUSES before touching the outfit - nothing is added and nothing is saved - when the mesh is on a
	// different skeleton than the ped's, when the mesh has no LOD 0 vertices, when the slot is not one of the
	// twelve, or when more than 26 textures are named (the letters run a..z and the widest aTexData row in the
	// corpus is 26). So `ok:false` from this tool always means the outfit is unchanged.
	// Verdict: ped, slot, slotIndex, drawableIndex, entryName, mesh, propMask, propMaskDenominator, textures,
	// texturesBound, numAvailTex, vertices, triangles, componentCreated, saved, problems[].
	// ⛔ DRAFT - never compiled and no gate has been run in the editor.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Add a new piece of clothing to a character as its own variation, instead of replacing one the game already has.", RudeAudience="agent"))
	static FString AddPedDrawable(const FString& OutfitAssetPath, const FString& Slot, const FString& MeshAssetPath, const FString& TextureAssetPaths);
	// RUDE_PEDVAR_END header

	// ---- WP13 vfx_move lane (RudeVfxMove.cpp): the two GDD Tier 3 formats, both READ-ONLY ----
	// One .ypt's effect rules as URudeParticleEffect DataAssets, one asset per EFFECT RULE, under
	// <DestFolder>/<ypt>/. EffectName is "<ypt>" for every rule in that dictionary or "<ypt>/<rule>"
	// for one; both halves are needed to name an effect, because 2,549 distinct effect-rule names occur
	// 10,268 times across the corpus's 1,240 ypt files and a rule name alone is ambiguous.
	// READS (measured over 10,268 effect rules, maintainer lane `vfx_move` (`LAWS.md`)): the 43 fields
	// every effect rule carries, plus EvolutionList on 8,441 of them; the rule's EventEmitters (27,676
	// corpus-wide) with the emitter and particle rule each names - 27,676/27,676 of those references
	// resolve inside the SAME file's dictionaries, so the lane never needs a cross-file lookup; and a
	// shallow read of each referenced emitter rule (creation/target domain shape - Cylinder 1,619 /
	// Sphere 1,413 / Box 1,114 over the base slot's 4,146) and particle rule (ShaderFile: ptfx_sprite
	// 21,074 / ptfx_trail 763 of 21,837, technique, draw type, behaviour list). Distances and offsets
	// become UE centimetres with the house Y mirror; everything else is carried as spelled, and EVERY
	// leaf field of every record is also kept in a raw field map so an unread field is visible, not
	// dropped.
	// ⛔ PREVIEW TIER and NOT a conversion: no Niagara system, no curve evaluated (40,055 of the
	// corpus's 51,340 effect-rule keyframe props carry zero keys), no particle material, no drawable or
	// texture imported, and NO WRITER - nothing in RUDE emits a .ypt. Full authoring is a later epic.
	// Returns JSON: {ok, ypt, slot, file, sha1, effectRules, matched, created, refilled, eventEmitters,
	// rulesRead, rulesUnresolved, keyframeProps, keyframePropsWithKeys, drawablesNotImported,
	// texturesNotImported, invalidNames, destFolder, tier, note, sample[]}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Read a particle-effect file's effects into the editor so you can look at what each one is made of. It is a read-only preview: nothing is turned into a working effect and nothing is written back."))
	static FString ImportParticleEffects(const FString& CorpusRoot, const FString& EffectName, const FString& DestFolder);

	// A placement stand-in for one imported effect: an ARudeParticlePreview with a wireframe sphere at
	// the effect's own culling radius and a text label. It spawns at LocationCm ("x,y,z" in
	// centimetres) PLUS the effect's ViewportCullingSphereOffset, so the marker is centred on the
	// culling volume, not on the point passed in; the verdict's locationCm says where it went.
	// ⛔ An APPROXIMATION of WHERE the effect sits and roughly how far it reaches - it does not
	// simulate, emit or render the effect and it is not a conversion of one. The marker is a SPHERE
	// whatever the emitter's creation domain says; the domain is recorded on the actor, not drawn.
	// Neutral by default: radius = ViewportCullingSphereRadius, else DistanceCullingCullDist, else
	// 100 cm, and radiusSource says which was used.
	// Returns JSON: {ok, effect, ypt, actor, locationCm, radiusCm, radiusSource, creationDomain,
	// approximation}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Drop a marker in the level showing where a particle effect would sit and roughly how far it reaches. The marker is centred on the effect's own culling sphere, so it can sit a little off the point you give. It is a stand-in, not the effect itself."))
	static FString PlaceParticlePreview(const FString& EffectAssetPath, const FString& LocationCm);

	// Re-read one ypt out of the corpus and count the lane's structural laws on it without creating a
	// single asset: the four dictionaries' sizes, the event-emitter count and how many of its
	// emitter/particle references resolve in-file, how many keyframe props carry keys, the shader-file
	// and creation-domain census, and the first effect rule's field-tag count. The instrument a gate
	// runs when it wants numbers rather than assets. `ok` is COMPUTED: true only when the file parses,
	// its root is <ParticleEffectsList>, and every reference resolves in-file (27,676/27,676
	// corpus-wide, so an unresolved one is news).
	// Returns JSON: {ok, ypt, slot, file, bytes, effectRules, emitterRules, particleRules, drawables,
	// textures, unreferencedStrings, eventEmitters, refsResolved, refsUnresolved, keyframeProps,
	// keyframePropsWithKeys, firstEffectFieldTags, shaderFiles{}, creationDomains{}}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Count what is inside one particle-effect file without importing anything.", RudeAudience="agent"))
	static FString ProbeYptXml(const FString& CorpusRoot, const FString& YptName);

	// One .mrf (a MoVE animation network) as a URudeMoveNetwork DataAsset: the node tree flattened
	// (index, parent, depth, type, name, clip reference, every leaf field verbatim), every transition
	// with its conditions, and the trigger and flag bit tables.
	// Measured over the corpus's 162 .mrf files (maintainer lane `vfx_move` (`LAWS.md`)): 42,772 typed
	// elements in 35 kinds, of which 9,136 are transition CONDITIONS - leaving 33,636 graph nodes in 23
	// kinds, the two sets disjoint; 8,000 transitions ALL carrying the same 18 fields (plus SynchronizerTagFlags
	// on the 493 whose SynchronizerType is Tag), 9,136 conditions in 12 kinds, 1,050 triggers and 945
	// flags; 124 networks root in a StateMachine and 38 in a bare State.
	// ⛔ READ-ONLY TIER: no AnimBlueprint, no AnimGraph, no UE state machine is generated, and NO
	// WRITER - nothing in RUDE emits a .mrf. A full AnimBlueprint projection is a later epic, and two
	// measured facts say why the graph alone is the honest deliverable: 25,908 of the 26,565 non-empty
	// node names are hash_XXXXXXXX (the file does not spell its own identifiers), and 5,046 of the
	// 5,264 clip records name a clip SET rather than a clip, whose contents live outside the .mrf.
	// Returns JSON: {ok, network, slot, file, sha1, nodes, states, stateMachines, clips, transitions,
	// transitionsResolved, transitionsUnresolved, conditions, triggers, flags, hashedNames, plainNames,
	// rootType, created, refilled, asset, tier, note, nodeTypes{}}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Read one animation network into the editor so its states and transitions can be looked at. It is a read-only view: no animation blueprint is built and nothing is written back."))
	static FString ImportMoveNetwork(const FString& CorpusRoot, const FString& NetworkName, const FString& DestFolder);

	// The graph of an imported URudeMoveNetwork as indented text, so it can be READ headlessly - the
	// point of the read-only tier. Each node prints as "<indent>[i] <type> <name> <role>" with its clip
	// reference when it has one; each transition as "-> <target> <duration>s <blend> <sync> if
	// <conditions>". MaxLines caps the output (default 200, "0" = all) and the verdict says whether it
	// truncated. The text also goes to the log, so a -script= run shows it without a viewer.
	// Returns JSON: {ok, network, nodes, transitions, lines, printed, truncated, text}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Print an imported animation network's states and transitions as plain text.", RudeAudience="agent"))
	static FString PrintMoveNetwork(const FString& NetworkAssetPath, const FString& MaxLines);

	// ---- WP13 cutscene_export lane (RudeCutsceneExport.cpp) ----
	// The container's own structure, read straight from the game's file - the measurement ImportCutscene and
	// ExportCutscene both rest on, available without importing anything. Reports the six top-level lists
	// (pCutsceneObjects, pCutsceneLoadEventList, pCutsceneEventList, pCutsceneEventArgsList, concatDataList,
	// discardFrameList), whether each cuts into <Item> slices that concatenate back to the block's own bytes
	// (the splice model's precondition), the event/argument/camera-cut counts, how many events carry no
	// arguments (iEventArgsIndex -1), whether the events are in time order, and how many numbers
	// <cameraCutList> holds. `ok` is COMPUTED from the slice exactness and from the document surviving the
	// editor's text loader byte-for-byte - it is not a liveness ping.
	// Measured over the maintainer's filebase (maintainer lane `cutscene_export` (`LAWS.md`), 2026-09-07):
	// 816 cutscenes / 201,718,385 bytes / 226,559 events; CRLF in 816/816; every one of the six lists slices
	// exactly in 816/816; <cameraCutList> holds 8,066 numbers across the corpus and its count matches the
	// file's camera-cut event count in only 53 of 816 files - which is why nothing derives one from the other.
	// CorpusRoot: filebase root (ledger type "cut") or a folder holding <name>.cut.pso.xml; CutName: e.g. ah_1_int.
	// When the corpus holds several copies of a cut name it opens the EFFECTIVE one (the game's own override
	// order) - 33 of 781 cut names in the maintainer's filebase have more than one copy and 32 of those differ.
	// ⛔ DRAFT (2026-09-07): never compiled and never run in the editor.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Look inside a GTA V cutscene file and report what it contains, without importing it.", RudeAudience="agent"))
	static FString ProbeCutsceneSource(const FString& CorpusRoot, const FString& CutName);

	// Move one cutscene event in time, on the sidecar ImportCutscene wrote (URudeCutsceneEvents). The sidecar is
	// the truth for an event's time; ExportCutscene splices that one line back into the source document, so this
	// is the scriptable edit the export gate uses.
	// EventsAssetPath: /Game/RUDE/Cutscenes/<cut>/DA_<cut>_events.
	// EventSelector: the event's ordinal in the sidecar's Events array (load list first, then the event list, in
	// file order), or "CAMERACUT:<k>" for the k-th camera-cut event, counting from 0.
	// NewTimeSeconds: seconds from the start of the cutscene; negative is refused.
	// ⛔ It does NOT move the matching camera-cut section in the Level Sequence. ExportCutscene compares the two
	// and, when both moved a cut to different times, refuses by name rather than picking a winner.
	// Returns JSON: {ok, asset, cut, index, list, eventId, argsType, from, to}.
	// ⛔ DRAFT (2026-09-07): never compiled and never run in the editor.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Move one event of an imported cutscene to a new time, in seconds.", RudeAudience="agent"))
	static FString SetCutsceneEventTime(const FString& EventsAssetPath, const FString& EventSelector, const FString& NewTimeSeconds);

	// Write an edited cutscene back out as a .cut.pso.xml by SPLICING the source document's own bytes: with no
	// edit the splice is a no-op, so the document written IS the source document, and an edited time rewrites
	// nothing but that event's own <fTime> digits. The read half is ImportCutscene; this is its write half.
	// ⛔ DRAFT (2026-09-07): never compiled, and no gate row has been run in the editor - the sentences below
	//    describe the CODE, not a run. ❓ No exported .cut has ever been loaded by the game; this lane's whole
	//    measure is byte identity against the source file, which is weaker than "the game accepts it".
	// What it reads as an edit:
	//   * the sidecar's Events[i].Time, written by SetCutsceneEventTime. The sidecar's fields are
	//     VisibleAnywhere, so the asset editor SHOWS them and cannot change them - there is no hand-edit
	//     path today, and the scriptable call is the only writer;
	//   * a camera-cut section moved in the Level Sequence's camera cut track - paired to the cutscene's
	//     rage__cutfCameraCutEventArgs events in file order (ImportCutscene's synthetic cut at 0, added when the
	//     first real cut is later, stands for no event and is allowed to be one extra section).
	//   * When both moved the SAME cut to different times it refuses by name rather than picking a winner.
	// REFUSES BY NAME, never writing a guess: an added or removed event / object / argument record (a new event
	// type lands here), an event whose type, id, argument index or list changed, an event whose <fTime> line does
	// not have the measured shape, a camera cut added to or removed from the Level Sequence, and an edit that
	// moves an event past its neighbour - the file stores its events in time order (816 of 816 measured files,
	// both lists), so that is a REORDER, which no splice can express.
	// What it never touches: <cameraCutList> (a DIFFERENT list - its count matches the file's camera-cut event
	// count in only 53 of 816 measured files, so RUDE never derives one from the other and says so in the
	// verdict), the argument records (11,929 of 125,684 are shared between events, so an event-shaped edit to one
	// would silently move another event's arguments), the concat rows, the discard frames, and the schema tail.
	// Everything the sidecar carried rides back out as the source file's own bytes, verbatim.
	// LevelSequenceAssetPath: /Game/RUDE/Cutscenes/<cut>/LS_<cut>; its DA_<cut>_events sidecar must sit beside it.
	// OutPath: a path ending .xml, or a folder to drop <cut>.cut.pso.xml into.
	// CorpusRoot: filebase root (ledger type "cut") or a folder; empty = the source file the sidecar recorded.
	// Options: DRYRUN (compute the splice and the verdict, write nothing), STRICTCUTLIST (refuse a camera-cut
	// time change instead of leaving <cameraCutList> as the file spells it).
	// Returns JSON: {ok, cut, levelSequence, eventsAsset, source, resolvedBy, out, written, sourceBytes,
	// bytesIdentical, events, eventsUnchanged, eventsEdited, cameraCutEdits, editsFromSequence, cameraCutSections,
	// cameraCutEvents, cutsPaired, objectIdMismatches, objects, eventArgs, concatRows, discardRows, lineEndings,
	// cameraCutListUntouched, edits[{event,list,cameraCut,editor,from,to}], refusals[], note}.
	// cameraCutListUntouched is COMPUTED
	// from the two documents; bytesIdentical describes the document RUDE would write, so on a refusal it reads
	// true while written reads false - read written and refusals[] for the outcome.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Write an edited cutscene back out as a .cut.pso.xml file, by splicing the source file's own bytes - nothing is rewritten but an edited event's time digits. DRAFT: never run yet."))
	static FString ExportCutscene(const FString& LevelSequenceAssetPath, const FString& OutPath,
	                              const FString& CorpusRoot, const FString& Options);

	// LOD lineage: the chain an entity hands over along (up through its parents) and its children.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Show what an object hands over to at distance (its LOD parents) and what hands over to it (its children).", RudeAudience="agent"))
	static FString LodLineage(const FString& ActorLabel);

	// Re-parent an entity in the LOD chain (empty ParentLabel = make it an orphan). Refuses links the
	// ymap format cannot express; previews the fields the export will derive.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Change which distant version an object hands over to. Leave the parent empty to make it stand alone.", RudeAudience="agent"))
	static FString SetLodParent(const FString& ActorLabel, const FString& ParentLabel);

	// Derived-vs-stored over every entity in the level; zero diffs on an untouched level is the proof.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Check every object's LOD links against its stored numbers and report any that disagree.", RudeAudience="agent"))
	static FString LodAudit();

	// Regenerate an HD entity's LOD parent from the HD mesh itself (GDD Wave 2 "swap a building,
	// press rebuild"): reduced mesh -> new drawable asset + new palette archetype; the existing LOD
	// parent is re-pointed at it, or one is placed and linked for an orphan.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Rebuild the distant version of a building from the building itself: a simplified copy becomes its new LOD.", RudeAudience="agent"))
	static FString MakeLodArchetype(const FString& ActorLabel, const FString& NewArchetypeName,
	                                const FString& TrianglePercent, const FString& LodDist, const FString& PaletteFolder);

	// Rebuild an SLOD chunk: merge the parent's children (the next-finer shells under it) into one
	// baked-atlas drawable, reduce it, wrap it in a palette archetype, re-point the parent entity.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Rebuild a far-distance chunk: merge everything under it into one simplified model with one texture sheet.", RudeAudience="agent"))
	static FString RebuildLodChunk(const FString& ParentLabel, const FString& NewArchetypeName,
	                               const FString& TrianglePercent, const FString& AtlasSize, const FString& PaletteFolder);

	// Rebake the LOD lights: every entity light extension in the level (or the YmapFilter's ymaps)
	// becomes one distant light in a <Name>_distlodlights / <Name>_lodlights ymap pair under OutDir/stream.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Rebuild the far-away lights (the dots you see at night from a distance) from every light on the placed objects.", RudeAudience="agent"))
	static FString RebakeLodLights(const FString& OutDir, const FString& Name, const FString& YmapFilter);

	// Edit one light of an entity (intensity, colour r,g,b, falloff, falloffExponent, coneInner, coneOuter,
	// position x,y,z in RAGE metres); the export writes it into the entity's light extension.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Change one light on a placed object (brightness, colour, reach, cone or position).", RudeAudience="agent"))
	static FString SetLightField(const FString& ActorLabel, const FString& LightIndex, const FString& Field, const FString& Value);

	// WP4 spike: new World Partition level + one Data Layer + one actor on it + save, headless.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Test that RUDE can create a streaming level with a toggleable layer and save it. Give a content path for the new level.", RudeAudience="agent"))
	static FString ProbeWorldPartitionLevel(const FString& LevelPath);

	// Shape round-trip of a ROUT XML through RUDE's own parser + writer: parse, re-spell, re-parse,
	// compare element paths / attributes / leaf text. ListPath = a text file of XML paths (one per
	// line) or a single XML path. Verdict: {ok, files, identical, differing, elements, attributes,
	// mismatches:[...]} - ok is false when any file differs. The WP3 gate; it must be green before
	// any exporter re-emits a carried subtree.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Check that RUDE can read a game XML file and write it back without losing or changing any element. Give one XML path or a list file.", RudeAudience="agent"))
	static FString XmlShapeRoundTrip(const FString& ListPath, const FString& OutDir);

	// Mode: empty = instanced display (one ISM per mesh per ymap, fast, not per-entity addressable);
	// "ACTORS" = one actor per entity carrying a URudeEntityComponent with every CEntityDef field
	// and its provenance - the editable, exportable scene. Verdict adds mode + actors.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Re-place an area you already imported, into the level you have open. Reads the manifest that Build Map Area wrote, so it spawns the objects again WITHOUT re-importing any models. Mode ACTORS places one editable actor per object instead of fast instanced batches."))
	static FString ImportScene(const FString& ManifestPath, const FString& MeshFolder,
	                           const FString& Filter, const FString& Mode);

	// Export a UStaticMesh as a ydr XML file (.ydr.xml) (the reverse
	// lane). Positions/normals/UVs inverse-transformed per the RUDE convention;
	// shader presets recovered from material slot names; texture names recovered
	// from bound RUDE MaterialInstances where present.
	// AssetPath: content path of the StaticMesh (e.g. "/Game/RUDE/Meshes/Props/prop_x").
	// OutXmlPath: absolute file path for the emitted *.ydr.xml.
	// Returns JSON: {ok, xmlPath, geometries, vertices, triangles, renderBucketUnrecovered,
	// collisionPrimitives, collisionFromRenderMesh} or {ok:false, error}.
	// collisionFromRenderMesh 1 = the mesh had NO AggGeom, so the embedded bound was re-derived
	// from the render mesh (a whole-mesh BVH). Counted 2026-08-05 (#40): RUDE has no collision
	// IMPORTER, so an imported asset always takes that branch, and it used to be silent under
	// ok:true. Declared gap, not a gate.
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

	// Show only what GTA shows at this hour. Hour = "0".."23".
	// ⭐ The game gates time-of-day at the ARCHETYPE level: 3,936 CTimeArchetypeDef carry a 24-bit
	// `timeFlags` hour mask (bit N = visible during hour N; the common masks are night windows,
	// hours 0-5 + 20-23). ImportScene groups each gated archetype into its own ISM component tagged
	// RUDE_TIME:<mask>, so this is a visibility sweep over exactly those components.
	// ⛔ Do NOT reimplement this as a shader/emissive gate - that was tried, and it is a UE-only
	// invention that cannot round-trip to GTA (LOG 2026-07-30).
	// Returns JSON: {ok, hour, gatedComponents, shown, hidden}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Set the time of day, so night-only signs and lit windows show or hide the way they do in game.", RudeAudience="agent"))
	static FString SetWorldHour(const FString& Hour);

	// Batch ImportYtd: ListPath = a text file of absolute *.ytd.xml paths, one per line.
	// Each entry's PixelFolder is DERIVED - the extractor writes the pixels to a sibling
	// "<stem>/" folder beside the XML (and resolve carries that sidecar with the winning copy),
	// so the pair is self-describing and no per-file pixel argument exists to get wrong.
	// Assets land in <DestFolder>/<TxdName>/ via the same path as ImportYtd. A txd whose content
	// folder already exists on disk is SKIPPED unless Mode is "FORCE" (re-import in place).
	// Progress goes to the log every 100. Returns JSON:
	// {ok, imported, texturesImported, texturesDeclared, skipped, failed, invalidNames,
	// missingPixels, usageDefaulted, usageUnknown, itemsWithoutName,
	// failedFiles:[...first 30]}. It used to parse ONE field out of the unit verdict
	// ("imported") and drop invalidNames and missingPixels - the two counters added
	// specifically so the 2026-07-30 silent texture loss could not recur.
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
	// Filter: "HD" (default) or "ALL" lod levels.
	// Mode: "FORCE" RE-IMPORTS meshes that already exist. ⭐ The only refresh path the yft/ydd lanes
	// have - they are reachable only through this tool, so without it a corpus that gains data can
	// refresh only its ydr meshes and the project silently becomes a MIX of two vintages (07-30).
	// Empty Mode = previous behaviour (skip existing); old 5-argument calls keep working because
	// FRudeInvoke does no arity check. "+FORCE" on Filter is still accepted as an alias, so a
	// deliberate FORCE can never degrade into a silent no-op. Textures remain a separate pass
	// until native BC decode lands. Returns JSON: {ok, ymapsMatched, ymapsParsed,
	// ymapsUnreadable, ymapsWithoutEntitiesNode, ymapsWithEntities, ymaps, entities,
	// entitiesSkipped, resolved, meshesImported, meshesSkipped, meshesFailed,
	// meshesMissingFromCorpus, <every per-mesh counter ImportYdr reports, summed>,
	// manifest, spawn:{...ImportScene stats}}.
	// ⛔ 2026-08-04: "ymaps" used to be the GLOB MATCH count and is now the number of ymaps
	// that CONTRIBUTED a scene - measured, 219 of 1,500 resolved ymaps (14.6%) carry an empty
	// <entities> element and inflated the old figure. ymapsMatched keeps the old number.
	// ok is COMPUTED: it forwards the nested spawn's ok (a "no editor world" spawn used to be
	// reported as a successful import) and fails on an unreadable ymap or a run that produced
	// no scene at all. The per-mesh counters are new here too: this lane used to reduce each
	// unit verdict to a single ok:true test and discard the rest.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Build a whole area of the map in your open level - brings in the models it needs and places them. WARNING: this REPLACES any area you loaded before."))
	static FString ImportMapArea(const FString& CorpusRoot, const FString& YmapPrefix,
	                             const FString& DestMeshFolder, const FString& Filter,
	                             const FString& Mode);

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
	                          const FString& Filter, const FString& Mode);

	// THE INTERIOR IMPORTER (v1) - the consumer for the MLO data the extractor emits
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
	// portals, portalRooms, entitySets, entities, entitiesMissingTransform, spawned, proxies,
	// unresolvedArchetypes, lights, lightsSkipped[, lightProblem], otherExtensions,
	// badAttachedRefs, unroomedEntities, meshesImported, meshesSkipped, meshesFailed,
	// meshesMissingFromCorpus, <every per-mesh counter ImportYdr reports, summed>}.
	// ⛔ 2026-08-04: "entities" is the SLOT count and stays so, because rooms and portals index
	// into it by ordinal - a malformed record now keeps its slot (bValid=false) instead of being
	// dropped, which used to re-base every attachedObjects index after it and silently attach the
	// wrong props to the wrong rooms with badAttachedRefs still reading 0. The per-mesh counters
	// are new: this lane and ImportMapArea are the only consumers of the yft/ydd import paths and
	// both used to discard the whole unit verdict.
	// 2026-09-06 (maintainer lane `mlo_export`): EVERY entity - room entities and entity-set entities - is now
	// its OWN ACTOR under its room / set actor, carrying a URudeMloEntityComponent (interior, set, ordinal,
	// the entity's raw <Item> slice, the source transform). The ISM path is retired: an instance had no
	// identity, so nothing could be moved and written back. Cost = one actor per entity (v_franklinshouse:
	// 157 room + 133 set entities). The root carries RUDE_MLO_Ytyp:<asset> + RUDE_MLO_YtypFile:<path>; room
	// actors add RUDE_MLO_RoomIndex:<i>; entity actors add RUDE_MLO_Entity. The verdict adds entityActors /
	// setEntityActors / rawSetMismatch (a set whose raw slices disagree with the parse is skipped and
	// counted; ok goes false), and the import now REFUSES a ytyp whose bytes do not cut into one slice per
	// parsed entity (never measured on the corpus: 539/539 blocks cut clean). Export: ExportMloYtyp.
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
	// use the cm->m Y-mirror and gta_quat = (x, -y, z, w) - the SAME involution the IMPORT
	// lane uses, because a ymap <rotation> stores the entity's INVERSE orientation (proven
	// against Rockstar's own <entitiesExtents>, 2026-08-03). The previous "(-x, y, -z, w),
	// bench-proven" shipped every authored entity facing the wrong way.
	// Returns JSON: {ok, ymapPath, entities}.
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
	// clean-room. The RSC7 container (pgDictionary<grcTexture> system segment +
	// page-aligned graphics segment + segment flags + raw deflate) was reversed from
	// our own reference diff pair and byte-verified (tools/write_ytd.py; ENGINEERING_LOG
	// "RSC7 binary container"). This is P5 step 1 - the texture writer needs no third-party exporter.
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

	// Every texture a mesh's materials reference (Diffuse / Normal / Specular parameters) into one
	// .ytd, downscaled to MaxDim - the LOD texture dictionary for a MakeLodArchetype output.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save all the textures a model uses as one GTA V texture file, optionally shrunk for a distant version.", RudeAudience="agent"))
	static FString ExportMeshTextures(const FString& AssetPath, const FString& OutYtdPath, const FString& MaxDim);

	// Export a UStaticMesh directly to a binary FiveM .ydr (RSC7 v165) - CLEAN-ROOM,
	// clean-room. P5 step 3, the last third-party-exporter dependency, retired. Emits a gtaDrawable:
	// GTAV1 vertex buffers (Pos/Normal/Colour0/UV, 36B stride) + u16 index buffers per
	// polygon group, ShaderGroup with the normal_spec/spec parameter template (external
	// -ytd texture stubs resolved by name), and the EMBEDDED phBoundComposite collision
	// (same serialization as ExportYbnBinary - whole-mesh GeometryBVH). All-in-system
	// (gfx=0), page-aware layout. Struct map: docs/ENGINEERING_LOG "ydr binary format".
	// AssetPath: content path of the StaticMesh. OutYdrPath: absolute *.ydr path.
	// ⛔ DECLARED GAP (#40, counted 2026-08-05): this writer builds the embedded bound from the
	// RENDER MESH, unconditionally - it never reads UBodySetup, so collisionFromRenderMesh is
	// always 1, and boundsIgnored counts UE collision primitives (box/sphere/capsule/convex) the
	// user authored and this export threw away. Neither gates ok; they exist so a run cannot read
	// as complete while substituting. Honouring AggGeom here is registered as expansion.
	// Returns JSON: {ok, ydrPath, geometries, vertices, triangles, bytes, sysFlags,
	// collisionFromRenderMesh, boundsIgnored}.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save a Static Mesh as a finished GTA V model the game loads directly, with its collision included."))
	static FString ExportYdrBinary(const FString& AssetPath, const FString& OutYdrPath, const FString& Options);

	// Batch ExportYdrBinary. AssetFolder = a content folder walked recursively, OR a text file of
	// content paths one per line. Filter = optional case-insensitive substring the asset NAME must
	// contain. Each mesh lands at <OutDir>/<AssetName>.ydr through the SAME path as the single-asset
	// tool, so conventions cannot drift between one prop and a district.
	// ⭐ Exists because export was per-asset while import was per-AREA: the continuity principle
	// (BENCHMARK_ADDON_CITY §5) says the difference between a trash can and a district must be batch
	// size, not a different workflow.
	// Returns JSON: {ok, considered, exported, failed, bytes, collisionFromRenderMesh,
	// boundsIgnored, outDir, failedAssets:[...first 30]} - the two collision fields are the unit's
	// declared gap (#40) summed, so a whole-district export cannot read as complete while every
	// asset in it shipped collision re-derived from its render mesh.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save many Static Meshes out as GTA V models at once, into one folder.", RudeAudience="agent"))
	static FString ExportYdrBinaryBatch(const FString& AssetFolder, const FString& OutDir,
	                                    const FString& Filter);

	// Parse a BINARY FiveM/GTA V .ydr (RSC7 v165) and report its whole drawable graph as JSON.
	// This is the READ side's foundation: RUDE can write binary but until now could only READ
	// the XML interchange form, so real game binaries (e.g. from an extracted filebase) could not
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
	// CLEAN-ROOM. P5 step 2. Emits a phBoundComposite wrapping one
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
