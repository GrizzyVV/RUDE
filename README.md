# RUDE — RAGE ↔ Unreal Development Environment

**Unreal Engine as a first-class FiveM mapping DCC.** Import GTA V map context into UE,
author with real level-editor tools, export straight back to FiveM.

Free forever. No paywall, no premium tier, no strings. Ever.

> **Status: early alpha (0.1).** The foundations are proven — placements authored in UE
> load in live FiveM, and RAGE drawables import as native StaticMesh assets — but this is
> young software under heavy active development. Expect sharp edges.

---

## What it does

| Working today | How |
|---|---|
| Author prop/map placements in UE → FiveM | Emit XML-format `.ymap`/`.ytyp` that FiveM (Legacy) loads natively — no binary conversion needed for the placement layer |
| Import GTA V drawables into UE | `ImportYdr`: CodeWalker-XML → `UStaticMesh` with per-shader material slots, in one call |
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
- To produce importable XML: [CodeWalker](https://github.com/dexyfex/CodeWalker)
  (export any asset as XML from your own installation)
- For FiveM **Enhanced** output: Cfx's Alchemist converts RUDE's Legacy output
  (binary lane — in progress)

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
triangle winding: reversed (the mirror flips handedness)
UVs: passed through raw (both engines are V-down)
rotations: identity-quat proven; full quaternion convention is open research
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
