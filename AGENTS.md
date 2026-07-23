# RUDE — Agent Onboarding

This file is for AI agents working with or on RUDE. Humans welcome too.

## Driving RUDE (the MCP toolset)

RUDE registers `RudeEditor.RudeToolset` into the editor's ToolsetRegistry at startup —
it appears alongside the engine's own toolsets on any MCP connection to the editor.
All tools take/return JSON-friendly values; results are JSON strings with an `ok` field.

| Tool | Signature | Notes |
|---|---|---|
| `Ping` | `() -> string` | Version + liveness |
| `ImportYdr` | `(XmlPath, DestFolder) -> json` | CodeWalker ydr XML → `UStaticMesh`. Material slots are named `<rage_shader_preset>__<geoIndex>` — bind materials by slot name |

## The conventions (violating these breaks round-trips)

1. **Transform:** `UE = (gta_x*100, −gta_y*100, gta_z*100)`; triangle winding **as-is**
   in the native importer (under the Y-mirror, RAGE winding already faces outward —
   verified in-editor; reversing it renders inside-out). OBJ-lane exports reverse
   winding because UE's OBJ importer adds its own flip. UVs raw (both engines V-down);
   rotations identity-proven only — do not emit non-identity quaternions until the
   convention is resolved and documented here.
2. **XML payload parsing:** never rely on line structure inside XML text content
   (UE's FXmlFile does not preserve it). Parse token streams sliced by the vertex
   layout's semantic widths.
3. **Emitted placement resources require `this_is_a_map 'yes'`** in fxmanifest.lua.
4. **FiveM Enhanced never loads XML assets** — Legacy output first, Alchemist converts.
5. **Asset updates are edit-in-place.** Delete-and-recreate severs referencers and can
   leave tombstones. Prefer modifying existing assets.
6. Mass imports should trigger and monitor derived-data (DDC) builds deliberately —
   never let a first level-open eat thousands of pending mesh builds.

## The hard walls (non-negotiable, enforced in review)

- **No Rockstar-derived data enters this repository.** Not meshes, not textures, not
  XML exports, not schema *dumps* of game files, not "just one test asset." Converted
  content lives only on the user's machine. Test fixtures must be synthetic.
- **No code from other GTA tooling.** RUDE is clean-room: we interoperate with file
  formats, we never port, translate, or reference the source of CodeWalker, Sollumz,
  or any other tool. Format knowledge comes from public documentation and from
  analysis of data the user exports from their own installation.
- **`.ysc` (compiled game scripts) are permanently out of scope.**
- **Free forever.** Reject any change that gates functionality behind payment.

## Working on RUDE (contributing agents)

- Editor-module C++ (UE 5.8). Tools are `static UFUNCTION`s on `URudeToolset` marked
  `meta = (AICallable)` — reflection generates the MCP schema; write real doc comments,
  they become the tool descriptions.
- Building: the editor's Live Coding lock blocks external builds — build with the
  editor closed, or patch function bodies via Live Coding (Ctrl+Alt+F11) for iteration.
- Verification bar: "compiles" is not "works". A format feature is done when its output
  has been loaded by the actual game (FiveM) or the actual editor, and a human has seen
  it. State what you verified and how.
