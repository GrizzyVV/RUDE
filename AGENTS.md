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
this repository. RUDE never opens or decrypts a game archive: it consumes a folder of XML that
some other tool (e.g. the companion **QUARRY**, or any extractor the user already owns) produced
from their own installation.

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
| **CLI** (`URudeCommandlet`) | `UnrealEditor-Cmd.exe <YourProject>.uproject -run=/Script/RudeEditor.RudeCommandlet -list` / `-tool=<Name>` | stdout via `LogRudeCLI`, plus an **exit code that is the tool's own verdict** (non-zero only on `"ok":false`) |
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

## 3. The tool surface — 24 tools

Sorted alphabetically, which is exactly the order `FRudeInvoke::CollectTools` produces, so this
table **diffs line-for-line against `-list`**. That is the intended way to check it.

`Audience`: **human** = shown in the panel (20 of them); **agent** = plumbing, tagged
`RudeAudience="agent"`, hidden from the panel but present on the CLI and MCP (4 of them).

| Tool | Parameters (in order) | What it does | Audience |
|---|---|---|---|
| `CaptureView` | `CamSpec, OutPng` | Aim the perspective viewport (`"x,y,z,pitch,yaw"` in UE cm/degrees; `;` also accepted) and write a PNG — the agent-vision primitive. Blocks on `FinishAllCompilation` first, then the shot lands on the NEXT draw: poll for the file | agent |
| `CreateFilebase` | `FilebaseRoot, GameRoot, Options` | Seed the **filebase**: a load-order-aware folder tree the user exports their own game files into (`00_base` < `10_update` < `20_dlc/NNN_name`, higher wins). Enumerates the install's directory names only — **no archive is opened or decrypted**. `Options` = `CORE` (default) or `ALL` | human |
| `ExportTexture` | `TexturePath, OutPngPath` | Write a `UTexture2D`'s source pixels (BGRA8) out as a PNG | human |
| `ExportYbn` | `AssetPath, OutXmlPath` | Mesh collision → standalone `.ybn` **XML**. Note: prop collision actually travels *inside* the ydr's embedded `<Bounds>`; this is for shared/world collision work | human |
| `ExportYbnBinary` | `AssetPath, OutYbnPath, WorldOffset` | Mesh collision → **binary** `.ybn` (RSC7 v43): phBoundComposite → GeometryBVH with quantized verts, stackless BVH and the mandatory subtree table, page-aware layout. `WorldOffset` = `"x,y,z"` GTA metres for absolute world placement (empty = mesh-local). Validated in game | human |
| `ExportYdr` | `AssetPath, OutXmlPath` | Static Mesh → `.ydr` **XML** (the editable interchange form): inverse transform, shader presets recovered from slot names, embedded collision `<Bounds>` | human |
| `ExportYdrBinary` | `AssetPath, OutYdrPath` | Static Mesh → **binary** `.ydr` (RSC7 v165): GTAV1 vertex layout, `normal_spec` parameter template with name-hash binding, embedded phBoundComposite collision, page-plan-safe layout. Drawable name = output filename. Validated in game | human |
| `ExportYmap` | `EntitiesJsonPath, MapName, OutDir` | Placement JSON (UE space) → a complete FiveM resource: `stream/<name>.ymap` + `fxmanifest.lua` carrying the required `this_is_a_map` | human |
| `ExportYtdBinary` | `TextureSpecs, OutYtdPath, MaxDim` | Textures → **binary** `.ytd` (RSC7 v13) with full mip chains stopping at 4x4. `TextureSpecs` = comma-joined `"ContentPath;RageName[;Usage[;Format]]"`; `Format` = `AUTO\|DXT1\|DXT5\|ATI2\|RAW` (AUTO: NORMAL→ATI2, meaningful alpha→DXT5, else DXT1). `MaxDim` box-downscales oversized sources (`0`/empty = no cap). Validated in game | human |
| `ExportYtyp` | `YdrSpecs, YtypName, OutYtypPath` | Emit a CMapTypes `.ytyp` — one `CBaseArchetypeDef` per drawable — with the in-game-proven collision model (embedded bounds gate flag bit 17 **and** a non-empty `physicsDictionary`). `YdrSpecs` = comma-separated `"absPath.ydr.xml[;txd[;physDict]]"` | human |
| `ImportArea` | `AreaName, CatalogPath, CorpusRoot, DestMeshFolder, Filter` | Import a district by its **human name** ("Downtown Los Santos") using an area catalog JSON, then delegate to `ImportMapArea` — one code path, no drift. Empty `AreaName` lists every alias. Underscores read as spaces (console arguments) | human |
| `ImportMapArea` | `CorpusRoot, YmapPrefix, DestMeshFolder, Filter` | **The one-call thread-pull:** archetype index from `<CorpusRoot>/ytyp` → parse `<CorpusRoot>/ymap/<prefix>*.xml` → import every referenced drawable (ydr / yft / ydd entry) → spawn via `ImportScene`. `YmapPrefix` is a **comma-separated list**; exact basenames ride as `<name>.ymap`. `Filter` = `HD` (default) or `ALL`. ⚠ REPLACES the previously spawned area | human |
| `ImportMlo` | `CorpusRoot, MloArchetypeName, DestMeshFolder, Filter` | Build an MLO **interior**: locate the `CMloArchetypeDef` (name matching is hash-tolerant both ways), import every mesh its entities reference, spawn each room's entities at MLO-local transforms under one root actor **at the world origin** (v1), and map `CLightAttrDef`s to point/spot lights. `Filter` = empty/`ALL`, or a comma-separated **room-name** list. Re-running replaces this archetype's actors by tag | human |
| `ImportScene` | `ManifestPath, MeshFolder, Filter` | Re-spawn a scene manifest into the open level **without re-importing anything**: one actor per ymap, one instanced-static-mesh component per unique drawable, proxy cubes for missing meshes, idempotent (clears its previous spawn). `Filter` = `HD` (default, i.e. HD/ORPHANHD lod levels) or `ALL` | human |
| `ImportYddEntry` | `XmlPath, EntryName, DestFolder` | Import ONE named entry out of a `.ydd.xml` drawable dictionary. `EntryName` is matched case-insensitively **and by joaat hash both ways** (entries are usually `hash_XXXXXXXX`); the imported mesh takes `EntryName`, the archetype-facing identity. An unknown entry fails loudly, listing what *is* there | human |
| `ImportYdr` | `XmlPath, DestFolder` | One `.ydr.xml` → `UStaticMesh`. Material slots named `<shader_preset>__<geoIndex>`; Material Instances auto-created from the RUDE masters with **RenderBucket-driven routing** (bucket is RAGE's authoritative signal — preset names lie); textures bound by name from `/Game/RUDE/Textures`; complex-as-simple collision so you can walk it in PIE. Reimport is edit-in-place | human |
| `ImportYdrBatch` | `ListPath, DestFolder, Mode` | Same lane over a text file of absolute `.ydr.xml` paths, one per line. Skip-if-exists (idempotent re-runs) unless `Mode="FORCE"`, which reimports in place — the re-bind pass after textures land | human |
| `ImportYtd` | `XmlPath, PixelFolder, DestFolder` | `.ytd.xml` + decoded PNGs → `UTexture2D`s with Usage-driven semantics (NORMAL → `TC_Normalmap` + sRGB off, SPECULAR → sRGB off, DIFFUSE → sRGB on). Assets land in `<DestFolder>/<TxdName>/` | human |
| `ImportYtdBatch` | `ListPath, DestFolder, Mode` | Same over a list file. **The pixel folder is derived, not passed**: the decoded pixels live in a sibling `<stem>/` folder beside each XML, so the pair is self-describing. Skip-if-exists unless `Mode="FORCE"` | human |
| `IngestExport` | `DumpFolder, SourceName, FilebaseRoot, Move` | File a **flat** export dump into the filebase, sorted by type into the right precedence slot, so nobody hand-sorts. `SourceName` = `base` / `update` / a DLC pack name (empty = the dump folder's own name). `Move` = `MOVE` (default) or `COPY` — ⚠ MOVE relocates recursively and replaces name collisions | human |
| `Ping` | *(none)* | Version + liveness. The panel shows this as a footer strip instead of a menu row — a person wants to *see* the plugin is alive, not run a tool to ask | agent |
| `ProbeYdrBinary` | `BinPath` | Parse a **binary** `.ydr` and report its whole drawable graph as JSON (shaders, models, geometries, vertex declarations, embedded bound). Reads untrusted files: every access bounds-checked, malformed input returns `ok:false`, a v159 (Enhanced) drawable is reported as such rather than silently misread | agent |
| `SaveAssets` | *(none)* | Save every dirty **content** package — never the level, which stays the operator's call. Exists so an agent-run import chain can persist its own work. Calls `FinishAllCompilation` first | agent |
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
4. **Rotations — TWO LANES with different maps. Do not unify them.**
   - **Export lane** (authoring for FiveM): `gta_quat = (-ue_x, ue_y, -ue_z, ue_w)` — an involution,
     bench-proven in game.
   - **Import lane** (reading `.ymap` XML out of the interchange folder):
     `ue_quat = (gx, -gy, gz, gw)` — a pure Y-mirror reflection. Verified against a curving
     boardwalk whose sections kink under the export-lane map and knit under this one. Identity
     rotations are unaffected either way, which is why a wrong choice here looks *mostly* right.
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
Content/Masters/                 M_RUDE_Opaque / Cutout / Decal / DecalGeo / Foliage / Terrain / Water
                                 (mounted at /RUDE/Masters; import routes each RAGE shader to one of
                                  these by RenderBucket first, preset name as fallback)
Source/RudeEditor/Private/
  RudeToolset.h                  THE tool surface - 24 UFUNCTIONs. Start here.
  RudeToolset.cpp                every implementation
  RudeInvoke.{h,cpp}             the ONE reflective call path (§1)
  RudeToolPanel.{h,cpp}          the Slate panel + the RUDE.Panel and RUDE.Run console commands
  RudeCommandlet.{h,cpp}         the CLI - argument handling only, dispatch is FRudeInvoke
  RudeEditorModule.cpp           registration into ToolsetRegistry + the tab spawner
```

---

## 9. Known gaps — read before you claim something works

Honest state, so nobody re-derives these the hard way:

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
