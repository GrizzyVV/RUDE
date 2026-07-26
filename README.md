# RUDE — RAGE ↔ Unreal Development Environment
<img width="2554" height="1400" alt="image" src="https://github.com/user-attachments/assets/6c0b2d52-c2a6-4643-953a-7329a4eaddc7" />


**Unreal Engine as a first-class FiveM mapping DCC.** Import GTA V map context into UE,
author with real level-editor tools, export straight back to FiveM.

Free forever. No paywall, no premium tier, no strings. Ever.

> **Status: early alpha (0.1) — with the big milestone passed.** A 100% RUDE-authored
> asset (binary `.ydr` + `.ytd` + `.ytyp`/`.ymap`) loads, renders, and collides in live
> FiveM with **zero CodeWalker involvement** — the entire UE→FiveM-Legacy export pipeline
> is clean-room native. On the import side, RUDE has ingested an entire GTA V island
> (Cayo Perico: 126 ymaps, 31k placed instances, textured) into a UE level. Young
> software, heavy active development, sharp edges.

---

## What it does

| Working today | How |
|---|---|
| **Export binary `.ydr`/`.ybn`/`.ytd` — no CodeWalker** | Clean-room RSC7 writers (`ExportYdrBinary`, `ExportYbnBinary`, `ExportYtdBinary`), validated in live FiveM: meshes with embedded collision, world collision bounds, DXT/BC texture dictionaries with mip chains |
| Author placements in UE → FiveM | `ExportYtyp` + `ExportYmap` emit the archetype/placement layer (XML, FiveM Legacy loads it natively) with the in-game-proven collision flag model and manifest |
| Import GTA V drawables into UE | `ImportYdr` (+`ImportYdrBatch`): CodeWalker-XML → `UStaticMesh` with per-shader material instances (RAGE RenderBucket-driven opaque/cutout/decal routing), textures bound by name, PIE-walkable collision |
| Ingest whole map areas | `ImportMapArea`: one call walks ytyp archetypes → ymap entities → every referenced drawable → spawns the area. `ImportScene` does the placement step from a manifest: one actor per ymap with instanced-static-mesh components (48k entities in one call) |
| Set up your own game files | `CreateFilebase` seeds a load-order-aware tree (base < update < DLC) and `IngestExport` files a flat export dump into it by type — directory names only, no archive is opened |
| Import texture dictionaries | `ImportYtd`: ytd XML + decoded pixels → `UTexture2D` with usage-correct semantics (normal maps, sRGB) |
| Agent-native operation | RUDE registers an MCP toolset inside the editor — AI agents drive imports/exports as first-class tools. Humans get the same functions as Blueprint-callable nodes |

**The roadmap** (phased; each phase ships when its in-game gate passes): full placement
studio → asset pipeline (meshes/collision/textures/materials) → MLO interior suite with
room/portal authoring → whole-map browsing CodeWalker-style → fully automated binary +
Enhanced output → generators the ecosystem lacks (audio-occlusion from your room graph,
navmesh/paths, structured timecycle editing).

## What it will never do

- **Ship Rockstar assets.** RUDE is machinery. Everything it converts comes from *your*
  legally owned GTA V installation, on *your* machine, and goes back into GTA V via FiveM.
  This repository contains no game data and never will.
- **Touch game code.** `.ysc` script files are out of scope, permanently.
- **Cost money.** See line one.

## Requirements

- Unreal Engine **5.8** (Windows)
- For the agent surface: the engine's **ToolsetRegistry** + **ModelContextProtocol**
  plugins (shipped Experimental in 5.8) enabled in your project
- For the **import** side only: [CodeWalker](https://github.com/dexyfex/CodeWalker)
  XML exports from your own installation (until RUDE's direct game-file reader lands).
  The **export** side needs no external tools — RUDE writes game-ready binaries itself.
- For FiveM **Enhanced** output: Cfx's Alchemist converts RUDE's Legacy output

## Install

1. Clone into your project's `Plugins/` folder:
   `git clone https://github.com/GrizzyVV/RUDE.git <YourProject>/Plugins/RUDE`
2. Build (editor closed):
   `Engine/Build/BatchFiles/Build.bat UnrealEditor Win64 Development -Project=<YourProject>.uproject`
3. Open the editor. The `RudeEditor.RudeToolset` registers automatically.

## Conventions (read before writing code or driving the tools)

The RUDE transform between coordinate systems, applied on import and inverted on export:

```
UE = ( gta_x * 100,  -gta_y * 100,  gta_z * 100 )   // meters→cm, Y mirror
triangle winding: PASS THROUGH as-is in the native importer (empirically pinned:
  under the Y-mirror, RAGE winding already faces outward in UE). If you route
  through OBJ instead, reverse it — UE's OBJ importer adds its own flip.
UVs: passed through raw (both engines are V-down)
rotations — TWO LANES with different maps (both empirically pinned; do not unify):
  EXPORT (authoring for FiveM): gta_quat = (-ue_x, ue_y, -ue_z, ue_w)
  IMPORT (reading CW-exported ymap XML): ue_quat = (gta_x, -gta_y, gta_z, gta_w)
  (the two paths cross CodeWalker in opposite directions; details in AGENTS.md)
```

FiveM manifest law for emitted placement resources: `this_is_a_map 'yes'` is **required**
(it reloads map storage on resource start; without it your ymap silently does nothing).
FiveM **Enhanced** never loads XML-format assets (binary RSC validation) — Legacy first,
then convert.

## For AI agents

See [AGENTS.md](AGENTS.md) — toolset call patterns, conventions, and the safety rules
that keep contributions clean. The short version: the toolset is
`RudeEditor.RudeToolset`, inputs/outputs are JSON, and nothing derived from game files
ever enters this repository.

## Contributing

Issues and PRs welcome. Two hard rules: no Rockstar-derived data in any commit
(meshes, textures, XML exports, RPF contents — none of it), and no code copied from
other GTA tooling (this project is clean-room; we interoperate with formats, not
codebases). Everything else is open for discussion.

## License

[Apache-2.0](LICENSE). Use it, fork it, learn from it, build servers with it — including
commercially, including by studios. Attribution travels with the code via [NOTICE](NOTICE);
the patent grant means your legal department can relax. RUDE is an independent community
tool, not affiliated with Rockstar Games, Take-Two Interactive, or Cfx.re.
