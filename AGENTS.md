# RUDE — Agent Onboarding

For AI agents working **with** RUDE (driving the tools) or **on** RUDE (writing its C++).
Humans welcome too.

> **`Source/RudeEditor/Private/RudeToolset.h` is THE tool surface.** This file is a map of it.
> If the two ever disagree, the header wins and this file is the bug — regenerate the table from
> `-list` (see below), never from memory. A hand-maintained tool list is exactly the thing that
> rotted last time: an earlier version of this document described 14 of 18 tools.

---

## 0. What RUDE is, in one paragraph

An **editor-only UE 5.8 plugin** (module `RudeEditor`, loading phase `Default`). It imports RAGE
(GTA V) map assets out of an XML **interchange folder** into real UE assets — Static Meshes,
Textures, Material Instances, placed instanced actors, MLO interiors — and writes FiveM-loadable
output back out, including **binary `.ydr` / `.ytd` / `.ybn` produced by RUDE's own clean-room RSC7
writers** (validated in live FiveM). No game data ships with the plugin, and none may ever enter
this repository. RUDE never opens or decrypts a game archive: it consumes a **corpus** - the folder ROUT
(the companion exporter) writes from the user's own installation. Since 2026-09-05 RUDE reads the
corpus through its LEDGERS (`_FILEBASE.json` + `_PROVENANCE.jsonl`, module `RudeIntake`,
`FRudeCorpus`), never by walking folders: `CorpusRoot` is the filebase root (the folder holding
`00_base/`), and every "where is this file" question resolves to the copy the game loads across
base/update/DLC slots. Texture pixels come from the corpus's DDS sidecars (`FRudeDds`).

---

## 1. The ONE core: `FRudeInvoke`

`RudeInvoke.{h,cpp}` is the single reflective call path into `URudeToolset`. Every surface —
Slate panel, CLI commandlet, MCP toolset, console command — marshals through it. Nothing enumerates
tools by hand anywhere in this codebase.

**The driveability contract (hard):**

```
static FString ToolName(const FString& A, const FString& B, ...);   // ALL params FString, FString return
```

`FRudeInvoke::IsAllStrings` enforces it. A tool with any non-`FString` parameter, or a non-`FString`
return, **is unreachable from all four surfaces** — the panel greys it out with the reason, and
`Call()` refuses rather than invoking a zeroed argument frame (which would look like a real run and
report success). If you need structured input, take a path to a JSON file (`ExportYmap`) or a
delimited spec string (`ExportYtdBinary`), never a struct.

**Why nothing may bypass it:** the moment a surface calls `URudeToolset::X()` directly, that surface
owns a copy of the argument order, the empty-argument policy, the verdict polarity and the failure
text — and they drift. `FRudeInvoke` also fixes the two subtle behaviours every surface must share:

- **Missing values pass as empty strings**, matching the MCP behaviour, so a 3-parameter tool driven
  with 2 arguments behaves identically everywhere.
- **Verdict polarity:** only an explicit `"ok":false` is a failure. Absence of a JSON envelope is
  not failure (`Ping` returns a plain sentence). The CLI exit code and the panel's status colour
  both come from `FRudeInvoke::ReportedFailure`.

Audience filtering is also here, and it is **tag-driven**: `CollectTools(bHumanOnly)` drops tools
marked `RudeAudience="agent"`. Never filter by a name list in a UI — the next agent-only tool
someone adds would silently appear on a human's menu.

---

## 2. The four surfaces

| Surface | How you drive it | Where the result lands |
|---|---|---|
| **Slate panel** (`SRudeToolPanel`) | `Window > Tools > RUDE`, or the `RUDE.Panel` console command (so it can be opened from `-ExecCmds`, a startup script, or CI — a tab spawner alone can only be opened by a human clicking menus) | The panel's read-only JSON box **and** `LogRudePanel`. Slate automation cannot read text inside a textbox, so the log line is the only machine-readable trace |
| **CLI** (`URudeCommandlet`) | `UnrealEditor-Cmd.exe <YourProject>.uproject -run=/Script/RudeEditor.RudeCommandlet -list` / `-tool=<Name>` / **`-script=<file>`** (one JSON object per line, `{"tool":"Name","args":{"Param":"value"}}`, run in order inside ONE process - the level survives between steps, so import -> edit -> export chains headless) | stdout via `LogRudeCLI`, plus an **exit code that is the tool's own verdict** (non-zero only on `"ok":false`). **The CLI saves dirty content at exit** and prints `save -> {saved, saveFailed}`; `-nosave` opts out. A headless run that does not show `saved` did not persist anything (measured 2026-09-05: two clean gates evaporated). Add `-NoLiveCoding`: any engine `Error:` line forces exit 1 even on `ok:true` |
| **MCP toolset** | `URudeToolset` is registered into the engine's `ToolsetRegistry` at module startup (retried on `PostEngineInit` if the registry is not up yet) and appears as `RudeEditor.RudeToolset` on any MCP connection to the editor. Requires the engine's **ToolsetRegistry** + **ModelContextProtocol** plugins | The tool's JSON string. Each tool's `UFUNCTION` doc comment becomes its MCP description — write real ones |
| **Console** `RUDE.Run` | `RUDE.Run <Tool> [arg]...` — from the editor console or, more usefully, `-ExecCmds="RUDE.Run ImportArea Downtown_Los_Santos ..."` at launch. No MCP session and no human required | `LogRudePanel`, which is what a headless driver reads anyway |

Two argument-passing traps, both measured:

- **`-ExecCmds` splits its own command list on commas.** A tool parameter that is itself a comma
  list (`ImportMapArea`'s prefix list, `ExportYtdBinary`'s specs) cannot be passed raw there.
  `CaptureView` therefore accepts `;` as well as `,` in its `CamSpec`.
- **Use the fully-qualified `-run=/Script/RudeEditor.RudeCommandlet`** and pass it in an argument
  **array**. The short form does not resolve, and a shell that splits the argument on the `.` makes
  UE die inside `FModuleManager::AddModule()` on an empty module name — an illegible failure.
  The CLI also accepts `-ParamName=value`, which is order-independent and clearer in scripts;
  named wins over positional when both are given.

---

## 3. The tool surface

> ⛔ **`Source/RudeEditor/Private/RudeToolset.h` IS the tool surface — this table is a summary and
> WILL lag.** It has now rotted twice: it said "24 tools" while the header declared 30, and its own
> stale-note then named only three missing rows while SIX were absent (`SetWorldHour` ·
> `FixLevelRefs` · `ExportYdrBinaryBatch` · `ImportScenarioRegion` · `PackAreaLevelInstance` ·
> `ImportVehicle`). A count in prose drifts every time a tool lands; the header cannot. **Read the
> header, and treat any disagreement as this file being stale.** (2026-07-31: all six rows added
> below — the table matches `-list` again, until it doesn't.)

Sorted alphabetically, which is exactly the order `FRudeInvoke::CollectTools` produces, so this
table **diffs line-for-line against `-list`**. That is the intended way to check it.

`Audience`: **human** = shown in the panel (22 of them); **agent** = plumbing, tagged
`RudeAudience="agent"`, hidden from the panel but present on the CLI and MCP (8 of them).

| Tool | Parameters (in order) | What it does | Audience |
|---|---|---|---|
| `BuildArchetypePalette` | `CorpusRoot, ManifestPath, DestFolder, MeshFolder` | One `URudeArchetype` asset per archetype the manifest's placements refer to (empty manifest = every archetype): 13 edit-native + 2 as-spelled fields, time mask, extensions/MLO subtrees verbatim, mesh link, provenance, the item's own XML as a raw slice of the source bytes. Downtown: 3,200 assets from 73 ytyps. Re-running refills existing assets (loaded first, then filled) | human |
| `BuildDistrictLevel` | `LevelPath, ManifestPath, MeshFolder, Filter` | The district as a World Partition level: one Runtime Data Layer per ymap (toggle a ymap like a layer), one actor per entity with its `URudeEntityComponent` on its ymap's layer, saved headless (map + `<Level>_Layers/DL_<ymap>` assets + external actors). Downtown: 148 layers, 14,248 actors; export from it 148/148 byte-identical | human |
| `CaptureView` | `CamSpec, OutPng, ViewMode, SettleSeconds` | Aim the perspective viewport (`"x,y,z,pitch,yaw"` in UE cm/degrees; `;` also accepted) and write a PNG — the agent-vision primitive. Blocks on `FinishAllCompilation` first, then the shot lands on the NEXT draw: poll for the file. `ViewMode` UNLIT answers "did the textures bind?" (a Lit shot multiplies albedo by scene lighting); `SettleSeconds` = minimum quiet time before the deferred shot fires (default 25) | agent |
| `CreateFilebase` | `FilebaseRoot, GameRoot, Options` | Seed the **filebase**: a load-order-aware folder tree the user exports their own game files into (`00_base` < `10_update` < `20_dlc/NNN_name`, higher wins). Enumerates the install's directory names only — **no archive is opened or decrypted**. `Options` = `CORE` (default) or `ALL` | human |
| `ExportLevelYmaps` | `OutDir, YmapFilter, CorpusRoot, NewEntitiesYmap` | The level readback: every actor carrying a `URudeEntityComponent` goes back to its source ymap as a FiveM resource (`<OutDir>/stream/<ymap>.ymap` + `fxmanifest.lua`; Legacy loads the XML form). The source file's bytes are SPLICED - only the `<entities>` block (and an extents line that had to grow) is replaced; an untouched entity re-emits its own XML verbatim, an edited one is rebuilt from its fields. Verdict: kept/edited/added/removed, `editsNotRebuilt` (an edited MLO instance goes out as read), `extentsGrown`, refusals (deletions in a file with LOD lineage). Gate 2026-09-05: 148/148 untouched files byte-identical | human |
| `ExportPaletteYtyps` | `OutDir, PaletteFolder, YtypFilter, CorpusRoot` | The palette back to ytyp files (FiveM resource): the source `<archetypes>` block is spliced - untouched archetypes verbatim, an edited base/time archetype rebuilt in the file's field order, unlisted archetypes as read (nothing is dropped; Wave 1 has no delete), an edited MLO archetype as read (counted `notRebuilt`). Gate: 73/73 byte-identical | human |
| `ExportTexture` | `TexturePath, OutPngPath` | Write a `UTexture2D`'s source pixels (BGRA8) out as a PNG | human |
| `ExportYbn` | `AssetPath, OutXmlPath` | Mesh collision → standalone `.ybn` **XML**. Note: prop collision actually travels *inside* the ydr's embedded `<Bounds>`; this is for shared/world collision work | human |
| `ExportYbnBinary` | `AssetPath, OutYbnPath, WorldOffset` | Mesh collision → **binary** `.ybn` (RSC7 v43): phBoundComposite → GeometryBVH with quantized verts, stackless BVH and the mandatory subtree table, page-aware layout. `WorldOffset` = `"x,y,z"` GTA metres for absolute world placement (empty = mesh-local). Validated in game | human |
| `ExportYdr` | `AssetPath, OutXmlPath` | Static Mesh → `.ydr` **XML** (the editable interchange form): inverse transform, shader presets recovered from slot names, embedded collision `<Bounds>` | human |
| `ExportYdrBinary` | `AssetPath, OutYdrPath, Options` | Static Mesh → **binary** `.ydr` (RSC7 v165): GTAV1 vertex layout, `normal_spec` parameter template with name-hash binding, embedded phBoundComposite collision, page-plan-safe layout. Drawable name = output filename. Validated in game | human |
| `ExportYdrBinaryBatch` | `AssetFolder, OutDir, Filter` | Batch `ExportYdrBinary`: walk a content folder recursively (or a list file of content paths), optional case-insensitive name-substring `Filter`; each mesh lands at `<OutDir>/<AssetName>.ydr` through the same code path as the single-asset tool | agent |
| `ExportYmap` | `EntitiesJsonPath, MapName, OutDir` | Placement JSON (UE space) → a complete FiveM resource: `stream/<name>.ymap` + `fxmanifest.lua` carrying the required `this_is_a_map` | human |
| `ExportYtdBinary` | `TextureSpecs, OutYtdPath, MaxDim` | Textures → **binary** `.ytd` (RSC7 v13) with full mip chains stopping at 4x4. `TextureSpecs` = comma-joined `"ContentPath;RageName[;Usage[;Format]]"`; `Format` = `AUTO\|DXT1\|DXT5\|ATI2\|RAW` (AUTO: NORMAL→ATI2, meaningful alpha→DXT5, else DXT1). `MaxDim` box-downscales oversized sources (`0`/empty = no cap). Validated in game | human |
| `ExportYtyp` | `YdrSpecs, YtypName, OutYtypPath` | Emit a CMapTypes `.ytyp` — one `CBaseArchetypeDef` per drawable — with the in-game-proven collision model (embedded bounds gate flag bit 17 **and** a non-empty `physicsDictionary`). `YdrSpecs` = comma-separated `"absPath.ydr.xml[;txd[;physDict]]"` | human |
| `FixLevelRefs` | `Mode` | Drop streaming-level entries whose package no longer exists on disk, then save the map. `Mode="APPLY"` writes; anything else reports what it WOULD remove and changes nothing | agent |
| `ImportArea` | `AreaName, CatalogPath, CorpusRoot, DestMeshFolder, Filter, Mode` | Import a district by its **human name** ("Downtown Los Santos") using an area catalog JSON, then delegate to `ImportMapArea` — one code path, no drift. Empty `AreaName` lists every alias. Underscores read as spaces (console arguments) | human |
| `ImportMapArea` | `CorpusRoot, YmapPrefix, DestMeshFolder, Filter, Mode` | **The one-call thread-pull:** archetype index from `<CorpusRoot>/ytyp` → parse `<CorpusRoot>/ymap/<prefix>*.xml` → import every referenced drawable (ydr / yft / ydd entry) → spawn via `ImportScene`. `YmapPrefix` is a **comma-separated list**; exact basenames ride as `<name>.ymap`. `Filter` = `HD` (default) or `ALL`. `Mode="FORCE"` re-imports meshes that already exist — the only refresh path the yft/ydd lanes have; empty = skip existing. ⚠ REPLACES the previously spawned area | human |
| `ImportMlo` | `CorpusRoot, MloArchetypeName, DestMeshFolder, Filter` | Build an MLO **interior**: locate the `CMloArchetypeDef` (name matching is hash-tolerant both ways), import every mesh its entities reference, spawn each room's entities at MLO-local transforms under one root actor **at the world origin** (v1), and map `CLightAttrDef`s to point/spot lights. `Filter` = empty/`ALL`, or a comma-separated **room-name** list. Re-running replaces this archetype's actors by tag | human |
| `ImportScenarioRegion` | `CorpusRoot, RegionName, Filter` | Import one scenario region (GTA's ambient life) as editable actors — every scenario point becomes its own actor, chaining-graph routes drawn visibly | human |
| `ExportScenarioRegion` | `OutDir, RegionName, CorpusRoot` | The scenario readback: the point actors of one imported region go back to the region file as a FiveM resource (`<OutDir>/stream/<region>.ymt`, XML form + `fxmanifest.lua`). The source bytes are SPLICED - only the top-level `<Points>/<MyPoints>` block is replaced; an untouched point re-emits its own slice verbatim, a moved/edited one is rebuilt (heading text kept when unrotated). Refuses deletions (the accel grid indexes points by ordinal); counts `cellCrossings` (a point moved out of its 64 m cell leaves the verbatim grid stale - in-game test judges). Gate (`scratchpad/wp10/scenarios`): untouched downtown byte-identical, one nudge = one item | human |
| `MoveScenarioPoint` | `RegionName, PointIndex, DeltaCm` | Nudge one imported scenario point by x,y,z centimetres (optionally `,yawDeg`); identity = region + ordinal. The scriptable edit the scenario export gate uses | agent |
| `ImportScene` | `ManifestPath, MeshFolder, Filter, Mode` | Re-spawn a scene manifest into the open level **without re-importing anything**: one actor per ymap, one instanced-static-mesh component per unique drawable, proxy cubes for missing meshes, idempotent (clears its previous spawn). `Filter` = `HD` (default, i.e. HD/ORPHANHD lod levels) or `ALL` | human |
| `ImportVehicle` | `CorpusRoot, VehicleName, DestFolder` | Import a vehicle: the body drawable plus its wheel drawable placed at each wheel bone | human |
| `ImportVehicleComposite` | `CorpusRoot, VehicleName, DestFolder` | GDD Tier 2 vehicle composite: the body drawable with the `_hi` fragment as LOD0 and the base's High/Medium/Low/VeryLow as LOD1-4 (5 LODs when both exist), every fragment child as a component at its bone (bounds per child counted by type), wheels at the wheel bones (v1's code), a `URudeVehicle` DataAsset carrying the handling.meta / vehicles.meta / carvariations fields BY NAME as spelled (204 meta files searched; later slots win) and the livery list. Measured on blista/taxi/burrito (LAWS.md). Gate: blista 24 geos, 68 bones, 4 wheels, 21 children, 56 handling fields; burrito 4 liveries. ⚠ No vehicle texture sidecars in the 2026-09-04 corpus (`vehicles.rpf` has 0 pixel folders) → every vehicle imports untextured until ROUT exports them; a built mesh is kept regardless (a counted condition, not a failure) |
| `SetVehicleLivery` | `ActorLabel, LiveryIndex` | Switch the body's livery texture (the `vehicle_paint3` DiffuseSampler2 slot, or the livery sampler the vehicle's shader declares) on the imported composite |
| `SandboxSetup` | `LevelPath, Location` | THE PIE SANDBOX (GDD 1b): places a ground-snapped PlayerStart / `RUDE_SANDBOX_SPAWN`, sets the level's GameMode override to `ARudeSandboxGameMode` (DefaultPawn = `ARudeSandboxPawn`: third-person, WASD/mouse/Shift/Space bound in code, no project input config), adds the sky rig if missing, saves. Then Play: the `URudeNativeShim` world subsystem answers `Rude.Native <native> [args]` in the console — `EnableIpl`/`RequestIpl`/`RemoveIpl` (the `DL_<ymap>` Data Layer runtime state + an actor visibility sweep), `RequestCutscene`/`StartCutscene` (plays `LS_<cut>` from ImportCutscene), `SetClockTime`/`NetworkOverrideClockTime` (drives the RUDE_SKY sun + the RUDE_TIME masks with a running clock), `ActivateInteriorEntitySet`, `SetScenarioGroupEnabled`, `StartAmbientAgents N` (`ARudeScenarioAgent`s walking the imported scenario chains), `Log`. Every call lands in the shim's event log and on screen. Test steps: `scratchpad/wp10/sandbox/PIE_TEST.md`. ❓ never run in PIE yet (Matt's eyes) |
| `EmitNativeSnippet` | `Kind, Name` | "Every export ships its invocation": the FiveM Lua for the real native behind a shim call — ipl (RequestIpl/RemoveIpl), cutscene (RequestCutscene/HasCutsceneLoaded/StartCutscene), entityset (GetInteriorAtCoords/ActivateInteriorEntitySet/RefreshInterior), scenario (SetScenarioGroupEnabled), clock (NetworkOverrideClockTime/SetClockTime). Empty Kind lists the kinds |
| `ImportYddEntry` | `XmlPath, EntryName, DestFolder` | Import ONE named entry out of a `.ydd.xml` drawable dictionary. `EntryName` is matched case-insensitively **and by joaat hash both ways** (entries are usually `hash_XXXXXXXX`); the imported mesh takes `EntryName`, the archetype-facing identity. An unknown entry fails loudly, listing what *is* there | human |
| `ImportYdr` | `XmlPath, DestFolder` | One `.ydr.xml` → `UStaticMesh`. Material slots named `<shader_preset>__<geoIndex>`; Material Instances auto-created from the RUDE masters with **RenderBucket-driven routing** (bucket is RAGE's authoritative signal — preset names lie); textures bound by name from `/Game/RUDE/Textures`; complex-as-simple collision so you can walk it in PIE. Reimport is edit-in-place | human |
| `ImportYdrBatch` | `ListPath, DestFolder, Mode` | Same lane over a text file of absolute `.ydr.xml` paths, one per line. Skip-if-exists (idempotent re-runs) unless `Mode="FORCE"`, which reimports in place — the re-bind pass after textures land | human ⚠ ydr-only by NAMING: the mesh name is the basename minus `.xml`, so a `.yft.xml` path yields a dotted name and is refused (165/165 on 2026-09-05); re-import fragments through `ImportMapArea … Mode=FORCE` |
| `ImportYtd` | `XmlPath, PixelFolder, DestFolder` | `.ytd.xml` + decoded PNGs → `UTexture2D`s with Usage-driven semantics (NORMAL → `TC_Normalmap` + sRGB off, SPECULAR → sRGB off, DIFFUSE → sRGB on). Assets land in `<DestFolder>/<TxdName>/` | human |
| `ImportYtdBatch` | `ListPath, DestFolder, Mode` | Same over a list file. **The pixel folder is derived, not passed**: the decoded pixels live in a sibling `<stem>/` folder beside each XML, so the pair is self-describing. Skip-if-exists unless `Mode="FORCE"` | human |
| `InspectMesh` | `AssetPath` | The numbers behind an imported mesh: render bounds, LOD0 vertex extents, triangle count, collision primitive counts and the farthest primitive. The instrument that measured a 7 m tarp imported as a 260 m sheet (2026-09-05) | agent |
| `InspectCollision` | `AssetPath` | What collision an imported model actually ended up with (simple primitives, complex mesh, physical materials), as numbers | agent |
| `LodLineage` | `ActorLabel` | The LOD chain an entity hands over along: up through its `LodParent` links (actor, archetype, ymap, source ordinal, level, lodDist, childLodDist, numChildren) and its children. `ActorLabel` is the actor label (= archetype name, not unique), `ymap:index` (the entity's source ordinal, unique) or `ymap:new[:archetype]` (an entity authored in RUDE, no ordinal yet). Links are resolved at build by the rule measured on downtown (ENGINEERING_LOG law 24: the parent ymap first, then the entity's own ymap, exactly one level coarser; 2,813/2,813 unique) |
| `SetLodParent` | `ActorLabel, ParentLabel` | Re-parent an entity in the LOD chain; empty `ParentLabel` orphans it. Refuses what the ymap format cannot express: a parent outside this ymap and its parent ymap, a parent not exactly one level coarser, a parent whose children are not all in the level (`bLodPartial`: its numChildren is verbatim). Previews the fields the export will derive. `lodDist`/`childLodDist` are authored (law 26) and are never touched |
| `LodAudit` | – | Derived-vs-stored over every entity (parentIndex, numChildren, HD/ORPHANHD from the links); an authored or link-edited entity counts as `pendingAtExport`, not a diff. On an untouched level this is 0 diffs - measured 14,248/14,248 on downtown 2026-09-06, the proof the derivation reproduces the game's data. `ExportLevelYmaps` runs the same derivation before keying, so an unchanged lineage exports verbatim and a re-parent rebuilds exactly the entities whose fields moved (gate: 3 entities across 2 files; restore = 148/148 byte-identical) |
| `MakeLodArchetype` | `ActorLabel, NewArchetypeName, TrianglePercent, LodDist, PaletteFolder` | GDD Wave 2 "swap a building, press rebuild": regenerate an HD entity's LOD parent from the HD mesh itself. The mesh DESCRIPTION is reduced (the ydr writer reads it; a render-only reduction exported the full mesh - measured) to `TrianglePercent` (default 30) into a new drawable asset beside the HD mesh; a new palette archetype wraps it (bounds from the mesh, `LodDist` default = the old parent's, textures = the HD's dictionary) with `SourceIndex -1` so `ExportPaletteYtyps` APPENDS it to the HD archetype's ytyp; the existing LOD parent entity is re-pointed at it (ordinal/parentIndex/numChildren untouched) or, for an orphan, a LOD entity is placed in the ymap the format allows and linked. `NewArchetypeName` defaults to `<hd>_rlod` (a RUDE default, not the game's convention). Then `ExportYdrBinary(mesh)` for the drawable. Then `ExportYdrBinary(mesh, <name>.ydr, NOBOUND)` (a LOD drawable carries no collision; a null bound pointer is a RAW zero - `PPTR(0)` would encode a live pointer into the header) and `ExportMeshTextures(mesh, <name>.ytd, 256)`. Gate 2026-09-06: 2,520 -> 756 triangles, probe clean with `hasEmbeddedBound:false`, 1 ymap entity changed, 1 ytyp item appended, everything else byte-identical |
| `ExportMeshTextures` | `AssetPath, OutYtdPath, MaxDim` | Every texture a mesh's material instances reference (the Diffuse / Normal / Specular parameters the ydr writer names as samplers) into ONE .ytd through `ExportYtdBinary`, downscaled to `MaxDim`. The LOD texture dictionary: `MakeLodArchetype` names the new archetype's txd after itself and this writes it (gate: 3 textures, 159 KB at 256) |
| `RebuildLodChunk` | `ParentLabel, NewArchetypeName, TrianglePercent, AtlasSize, PaletteFolder` | GDD Wave 2 "chunk rebuild": merge an SLOD parent's children (the next-finer shells placed under it) into ONE baked-atlas drawable (the engine's merge-with-material-baking, normal map on, `AtlasSize` default 1024), reduce it to `TrianglePercent` (default 50), re-instance the baked atlases onto `M_RUDE_Opaque` as `<name>_a` / `<name>_n`, wrap it in a palette archetype (own txd, the old parent's lodDist, bounds from the mesh) and re-point the parent entity at it, moving it to the merged pivot. Lineage untouched. Refuses HD entities, childless parents and `RUDE_LOD_PARTIAL` parents. ⛔ Material baking RENDERS: the CLI must run with `-AllowCommandletRendering` or the merge dies with an access violation inside the engine (measured 2026-09-06). Measured against the game: `dt1_lod_03_04_05_11` (4 blocks, 10 children, 20,398 tris) -> 10,126 tris (the game's chunk: 7,984), 1024 atlas pair; 1 ymap entity changes (archetype + position), 1 ytyp item appended, everything else byte-identical; ydr no bound, probe clean |
| `RebakeLodLights` | `OutDir, Name, YmapFilter` | GDD Wave 2 "lodlight rebake": every entity light extension in the level (or the filter's ymaps) becomes one distant light in a `<Name>_distlodlights.ymap` (parent: positions + RGBI) / `<Name>_lodlights.ymap` (child: direction / falloff / exponent / timeAndStateFlags / hash / cone bytes / corona) pair under `OutDir/stream`, in the game's own spelling (XML; FiveM loads XML ymaps). Packings measured on downtown (ENGINEERING_LOG law 32): position = entity transform on `posn`; RGBI = round(I·255/50)<<24 | rgb; timeAndStateFlags = timeFlags | (point ? 4 : 8)<<24; falloff/exponent verbatim; cone bytes = trunc(angle·127.5/90) ≤ 127; direction rotated. ⛔ `hash`: formula unknown (34 candidates refuted, law 33) → unique atDataHash(guid, index); the in-game test judges. Gate (`scratchpad/wp9/compare_lodlights.py`): 832 rebaked vs the game's — 828 at the same position, rgb 807, timeFlags 767, falloff 709, exponent 750, coneInner 795, direction 703 |
| `SetLightField` | `ActorLabel, LightIndex, Field, Value` | Edit one light of a placed entity from the CLI (Matt edits the `RudeLight<i>` component in Details). Every `CLightAttrDef` in an entity's carried `<extensions>` is a real UE light on the actor at build/import (spot for lightType 2, point otherwise; RAGE intensity x100 -> candelas, falloff m -> attenuation, exponent clamped 2..16, cones as given, direction -> rotation; tags `RUDE_LIGHT:<i>` + a key of the mirrored fields). At export a light whose mirrored fields changed rewrites ONLY those fields inside the entity's extension instance (UE holds some values approximately: a 90 deg cone clamps to 89, so untouched fields keep the game's spelling) and the entity keys as edited. Fields: intensity, colour r,g,b, falloff, falloffExponent, coneInner, coneOuter, position x,y,z (RAGE metres, entity-local). Gate 2026-09-06: intensity 8 -> 12 on `dt1_02_strm_0:150` changes exactly that one line in one file; restore = 148/148 byte-identical |
| `ImportPaths` | `CorpusRoot, CellName, Filter` | One square of the road/footpath network (`nodesNNNN.ynd`, 512 m cells from −8192, cell = row·32+col; downtown = nodes464) as one actor per node carrying a `URudePathNodeComponent` (every node field edit-native + provenance) with the reciprocal links drawn as splines. Measured over 5 cells: field order uniform 5,988/5,988 nodes, 12,776 links, positions on a ¼ m / 1⁄32 m grid, in-cell links 100 % reciprocal |
| `MovePathNode` | `CellName, NodeID, DeltaCm` | Nudge one node (snapped to the ¼ m / 1⁄32 m grid the file uses) |
| `ExportPaths` | `OutDir, CellName, CorpusRoot` | The cell back out as `stream/<cell>.ynd` (XML): untouched nodes verbatim from raw slices, moved nodes rebuilt; no add/delete; stale link lengths and y-order breaks counted. Gate: nodes464 untouched byte-identical, one move = one node. ❓ FiveM's acceptance of an XML ynd is untested |
| `ImportPed` | `CorpusRoot, PedName, DestFolder` | A ped: USkeleton (yft bones, 106 for a_m_m_business_01), one USkeletalMesh per component drawable with real skin weights (`BlendWeights` Σ=255 on 27,808/27,808 measured vertices; indices are skeleton positions via `<BoneIDs>`), materials via the static lane, a `URudePedOutfit` DataAsset (the variation matrix from the ped's CPedVariationInfo + a bone name→tag map) and a preview actor wearing drawable 0. Gate: 8 drawables, 21,607 skinned vertices, 0 unweighted, 12 sections. ⚠ Textures: the corpus has no pixel sidecars for cdimages ytds, so texture assets do not import yet (names resolve 24/24) |
| `ImportClipDictionary` | `CorpusRoot, YcdName, SkeletonAssetPath, DestFolder` | A ycd as one UAnimSequence per animation on the given skeleton (bones by name through the outfit's tag map, then by tag), frame rate (FrameCount−1)/Duration (30 fps 27/30 measured), sequences stitched on their shared boundary frame (stride = SequenceFrameLimit), quantised/indirect/cached-quaternion channels decoded, clip tags → notifies. Gate: 49/49 measured rows (frames, bones, first/last keys) |
| `ImportCutscene` | `CorpusRoot, CutName, DestFolder` | A `.cut` as a Level Sequence: spawnable camera with transform keys from the `<cut>-<k>.ycd` camera parts (tiled at the cameraCutList split times, 2/2 measured), a camera-cut track per camera-cut event, and a `URudeCutsceneEvents` DataAsset carrying every event / arg / object / concat row verbatim. Gate: ah_1_int 552 events, 39 cuts, parts tile 134.2 s exactly |
| `ImportTimecycles` | `CorpusRoot, DestFolder` | Every timecycle modifier (12 files, 2,974 modifiers, 325-word mod vocabulary) as a `URudeTimecycle` DataAsset (mods as name → value/weight + provenance; a later slot's copy wins) |
| `ExportTimecycles` | `OutDir, DestFolder, CorpusRoot` | The modifier files back out under `<OutDir>/<slot>/<file>` (CRLF-safe splice: untouched verbatim, edited rebuilt). Gate: 5/5 effective files byte-identical (7 shadowed copies are not written) |
| `ImportText` | `CorpusRoot, TableName, DestFolder` | A gxt2 text table (binary: `2TXG, count, sorted (hash, offset) pairs, 2TXG, size, NUL-terminated UTF-8`, verified on 600 files / 310,320 entries) as a UStringTable keyed by the 8-hex hash; `name@language` picks a language archive |
| `ExportText` | `StringTableAsset, OutGxt2Path` | The String Table back out as gxt2 in that exact layout. Gate: 3 round trips byte-identical incl. `global` (3.8 MB, 70,761 entries) |
| `BuildBlipCatalog` | `CorpusRoot, DestFolder` | The 954 `radar_*` blip names from minimap.gfx's export table + the 4 sprite sheets, as a `URudeBlipCatalog` DataAsset. ⚠ The corpus carries no pixels for the sheets (cdimages ytds have no sidecars) and no per-sprite UVs — names only until ROUT exports them |
| `ExportAwc` | `SoundWaveAssetPath, OutAwcPath, StreamName` | A USoundWave as a plaintext single-stream PCM16 awc in the game's own layout (stream word = chunkCount<<29 \| joaat, data/format/peak chunks; flags 0xFF01 — the game's plaintext-PCM class). Self-checks by re-parsing. Gate: 81,090 frames at 48 kHz, accepted and round-tripped byte-identical by ROUT's own writer |
| `ImportAwc` | `CorpusRoot, AwcName, DestFolder` | PCM tracks of a game awc as USoundWaves; ADPCM / encrypted / payload-absent tracks are counted refusals. ⚠ The 2026-09-04 corpus export ran the audio lane without payloads, so every corpus awc reports `payloadAbsent` until ROUT re-exports audio |
| `CatalogLane` | `CorpusRoot, Type, NameFilter, DestFolder` | The honest passthrough tier for yed / yld / yfd / ypdb / ynv / mrf / ypt: one `URudeCarriedAsset` per file (raw XML inline up to 8 MB, provenance, a one-line count summary). Gate: 28 ypdb files → 28 assets |
| `DebugDrawNavmesh` | `CorpusRoot, YnvName` | A ynv's polygons as persistent debug lines in the level. ⚠ Rough: the polygon index decode flags every polygon degenerate on the measured file (1,066 lines drawn from points/portals only) |
| `ExportScenarioRegion` | `OutDir, RegionName, CorpusRoot` | A scenario region back out as `stream/<region>.ymt` (XML): untouched points verbatim from raw slices, moved points rebuilt, deletions refused, 64 m accel-grid cell crossings counted. Measured: 144 regions / 109,198 points, one item shape everywhere. Gate: downtown 823 points byte-identical untouched; one nudge = one item |
| `MoveScenarioPoint` | `RegionName, PointIndex, DeltaCm` | Nudge one scenario point by its ordinal in the region file |
| `IngestExport` | `DumpFolder, SourceName, FilebaseRoot, Move` | File a **flat** export dump into the filebase, sorted by type into the right precedence slot, so nobody hand-sorts. `SourceName` = `base` / `update` / a DLC pack name (empty = the dump folder's own name). `Move` = `MOVE` (default) or `COPY` — ⚠ MOVE relocates recursively and replaces name collisions | human |
| `MoveRudeEntity` | `SourceYmap, SourceIndex, DeltaCm` | Nudge one placed entity by x,y,z centimetres (identity = source ymap + ordinal). The scriptable edit the export gate uses | agent |
| `NewLevel` | `Partitioned` | A fresh untitled level (`true` = World Partition) for the tools that follow in a `-script=` chain - e.g. build an interior with `ImportMlo`, then `SaveLevel` it as its own level asset | agent |
| `OpenLevel` | `LevelPath` | Load a saved level for the tools that follow in a `-script=` chain. For a World Partition level it holds a reference to every actor descriptor so the actors load and stay loaded (a WP level loads none by itself headless) | agent |
| `PickAt` | `CamSpec, U, V, Aspect` | What is under a pixel of a CaptureView frame: same CamSpec, U/V 0..1 across/down (HFOV 90, default aspect 2103:1230). A ray-versus-bounds test over every placed entity (no physics needed headless); names the first VISIBLE hit (actor, archetype, ymap, LOD level, mesh, master materials) and what the ray passed through. Rail: before the second hypothesis about a picture, ask the frame what it is | agent |
| `PlaceArchetype` | `PaletteFolder, ArchetypeName, LocationCm, RotationDeg, TargetYmap` | Author a NEW placement from the palette: an entity actor (mesh from the palette's link) at a UE location/rotation, destined for `TargetYmap` (a new ymap name, or an existing one to append to). Defaults the way the game's own new content does (ORPHANHD, PRI_REQUIRED, flags 1572864, lodDist from the archetype, guid hashed from ymap:archetype:position). Export with `ExportLevelYmaps NewEntitiesYmap=<same>`; a new ymap gets v1's in-game-proven header | human |
| `PlaceInterior` | `MloArchetypeName, LevelAsset` | A Level Instance of an interior level (built by `ImportMlo` in its own level and saved with `SaveLevel`) at every `CMloInstanceDef` of that archetype in the open level, ATTACHED to the placement's entity actor (its proxy cube hidden; the `URudeEntityComponent` stays the export's truth, so the interior follows the entity when moved) | human |
| `PackAreaLevelInstance` | `AreaName, ActorTag, Mode` | Pack an already-spawned area into a Level Instance at `/Game/RUDE/Areas/<slug>`. ⛔ PivotType stays WorldOrigin (RAGE placements are absolute — re-basing mis-places the area silently on export). `Mode="HEADLESS"` builds with no dialog; anything else uses the engine path with its modal Save-As. ⚠ Measured 2026-09-05: from an UNTITLED scratch level in a commandlet the headless path packed 0 of 5 collected actors (the level was created empty) - build interiors inside their own level instead (`NewLevel` → `ImportMlo` → `SaveLevel` → `PlaceInterior`) | agent |
| `ProbeWorldPartitionLevel` | `LevelPath` | WP4 spike: a World Partition level + one Data Layer + one actor on it + save, headless. Proven 2026-09-05 (`/Game/RUDE/Levels/WPProbe2`) | agent |
| `Ping` | *(none)* | Version + liveness. The panel shows this as a footer strip instead of a menu row — a person wants to *see* the plugin is alive, not run a tool to ask | agent |
| `ProbeYdrBinary` | `BinPath` | Parse a **binary** `.ydr` and report its whole drawable graph as JSON (shaders, models, geometries, vertex declarations, embedded bound). Reads untrusted files: every access bounds-checked, malformed input returns `ok:false`, a v159 (Enhanced) drawable is reported as such rather than silently misread | agent |
| `XmlShapeRoundTrip` | `ListPath, OutDir` | Parse a game XML with `FXmlFile`, re-spell it, re-parse, compare shape (paths/attributes/leaf text). Proves the WRITER, not bytes: `FXmlFile` joins multi-line text with spaces, which is why exporters splice source bytes instead of re-spelling | agent |
| `RegenerateMasters` | | Walk `/RUDE/Masters/Gen` and run every generated master through the generator; a stale one (a bucket-1 glass master without `OpacityScale`) is regenerated in place so its instances update | agent |
| `SaveAssets` | *(none)* | Save every dirty **content** package — never the level, which stays the operator's call. Exists so an agent-run import chain can persist its own work. Calls `FinishAllCompilation` first | agent |
| `SaveLevel` | `LevelPath` | Save the open level to a content path (an untitled level needs one). Headless-safe: `SaveMap`, then the direct map save if no `.umap` landed | agent |
| `SetArchetypeField` | `PaletteFolder, ArchetypeName, Field, Value` | Set one property on a palette archetype by name (reflection, value as text). The scriptable palette edit the export gate uses | agent |
| `SetLodView` | `Level` | Which LOD level of the placed lineage is visible: `HD` (default: HD + ORPHANHD), `LOD`, `SLOD1`..`SLOD4`, or `ALL` stacked. Every level is PLACED (export needs them all) and tagged `RUDE_LOD:<level>`; only visibility changes. The game shows one level of a lineage at a time - a level built with every level visible shows SLOD shells standing on their HD buildings (Matt, 2026-09-05) | human |
| `SetYmapVisible` | `YmapName, Visible` | Show or hide one ymap's placed actors (folder `RUDE_LS/<ymap>`) - the editor's stand-in for the game's IPL toggle. Script-controlled maps (CMapData `flags` bit 0, measured over downtown's 158 ymaps: every mission/variant file and only those) are placed but start hidden, and their Data Layer's initial runtime state is Unloaded; the reflection proxy that looked like a "blue glass tower" is one of them | human |
| `SetWorldHour` | `Hour` | Show only what GTA shows at this hour (`0`–`23`): a visibility sweep over the `RUDE_TIME`-tagged ISM components built from the game's own 24-bit `timeFlags` hour masks. Never a shader gate | agent Since 2026-09-06 it also sweeps the district's UStaticMeshComponent masks (not only ISM) and pitches the RUDE_SKY sun (elevation 90·sin(π(h−6)/12)), so an hour can be captured headlessly: `-ExecCmds="RUDE.Run SetWorldHour 22,RUDE.Run CaptureView …"` — ExecCmds splits on COMMAS, not semicolons; `Rude.Native` answers only in a running game world (PIE). Night proof: `scratchpad/wp10/sandbox/downtown_night_street.png` |
| `SpawnSeaLevel` | `SizeMetres, ZMetres` | Spawn/move the sea-level reference plane (GTA's ocean sits at world z=0). A **visual reference, not game data** — FiveM water comes from `water.xml`, which RUDE does not read or write. Lands in the `RUDE_ENV` folder so respawns leave it alone | human |

### Pipeline order (what to call, in what order)

- **Set up a project folder:** `CreateFilebase` → export your own game files with whatever extractor
  you use → `IngestExport` per dump. Everything downstream reads that folder.
- **Stand up a map area:** `ImportArea` (by name) or `ImportMapArea` (by prefix list) →
  `ImportYtdBatch` for that area's texture dictionaries → `ImportYdrBatch` with `Mode="FORCE"` to
  rebind materials against the now-present textures → `SaveAssets` → `CaptureView` to look at it.
- **Stand up an interior:** `ImportMlo`.
- **Author, then ship a prop:** edit the Static Mesh in UE → `ExportYdrBinary` +
  `ExportYtdBinary` (+ `ExportYbnBinary` for world collision) → `ExportYtyp` → `ExportYmap`.
  The XML variants (`ExportYdr`, `ExportYbn`) exist for inspection and for tools that want text.

---

## 4. Adding or changing a tool

1. **Declare it on `URudeToolset` in `RudeToolset.h`** as `static FString Fn(const FString&...)`.
   Any other signature is unreachable — see §1.
2. **`meta = (AICallable, RudeHelp="...")`**, plus `RudeAudience="agent"` when it is plumbing a
   person should never see on a menu.
   - The **doc comment** is the technical description: parameters, formats, struct offsets, page
     plans, the laws that make the output valid. Agents and the next maintainer both need it, and
     UHT turns it into the MCP description and the panel's "Technical detail" section.
   - **`RudeHelp` is the plain-language one**, written for a non-technical human — task-first, no
     jargon. These are deliberately two different texts. Do **not** simplify the doc comment to
     serve the panel; that costs the agents the detail they run on.
3. **Return a JSON verdict**, always, with an `ok` field and the numbers that let a caller check the
   run without opening the editor (counts imported / skipped / failed, and the first N failures by
   name). A tool that returns nothing meaningful cannot be verified by any of the four surfaces.
4. **Fail loudly.** No silent fallbacks and no silent defaults. If a fallback path is legitimate, it
   must state *why* it fell back — a plausible-looking message covering a swallowed exception has
   already cost this project a full test cycle.
5. **Do not add a tool list anywhere.** The panel, the CLI's `-list` and the MCP schema are all
   generated. This document is the only prose list, and it is checked against `-list`.

---

## 5. Build and drive loop

**⛔ ANY change to the toolset requires a FULL rebuild with the editor closed. Live Coding DROPS
the toolset.** Measured 2026-07-24: even a body-only change to `RudeToolset.cpp` patched via
`Ctrl+Alt+F11` left the toolset de-registered — `describe_toolset RudeEditor.RudeToolset` returned
`tools:[]` and `Ping` came back "no longer available". The registry does not re-scan under a Live
Coding module reload. This is not a "prefer to"; a Live-Coded session will silently lie to you about
what exists.

```
:: 1. close the editor  (do NOT Stop-Process it - see below)
<UE_ROOT>\Engine\Build\BatchFiles\Build.bat UnrealEditor Win64 Development -Project="<path>\<YourProject>.uproject"
:: 2. reopen, landing straight on the panel:
UnrealEditor.exe "<path>\<YourProject>.uproject" -ExecCmds="RUDE.Panel"
:: or drive a tool headlessly at launch:
UnrealEditor.exe "<path>\<YourProject>.uproject" -ExecCmds="RUDE.Run Ping"
:: or fully headless, with an exit code:
UnrealEditor-Cmd.exe "<path>\<YourProject>.uproject" -run=/Script/RudeEditor.RudeCommandlet -list
```

- **Do not `Stop-Process` the editor.** An unclean shutdown plants a "Restore Packages" modal that
  reappears on every relaunch, and **a modal dialog blocks the editor main thread**, so every MCP
  call times out while the process still reports Responding and the log still looks healthy. Close
  via `CloseMainWindow()` and poll for exit.
- **When anything hangs, LOOK at the screen first.** The first diagnostic for an unresponsive editor
  is a screenshot, not a restart. (Prefer `CaptureView` for the viewport; a desktop screenshot
  captures whatever is on the operator's primary monitor, which may not be Unreal at all.)
- **`InputCore` is a required `Build.cs` dependency**, not an optional one: `SComboBox`/`SListView`
  reference `EKeys`, so omitting it fails at link with ten unresolved symbols rather than a legible
  error.
- The plugin's module loading phase is **`Default`, and that is load-bearing**: a commandlet class
  must exist by the time the engine resolves `-run=`, which happens before `PostEngineInit`. At the
  later phase the CLI could not run at all. Registration is nevertheless order-*independent* (it
  retries on `PostEngineInit`), because a phase change must not be able to silently cost the MCP
  surface.

### The verification bar

"Compiles" is not "works", and **a JSON verdict is not a rendered frame.**

- A format feature is done when its output has been loaded by the actual game (FiveM) or the actual
  editor, **and a human has seen it**. State what you verified and how.
- **Import counts are not render evidence.** Instances spawned, meshes imported, `0 failed` — all of
  those count *spawns*, not pixels. Claiming "the city imports" from them is a category error that
  has already produced one wrong "defect" investigation in this project.
- **An unsynchronised screenshot is not a measurement.** In UE 5 a Static Mesh that is still
  compiling renders *nothing at all*, and compilation finishes **smallest-first** — so a capture
  fired right after an import shows the small props and drops the large meshes, which is
  indistinguishable by eye from a real "large meshes don't render" defect. `CaptureView` blocks on
  `FAssetCompilingManager::FinishAllCompilation()` for exactly this reason: **never remove that
  barrier.** `ok:true` from a capture means "a shot was requested", never "the scene was ready".
  (Two `CaptureView` calls in the same frame collide — the shot lands on the next draw, so only the
  last request survives. Issue them in separate runs.)

---

## 6. Conventions (violating these breaks round-trips)

1. **Transform:** `UE = (gta_x*100, -gta_y*100, gta_z*100)` — metres→cm with a Y mirror. The
   inverse is the same map (an involution).
2. **Triangle winding: pass through as-is** in the native importer. Under the Y-mirror, RAGE winding
   already faces outward in UE; reversing it renders inside-out. The OBJ lane *does* reverse, because
   UE's OBJ importer adds its own handedness flip.
3. **UVs:** raw pass-through (both engines are V-down).
4. **Rotations — ONE map for ymap entities, both directions.** ⛔ This entry previously said "TWO
   LANES with different maps, do not unify them", and that was wrong in a way that shipped: the two
   maps differ by a conjugation, so a UE → GTA → UE round trip inverted every rotation and every
   authored entity reached FiveM facing the wrong way.
   - **A ymap `<rotation>` stores the entity's INVERSE orientation.** So both directions use
     `(x, -y, z, w)` — an involution, and its own inverse.
   - **Proven against Rockstar's own data, not by reasoning:** a ymap declares
     `<entitiesExtentsMin/Max>`, so transforming each archetype's bbox by the entity transform and
     unioning must reproduce it. On single-entity ymaps the inverse reproduces the declared extents
     exactly (0.0000 m) while the forward quaternion is 22.77 m out. Corpus-wide, 81.25% of
     1,690,098 rotations differ between the two maps and 79.54% by more than 5° — visible at a
     glance in game, had an exported placement ever been looked at.
   - ⚠ **NOT a global rule.** A phBound `CompositeTransform` stores a **forward** matrix and keeps
     the pure mirror `(-x, y, -z, w)` (verified on 4,031/4,031 real composite children). The
     distinction is *ymap entity = inverse-stored, phBound = forward-stored* — not *two lanes*.
5. **XML payload parsing:** never rely on line structure inside XML text content — UE's `FXmlFile`
   does not preserve it. Parse a token stream sliced by the vertex layout's semantic widths.
6. **Emitted placement resources require `this_is_a_map 'yes'`** in `fxmanifest.lua`, or the ymap
   silently does nothing.
7. **FiveM Legacy loads XML-content `.ymap`/`.ytyp` natively; FiveM Enhanced never loads XML
   assets** (binary RSC validation). Drawables always need binary on both. Emit Legacy first; Cfx's
   Alchemist converts Legacy → Enhanced (binary input only, one direction).
8. **Asset updates are edit-in-place.** Delete-and-recreate severs referencers and leaves
   tombstones — recreating the same name in-session fails outright.
9. **Texture packages are the sharp edge.** Re-initialising `Source` over a texture whose package was
   already **saved this session** corrupts its bulkdata, and re-importing does not heal it. A refresh
   pass must recreate the packages fresh; block until texture compilation is quiet before saving, so
   a save can never land mid-build.
10. **Garbage collection during batch imports must pass `GARBAGE_COLLECTION_KEEPFLAGS`.** Collecting
    with `RF_NoFlags` ignores the `RF_Standalone` protection on freshly imported, unsaved assets and
    sweeps everything imported so far. This shipped once: 1,667 meshes imported, 73 survived. Every
    test at the time was under the 100-mesh checkpoint threshold, so no gate could see it.
11. Mass imports should trigger and monitor derived-data builds deliberately — never let a first
    level-open eat thousands of pending mesh builds.

---

## 7. The hard walls (non-negotiable, enforced in review)

- **No Rockstar-derived data enters this repository.** Not meshes, not textures, not XML exports,
  not schema *dumps* of game files, not "just one test asset". Converted content lives only on the
  user's machine. Test fixtures must be synthetic.
- **No code from other GTA tooling.** RUDE is clean-room: we interoperate with file *formats*, and
  we never port, translate, or read the source of any other GTA tool. Format knowledge comes from
  public documentation and from analysis of data the user exports from their own installation.
  (Stating that we did *not* use a tool is the opposite of affiliation, and is exactly the record a
  licence challenge would want — those assertions stay.)
- **No affiliation with, or instructions to use, any specific third-party extractor** in text the
  plugin authors — including the READMEs and manifests RUDE *writes onto the user's disk*. Say
  "whatever extractor you already use". This rule is about generated text as much as source
  comments; it has been violated exactly there before.
- **RUDE must not bundle, vendor, submodule or auto-download any key material or any external
  extractor.** It consumes a folder. That is the entire contract, and it is what keeps this plugin
  clean regardless of what any other project chooses to ship.
- **`.ysc` (compiled game scripts) are permanently out of scope.**
- **Free forever.** Reject any change that gates functionality behind payment.

---

## 8. Repo layout

```
RUDE.uplugin                     module RudeEditor, LoadingPhase Default, depends on ToolsetRegistry
Content/Masters/                 M_RUDE_Opaque / Cutout / DecalGeo / Foliage / Terrain / Water / Detail
                                 + Gen/ (30 signature-generated masters — committed, so a fresh clone
                                  ships all 37; EnsureGeneratedMaster remains the load-or-create path)
                                 (mounted at /RUDE/Masters; import routes each RAGE shader by
                                  RenderBucket first, preset name as fallback)
Source/RudeEditor/Private/
  RudeToolset.h                  THE tool surface (every UFUNCTION tool). Start here.
  RudeToolset.cpp                the core (4k lines): master generation, the ydr/yft XML reader
                                 (RudeYdr), bounds (RudeBound), textures, the single-file import tools
  RudeMapLanes.cpp               the map/area lane (split 2026-09-06): corpus filebase + archetype
                                 index, ImportMapArea / ImportArea / ImportMlo, ExportYtyp / ExportYmap,
                                 batch importers, SaveAssets, FixLevelRefs, SetWorldHour, ImportScene
  RudeBinaryLanes.cpp            the binary lane (split 2026-09-06): the RSC7 ydr reader, the ytd and
                                 ybn writers, ExportYdrBinary[Batch] / ExportYtdBinary / ExportYbnBinary /
                                 ProbeYdrBinary
  RudeLevelTools.cpp             the level lane (split 2026-09-06): BuildDistrictLevel / OpenLevel /
                                 palette / interiors / ExportLevelYmaps / SetLodView / PickAt / InspectMesh /
                                 LOD lineage (LodLineage / SetLodParent / LodAudit + the resolver)
  RudeToolsetInternal.h          private plumbing shared by the .cpp lanes (RudeJsonEscape, the XML
                                 re-speller, RudeSpawnEntityActor, RudeSaveDirty + its counters)
  RudeScenario/BuildArea/Vehicle.cpp   lane implementations split out for size — still URudeToolset
                                 statics, reached only through FRudeInvoke
  RudeInvoke.{h,cpp}             the ONE reflective call path (§1)
  RudeToolPanel.{h,cpp}          the Slate panel + the RUDE.Panel and RUDE.Run console commands
  RudeCommandlet.{h,cpp}         the CLI - argument handling only, dispatch is FRudeInvoke
  RudeEditorModule.cpp           registration into ToolsetRegistry + the tab spawner
```

---

## 9. Known gaps — read before you claim something works

Honest state, so nobody re-derives these the hard way:

- **`SaveAssets` persists NOTHING under `-unattended`** (measured 2026-07-31): in a commandlet /
  `-unattended` run, `FEditorFileUtils::SaveDirtyPackages` returns `false` and writes no packages —
  the tool reports `ok:false` and no `.uasset` lands on disk. So a CLI-driven import chain that ends
  with `SaveAssets` (the documented pattern in §3) silently loses all its work. Current operating
  model: building happens with the editor OPEN (agent via MCP, or a human); but the CLI is listed as
  a first-class surface in §2, so headless persistence is an open product question tracked on the
  maintainer's register — not a wontfix.
- **`ImportArea` needs an area-catalog JSON that this repository does not ship** (checked 2026-07-28:
  no such file is tracked). Without one, use `ImportMapArea` with an explicit prefix list.
- **`ExportYmap` overwrites `fxmanifest.lua` unconditionally** — no existence check, and the write
  result is not tested. Exporting twice into a resource that has hand-added `client_script` or
  `files` entries destroys them. It should merge.
- **The panel is a generic tool driver, not an authoring workflow.** Every parameter is a free-text
  field: no file pickers, no area menu, no browse buttons.
- **`ImportMlo` is v1:** the interior spawns at the world origin, not at its ymap world transform;
  portals and entity sets are summarised in the verdict rather than spawned; the light intensity
  scale is a single named **uncalibrated** constant; and several `CLightAttrDef` fields (flags,
  corona/volumetric, falloff exponent, bone frames) are carried but deliberately unmapped because no
  proven UE equivalent has been established.
- **There is no release artifact.** Installation today is: clone into `<YourProject>/Plugins/`, then
  build from source with UE 5.8 and the ToolsetRegistry + ModelContextProtocol plugins enabled.
- **RUDE cannot open a game archive** and never will — it is a folder consumer by design. The import
  side is only as good as the interchange folder someone hands it.
