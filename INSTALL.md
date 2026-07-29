# Installing RUDE

Written for someone who maps for FiveM and has never opened Unreal Engine. If you have
shipped UE plugins before, [route B](#route-b--build-from-source) in four lines is all you
need.

RUDE is an **editor plugin**: it lives inside an Unreal project and adds tools to the editor.
There is no `RUDE.exe`.

---

## Step 0 · Unreal Engine 5.8

Install **Unreal Engine 5.8 for Windows** from the Epic Games Launcher.

⚠ **5.8 exactly.** RUDE is built and tested only against 5.8, and it depends on an engine
plugin (`ToolsetRegistry`) that ships with 5.8. On a different engine version the editor
warns that the plugin was built for another build and offers to load it anyway — say no.
It is not supported and it is not going to work.

Then make a project (Games ▸ Blank is fine) and note where it lives. Everything below is
relative to that project folder — the one containing `<YourProject>.uproject`.

---

## Which route?

| | Route A — precompiled | Route B — build from source |
|---|---|---|
| Needs Visual Studio | **no** | yes |
| Works in a Blueprint-only project | yes | yes, but a C++ project is smoother |
| Works on any 5.8 install | **only the engine build it was compiled against** | yes |
| Get updates by | downloading the next release | `git pull` and rebuild |
| Time | minutes | 10–30 minutes the first time |

**Take route A if a release exists for your engine build.** Take route B otherwise, or if
you want to change RUDE.

---

## Route A — precompiled release

1. Download `RUDE-<version>-UE5.8-Win64-Binary.zip` from the
   [Releases page](https://github.com/GrizzyVV/RUDE/releases).
2. Check the release notes name **your** engine build. A precompiled Unreal plugin only
   loads into the engine build it was compiled against; a mismatch produces
   *"Binaries for the 'RUDE' plugin are missing or incompatible with the current engine
   version"* on startup. If yours is not listed, use route B — nothing is wrong with your
   install.
3. Create a `Plugins` folder next to your `.uproject` if there is not one already.
4. Unzip so that you end up with exactly this — one `RUDE` folder, with the `.uplugin`
   directly inside it:

   ```
   <YourProject>/
     <YourProject>.uproject
     Plugins/
       RUDE/
         RUDE.uplugin          <- directly inside RUDE/, not one level deeper
         Binaries/
         Content/
         Source/
         ... the rest
   ```

   ⚠ The single most common install mistake is a doubled folder —
   `Plugins/RUDE/RUDE/RUDE.uplugin`. If the editor never mentions RUDE at all, check this
   first.
5. Continue at [Step 3 · Enable it](#step-3--enable-it).

---

## Route B — build from source

### B1 · Install a compiler

Unreal builds C++ with the Microsoft toolchain. From the
[Visual Studio](https://visualstudio.microsoft.com/) installer (the **free** Community
edition is fine), install **Visual Studio 2022 version 17.8 or newer**, and tick these
workloads:

- **Desktop development with C++**
- **Game development with C++**
- **.NET desktop development** (Unreal's build tool is a .NET program)

and, under Individual components:

- **Windows 11 SDK (10.0.22621.0)**
- **MSVC v143 build tools (x64/x86)**

*Source for these numbers: `Engine/Config/Windows/Windows_SDK.json` in your own 5.8 install
— it is the file the build tool actually checks against.* It requires Visual Studio 2022
≥ 17.8 (or 2026 ≥ 18.0), an MSVC toolchain ≥ **14.38.33130**, and a Windows SDK ≥
**10.0.19041.0**, preferring **10.0.22621.0**.

⚠ **Some MSVC toolchains are explicitly banned by the engine** because they miscompile:
`14.39.x`, `14.40.x`–`14.43.x`, `14.44.0`–`14.44.35210`, and `14.50.0`–`14.50.35722`. If
your build dies with an internal compiler error or the editor crashes in code that looks
fine, update MSVC before you debug anything else. The engine prefers `14.44.35207+` or
`14.50.35717+`.

### B2 · Get the code

```
cd <YourProject>
mkdir Plugins
git clone https://github.com/GrizzyVV/RUDE.git Plugins/RUDE
```

Confirm `<YourProject>/Plugins/RUDE/RUDE.uplugin` exists. If your path has a `RUDE/RUDE`
in it, move it up one level.

### B3 · Build

**Close the Unreal editor first.** An open editor holds a lock on the very files the build
writes, and the build will fail in a way that does not say so.

Easiest: just open `<YourProject>.uproject`. Unreal notices the new module and offers to
rebuild it. Say yes and wait.

If that fails, or you want to see the errors, build explicitly:

```
"<EngineDir>\Engine\Build\BatchFiles\Build.bat" ^
    UnrealEditor Win64 Development ^
    -Project="<abs path>\<YourProject>.uproject" -WaitMutex
```

Replace `<EngineDir>` with your engine install (for a launcher install, typically
`C:\Program Files\Epic Games\UE_5.8`). Use absolute paths for the project.

If the project is Blueprint-only, right-click the `.uproject` ▸ **Generate Visual Studio
project files** first. It is not strictly required, but it makes every subsequent error
message far more useful.

---

## Step 3 · Enable it

1. Open your project in Unreal 5.8.
2. **Edit ▸ Plugins**, search `RUDE`, tick **Enabled**.
3. Unreal will warn that this enables **experimental** engine plugins. That is expected:
   RUDE depends on the engine's `ToolsetRegistry` plugin, which ships Experimental in 5.8
   and is disabled by default. Accept it — RUDE cannot register its tools without it.
4. Restart the editor when prompted.

**Only if you want AI agents to drive RUDE**, also enable **Unreal MCP**
(`ModelContextProtocol`) in the same window. It is likewise Experimental and disabled by
default. Humans, the console and the CLI do not need it.

---

## Step 4 · Check it actually loaded

In the editor's console (the box at the bottom of the Output Log; press `` ` `` if you
cannot see it), type:

```
RUDE.Run Ping
```

The Output Log should print a JSON line containing the RUDE version. Then:

```
RUDE.Panel
```

A **RUDE** tab opens listing every tool, with a field per parameter and each tool's own
plain-language description. It is also under **Window ▸ Tools ▸ RUDE**.

If `RUDE.Run` is "not recognised", the plugin did not load. In order of likelihood:
the folder is nested wrong (`Plugins/RUDE/RUDE/`); the plugin is not ticked in
Edit ▸ Plugins; you are on an engine version other than 5.8; or, on route B, the build did
not actually succeed — scroll the build output for the first error, not the last.

---

## Step 5 · You still need a filebase

RUDE is installed and does nothing useful yet, because it has no assets to read. It does
**not** open your GTA V archives — see the README's
[Why RUDE reads a folder](README.md#why-rude-reads-a-folder-and-not-your-game-archives).

You need a folder of extracted assets in the XML form, laid out per
[the folder contract](README.md#the-folder-contract), produced by an extractor you supply.
Start it with:

```
RUDE.Run CreateFilebase  <where to put it>  <your GTA V install folder>  CORE
```

which seeds the tree, shaped to your own install, reading directory names only — it opens
no archive. It also writes its own `README.md` into that folder explaining how to fill it.

**We do not yet publish an extractor.** That is the open gap in getting started, and it is
stated plainly in the README rather than buried here.

---

## Uninstalling

Delete `<YourProject>/Plugins/RUDE`. Anything RUDE imported stays in your project's
`Content` folder as ordinary Unreal assets; the material instances it created will lose
their master materials, because those ship inside the plugin.

## Updating

- Route A: delete the `Plugins/RUDE` folder and unzip the new release in its place.
- Route B: `git pull` in `Plugins/RUDE`, close the editor, rebuild.

Either way, read [CHANGELOG.md](CHANGELOG.md) first. RUDE is pre-1.0: a minor version bump
is allowed to change a tool's parameters or its output, and the changelog is where that is
called out.
