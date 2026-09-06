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
	// PixelFolder: folder of decoded PNGs matching the entry names (BC-decode
	// happens offline until native decode lands).
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

	// ---- wp10 configs: timecycle modifiers (scratchpad/wp10/configs/LAWS.md) ----
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

	// ---- vehicle paths (ynd) - WP10 paths lane; laws in scratchpad/wp10/paths/LAWS.md ----------
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
	// scratchpad/wp10/peds/LAWS.md (a_m_m_business_01: 106 bones, 8 drawables, 46 textures, 4 components).
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
	// USkeleton. Laws measured over 5 dictionaries / 30 animations (scratchpad/wp10/anims/LAWS.md):
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
	// burrito 2026-09-06 (scratchpad/wp10/vehicles/LAWS.md): doors/bonnet/boot are skinned parts of the
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
	// Laws + denominators: scratchpad/wp11/ydd_writer/LAWS.md. In-game load: NOT yet verified (Matt's test).
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save clothing pieces (skinned meshes on a ped skeleton) as a finished GTA V clothing file the game loads directly."))
	static FString ExportYddBinary(const FString& SkeletalMeshAssetPaths, const FString& DrawableNames,
	                               const FString& OutYddPath, const FString& Options);

	// Parse a BINARY .ydd and report it as JSON: entries (name, hash, ascending), per entry shaders / models /
	// geometries / vertices / triangles / declarations, skin facts (vertices whose 4 weight bytes sum to 255,
	// max blend index, indices inside the bone-id table, bone-id table size and identity), skeleton / bound
	// presence, and the single-ownership audit (advisory on game files - an embedded texdict may share).
	// Reads untrusted files: every access bounds-checked; malformed input returns ok:false.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Inspect a GTA V clothing file and report its internals as raw JSON, without importing.", RudeAudience="agent"))
	static FString ProbeYddBinary(const FString& BinPath);

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
	// Each entry's PixelFolder is DERIVED - QUARRY writes the decoded pixels to a sibling
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

	// Every texture a mesh's materials reference (Diffuse / Normal / Specular parameters) into one
	// .ytd, downscaled to MaxDim - the LOD texture dictionary for a MakeLodArchetype output.
	UFUNCTION(BlueprintCallable, Category = "RUDE", meta = (AICallable, RudeHelp="Save all the textures a model uses as one GTA V texture file, optionally shrunk for a distant version.", RudeAudience="agent"))
	static FString ExportMeshTextures(const FString& AssetPath, const FString& OutYtdPath, const FString& MaxDim);

	// Export a UStaticMesh directly to a binary FiveM .ydr (RSC7 v165) - CLEAN-ROOM,
	// no CodeWalker. P5 step 3, the LAST CodeWalker dependency. Emits a gtaDrawable:
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
