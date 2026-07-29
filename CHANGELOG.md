# Changelog

All notable changes to RUDE are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is described in
[RELEASING.md](RELEASING.md).

**RUDE is pre-1.0.** While the major version is `0`, a **minor** bump is allowed to change a
tool's parameters or the bytes it writes. Those changes are always listed under
*Changed* or *Removed* — read them before updating.

Anything marked **proven in-game** means the output was loaded by live FiveM and a human
looked at it. Anything marked **measured** carries the number it was measured at.

---

## [Unreleased]

Nothing yet.

---

## [0.2.0] — unreleased draft

The first release with a real workflow in it. 0.1.0 could import one model; 0.2.0 imports
whole districts and writes finished game binaries back out, with four ways to drive it.

The tool surface went from **2 tools to 24**.

### Added

**Export — the clean-room binary writers.** All three write the packed format the game loads
directly, with no external tool anywhere in the path.

- `ExportYdrBinary` — `StaticMesh` → binary `.ydr` (RSC7 v165), with `phBoundComposite`
  collision embedded in the drawable. **Proven in live FiveM Legacy:** renders, textures and
  collides at 1, 2 and 8 material slots.
- `ExportYtdBinary` — `Texture2D`(s) → binary `.ytd` (RSC7 v13) with real BC compression and
  full mip chains, stopping at 4×4 (the minimum DXT block — smaller mips break the streamer's
  per-mip size arithmetic). `AUTO` format selection picks ATI2 for normal maps, DXT5 when the
  source has meaningful alpha, else DXT1. Optional `MaxDim` downscale cap. Proven in-game.
- `ExportYbnBinary` — `StaticMesh` collision → binary `.ybn` (RSC7 v43): a
  `phBoundComposite` wrapping one `phBoundGeometryBVH` with quantized vertices and a
  constructed stackless BVH. `WorldOffset` places static world collision in absolute
  coordinates. Proven in-game.

**Export — the placement layer.**

- `ExportYtyp` — archetype definitions with the in-game-proven collision model: embedded
  bounds gate flag bit 17 *and* a non-empty physics dictionary, together, as the switch.
- `ExportYmap` — a complete, drop-in FiveM resource: `stream/<name>.ymap` plus an
  `fxmanifest.lua` carrying the required `this_is_a_map 'yes'`. Without that line FiveM loads
  the resource and silently does nothing.
- `ExportYdr`, `ExportYbn` — the editable XML form of both, round-trip verified.
- `ExportTexture` — `Texture2D` source pixels → PNG.

**Import — from one model to a city.**

- `ImportMapArea` — the one-call thread-pull: index every archetype definition, parse the
  matching placements, import every model they reference, spawn the area as instanced
  meshes. **Measured 2026-07-28 on Downtown Los Santos: 158 placement files → 13,135
  instances placed, 17 unresolved proxies, 0 failures**, textured and seen on screen.
- `ImportArea` — the same by human district name ("Downtown Los Santos") instead of a
  filename prefix, resolved through a catalog. ⚠ *The catalog itself is not published yet* —
  see the README's limits table.
- `ImportScene` — re-place an area you already imported, without re-importing any model.
  Idempotent: it clears its own previous spawn.
- `ImportYdrBatch` — many models from a list file, skip-if-exists, with a `FORCE` mode that
  re-imports in place to re-bind materials after a texture pass.
- `ImportYtd` / `ImportYtdBatch` — texture dictionaries → `Texture2D` with usage-correct
  semantics (normal maps, sRGB rules). The batch derives each pixel folder from the XML's own
  path, so the XML and its pixels cannot be mismatched by a wrong argument.
- `ImportYddEntry` — pull one named model out of a dictionary. Names match
  case-insensitively *and* by hash both ways, because the placement→archetype→dictionary
  joins are hash-to-hash.
- Fragment (`.yft`) import, through the same lane — a fragment's main visual mesh now
  resolves instead of becoming a proxy cube. **Measured: Downtown proxies fell 6,227 → 17.**
- `ImportMlo` — interiors: rooms, their props, and their lights, spawned under one root actor
  at the world origin. Light instances map honestly — point/spot/capsule by type, with
  position, colour, intensity and falloff consumed; every field that has no proven Unreal
  equivalent is listed as unmapped in the tool's own documentation rather than guessed at.
- `ProbeYdrBinary` — parse a binary `.ydr` and report its whole drawable graph as JSON,
  without importing. Every read is bounds-checked; a malformed file returns a failure, and an
  Enhanced-format drawable is reported as such rather than silently misread.

**Setting up a project.**

- `CreateFilebase` — seed a load-order-aware folder tree shaped to your own game install.
  Reads directory names only; opens no archive and decrypts nothing. Writes a manifest with a
  build fingerprint, and a README explaining how to fill it.
- `IngestExport` — file a flat export dump into that tree, sorted by type into the right
  precedence slot, so nobody hand-sorts anything.

**Ways to drive it.** Every surface calls one reflective core, so none can drift from another.

- **A human panel** — `Window ▸ Tools ▸ RUDE`, or `RUDE.Panel`. Lists every tool, generates a
  field per real parameter, shows that tool's own plain-language help, prints its JSON.
- **A CLI** — `-run=/Script/RudeEditor.RudeCommandlet -tool=<Name>`, every tool headless,
  by position or by name, with the exit code carrying that tool's own verdict.
- **A console command** — `RUDE.Run <Tool> <arg>…`, which also means from `-ExecCmds` at
  editor launch, so a scripted import needs no MCP session and no human at the panel.
- **MCP** — unchanged, and now sharing the same core as the other three.

**Materials, and seeing the result.**

- A master material library shipped as plugin content: opaque, cutout, decal, decal-geometry,
  foliage, terrain and water masters, with per-slot material instances derived automatically.
- Terrain: four-layer normalized weighted blend with vertex colours.
- Foliage: two-sided master; decals hide themselves when their texture is missing rather than
  rendering as a grey rectangle.
- `SpawnSeaLevel` — a sea-level reference plane at GTA world z=0, so coastal authoring can see
  where the water sits. A **visual guide, not game data**: FiveM water comes from `water.xml`,
  which RUDE does not read or write.
- `CaptureView` — position the viewport and save a screenshot, so an agent can verify its own
  work with no human at the machine.
- `SaveAssets` — persist every changed RUDE asset. It deliberately never saves your level;
  saving the map stays your decision.

### Changed

- **Material routing is driven by the render bucket, not the shader preset name.** Preset
  names lie: bucket 0 → opaque, 1 and 3 → cutout, 2 → decal. Foliage presets route to the
  cutout master.
- **Imported meshes use complex-as-simple collision** so you can walk the city in Play-In-Editor.
- **Re-importing an asset edits it in place.** Delete-and-recreate severs everything that
  referenced the asset and can leave tombstones behind.
- **Re-import resets prior mesh state** — material slots and source models — instead of
  layering new state on top of stale state.
- **Triangle winding is passed through as-is** in the native importer. Under the Y mirror,
  RAGE winding already faces outward in Unreal; reversing it renders the whole city
  inside-out. The OBJ lane still reverses, because Unreal's OBJ importer adds its own flip.
- **The rotation convention is pinned by in-game experiment**, not derived on paper, and the
  import and export lanes use deliberately *different* maps. Do not unify them.
- **The crash gates moved inside the writer.** `ExportYdrBinary` now self-verifies before it
  writes a byte, instead of relying on a separate check somebody could forget to run.
- The panel splits its tool list by audience and leads with plain language, so the first
  thing a human sees is not agent plumbing.

### Fixed

Everything here was found by loading the output into the actual game or editor.

- **`ERR_MEM_MULTIALLOC_FREE` on load** — the `.ydr` page plan has to be uniform-P; a
  non-uniform plan crashes the game at stream time.
- **In-game crash from over-declared shaders** — only declare a shader preset whose registers
  are actually emitted.
- **In-game crash from aliased ownership** — never alias a vertex format or a texture stub
  between two owners in the written file.
- **Struct spacing** — vertex buffer, index buffer and geometry structs must be padded to the
  spacing the game expects, not merely to something self-consistent.
- **Multi-geometry `.ydr` correctness**, plus `u16` count guards and texture name slots sized
  to fit any name length.
- **`.ybn` page encoding** — a 64 KB page cap, no struct spanning a page boundary, and the
  mandatory `m_Trees` subtree table without which the bound never loads.
- **Vertex declaration decode** — a full semantic width table with misalignment guards, so a
  layout RUDE has not seen fails loudly instead of reading garbage.
- **Garbage collection swept freshly imported assets.** GC now runs with keep-flags; without
  them a large import could lose its own work mid-run.
- ⛔ **`CaptureView` photographed a half-built scene.** A still-compiling `StaticMesh` renders
  nothing, and compilation finishes smallest-first — so an unsynchronised screenshot showed
  the small props and dropped the large meshes, which looks exactly like a real "big meshes
  are missing" defect and cost a full false investigation. `CaptureView` now blocks on
  compilation before it shoots. **An unsynchronised screenshot is not a measurement.**
- Several tool help strings that a human could not parse, and `ProbeYdrBinary` hidden from the
  human panel — it returns a diagnostic dump, not an answer.

### Known limitations in this release

Stated here because a changelog that only lists wins is a sales brochure. The full table is in
the [README](README.md#capability-and-limits--what-actually-works-today).

- **You must supply a filebase.** RUDE opens no game archive and ships no extractor.
- **`ImportArea`'s district catalog is not published**, so use `ImportMapArea` with a raw
  filename prefix.
- **No importer** for standalone collision (`.ybn`), scenarios, water, timecycles, navmesh,
  particles or audio.
- **No export** for interiors or fragments — the binary metadata writer does not exist yet.
- **Fragment import is visual-only**: the main drawable, no breakable pieces, no cloth.
- **`ImportMlo` is v1**: the interior spawns at the world origin, and portals and entity sets
  are summarised in the result rather than spawned.
- **FiveM Enhanced** is reached by exporting Legacy and converting with Cfx Alchemist.
  Enhanced never loads XML-form assets, so Legacy binaries come first, always.

---

## [0.1.0] — 2026-07-23

Initial public source drop: the repository, the licence, and a working proof of the idea.

⚠ **No release artifact was ever published for 0.1.0** — the version existed only as a
version string in `RUDE.uplugin`. 0.2.0 is the first version with a downloadable release.

### Added

- The `RudeEditor` editor module and the `URudeToolset` MCP toolset, registered into the
  engine's ToolsetRegistry at editor startup.
- `Ping` — version and liveness.
- `ImportYdr` — one RAGE drawable in XML form → a `UStaticMesh`, with the GTA↔Unreal
  transform convention applied.
- Apache-2.0 licence, `NOTICE`, and the contributor/agent hard walls in `AGENTS.md`.

<!-- Tag history starts at v0.2.0: 0.1.0 was never tagged, so it is linked by commit. -->
[Unreleased]: https://github.com/GrizzyVV/RUDE/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/GrizzyVV/RUDE/compare/696710c...v0.2.0
[0.1.0]: https://github.com/GrizzyVV/RUDE/commit/696710c
