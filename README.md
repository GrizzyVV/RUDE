# RUDE — RAGE ↔ Unreal Development Environment

<img width="2554" height="1400" alt="image" src="https://github.com/user-attachments/assets/6c0b2d52-c2a6-4643-953a-7329a4eaddc7" />

**Unreal Engine as a first-class FiveM mapping DCC.** Bring GTA V map context into Unreal,
build with real level-editor tools, and write finished files straight back out to FiveM.

Free forever. No paywall, no premium tier, no strings. Ever. [Apache-2.0](LICENSE).

> **Status: early alpha.** Import and export are both real and both proven — a
> 100% RUDE-authored asset (binary `.ydr` + `.ytd` + `.ytyp`/`.ymap`) loads, renders and
> collides in live FiveM Legacy, and on the way in, one call has built Downtown Los Santos
> in an Unreal level (158 ymaps → 13,135 placed instances, 0 failures). It is young
> software under heavy development, with sharp edges and honest gaps — both are listed
> below, in the same table, on purpose.

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
  binaries — on the way *in*. On the way *out* it writes the real packed binaries the game
  loads directly.

## What you still cannot do — read before you download

Being straight about this is more useful than a feature list.

| | |
|---|---|
| ⛔ **You cannot go from a fresh game install to a working project with RUDE alone.** | RUDE needs a *filebase* — a folder of extracted assets. It does not produce one and ships no tool that does. You supply it. |
| 🔴 **We do not yet publish an extractor you can download.** | Its companion tool, QUARRY, does this job, but **QUARRY has no public release** — how it gets distributed is an unsolved product question, not a finished thing we are hiding. Until then RUDE works with a folder produced by **any** extractor you already own, as long as the folder matches [the contract](#the-folder-contract). |
| 🔴 **`ImportArea "Downtown"` needs a district catalog that is not in this repo yet.** | The named-district lane reads a JSON catalog of ymap prefixes. It is not published. Use `ImportMapArea` with a raw filename prefix in the meantime — same code path underneath. |
| 🔴 **No importer for standalone collision (`.ybn`), scenarios, water, timecycles, navmesh, particles or audio.** | Map geometry, textures, placements and interiors import. Everything else in a ymap's orbit does not. |
| 🔴 **No export path for interiors (MLO) or fragments (`.yft`).** | You can *read* an interior into Unreal. You cannot yet write one back out — the binary metadata writer does not exist. |
| ⛔ **`.ysc` compiled game scripts are permanently out of scope.** | Not a gap. A decision. |
| ⛔ **RUDE will never ship Rockstar assets.** | It is machinery. Everything it converts comes from *your* legally-owned GTA V install, on *your* machine, and goes back into GTA V via FiveM. There is no game data in this repository and there never will be. |

---

## Requirements

| | |
|---|---|
| **Unreal Engine** | **5.8**, Windows. RUDE is built and tested only against 5.8, and depends on the `ToolsetRegistry` plugin that ships with it. No other version is supported. |
| **Visual Studio** | Only if you are building RUDE from source (see [INSTALL.md](INSTALL.md)). A precompiled release needs none. |
| **A GTA V installation** | Yours. RUDE never reads it directly, but it is where your filebase comes from. |
| **An extractor** | Any tool that can produce the XML form of `.ydr`/`.ydd`/`.yft`/`.ytd`/`.ytyp`/`.ymap`. Not supplied by RUDE. |
| **Engine plugin: `ToolsetRegistry`** | Ships with 5.8 as Experimental. RUDE declares it as a dependency, so enabling RUDE enables it. |
| **Engine plugin: `ModelContextProtocol`** | **Only** if you want AI agents to drive RUDE. Humans and scripts do not need it. |
| **Cfx Alchemist** | **Only** for FiveM **Enhanced**. RUDE writes Legacy; Alchemist converts Legacy → Enhanced. Run it as a separate step. |

**Install:** see [INSTALL.md](INSTALL.md). It is a separate file because installing a
compiled Unreal plugin has two genuinely different routes (drop-in binary vs. build from
source) and a first-time Unreal user needs the long version, not three lines.

---

## The five steps, from a new user's point of view

This is the acceptance test RUDE is built against. Each step says plainly where it stands.

### 1 · Get the tools — 🟡 partly solved

Download a RUDE release and put it in your project. That part is real and documented.
**You will also need an extractor to fill a filebase, and we do not publish one yet.**
That is the honest state of step 1 and the largest open problem in the product.

### 2 · Prepare a project folder from your game files — 🟡 you supply the extractor

RUDE gives you the *shape* of the folder and files things into it; it does not fill it.

```
CreateFilebase(FilebaseRoot, GameRoot, "CORE")
```

seeds a load-order-aware tree matched to *your* install. It reads **directory names only** —
it opens no archive and decrypts nothing. Then, for each source you extract:

```
IngestExport(DumpFolder, SourceName, FilebaseRoot, "MOVE")
```

files a flat dump into the right slot by type, so you never sort anything by hand.
`SourceName` is `base`, `update`, or a DLC pack name such as `mpbiker`.

⚠ **The importers do not read that tree directly.** They read a *flat* view of it — see
[the folder contract](#the-folder-contract). Flattening means resolving load order (the same
asset name exists in the base game, in `update.rpf` and in several DLC packs, and the last
one wins). Whatever produces your filebase should also produce that flat view.

### 3 · Open it in Unreal, in context — ✅ works

```
ImportMapArea(CorpusRoot, "dt1_", "/Game/RUDE/Meshes", "HD")
```

One call walks every archetype definition, parses the matching placement files, imports every
model they reference, and spawns the area into your open level as instanced meshes. Textures
come in with `ImportYtdBatch`. Interiors come in with `ImportMlo`.

*Measured 2026-07-28:* Downtown Los Santos — 158 placement files, **13,135 instances placed,
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
| a complete, drop-in FiveM resource | `ExportYmap(EntitiesJsonPath, "my_map", "out/")` |

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

RUDE's importers read a **flat corpus**: one file per asset name, extensions doubled
(`<name>.<type>.xml`), no load-order slots. Point `CorpusRoot` at a folder shaped like this:

```
<CorpusRoot>/
  ytyp/<anything>.xml          archetype definitions — RUDE indexes every file here
  ymap/<prefix>*.xml           placements — ImportMapArea globs these by prefix
  ydr/<assetname>.ydr.xml      one drawable per file
  yft/<assetname>.yft.xml      one fragment per file
  ydd/<dictname>.ydd.xml       a dictionary — RUDE picks the entry it needs by name
  ytd/<txdname>.ytd.xml        a texture dictionary
  ytd/<txdname>/*.png          its decoded pixels, in a folder beside the XML
```

Rules, stated so any extractor can satisfy them:

- **Any producer.** RUDE cares about the shape of this folder and nothing else. It does not
  know or ask which tool wrote it, and it must stay that way.
- **Flat and already resolved.** RUDE walks no precedence. If the same asset name exists in
  several sources, the *one* file present here must already be the winning copy — otherwise
  you are authoring against the wrong build and it fails silently, which is the worst way to
  fail. Load order is the extractor's job because the extractor is what can read it.
- **Names are lowercase asset names**, matching the drawable name the `.ytyp` refers to.
- **Texture pixels are a sibling folder** named for the XML's stem. `ImportYtdBatch` derives
  that path rather than taking it as an argument, so the pair cannot be mismatched.
- **`_FILEBASE.json`** at the staging root records which install the folder was cut from,
  the precedence order, and a build fingerprint — so a project can notice it is being fed a
  different game build than it was authored against.

The staging tree that `CreateFilebase` seeds is a *different* shape — `00_base/`,
`10_update/`, `20_dlc/NNN_<name>/`, each with type subfolders. That tree preserves load
order; the flat corpus is the resolved view of it. **Do not point `CorpusRoot` at the
staging root** — the importers will find nothing and say so.

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
human. ◑ means it works within a stated boundary. 🔴 means it does not exist.

### Import — game files into Unreal

| Capability | Tool | State |
|---|---|---|
| Model (`.ydr`) → `StaticMesh`, materials, textures bound by name, walkable collision | `ImportYdr`, `ImportYdrBatch` | ✅ |
| One model out of a dictionary (`.ydd`) | `ImportYddEntry` | ✅ |
| Fragment (`.yft`) → its **main visual mesh** | via the area importers | ◑ visual drawable only — no breakable pieces, no cloth, no child parts |
| Texture dictionary (`.ytd`) → `Texture2D` with correct normal/spec/sRGB handling | `ImportYtd`, `ImportYtdBatch` | ✅ needs decoded PNGs beside the XML |
| A whole map area: archetypes → placements → models → spawned in your level | `ImportMapArea` | ✅ 13,135 instances / 0 failures, measured |
| The same, by human district name | `ImportArea` | ◑ code works; **the district catalog is not published yet** |
| Re-place an area you already imported, without re-importing models | `ImportScene` | ✅ |
| Interior (MLO): rooms, props and lights | `ImportMlo` | ◑ v1 — spawns at the world origin; portals and entity sets are summarised, not spawned; several light fields are deliberately unmapped and listed in the tool's own docs |
| Seed / fill a filebase | `CreateFilebase`, `IngestExport` | ✅ directory names only; no archive is opened |
| Inspect a binary `.ydr` without importing it | `ProbeYdrBinary` | ✅ diagnostic JSON |
| Standalone collision (`.ybn`) | — | 🔴 no importer |
| Scenarios · water · timecycle · navmesh · particles · audio occlusion | — | 🔴 none |
| Vehicles · peds · clothing as *systems* | — | 🔴 none |

### Export — Unreal back out to FiveM

| Capability | Tool | State |
|---|---|---|
| `StaticMesh` → binary `.ydr` the game loads directly, collision embedded | `ExportYdrBinary` | ✅ **proven in live FiveM Legacy** — renders, textures, collides |
| `Texture2D`(s) → binary `.ytd` with real BC compression and mip chains | `ExportYtdBinary` | ✅ proven in-game |
| Collision → binary `.ybn`, optionally placed in absolute world coordinates | `ExportYbnBinary` | ✅ proven in-game |
| Archetype definitions → `.ytyp`, with the in-game-proven collision flag model | `ExportYtyp` | ✅ |
| Placements → a complete resource: `stream/<name>.ymap` + `fxmanifest.lua` | `ExportYmap` | ✅ |
| Model / collision → the editable XML form | `ExportYdr`, `ExportYbn` | ✅ |
| Texture → PNG | `ExportTexture` | ✅ |
| Interior (MLO) → `.ytyp` | — | 🔴 the binary metadata writer does not exist |
| Fragment (`.yft`) | — | 🔴 |
| FiveM **Enhanced** | — | ◑ export Legacy, then convert with Cfx Alchemist. Enhanced never loads XML-form assets, so Legacy binaries first, always. |

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
