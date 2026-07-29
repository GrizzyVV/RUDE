<#
.SYNOPSIS
    Package a RUDE source release and verify everything RELEASING.md says must be true.

.DESCRIPTION
    A release checklist that only lives in a document is a checklist that gets skipped on the
    release that mattered. This script performs every item of RELEASING.md that a machine can
    decide, and REFUSES TO PACKAGE if any of them fails. It deliberately does not "fix"
    anything it finds - a version mismatch is a decision for a human, and a script that
    silently corrects one hides the fact that two sources of truth drifted.

    What it does NOT do, on purpose:
      * It does not build. Building the editor module requires the Unreal editor to be
        CLOSED (an open editor locks the files the build writes and fails in a way that does
        not say so), which is not a state a packaging script can assert. It prints the
        RunUAT command instead.
      * It does not tag or publish. It prints those commands too, so the person running them
        has read them.

    The source zip is produced with `git archive`, not by copying the working tree. That is
    the load-bearing choice in this script: git archive emits TRACKED FILES ONLY, so build
    output, local scratch files and anything untracked physically cannot reach the artifact.
    The --prefix flag guarantees the zip unpacks to exactly one RUDE/ folder, which is the
    single most common install failure (Plugins/RUDE/RUDE/RUDE.uplugin) removed by
    construction rather than by remembering.

.PARAMETER Version
    The VersionName being released, e.g. "0.2.0". Must match RUDE.uplugin, the
    GetToolsetVersion() literal in RudeToolset.h, and a CHANGELOG.md heading.

.PARAMETER OutDir
    Where the artifacts are written. Created if absent. Must be OUTSIDE the repository.

.PARAMETER AllowDirty
    Package despite uncommitted changes. For dry runs only - the resulting zip does not
    correspond to any commit, so it can never be reproduced. Never publish one.

.EXAMPLE
    .\Tools\Package-Release.ps1 -Version 0.2.0 -OutDir B:\RUDE_Release
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Version,
    [Parameter(Mandatory = $true)] [string] $OutDir,
    [switch] $AllowDirty
)

$ErrorActionPreference = 'Stop'

# The plugin root is this script's parent's parent - Tools/ sits directly under it. Derived
# rather than passed so the script cannot be pointed at the wrong tree by a stale argument.
$PluginRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

$Failures = @()
$Warnings = @()
function Fail([string] $Message) { $script:Failures += $Message; Write-Host "  FAIL  $Message" -ForegroundColor Red }
function Warn([string] $Message) { $script:Warnings += $Message; Write-Host "  WARN  $Message" -ForegroundColor Yellow }
function Pass([string] $Message) { Write-Host "  ok    $Message" -ForegroundColor DarkGray }

Write-Host ""
Write-Host "RUDE release packager - v$Version" -ForegroundColor Cyan
Write-Host "  plugin root : $PluginRoot"
Write-Host "  output      : $OutDir"
Write-Host ""

# A version string that is not MAJOR.MINOR.PATCH breaks the tag, the compare links, and
# Unreal's own version parsing. Reject it here rather than three steps later.
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version '$Version' is not MAJOR.MINOR.PATCH. See RELEASING.md section 1."
}
if ($OutDir.ToLowerInvariant().StartsWith($PluginRoot.ToLowerInvariant())) {
    throw "OutDir must be outside the repository, or the artifact ends up inside the next artifact."
}

# ---------------------------------------------------------------------------------------
# 1. Repository state
# ---------------------------------------------------------------------------------------
Write-Host "[1] repository state"
Push-Location $PluginRoot
try {
    $Status = & git status --porcelain
    if ($LASTEXITCODE -ne 0) { throw "not a git repository: $PluginRoot" }

    if ($Status) {
        if ($AllowDirty) {
            Warn "working tree is DIRTY and -AllowDirty was passed. This artifact matches no commit - do not publish it."
        }
        else {
            Fail "working tree is dirty. Commit or stash first (git archive packages HEAD, not your edits, so the zip would silently NOT contain them)."
        }
    }
    else { Pass "working tree clean" }

    $Branch = (& git rev-parse --abbrev-ref HEAD).Trim()
    if ($Branch -ne 'main') { Warn "on branch '$Branch', not 'main'" } else { Pass "on main" }

    $Head = (& git rev-parse --short HEAD).Trim()
    Pass "HEAD = $Head"

    $ExistingTag = & git tag --list "v$Version"
    if ($ExistingTag) { Fail "tag v$Version already exists. A released version is never re-cut - bump instead." }

    # ---------------------------------------------------------------------------------------
    # 2. Content audit - the hard walls. Checked against the INDEX, not the disk, because the
    #    index is what git archive will ship. A file can sit in the working tree ignored and
    #    harmless; the same file tracked is a published byte of somebody else's copyright.
    # ---------------------------------------------------------------------------------------
    Write-Host "[2] content audit (tracked files only - that is what ships)"
    $Tracked = & git ls-files

    # RAGE asset extensions and archive containers. .gitignore already covers these, but
    # `git add -f` bypasses it without a word, and this is the wall that must not be crossed.
    $Forbidden = @('.ydr', '.ydd', '.yft', '.ybn', '.ytd', '.ytyp', '.ymap', '.ymt', '.ysc', '.rpf', '.dds', '.awc', '.dat')
    $Offenders = @()
    foreach ($File in $Tracked) {
        foreach ($Ext in $Forbidden) {
            # Matches both `foo.ydr` and the doubled `foo.ydr.xml` interchange form.
            if ($File.ToLowerInvariant().Contains($Ext + '.') -or $File.ToLowerInvariant().EndsWith($Ext)) {
                $Offenders += $File
                break
            }
        }
    }
    if ($Offenders.Count -gt 0) {
        Fail "game-derived data is TRACKED - this must never ship:`n        $($Offenders -join "`n        ")"
    }
    else { Pass "no game-derived data tracked" }

    # RUDE must never bundle, vendor or submodule an archive-reading tool: doing so imports
    # that tool's legal exposure into this Apache-2.0 plugin, which is the entire reason the
    # two are separate products.
    if (Test-Path (Join-Path $PluginRoot '.gitmodules')) {
        Fail ".gitmodules exists. RUDE must not submodule an extractor or any archive/crypto code."
    }
    else { Pass "no submodules" }

    # ---------------------------------------------------------------------------------------
    # 3. Required files - a release without these is not installable by a stranger
    # ---------------------------------------------------------------------------------------
    Write-Host "[3] required files"
    $Required = @('RUDE.uplugin', 'LICENSE', 'NOTICE', 'README.md', 'INSTALL.md', 'CHANGELOG.md', 'RELEASING.md', 'AGENTS.md')
    foreach ($Name in $Required) {
        if ($Tracked -contains $Name) { Pass $Name }
        else { Fail "$Name is missing or untracked - it will not be in the zip" }
    }
    # Apache-2.0 section 4(d): the NOTICE must travel with redistributions, and a release zip
    # IS a redistribution. git archive carries it automatically once it is tracked.
    if ($Tracked -contains 'NOTICE') { Pass "NOTICE will travel with the artifact (Apache-2.0 4(d))" }

    if ($Tracked -contains 'Resources/Icon128.png') {
        Pass "Resources/Icon128.png present"
    }
    else {
        Warn "Resources/Icon128.png missing - Unreal's plugin browser loads exactly that path, so RUDE shows a generic placeholder in the one screen every new user visits."
    }

    # ---------------------------------------------------------------------------------------
    # 4. Version agreement across all three sources of truth
    # ---------------------------------------------------------------------------------------
    Write-Host "[4] version agreement"

    $UpluginPath = Join-Path $PluginRoot 'RUDE.uplugin'
    $Uplugin = Get-Content -Raw -Path $UpluginPath | ConvertFrom-Json

    if ($Uplugin.VersionName -eq $Version) { Pass "RUDE.uplugin VersionName = $Version" }
    else { Fail "RUDE.uplugin VersionName is '$($Uplugin.VersionName)', expected '$Version'" }

    if ($Uplugin.Version -is [int] -or $Uplugin.Version -is [double]) {
        Pass "RUDE.uplugin Version (build counter) = $($Uplugin.Version)"
        Write-Host "        remember: the build counter must be strictly greater than the last RELEASED one." -ForegroundColor DarkGray
    }
    else { Fail "RUDE.uplugin Version is not a number" }

    if ($Uplugin.EngineVersion) { Pass "EngineVersion = $($Uplugin.EngineVersion)" }
    else { Warn "no EngineVersion declared - users on the wrong engine get a compile error instead of a clear warning at load. See RELEASING.md section 1." }

    # The literal agents and the CLI actually read. The .uplugin can be perfect while this is
    # stale, and nothing in the editor would show it.
    $HeaderPath = Join-Path $PluginRoot 'Source/RudeEditor/Private/RudeToolset.h'
    $Header = Get-Content -Raw -Path $HeaderPath
    $Match = [regex]::Match($Header, 'GetToolsetVersion\s*\(\s*\)\s*const\s+override\s*\{\s*return\s+TEXT\("([^"]+)"\)')
    if (-not $Match.Success) {
        Fail "could not find the GetToolsetVersion() literal in RudeToolset.h - the check cannot be trusted, so it fails rather than passing quietly"
    }
    elseif ($Match.Groups[1].Value -eq $Version) {
        Pass "GetToolsetVersion() = $Version"
    }
    else {
        Fail "GetToolsetVersion() returns '$($Match.Groups[1].Value)', expected '$Version'. This is the string every agent and CLI caller reads."
    }

    # ---------------------------------------------------------------------------------------
    # 5. Changelog section - also the release notes body
    # ---------------------------------------------------------------------------------------
    Write-Host "[5] changelog"
    $ChangelogPath = Join-Path $PluginRoot 'CHANGELOG.md'
    $Lines = Get-Content -Path $ChangelogPath
    $Start = -1
    $End = $Lines.Count
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match ('^##\s+\[' + [regex]::Escape($Version) + '\]')) { $Start = $i; continue }
        if ($Start -ge 0 -and $Lines[$i] -match '^##\s+\[') { $End = $i; break }
    }
    $NotesBody = $null
    if ($Start -lt 0) {
        Fail "CHANGELOG.md has no '## [$Version]' section"
    }
    else {
        if ($Lines[$Start] -match 'unreleased') {
            Fail "the [$Version] heading still says 'unreleased'. Date it: ## [$Version] - $(Get-Date -Format 'yyyy-MM-dd')"
        }
        $NotesBody = ($Lines[$Start..($End - 1)] -join "`n").Trim()
        Pass "found [$Version] section, $($End - $Start) lines"
    }

    # ---------------------------------------------------------------------------------------
    # 6. Verdict, then package
    # ---------------------------------------------------------------------------------------
    Write-Host ""
    if ($Failures.Count -gt 0) {
        Write-Host "REFUSING TO PACKAGE - $($Failures.Count) failure(s):" -ForegroundColor Red
        foreach ($F in $Failures) { Write-Host "  - $F" -ForegroundColor Red }
        Write-Host ""
        exit 1
    }
    if ($Warnings.Count -gt 0) {
        Write-Host "$($Warnings.Count) warning(s) - readable, not fatal:" -ForegroundColor Yellow
        foreach ($W in $Warnings) { Write-Host "  - $W" -ForegroundColor Yellow }
        Write-Host ""
    }

    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

    $SourceZip = Join-Path $OutDir "RUDE-$Version-Source.zip"
    Write-Host "[6] packaging"

    # --prefix guarantees a single top-level RUDE/ folder. Without it, unzipping scatters the
    # plugin across the user's Plugins folder or nests it one level too deep - the failure
    # this flag makes impossible is the one users actually hit.
    & git archive --format=zip --prefix=RUDE/ --output="$SourceZip" HEAD
    if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
    Pass "wrote $SourceZip ($([math]::Round((Get-Item $SourceZip).Length / 1MB, 2)) MB)"

    $NotesPath = Join-Path $OutDir 'release-notes.md'
    $Notes = @()
    $Notes += $NotesBody
    $Notes += ""
    $Notes += "---"
    $Notes += ""
    $Notes += "## Installing"
    $Notes += ""
    $Notes += "See [INSTALL.md](https://github.com/GrizzyVV/RUDE/blob/v$Version/INSTALL.md). RUDE is an Unreal Engine **5.8** editor plugin - it is not a standalone program."
    $Notes += ""
    $Notes += "- ``RUDE-$Version-Source.zip`` - works on any 5.8 install, needs Visual Studio 2022 (17.8+) to build."
    $Notes += "- ``RUDE-$Version-UE5.8-Win64-Binary.zip`` - no compiler needed, but it only loads into the engine build it was compiled against. **State that build here before publishing.**"
    $Notes += ""
    $Notes += "Unzip so you get ``<YourProject>/Plugins/RUDE/RUDE.uplugin`` - one RUDE folder, not two."
    $Notes += ""
    $Notes += "## What you still cannot do"
    $Notes += ""
    $Notes += "RUDE reads a folder of GTA V assets in XML form. **It does not open ``.rpf`` archives and ships no extractor**, so you must produce that folder yourself with a tool you already have. There is no published extractor yet. If you install RUDE and it appears to do nothing, this is why - see the README's limits table before opening an issue."
    $Notes += ""
    ($Notes -join "`n") | Set-Content -Path $NotesPath -Encoding UTF8
    Pass "wrote $NotesPath (review it - the engine build line is a placeholder)"

    $SumsPath = Join-Path $OutDir 'SHA256SUMS.txt'
    $Sums = @()
    foreach ($Artifact in (Get-ChildItem -Path $OutDir -Filter "RUDE-$Version-*.zip")) {
        $Hash = (Get-FileHash -Path $Artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $Sums += "$Hash  $($Artifact.Name)"
    }
    ($Sums -join "`n") | Set-Content -Path $SumsPath -Encoding UTF8
    Pass "wrote $SumsPath ($($Sums.Count) artifact(s))"

    # ---------------------------------------------------------------------------------------
    # 7. What the human still has to do. Printed, not performed - each of these either needs
    #    the editor closed or is irreversible once published.
    # ---------------------------------------------------------------------------------------
    Write-Host ""
    Write-Host "STILL YOURS TO DO - none of this can be asserted by a script:" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  1. Build the binary artifact (editor CLOSED):" -ForegroundColor White
    Write-Host "       <EngineDir>\Engine\Build\BatchFiles\RunUAT.bat BuildPlugin ^"
    Write-Host "           -Plugin=`"$PluginRoot\RUDE.uplugin`" ^"
    Write-Host "           -Package=`"$OutDir\RUDE`" -TargetPlatforms=Win64 -StrictIncludes"
    Write-Host "     then zip that folder as RUDE-$Version-UE5.8-Win64-Binary.zip and re-run"
    Write-Host "     this script so SHA256SUMS.txt covers it."
    Write-Host ""
    Write-Host "  2. Smoke-test the BINARY zip in a project that has never had RUDE in it:" -ForegroundColor White
    Write-Host "     unzip, enable, restart, 'RUDE.Run Ping'. A packaged plugin that does not"
    Write-Host "     load cannot be caught by inspection - only by installing it."
    Write-Host ""
    Write-Host "  3. Name the exact engine build in release-notes.md. A precompiled Unreal" -ForegroundColor White
    Write-Host "     plugin only loads into the build it was compiled against."
    Write-Host ""
    Write-Host "  4. Publish:" -ForegroundColor White
    Write-Host "       git tag -a v$Version -m `"RUDE v$Version`""
    Write-Host "       git push origin v$Version"
    Write-Host "       gh release create v$Version `"$SourceZip`" `"$SumsPath`" ``"
    Write-Host "           --title `"RUDE v$Version`" --notes-file `"$NotesPath`""
    Write-Host ""
    Write-Host "  5. Download your OWN release and install from it - not from the build" -ForegroundColor White
    Write-Host "     output. They are not always the same file."
    Write-Host ""
}
finally {
    Pop-Location
}
