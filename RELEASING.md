# Releasing RUDE

How a RUDE version is numbered, what a release must contain for a stranger to actually
install it, and how to produce one from this repository.

This file is the checklist. `Tools/Package-Release.ps1` mechanizes the parts of it that a
script can check, because a guarantee that depends on somebody remembering is not a
guarantee.

---

## 1 · The versioning scheme

`RUDE.uplugin` carries two version fields and they mean different things. Unreal reads both;
only one is for humans.

| Field | Type | Meaning | Rule |
|---|---|---|---|
| `Version` | integer | A monotonic **build counter**. Unreal compares it numerically when reasoning about plugin upgrades. It is not derived from the semantic version. | **+1 on every published release.** Never reuse it, never decrease it. |
| `VersionName` | string | The human `MAJOR.MINOR.PATCH` shown in the plugin browser, returned by `Ping`, and used for the tag and release title. | Semantic, per the table below. |

### What the semantic parts mean while RUDE is 0.x

- **MAJOR stays `0`** until all five steps of the product's own acceptance test are green for
  a new user who starts with nothing but a game installation. Reaching `1.0` is a statement
  about the *product*, not about the code being tidy. Today step 1 (a new user can obtain and
  install the tools) is not green, because there is no published extractor.
- **MINOR** — a new tool, a changed tool signature, a changed on-disk output, or a removal.
  Breaking changes are permitted in a minor bump while MAJOR is `0`; that is what `0.x` means.
  **Every one of them must be listed under *Changed* or *Removed* in the changelog.** A silent
  breaking change is the one thing that makes a pre-1.0 tool untrustworthy.
- **PATCH** — fixes, and documentation-only changes, that alter neither a tool's parameters
  nor the bytes it writes.

### Three places carry the version, and they can drift

1. `RUDE.uplugin` → `Version` **and** `VersionName`
2. `RudeToolset.h` → `GetToolsetVersion()` — **this is the string agents and the CLI read**,
   so a stale value here misreports the version to every automated consumer while the plugin
   browser looks correct
3. `CHANGELOG.md` → the top released heading, and the git tag `v<VersionName>`

`Tools/Package-Release.ps1` refuses to package when these disagree. Do not "fix" a
disagreement by editing the script.

### The exact `.uplugin` edits to adopt this

Applied to the current descriptor, cutting **0.2.0**. Five changes:

```jsonc
{
	"FileVersion": 3,
	"Version": 2,                    // was 1  — the build counter, +1 per release
	"VersionName": "0.2.0",          // was "0.1.0"
	"FriendlyName": "RUDE",
	"Description": "RAGE <-> Unreal Development Environment. Unreal as a first-class FiveM mapping DCC: import GTA V map context, author natively, export to FiveM. Free forever.",
	"Category": "RUDE",
	"CreatedBy": "Grizzy",
	"CreatedByURL": "https://github.com/GrizzyVV",
	"DocsURL": "https://github.com/GrizzyVV/RUDE#readme",       // was the bare repo URL
	"MarketplaceURL": "",                                       // was the repo URL — RUDE is not on a marketplace
	"SupportURL": "https://github.com/GrizzyVV/RUDE/issues",    // was the bare repo URL
	"EngineVersion": "5.8.0",        // NEW
	"CanContainContent": true,
	"IsBetaVersion": true,
	"IsExperimentalVersion": false,
	"Installed": false,
	"Modules": [ ... unchanged ... ],
	"Plugins": [ ... unchanged ... ]
}
```

Why `EngineVersion`, and why exactly `"5.8.0"`:

- Unreal parses it and compares it against the running engine. If they differ in
  major, minor or patch, the user gets *"The 'RUDE' plugin was designed for build 5.8.0.
  Attempt to load it anyway?"* — a warning with a way out, not a hard block. That is the
  right strength: RUDE genuinely does not work on 5.7, and the user should be told at load
  time rather than discovering it as a compile error.
- The changelist component is deliberately omitted. A version string with changelist `0` is
  treated as "no changelist", so the comparison stops after the patch component and a
  legitimate 5.8.0 build of any changelist stays compatible. Writing a changelist in would
  make every user of a different 5.8 build see a spurious warning.
- *Grounded in `Engine/Source/Runtime/Projects/Private/PluginManager.cpp`
  (`FPluginManager::IsPluginCompatible`, and the comment at its call site: "This is a soft
  requirement, so allow the user to skip over it") and
  `Runtime/Core/Private/Misc/EngineVersion.cpp` (`FEngineVersionBase::GetNewest`,
  `HasChangelist`), read from the 5.8 install this ships against.*

`MarketplaceURL` currently points at the GitHub repo, which is wrong in meaning even though
it is harmless in effect — the 5.8 plugin-browser tile does not read the field at all
(`SPluginTile.cpp` never mentions it; only the descriptor and the plugin metadata editor
form carry it). Set it to `""`, or drop the key entirely. RUDE is not on a marketplace, and
pointing the field at the repo just duplicates `DocsURL` while asserting a storefront that
does not exist. `SupportURL` and `DocsURL` are the two that should differ from each other:
one is where you read, one is where you complain.

`IsBetaVersion: true` stays until `1.0`. It is what makes the plugin browser mark RUDE as
beta, which is accurate.

---

## 2 · What a GitHub release must contain

A release exists so a stranger can install the plugin without reading the repository. If any
row below is missing, they cannot.

| Item | Required | Why |
|---|---|---|
| Tag `v<VersionName>` on the released commit | **yes** | The changelog links to it, and it is how anyone reproduces the exact tree |
| Title `RUDE v<VersionName>` | **yes** | |
| Body: the changelog section for this version, verbatim | **yes** | Nobody clicks through to `CHANGELOG.md` |
| Body: **the engine build** the binaries were compiled against | **yes, if binaries are attached** | A precompiled Unreal plugin only loads into the engine build it was built with. Omitting it produces the single most common support question there is |
| Body: **what a new user still cannot do**, with the filebase gap named | **yes** | Somebody downloading this will otherwise install it, find it does nothing, and conclude it is broken. It is not broken; it is incomplete, and saying so is cheaper than the issue thread |
| `RUDE-<version>-Source.zip` | **yes** | The universal route. Works on any 5.8 install, needs Visual Studio |
| `RUDE-<version>-UE5.8-Win64-Binary.zip` | strongly recommended | The **only** route that works with no compiler. This is what makes RUDE installable by a mapper who has never opened Unreal |
| `SHA256SUMS.txt` | recommended | Both zips, so a download can be checked |
| ⛔ QUARRY, an extractor, key material, or a link that auto-downloads any of them | **never** | RUDE is Apache-2.0 and deliberately carries no archive or crypto code. Bundling or auto-fetching a tool that does imports that exposure straight into this plugin and destroys the reason the two are separate products. Naming a companion tool in prose is fine; shipping, vendoring, submoduling or auto-downloading it is not |
| ⛔ Any Rockstar-derived data — meshes, textures, XML exports, archive contents | **never** | Including "just one test asset" |

### Both zips must unpack to exactly one folder

```
RUDE/
  RUDE.uplugin
  Source/ …
  Content/ …
  Resources/Icon128.png
  README.md  INSTALL.md  CHANGELOG.md  RELEASING.md  AGENTS.md  LICENSE  NOTICE
```

plus `Binaries/` and `Intermediate/` in the binary zip. A doubled `RUDE/RUDE/` is the most
common install failure and it is entirely preventable at packaging time — the script checks it.

---

## 3 · Cutting a release

### Before you start

- [ ] Working tree clean, on `main`, pushed.
- [ ] The build succeeds **with the editor closed** — an open editor holds a lock on the
      files the build writes and fails in a way that does not say so.
- [ ] The tools you changed have been run against real data and the result looked at.
      "It compiles" is not "it works".

### Version bump

- [ ] `RUDE.uplugin`: `Version` +1, `VersionName` set. Apply the other edits in §1 if this is
      the first release adopting the scheme.
- [ ] `RudeToolset.h`: `GetToolsetVersion()` returns the same string.
- [ ] `CHANGELOG.md`: rename the draft heading to `## [X.Y.Z] — YYYY-MM-DD`, add a fresh
      empty `## [Unreleased]`, update the compare links at the bottom.
- [ ] Rebuild, and confirm `RUDE.Run Ping` reports the new version. This is the only check
      that proves the header edit actually took — the `.uplugin` string will look right
      either way.

### Content audit — the hard walls

- [ ] `git ls-files` contains **no** `.ydr .ydd .yft .ybn .ytd .ytyp .ymap .ymt .rpf .dds`
      and no game-derived XML. (`.gitignore` covers these; check anyway — an `-f` add
      bypasses it silently.)
- [ ] No key material, no archive-reading code, no crypto.
- [ ] No extractor bundled, vendored, submoduled, or auto-downloaded.
- [ ] `NOTICE` is present and current, and is included in **both** zips. Apache-2.0 §4(d)
      requires it to travel with redistributions, and the zips *are* redistributions.
- [ ] Every third-party grant RUDE actually relies on is reproduced in `NOTICE`, verbatim,
      with its copyright line. A permissive licence is only a grant while its notice is
      retained.
- [ ] Skim `README.md`, `INSTALL.md`, `AGENTS.md` and every string the plugin **writes to
      disk** — `CreateFilebase`'s generated README and manifest, and every tool's `RudeHelp`
      — against the project's no-affiliation rule. Text the tool writes ships as surely as
      code does, and it is the part nobody re-reads.

### Documentation

- [ ] README's capability table matches what the code does *now*. It is the table people
      quote back at you; a stale ✅ is worse than a missing row.
- [ ] Every measured claim still holds, or the number is updated. A figure measured against
      an earlier build can silently become false — a "71.6% of the map imports" ceiling
      stopped being true the day fragment and dictionary import landed.
- [ ] INSTALL.md's engine and toolchain versions match what the release was built with.

### Build the artifacts

Source zip — this is just the repository at the tag, without build output:

```powershell
pwsh -File Tools\Package-Release.ps1 -Version 0.2.0 -OutDir B:\RUDE_Release
```

Binary zip — compile the plugin standalone against the target engine:

```
"<EngineDir>\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
    -Plugin="<abs>\Plugins\RUDE\RUDE.uplugin" ^
    -Package="B:\RUDE_Release\RUDE" ^
    -TargetPlatforms=Win64 ^
    -StrictIncludes
```

Two constraints the automation tool enforces, both of which fail late and confusingly if you
trip them: **`-Package` must not be inside the plugin folder**, and it **must not be inside
the engine directory**. `-StrictIncludes` builds without the shared PCH, which catches
missing `#include`s that only ever compiled by luck — worth the extra minutes on a release
build.

Then zip the produced `RUDE` folder as `RUDE-<version>-UE5.8-Win64-Binary.zip`.

- [ ] **Smoke-test the binary zip in a project that has never had RUDE in it**, on a clean
      engine install if you can reach one: unzip, enable, restart, `RUDE.Run Ping`. A
      packaged plugin that does not load is the failure this whole checklist exists to
      prevent, and it cannot be caught by inspection.

### Publish

```powershell
git tag -a v0.2.0 -m "RUDE v0.2.0"
git push origin v0.2.0
gh release create v0.2.0 `
    B:\RUDE_Release\RUDE-0.2.0-Source.zip `
    B:\RUDE_Release\RUDE-0.2.0-UE5.8-Win64-Binary.zip `
    B:\RUDE_Release\SHA256SUMS.txt `
    --title "RUDE v0.2.0" --notes-file B:\RUDE_Release\release-notes.md
```

- [ ] Download your own release into a scratch folder and install from it. Not the build
      output — **the artifact a stranger gets**. They are not always the same file.

---

## 4 · Notes for whoever does this next

- **Precompiled plugins are engine-build-specific.** A plugin compiled against a source-built
  engine will not load in a launcher install of the same version number. Build the binary
  artifact against the engine your users actually have, and name that build in the release
  notes. If you cannot, ship only the source zip and say so — that is honest and it still
  works.
- **`Resources/Icon128.png` is not optional if you care how this looks.** Unreal's plugin
  browser loads exactly that path relative to the plugin folder
  (`Engine/Plugins/Editor/PluginBrowser/…/SPluginTile.cpp`); with no file there, RUDE gets a
  generic placeholder in the one screen every new user visits.
- **Cutting a release is the only moment the docs get audited.** Nothing else in the workflow
  forces a read of the README against the code. Use it.
