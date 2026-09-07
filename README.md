# RUDE — RAGE ↔ Unreal Development Environment

<img width="2554" height="1400" alt="image" src="https://github.com/user-attachments/assets/6c0b2d52-c2a6-4643-953a-7329a4eaddc7" />

**Unreal Engine as a first-class FiveM mapping DCC.** Bring GTA V map context into Unreal,
build with real level-editor tools, and write finished files straight back out to FiveM.

Free forever. No paywall, no premium tier, no strings. Ever. [Apache-2.0](LICENSE).

> **Status: alpha, whole surface present, most of it unconfirmed in the running game.** Import
> and export are both real: a 100% RUDE-authored asset (binary `.ydr` + `.ytd`, XML `.ytyp`/`.ymap`)
> loads, renders and collides in live FiveM Legacy, one call builds Downtown Los Santos in an
> Unreal level (`BuildDistrictLevel`: 158 ymaps → 14,248 entities as editable actors, written
> back byte-identical when untouched), and the rest of the game's content - interiors, scenarios,
> paths, vehicles, peds, clothing, props, animations, cutscenes, timecycles, text, audio - has an
> import and, where a writer exists, an export. **Most of that newer surface has been measured
> against the game's own files but never yet confirmed in the running game.** The tables below
> say which is which, on purpose.

---

## Read this first if you have never opened Unreal

RUDE is an **editor plugin**. It is not a standalone program — it lives inside an Unreal
Engine project and adds tools to the editor. So the shape of the day is:

1. Install Unreal Engine 5.8, make a project, drop RUDE into it. ([INSTALL.md](INSTALL.md))
2. Point RUDE at a folder of GTA V assets **you extracted from your own game install**.
3. Build a piece of the city in Unreal, change it, and export a FiveM resource folder.

Two things surprise people coming from a Blender-based mapping workflow:

- **RUDE does not open `.rpf` archives.** It never touches your game archives, has no
  decryption code in it, and never will — see [Why RUDE reads a folder](#why-rude-reads-a-folder-and-not-your-game-archives).
  Getting the assets out is a **separate step with a separate tool**.
- **RUDE reads the plain-text XML form of RAGE assets** (`prop_x.ydr.xml`), not the packed
  binaries — on the way *in*. On the way *out* it writes packed binaries for models, textures,
  collision and clothing, and the XML form FiveM Legacy loads for placement and definition files.

## What you still cannot do — read before you download

Being straight about this is more useful than a feature list.

| | |
|---|---|
| ⛔ **You cannot go from a fresh game install to a working project with RUDE alone.** | RUDE needs a *filebase* — a folder of extracted assets in a documented shape. It does not produce one and ships no code that reads a game archive. You run an extractor yourself. |
| 🟡 **The extractor is a separate, public tool.** | The maintainer's own extractor, [ROUT](https://github.com/GrizzyVV/ROUT---RAGE-Exporter-App), writes exactly the filebase RUDE reads (see [the folder contract](#the-folder-contract)). Any other tool that produces that shape works too; RUDE never asks which one wrote it. Getting the assets out is still the longest part of day one. |
| 🟡 **`ImportArea "Downtown"` needs a district catalog, and you generate it.** | The named-district lane reads a JSON catalog of ymap prefixes. RUDE ships none and never will — it is derived from the game's own files, and there is no game data in this repository. `BuildAreaCatalog` writes one from your own filebase in a single call; until you run it, `ImportArea` refuses and names that command. `ImportMapArea` with a raw filename prefix works either way — same code path underneath. |
| ❓ **Everything newer than the map lane is measured, not yet played.** | Interiors, scenarios, paths, vehicles, peds, clothing, props, animations, cutscenes, timecycles, text and audio all import; their exports are checked byte-for-byte or field-by-field against the game's own files, and some XML-form outputs (`.ymt`, `.ynd`, `.ytyp` interiors) have **not** yet been streamed by FiveM to prove the game accepts that form. The tables below mark every such row ❓. |
| 🔴 **No fragment (`.yft`) writer, no ped-variation table writer, no navmesh, water or particle lanes.** | Vehicles and weapons import and can be posed, fitted and reskinned, but cannot be written back as NEW vehicles or weapons; new clothing goes into the game by *replacing* a ped's own dictionary, not by adding a variation row. |
| ◑ **Game audio: only plain PCM tracks import.** | `ImportAwc` imports PCM tracks and counts ADPCM and encrypted ones honestly; the game's own banks are encrypted per chunk, so no game sound has been imported yet. Your **own** sounds export to the game's format. |
| ⛔ **`.ysc` compiled game scripts are permanently out of scope.** | Not a gap. A decision. |
| ⛔ **RUDE will never ship Rockstar assets.** | It is machinery. Everything it converts comes from *your* legally-owned GTA V install, on *your* machine, and goes back into GTA V via FiveM. There is no game data in this repository and there never will be. |

---

## Requirements

| | |
|---|---|
| **Unreal Engine** | **5.8**, Windows. RUDE is built and tested only against 5.8, and depends on the `ToolsetRegistry` plugin that ships with it. No other version is supported. |
| **Visual Studio** | Needed today: RUDE is built from source (see [INSTALL.md](INSTALL.md)). A precompiled release, when one exists, will need none. |
| **A GTA V installation** | Yours. RUDE never reads it directly, but it is where your filebase comes from. |
| **An extractor** | [ROUT](https://github.com/GrizzyVV/ROUT---RAGE-Exporter-App) (public, the maintainer's own), or any tool that writes the filebase shape in [the folder contract](#the-folder-contract). Not part of RUDE. |
| **Engine plugin: `ToolsetRegistry`** | Ships with 5.8 as Experimental. RUDE declares it as a dependency, so enabling RUDE enables it. |
| **Engine plugin: `ModelContextProtocol`** | **Only** if you want AI agents to drive RUDE. Humans and scripts do not need it. |
| **Engine plugin: `ChaosVehiclesPlugin`** | Ships with the engine; RUDE declares it for the editor test-drive (`BuildDriveable`). Enabling RUDE enables it. |
| **Cfx Alchemist** | **Only** for FiveM **Enhanced**. RUDE writes Legacy; Alchemist converts Legacy → Enhanced. Run it as a separate step. |

**Install:** see [INSTALL.md](INSTALL.md). It is a separate file because installing a
compiled Unreal plugin has two genuinely different routes (drop-in binary vs. build from
source) and a first-time Unreal user needs the long version, not three lines.

---

## The five steps, from a new user's point of view

This is the acceptance test RUDE is built against. Each step says plainly where it stands.

### 1 · Get the tools — 🟡 clone and build; releases follow

There is no precompiled RUDE release yet: clone this repository and build it into your project
([INSTALL.md](INSTALL.md), route B - four lines once Visual Studio is installed). Then get an
extractor: the maintainer's, [ROUT](https://github.com/GrizzyVV/ROUT---RAGE-Exporter-App), is
public and writes the filebase RUDE reads. It is a separate program with its own README; RUDE
contains none of it and none of its key material.

### 2 · Prepare a filebase from your game files — 🟡 you run the extractor

Run the extractor against your own install. It writes a **filebase**: a load-order tree
(`00_base/`, `10_update/`, `20_dlc/NNN_<pack>/`) of one XML per asset plus two ledgers at the
root, and, when asked, the texture pixels beside each dictionary. That root is what every RUDE
importer takes as `CorpusRoot`. The shape is documented in [the folder contract](#the-folder-contract)
so that any other extractor can produce it.

The whole game is large (hundreds of gigabytes as XML with pixels) and you do not need all of
it: an extractor that can target a district, a vehicle or a ped by name gets you working in
minutes. Texture pixels are optional per dictionary - a dictionary exported without them
imports as names only, and RUDE says so in its verdict.

`CreateFilebase` and `IngestExport` are still here for a filebase assembled by hand from
several dumps (they read directory names only and open no archive), but with a ledger-writing
extractor you do not need them.

### 3 · Open it in Unreal, in context — ✅ works

```
ImportMapArea(CorpusRoot, "dt1_", "/Game/RUDE/Meshes", "HD", "ACTORS")
```

One call walks every archetype definition, parses the matching placement files, imports every
model they reference, and spawns the area into your open level - with `Mode = "ACTORS"` as one
editable actor per entity, which is the form every export lane reads; leave `Mode` empty for
fast, view-only instanced meshes. Textures are a separate pass: `ImportYtdBatch` on the
dictionaries the area names (they resolve through the game's own parent-dictionary tables), then
`ImportYdrBatch` with `FORCE` to rebind. For a whole district as a World Partition level with
run-time layers per ymap, use `BuildDistrictLevel` instead. Interiors come in with `ImportMlo`,
every prop as its own editable actor.

*Measured 2026-07-28 (`ImportMapArea`, instanced mode):* Downtown Los Santos — 158 placement files, **13,135 instances placed,
17 unresolved proxies, 0 failures** — rendered and textured, seen on screen, not inferred
from a success code.

### 4 · Change it, save it, export it ready for FiveM — ✅ works, and it is the strongest part

Author in Unreal like it is Unreal. Then:

| You want | Call |
|---|---|
| a game-ready model with its collision inside it | `ExportYdrBinary(AssetPath, "out/stream/prop_x.ydr")` |
| its textures | `ExportYtdBinary(TextureSpecs, "out/stream/prop_x.ytd", "0")` |
| standalone / world collision | `ExportYbnBinary(AssetPath, "out/stream/x.ybn", WorldOffset)` |
| the definitions that tell the game what those models are | `ExportYtyp(YdrSpecs, "my_map", "out/stream/my_map.ytyp")` |
| a complete, drop-in FiveM resource for new placements | `ExportYmap(EntitiesJsonPath, "my_map", "out/")` |
| the district you edited, written back into the game's own ymaps (untouched entities byte-identical) | `ExportLevelYmaps(OutDir, YmapFilter, CorpusRoot, NewEntitiesYmap)` |
| an interior you edited, written back into its ytyp | `ExportMloYtyp(OutDir, MloArchetypeName, CorpusRoot)` |
| a ped's clothing and props as a resource that replaces the game's own files | `ExportPedReplace(OutfitAssetPath /* or just the ped name */, OutDir, Options)` |
| an edited texture dictionary as a replace resource | `ExportTxdReplace(DictName, OutDir, MaxDim)` |

Trailing parameters may be omitted on every surface; the tool sees them as empty strings.

`ExportYmap` writes `stream/<name>.ymap` **and** an `fxmanifest.lua` containing the required
`this_is_a_map 'yes'` — without that line FiveM loads your resource and silently does nothing.

### 5 · Drive it however you work — ✅ all four surfaces exist

| Surface | How |
|---|---|
| **Panel** (human) | `Window ▸ Tools ▸ RUDE`, or type `RUDE.Panel` in the editor console. Lists every tool, generates a field per parameter, shows that tool's own help, prints its JSON result. |
| **Console** | `RUDE.Run <Tool> <arg> <arg> …`, which also means it works from `-ExecCmds` at editor launch. |
| **CLI** (headless) | `UnrealEditor-Cmd.exe <Project>.uproject -run=/Script/RudeEditor.RudeCommandlet -tool=<Name> …` — every tool, no window, exit code carries that tool's own verdict. |
| **Agents** (MCP) | RUDE registers `RudeEditor.RudeToolset`, so every tool appears as a first-class MCP tool. See [AGENTS.md](AGENTS.md). |

All four call **one** reflective core, so no surface can drift from another. Every tool
returns a JSON string with an `ok` field; failures are loud and say why.

---

## The folder contract

RUDE's importers read a **filebase**: a staging tree cut from your game files by an extractor you
run yourself, kept in load order, described by two ledgers at its root. Point `CorpusRoot` at
that root:

```
<CorpusRoot>/
  _FILEBASE.json                 which install it was cut from, the slot precedence, a build fingerprint
  _PROVENANCE.jsonl              one line per exported file: slot, in-archive path, type, name, sha1
  00_base/<archive path>/<name>.<type>.xml      the base game, one XML per asset, path mirrors the archive
  10_update/<archive path>/...                  the title update
  20_dlc/NNN_<pack>/<archive path>/...          each DLC pack, numbered in load order
  .../<txdname>/<texture>.dds                   a texture dictionary's pixels, in a folder beside its XML
```

Rules, stated so any extractor can satisfy them:

- **Any producer.** RUDE cares about the shape of this tree and its two ledgers, and nothing
  else. It does not know or ask which tool wrote it. The maintainer's own extractor is public and
  writes exactly this shape; anything else that does is equally welcome.
- **Load order is resolved by RUDE, from the ledger.** Every copy of an asset across base, update
  and DLC slots stays on disk; RUDE picks the copy the game would load (highest slot wins, ties
  broken by path so the answer is machine-independent). In the ped and vehicle lanes a texture
  dictionary copy that carries its pixel folder wins over a higher-slot copy without one - an
  extractor may export pixels for only part of the game, and that must not blank a texture the
  base game has. The prop lane imports the dictionary path you list.
- **The metadata joins need the ledger.** Vehicles (`vehicles.meta`, `handling.meta`,
  `carvariations`), peds (`<ped>.ymt`), texture-parent tables (`gtxd`) and the vehicle
  texture-sharing table are found by content through the ledger, across every copy. A flat folder
  cannot answer those questions.
- **Without the ledgers, only the by-name lanes fall back to a flat folder.** `ImportPed`,
  `ImportVehicle` (the v1 single-fragment lane), `ImportScenarioRegion` and `ImportClipDictionary`
  accept `<CorpusRoot>/<name>.<type>.xml` files beside each other when the two ledgers are absent -
  one file per name, already the winning copy, pixels in a sibling folder named for the XML's stem
  (DDS or PNG). The single-file tools (`ImportYdr`, `ImportYtd`, `ImportYddEntry`) take an XML path
  directly and never see `CorpusRoot`. Every map, interior and vehicle-composite lane needs the
  ledgers and says so when they are missing.
- **Names are lowercase asset names**, matching the drawable name the `.ytyp` refers to.
- **Texture pixels are a sibling folder** named for the XML's stem. `ImportYtdBatch` derives that
  path; `ImportYtd` derives it when `PixelFolder` is left empty, so the pair cannot be mismatched.

## Why RUDE reads a folder and not your game archives

A deliberate design decision, not an unfinished feature:

- GTA V's archives are encrypted, and reading them needs key material that belongs to
  Rockstar. **RUDE ships none of it, and no code that handles it.** That keeps this plugin —
  the thing we want widely adopted, embedded in other people's projects and studios' build
  pipelines — clean of the one question that could get it taken down.
- It also means RUDE is **not coupled to one extractor**. Any tool that can produce the
  folder above works, today, with no change to RUDE.

The cost is honest and it is on you: you need a filebase before RUDE is useful, and
producing one is currently the hardest part of getting started.

---

## Capability and limits — what actually works today

Everything marked ✅ has been loaded by the real game or the real editor and looked at by a
human. ◑ means it works within a stated boundary. **❓ means the output was measured against
the game's own files (byte-identical where a writer exists, field-by-field otherwise) but has
not yet been confirmed in the running game.** 🔴 means it does not exist. The authoritative,
per-tool list with every gate's numbers is [AGENTS.md](AGENTS.md) §3; the compiled tool
surface is `RudeToolset.h`.

### Import — game files into Unreal

| Capability | Tool | State |
|---|---|---|
| Model (`.ydr`) → `StaticMesh`, materials, textures bound by name, walkable collision | `ImportYdr`, `ImportYdrBatch` | ✅ |
| One model out of a dictionary (`.ydd`) | `ImportYddEntry` | ✅ |
| Fragment (`.yft`) → its main visual mesh | via the area importers | ◑ visual drawable only |
| Texture dictionary (`.ytd`) → `Texture2D` with normal/spec/sRGB handling | `ImportYtd`, `ImportYtdBatch` | ✅ needs pixels beside the XML (DDS or PNG) |
| A whole map area: archetypes → placements → models → **one editable actor per entity**, LOD lineage, lights, time-of-day flags, script-controlled maps | `ImportMapArea`, `ImportArea`, `ImportScene` | ✅ Downtown: 158 ymaps, 14,248 entities |
| Interior (MLO): rooms, every prop as its own actor, entity sets, lights (portal data carried, not spawned) | `ImportMlo`, `SetEntitySet` | ✅ in editor · ❓ sets in-game |
| **A NEW interior authored in Unreal**: rooms as box volumes, portals as slabs that find their two rooms, any prop inside a room joins it | `NewMloInterior`, `AddMloRoom`, `AddMloPortal`, `AddMloProp` | ✅ in editor · ❓ never streamed |
| Car generators (parking spawns) as markers | `ImportCarGenerators`, `MoveCarGenerator` | ❓ |
| Scenario regions (ambient life) as editable points | `ImportScenarioRegion` | ❓ |
| Vehicle and footpath node graphs (`.ynd`) | `ImportPaths`, `MovePathNode` | ❓ |
| Vehicles as composites: 5 LODs, every part at its bone, handling and colours joined by the game's own tables, liveries, shared interior textures | `ImportVehicleComposite`, `SetVehicleLivery` | ✅ in editor · ❓ never driven in-game |
| A drivable version of an imported vehicle (Chaos) for the editor sandbox | `BuildDriveable` | ❓ built, never played |
| Weapons as composites: the fragment plus every component the meta lists, fitted to its socket, defaults on and alternates hidden | `ImportWeapon`, `SetWeaponComponent` | ✅ in editor, seen on screen · ❓ never held in-game |
| Peds: skeleton, skinned parts (all three detail groups), the variation matrix, outfits, props (hats, glasses) | `ImportPed`, `SetPedOutfit`, `SetPedProp`, `InspectPedLods` | ✅ in editor · ❓ prop attach frame |
| Animations (`.ycd`) → `AnimSequence`, cutscenes → Level Sequence | `ImportClipDictionary`, `ImportCutscene` | ❓ axis conventions |
| Timecycles, game text (`.gxt2`), blip catalog | `ImportTimecycles`, `ImportText`, `BuildBlipCatalog` | ❓ |
| Seed / fill a filebase by hand | `CreateFilebase`, `IngestExport` | ✅ directory names only |
| Inspect a binary `.ydr` / `.ydd` / a mesh's materials without importing | `ProbeYdrBinary`, `ProbeYddBinary`, `InspectMesh` | ✅ diagnostic JSON |
| Standalone collision (`.ybn`) as an import | — | 🔴 |
| Game audio (`.awc`) | `ImportAwc` | ◑ plain PCM tracks import; ADPCM and encrypted tracks are counted and skipped - no game sound imported yet |
| Navmesh · water | `DebugDrawNavmesh`, `SpawnSeaLevel` | ◑ view-only aids; no authoring lane |
| Particles and the game's remaining file types | `CatalogLane` | ◑ carried through as a catalogued passthrough, not edited |

### Export — Unreal back out to FiveM

| Capability | Tool | State |
|---|---|---|
| `StaticMesh` → binary `.ydr` the game loads directly, collision embedded | `ExportYdrBinary` | ✅ **proven in live FiveM Legacy** |
| `Texture2D`(s) → binary `.ytd` with real BC compression and mip chains | `ExportYtdBinary` | ✅ proven in-game |
| Collision → binary `.ybn` | `ExportYbnBinary` | ✅ proven in-game |
| Archetype definitions → `.ytyp` | `ExportYtyp`, `ExportPaletteYtyps` | ✅ `ExportYtyp` proven in-game · ❓ `ExportPaletteYtyps` byte-identical on 73/73 untouched, not yet streamed |
| New placements → a complete resource (`stream/<name>.ymap` + `fxmanifest.lua`) | `ExportYmap` | ✅ |
| **The map you edited, back into the game's own ymaps** — untouched entities byte-identical, moved ones re-spelled, LOD lineage, lights and car generators written back | `ExportLevelYmaps` | ✅ byte-identical on 148/148 untouched · ❓ an edited district ymap has not yet been streamed in-game |
| Rebuilt LOD models and distant-light bakes | `MakeLodArchetype`, `RebuildLodChunk`, `RebakeLodLights` | ❓ |
| Interior (MLO) you edited → its `.ytyp` (byte-safe splice) | `ExportMloYtyp` | ❓ XML-form ytyp acceptance |
| **A NEW interior → its own `.ytyp` + the `.ymap` that places it**, as a streamable resource; a refusal writes nothing | `ExportNewMlo` | ❓ 257 structural checks pass, 5 refusal gates refuse; never streamed |
| Scenario region / path cell you edited → `.ymt` / `.ynd` (byte-safe) | `ExportScenarioRegion`, `ExportPaths` | ❓ XML-form acceptance |
| Skinned clothing → binary `.ydd`; a ped's whole outfit + props + textures as a replace resource | `ExportYddBinary`, `ExportPedReplace` | ❓ never loaded in-game |
| A texture dictionary → replace resource | `ExportTxdReplace` | ❓ |
| An animation edited in Unreal → a clip dictionary, written against the game's own file as the template | `ExportClipDictionary`, `ProbeYcdXml` | ❓ **text form only** - an untouched dictionary comes back byte-identical, but nothing packs the text back into a loadable binary today, in RUDE or anywhere else |
| Timecycles / text / your own sounds | `ExportTimecycles`, `ExportText`, `ExportAwc` | ❓ registration in-game |
| Model / collision → the editable XML form; texture → PNG | `ExportYdr`, `ExportYbn`, `ExportTexture` | ✅ interchange and inspection output (the XML form itself is not game-loadable) |
| Fragment (`.yft`) — a new vehicle, weapon or breakable | — | 🔴 no writer. Editing an existing fragment is reachable through a round-trip writer; authoring one that has no original needs a constructive writer that does not exist yet |
| FiveM **Enhanced** | — | ◑ export Legacy, then convert with Cfx Alchemist. Enhanced never loads XML-form assets. |

### Conventions worth knowing before you drive anything

```
UE = ( gta_x * 100,  -gta_y * 100,  gta_z * 100 )     metres → centimetres, Y mirrored
triangle winding: passed through as-is (under the Y mirror, RAGE winding already
  faces outward in Unreal). The OBJ lane is the exception — reverse it there, because
  Unreal's OBJ importer adds its own flip.
UVs: raw, both engines are V-down.
rotations — ONE map, both directions. A ymap <rotation> stores the entity's INVERSE
  orientation, and the map is its own inverse:
  EXPORT  (authoring for FiveM):  gta_quat = ( ue_x, -ue_y,  ue_z,  ue_w)
  IMPORT  (reading placement XML): ue_quat = (gta_x, -gta_y, gta_z, gta_w)
  (Corrected 2026-08-04: this described two DIFFERENT maps, so a UE→GTA→UE round trip
   conjugated every rotation. Adjudicated over all 8,016 ymap against Rockstar's own
   <entitiesExtents> with a no-rotation control: inverse reproduces them to 0.0002 m,
   forward is 1.09 m out. A phBound CompositeTransform is the exception — it stores a
   FORWARD matrix and keeps the pure mirror.)
```

Details, and the reasoning behind each, live in [AGENTS.md](AGENTS.md).

---

## Docs in this repository

| File | What it is |
|---|---|
| [INSTALL.md](INSTALL.md) | Getting RUDE into an Unreal 5.8 project, both routes, with the failure modes |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each version |
| [RELEASING.md](RELEASING.md) | How a release is cut, and what one must contain |
| [AGENTS.md](AGENTS.md) | The tool surface, conventions, and rules for contributors and AI agents |
| [NOTICE](NOTICE) | Attribution that travels with the code |

## Contributing

Issues and pull requests are welcome. Three hard rules:

1. **No Rockstar-derived data in any commit** — no meshes, textures, XML exports or archive
   contents. Test fixtures must be synthetic.
2. **No code lifted from other GTA tooling.** RUDE is clean-room: we interoperate with file
   *formats*, never with other projects' *source*. Format knowledge comes from public
   documentation and from analysing data you exported from your own installation.
3. **Free forever.** Any change that gates functionality behind payment gets rejected.

"It compiles" is not "it works". A format feature is done when its output has been loaded by
the actual game or the actual editor, and a human has seen it. Say what you verified and how.

## Licence

[Apache-2.0](LICENSE). Use it, fork it, learn from it, build servers with it — including
commercially, including inside a studio. Attribution travels with the code via
[NOTICE](NOTICE), which you must keep in redistributions (Apache-2.0 §4(d)); the patent
grant means your legal department can relax.

RUDE is an independent community tool. It is **not affiliated with, endorsed by, or
sponsored by** Rockstar Games, Take-Two Interactive, or Cfx.re.
