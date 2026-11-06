#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Builds, publishes and PACKAGES a Harc's Build release zip. Use this instead of packaging by hand.

.DESCRIPTION
    ⚠⚠ THE MISTAKE THIS SCRIPT EXISTS TO PREVENT ⚠⚠

    HaloCheckpointManager.exe in the release zip must be the SINGLE-FILE PUBLISH output
    (~4.5 MB), NOT the plain build output (~167 KB).

    The build's post-build step does:
        copy $(TargetDir)HCMExternal.exe $(TargetDir)HaloCheckpointManager.exe
    which produces a 167 KB framework-dependent STUB. That stub needs HCMExternal.dll beside it -
    and HCMExternal.dll is NOT in the release manifest, so the stub cannot start. A zip built from
    the plain build output looks complete, is the right file count, and is broken.

    V5.10.16 and V5.10.17 were both shipped this way and had to be re-packaged.

    The real exe comes from `dotnet publish -p:PublishSingleFile=true`, which bundles HCMExternal.dll
    into the exe. That is why it is ~4.5 MB and why nothing else needs to ship beside it.

    This script publishes first, packages from the publish output, and HARD FAILS if the exe is
    smaller than the sanity floor. Do not remove that check.

    BUILD-ONLY MODE (-BuildOnly): compiles the native DLL(s) and stops - no publish, no zip, no version
    check, no tag needed. Optionally applies a .patch first (-Patch) and copies the fresh DLL(s) into an
    install folder (-DeployTo), keeping a timestamped backup of whatever was there.

.PARAMETER Tag
    Release tag, e.g. V5.10.18. Used only to name the staging folder. Release mode only.

.PARAMETER SkipBuild
    Skip the HCMInternal native build (use when it is already current and the game is closed). Release mode only.

.PARAMETER BuildOnly
    Just compile. Builds HCMInternal (add -All for HCMInterproc and HCMSpeedhack too) and reports where the
    DLL landed: HCMExternal\bin\x64\Release\net7.0-windows\.

.PARAMETER All
    With -BuildOnly: build all three native targets instead of HCMInternal alone.

.PARAMETER Patch
    Path to a .patch file (git diff format) to apply to the source before building. Skipped if it is already
    applied, so re-running the same command is safe. Needs git on PATH. Works in either mode.

.PARAMETER Toolset
    Override the projects' C++ toolset (they ask for v143 = VS 2022), e.g. -Toolset v145 to build with Visual
    Studio 2026's own compiler when the v143 tools are not installed. Works for releases and -BuildOnly; without
    it, a missing v143 falls back to the newest toolset this MSBuild has.

.PARAMETER OutputDir
    Where finished builds go. Default:
    D:\Games\steamapps\common\Halo The Master Chief Collection\HCM\HaloCheckpointManager_Spooky_Ghost_Build
    Release mode: after the zip is verified, the staged release files and the zip are copied there (created if missing).
    Your own HCMInternalConfig.xml, Saves and Logs in that folder are kept - the shipped config is only copied when the
    folder has none. -BuildOnly: used as -DeployTo when that is not given and the folder exists.

.PARAMETER DeployTo
    With -BuildOnly: folder of the HCM install you run (the one holding HaloCheckpointManager.exe). The built
    DLL(s) are copied there; the old ones are renamed to <name>.bak-<yyyyMMdd-HHmmss> first.

.PARAMETER VcpkgBaseline
    With -BuildOnly: the vcpkg commit written as "builtin-baseline" into each built project's vcpkg.json when it has
    none. Visual Studio's bundled vcpkg refuses a manifest without one (a classic vcpkg clone does not care, which is
    why the repo never needed it). Default = the 2026.07.29 registry release.

.PARAMETER KeepSourceVersion
    With -BuildOnly -DeployTo: do NOT re-stamp the built DLL(s). By default the build is stamped with the version of
    the HaloCheckpointManager.exe already in the deploy folder, because HCMExternal refuses to start when
    HCMInternal.dll's file version differs from its own ("wrong file version"). The .rc files are restored afterwards.

.PARAMETER SourceRepo
    Where to look up the submodule commits when this folder is not a git clone (a GitHub "Download ZIP" has empty
    submodule folders - safetyhook, type_safe, ImGuiColorTextEdit). Only the commit pointers are read from it.

.EXAMPLE
    .\make-release.ps1 -Tag V5.10.18
    gh release create V5.10.18 .\artifacts\V5.10.18\1-HaloCheckpointManager_Harcs_Build_Windows-x64.zip ...

.EXAMPLE
    .\make-release.ps1 -BuildOnly -Patch .\believed-objects-and-chase-hotkey.patch -DeployTo "D:\Tools\HCM"
    Applies the patch, builds HCMInternal.dll, and drops it into D:\Tools\HCM (old one backed up).

.EXAMPLE
    .\make-release.ps1 -BuildOnly
    Just rebuilds HCMInternal.dll from the current source.
#>
[CmdletBinding(DefaultParameterSetName = 'Release')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Release')][string]$Tag,
    [Parameter(ParameterSetName = 'Release')][switch]$SkipBuild,
    [Parameter(Mandatory = $true, ParameterSetName = 'BuildOnly')][switch]$BuildOnly,
    [Parameter(ParameterSetName = 'BuildOnly')][switch]$All,
    [Parameter(ParameterSetName = 'BuildOnly')][string]$DeployTo,
    [Parameter(ParameterSetName = 'BuildOnly')][switch]$KeepSourceVersion,
    [string]$Toolset,
    # where finished builds go (release: the staged files + zip; -BuildOnly: the default -DeployTo)
    [string]$OutputDir = 'D:\Games\steamapps\common\Halo The Master Chief Collection\HCM\HaloCheckpointManager_Spooky_Ghost_Build',
    [Parameter(ParameterSetName = 'BuildOnly')][string]$VcpkgBaseline = '9e593bb18ea69cc5095e012465dcd675a822ed0d',   # vcpkg 2026.07.29
    [string]$Patch,
    [string]$SourceRepo = 'https://github.com/harctheshark/HaloCheckpointManager---Harcs-Build.git'
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$out  = Join-Path $repo "HCMExternal\bin\x64\Release\net7.0-windows"
$pub  = Join-Path $out "win-x64\publish"

# ⚠ The exe must be the single-file publish. 4,509,629 B at time of writing; the floor is deliberately
# generous so a legitimately growing exe does not trip it, while the 167 KB stub always does.
$EXE_MIN_BYTES = 1MB

# The exact 8 files a release ships. Anything else staged is a bug, not a bonus.
$FROM_PUBLISH = @{ 'HCMExternal.exe' = 'HaloCheckpointManager.exe' }   # published name -> shipped name
$FROM_BUILD   = @(
    'HCMHotkeyConfig.xml',
    'HCMInternal.dll',
    'HCMInternalConfig.xml',
    'HCMInterproc.dll',
    'HCMSpeedhack.dll',
    'InternalPointerData.xml'
)
$FROM_REPO    = @('THIRD-PARTY-LICENSES.md')

function Fail($msg) { Write-Host "`nRELEASE ABORTED: $msg" -ForegroundColor Red; exit 1 }

# ---- 0. nothing may hold the outputs -------------------------------------------------------------
# What locks the outputs is an HCM module LOADED in a process, not a game merely running: HCMInternal stays
# resident in the game it was injected into, and HCM itself holds its own files. So HCM's processes always block,
# and a game blocks only when an HCM*.dll is loaded in it - or when its module list cannot be read (fail safe).
# (A running game WITHOUT HCM injected used to block too, which forced closing MCC for no reason.)
$blockers = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'HCM|HaloCheckpoint' })
foreach ($g in @(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -match 'halo|MCC' -and $_.ProcessName -notmatch 'HCM|HaloCheckpoint' })) {
    try {
        if (@($g.Modules | Where-Object { $_.ModuleName -match '^HCM' }).Count -gt 0) { $blockers += $g }
    } catch { $blockers += $g }
}
if ($blockers.Count -gt 0) {
    Fail ("close these first (they lock HCMInternal.dll): " + (($blockers | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', '))
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "vswhere not found" }
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (-not $msbuild) { Fail "MSBuild not found" }

# Which C++ toolsets can THIS MSBuild actually use? It only looks in its own VC\v1xx\Platforms\x64\PlatformToolsets,
# so a v143 installed for another VS version (or not registered there) still gives MSB8020. If v143 is not in that
# list and no -Toolset was given, fall back to the newest one that is. Returns the extra MSBuild arguments.
function Get-ToolsetArgs {
    $vsRoot  = $msbuild.Substring(0, $msbuild.IndexOf('\MSBuild\', [StringComparison]::OrdinalIgnoreCase))   # install root
    $vcDir   = Get-ChildItem (Join-Path $vsRoot 'MSBuild\Microsoft\VC') -Directory -Filter 'v*' -ErrorAction SilentlyContinue |
               Sort-Object { [int]($_.Name.TrimStart('v')) } -Descending | Select-Object -First 1
    $toolsets = @()
    if ($vcDir) {
        $toolsets = @(Get-ChildItem (Join-Path $vcDir.FullName 'Platforms\x64\PlatformToolsets') -Directory -ErrorAction SilentlyContinue |
                      Where-Object { $_.Name -match '^v\d+$' } | ForEach-Object { $_.Name } |
                      Sort-Object { [int]($_.TrimStart('v')) } -Descending)
    }
    Write-Host ("      MSBuild: {0}" -f $msbuild) -ForegroundColor DarkGray
    Write-Host ("      x64 toolsets it can use: {0}" -f $(if ($toolsets) { $toolsets -join ', ' } else { '(none found)' })) -ForegroundColor DarkGray
    $use = $script:Toolset
    if (-not $use -and $toolsets.Count -gt 0 -and $toolsets -notcontains 'v143') {
        $use = $toolsets[0]
        Write-Host "      v143 (VS 2022) is not usable by this MSBuild - building with $use instead" -ForegroundColor Yellow
    }
    if ($use) { Write-Host "      toolset override: $use" -ForegroundColor Yellow; return @("/p:PlatformToolset=$use") }
    return @()
}

# ---- optional: apply a patch to the source -------------------------------------------------------
if ($Patch) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "-Patch needs git on PATH (https://git-scm.com/download/win)" }
    if (-not (Test-Path $Patch)) { Fail "patch not found: $Patch" }
    $patchPath = (Resolve-Path $Patch).Path
    Push-Location $repo
    # git writes its "does not apply" chatter to stderr; under Windows PowerShell 5.1 with 'Stop' that would
    # throw instead of just setting $LASTEXITCODE, so relax it for this block only
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        # already applied? (the reverse applies cleanly) -> nothing to do, so re-running is harmless
        & git apply --reverse --check --ignore-whitespace $patchPath 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[patch] $([IO.Path]::GetFileName($patchPath)) is already applied - skipping" -ForegroundColor DarkGray
        } else {
            $applied = $false
            # plain first, then tolerant of CRLF/LF differences, then a 3-way merge (needs a git repo)
            foreach ($opts in @(@(), @('--ignore-whitespace'), @('--3way', '--ignore-whitespace'))) {
                & git apply --check @opts $patchPath 2>$null
                if ($LASTEXITCODE -eq 0) {
                    & git apply @opts $patchPath
                    if ($LASTEXITCODE -eq 0) { $applied = $true; break }
                }
            }
            if (-not $applied) {
                Write-Host "`ngit's report for the plain apply:" -ForegroundColor Yellow
                & git apply --check --ignore-whitespace $patchPath
                Fail "patch does not apply - the files it touches have changed since it was made (see above)"
            }
            Write-Host "[patch] applied $([IO.Path]::GetFileName($patchPath))" -ForegroundColor Green
        }
    } finally { $ErrorActionPreference = $eap; Pop-Location }
}

# ---- submodules (safetyhook, type_safe, ImGuiColorTextEdit) ----------------------------------------
# A GitHub "Download ZIP" leaves every submodule folder EMPTY, which fails the build at
# `#include "safetyhook.hpp"`. Fill them: a real clone runs `git submodule update`; otherwise the exact commits
# are looked up in -SourceRepo (a blobless, no-checkout clone: only the tree pointers are read) and each
# submodule is fetched at that commit. If the lookup fails, the submodule's default branch is used, with a warning.
function Ensure-Submodules {
    $gm = Join-Path $repo '.gitmodules'
    if (-not (Test-Path $gm)) { return }
    $subs = @()
    $cur = $null
    foreach ($line in Get-Content $gm) {
        if ($line -match '^\s*\[submodule') { $cur = @{}; $subs += $cur }
        elseif ($cur -and $line -match '^\s*path\s*=\s*(.+?)\s*$') { $cur.path = $Matches[1] }
        elseif ($cur -and $line -match '^\s*url\s*=\s*(.+?)\s*$')  { $cur.url  = $Matches[1] }
    }
    $missing = @($subs | Where-Object {
        $_.path -and $_.url -and -not (Get-ChildItem (Join-Path $repo $_.path) -Force -ErrorAction SilentlyContinue | Select-Object -First 1)
    })
    if ($missing.Count -eq 0) { return }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Fail ("submodule folders are empty ({0}) and git is not on PATH to fetch them" -f (($missing | ForEach-Object { $_.path }) -join ', '))
    }
    Write-Host ("[submodules] empty: {0}" -f (($missing | ForEach-Object { $_.path }) -join ', ')) -ForegroundColor Cyan

    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        & git -C $repo rev-parse --is-inside-work-tree 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) {
            & git -C $repo submodule update --init --recursive
            if ($LASTEXITCODE -ne 0) { Fail "git submodule update failed" }
            return
        }

        # not a clone: read the pinned commits from the source repo without downloading its files
        $pins = @{}
        $probe = Join-Path ([IO.Path]::GetTempPath()) ("hcm-pins-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
        Write-Host "      looking up pinned commits in $SourceRepo ..." -ForegroundColor DarkGray
        $prompt = $env:GIT_TERMINAL_PROMPT; $env:GIT_TERMINAL_PROMPT = '0'   # a private repo fails fast instead of waiting for a login
        & git clone --quiet --depth 1 --filter=blob:none --no-checkout $SourceRepo $probe 2>$null
        $env:GIT_TERMINAL_PROMPT = $prompt
        if ($LASTEXITCODE -eq 0) {
            foreach ($s in $missing) {
                $entry = & git -C $probe ls-tree HEAD -- ($s.path -replace '\\', '/') 2>$null
                if ($entry -match '^160000 commit ([0-9a-f]{40})') { $pins[$s.path] = $Matches[1] }
            }
        } else {
            Write-Host "      could not read $SourceRepo (private, offline, or moved)" -ForegroundColor Yellow
        }
        if (Test-Path $probe) { Remove-Item $probe -Recurse -Force -ErrorAction SilentlyContinue }

        foreach ($s in $missing) {
            $dst = Join-Path $repo $s.path
            if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
            $sha = $pins[$s.path]
            if ($sha) {
                Write-Host ("      {0} @ {1}" -f $s.path, $sha.Substring(0, 12)) -ForegroundColor DarkGray
                & git init --quiet $dst
                & git -C $dst fetch --quiet --depth 1 $s.url $sha
                if ($LASTEXITCODE -eq 0) { & git -C $dst checkout --quiet FETCH_HEAD }
                if ($LASTEXITCODE -ne 0) { Remove-Item $dst -Recurse -Force; $sha = $null }
            }
            if (-not $sha) {
                Write-Host ("      {0}: pinned commit unknown - using the default branch of {1}" -f $s.path, $s.url) -ForegroundColor Yellow
                & git clone --quiet --depth 1 $s.url $dst
                if ($LASTEXITCODE -ne 0) { Fail "could not clone $($s.url)" }
            }
            # nested submodules (type_safe needs external/debug_assert)
            if (Test-Path (Join-Path $dst '.gitmodules')) { & git -C $dst submodule update --init --recursive --depth 1 }
            if ($LASTEXITCODE -ne 0) { Fail "nested submodules of $($s.path) failed" }
        }
    } finally { $ErrorActionPreference = $eap }
}
Ensure-Submodules

if ($PSCmdlet.ParameterSetName -eq 'BuildOnly' -and -not $DeployTo -and $OutputDir -and (Test-Path $OutputDir -PathType Container)) {
    $DeployTo = $OutputDir
    Write-Host "      deploying to the output folder: $DeployTo" -ForegroundColor DarkGray
}

# ---- BUILD-ONLY MODE: compile, optionally deploy, stop ---------------------------------------------
if ($BuildOnly) {
    $targets = if ($All) { @('HCMInternal', 'HCMInterproc', 'HCMSpeedhack') } else { @('HCMInternal') }
    $started = Get-Date
    Write-Host ("[build] {0} (Release x64) ..." -f ($targets -join ', ')) -ForegroundColor Cyan
    $targetArg = '/t:' + ($targets -join ';')
    # Visual Studio's bundled vcpkg needs a "builtin-baseline" in every manifest it installs from (and git on PATH to
    # fetch the registry at that commit). Add one where missing; manifests that already pin one are left alone.
    foreach ($t in $targets) {
        $manifest = Join-Path $repo "$t\vcpkg.json"
        if (-not (Test-Path $manifest)) { continue }
        $json = [IO.File]::ReadAllText($manifest)
        if ($json -match '"builtin-baseline"') { continue }
        $brace = $json.IndexOf('{')
        if ($brace -lt 0) { Fail "$manifest is not a JSON object" }
        $json = $json.Substring(0, $brace + 1) + "`r`n  `"builtin-baseline`": `"$VcpkgBaseline`"," + $json.Substring($brace + 1)
        [IO.File]::WriteAllText($manifest, $json, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "      $t\vcpkg.json: added builtin-baseline $VcpkgBaseline" -ForegroundColor Yellow
    }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Host "      warning: git is not on PATH - Visual Studio's vcpkg needs it to fetch packages" -ForegroundColor Yellow
    }

    # HCMExternal.exe only starts if every HCM*.dll carries ITS version. When deploying into an existing install
    # whose exe is a different version than this source, stamp the build with the exe's version (the .rc files
    # are put back afterwards; only HCMExternal's build runs updateVersionInfo.ps1, so nothing else rewrites them).
    $rcBackup = @{}
    if ($DeployTo -and -not $KeepSourceVersion) {
        $installedExe = Join-Path $DeployTo 'HaloCheckpointManager.exe'
        if (Test-Path $installedExe) {
            $vi = [Diagnostics.FileVersionInfo]::GetVersionInfo($installedExe)
            $exeVer = "$($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart)"
            foreach ($t in $targets) {
                $rc = Join-Path $repo "$t\$t.rc"
                if (-not (Test-Path $rc)) { continue }
                $orig = [IO.File]::ReadAllText($rc)
                $m = [regex]::Match($orig, '\bFILEVERSION\s+([0-9]+),([0-9]+),([0-9]+),([0-9]+)')
                $srcVer = if ($m.Success) { '{0}.{1}.{2}.{3}' -f $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value, $m.Groups[4].Value } else { '?' }
                if ($srcVer -eq $exeVer) { continue }
                $comma = $exeVer.Replace('.', ',')
                $new = [regex]::Replace($orig, '\bFILEVERSION\b[^\r\n]*', "FILEVERSION $comma")
                $new = [regex]::Replace($new, '\bPRODUCTVERSION\b[^\r\n]*', "PRODUCTVERSION $comma")
                $new = [regex]::Replace($new, 'VALUE "FileVersion"[^\r\n]*', "VALUE ""FileVersion"",  ""$exeVer""")
                $new = [regex]::Replace($new, 'VALUE "ProductVersion"[^\r\n]*', "VALUE ""ProductVersion"",  ""$exeVer""")
                $rcBackup[$rc] = $orig
                [IO.File]::WriteAllText($rc, $new, (New-Object System.Text.UTF8Encoding($false)))
                Write-Host "      $t.dll stamped $exeVer to match your HaloCheckpointManager.exe (source is $srcVer)" -ForegroundColor Yellow
            }
        }
    }

    $extra = @(Get-ToolsetArgs)
    & $msbuild (Join-Path $repo 'HaloCheckpointManager.sln') $targetArg /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal @extra
    $buildCode = $LASTEXITCODE
    foreach ($rc in $rcBackup.Keys) { [IO.File]::WriteAllText($rc, $rcBackup[$rc], (New-Object System.Text.UTF8Encoding($false))) }
    if ($buildCode -ne 0) {
        Fail ("build failed - the first 'error' line above is the one to look at. If it mentions vcpkg or a missing " `
            + "boost/imgui/plog header, run 'vcpkg integrate install' once (Visual Studio's copy lives in " `
            + "<VS install>\VC\vcpkg) and build again.")
    }

    $built = @()
    foreach ($t in $targets) {
        $dll = Join-Path $out "$t.dll"
        if (-not (Test-Path $dll)) { Fail "build reported success but $dll is missing" }
        $item = Get-Item $dll
        if ($item.LastWriteTime -lt $started.AddSeconds(-5)) {
            Write-Host "      note: $t.dll was up to date (not relinked)" -ForegroundColor DarkGray
        }
        Write-Host ("      {0,-18} {1,14:N0} bytes  {2}" -f $item.Name, $item.Length, $item.LastWriteTime) -ForegroundColor DarkGray
        $built += $item
    }

    if ($DeployTo) {
        if (-not (Test-Path $DeployTo -PathType Container)) { Fail "deploy folder not found: $DeployTo" }
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        foreach ($item in $built) {
            $dst = Join-Path $DeployTo $item.Name
            if ((Test-Path $dst) -and ((Resolve-Path $dst).Path -eq $item.FullName)) {
                Write-Host "      $($item.Name): deploy folder IS the build folder - nothing to copy" -ForegroundColor DarkGray
                continue
            }
            if (Test-Path $dst) {
                Rename-Item $dst "$($item.Name).bak-$stamp"
                Write-Host "      backed up old $($item.Name) -> $($item.Name).bak-$stamp" -ForegroundColor DarkGray
            }
            Copy-Item $item.FullName $dst -Force
            Write-Host "[deploy] $($item.Name) -> $DeployTo" -ForegroundColor Green
        }
        # HCMInternal reads its addresses from InternalPointerData.xml next to the exe (this fork never downloads it).
        # A DLL built from newer source needs the matching XML, or features it added fail with "pointerData was null".
        if ($targets -contains 'HCMInternal') {
            $xmlSrc = Join-Path $repo 'HCMInternal\InternalPointerData.xml'
            $xmlDst = Join-Path $DeployTo 'InternalPointerData.xml'
            if (Test-Path $xmlSrc) {
                $same = (Test-Path $xmlDst) -and ((Get-FileHash $xmlSrc).Hash -eq (Get-FileHash $xmlDst).Hash)
                if (-not $same) {
                    if (Test-Path $xmlDst) {
                        Rename-Item $xmlDst "InternalPointerData.xml.bak-$stamp"
                        Write-Host "      backed up old InternalPointerData.xml -> InternalPointerData.xml.bak-$stamp" -ForegroundColor DarkGray
                    }
                    Copy-Item $xmlSrc $xmlDst -Force
                    Write-Host "[deploy] InternalPointerData.xml -> $DeployTo" -ForegroundColor Green
                }
            }
        }
    }

    Write-Host "`nOK" -ForegroundColor Green
    Write-Host "  built: $out"
    if (-not $DeployTo) { Write-Host "  copy HCMInternal.dll from there next to your HaloCheckpointManager.exe (or re-run with -DeployTo <folder>)" }
    exit 0
}

# ---- 1. native build -----------------------------------------------------------------------------
if (-not $SkipBuild) {
    # ⚠ ALL THREE native targets, not just HCMInternal. A version bump edits every HCM*.rc, but a target
    # that is not rebuilt keeps the OLD version resource - so V5.10.18 was about to ship with
    # HCMInterproc.dll and HCMSpeedhack.dll still stamped 5.10.17 while their .rc files said 5.10.18.
    # Caught by the version check below, which is why it exists.
    Write-Host "[1/4] Building HCMInternal, HCMInterproc, HCMSpeedhack ..." -ForegroundColor Cyan
    $extra = @(Get-ToolsetArgs)
    & $msbuild (Join-Path $repo 'HaloCheckpointManager.sln') /t:"HCMInternal;HCMInterproc;HCMSpeedhack" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal @extra
    if ($LASTEXITCODE -ne 0) { Fail "native build failed" }
} else { Write-Host "[1/4] Skipping native build (-SkipBuild)" -ForegroundColor DarkGray }

# ---- 2. THE STEP THAT GETS FORGOTTEN -------------------------------------------------------------
Write-Host "[2/4] Publishing HCMExternal (single-file) ..." -ForegroundColor Cyan
Push-Location $repo
& dotnet publish "HCMExternal\HCMExternal.csproj" -c Release -r win-x64 -p:Platform=x64 `
    --self-contained false -p:PublishSingleFile=true --nologo -v quiet
$pubCode = $LASTEXITCODE
Pop-Location
if ($pubCode -ne 0) { Fail "dotnet publish failed (exit $pubCode)" }

$publishedExe = Join-Path $pub 'HCMExternal.exe'
if (-not (Test-Path $publishedExe)) { Fail "no published exe at $publishedExe" }

# ---- 3. THE GUARD. DO NOT REMOVE. ----------------------------------------------------------------
$exeSize = (Get-Item $publishedExe).Length
Write-Host ("      published exe: {0:N0} bytes" -f $exeSize) -ForegroundColor DarkGray
if ($exeSize -lt $EXE_MIN_BYTES) {
    Fail ("published exe is only {0:N0} bytes. A real single-file publish is ~4.5 MB; anything this small " -f $exeSize) `
        + "is the framework stub, which cannot start without HCMExternal.dll beside it. This is exactly how " `
        + "V5.10.16 and V5.10.17 shipped broken. Check that PublishSingleFile actually took effect."
}

# ---- 4. stage and zip ----------------------------------------------------------------------------
Write-Host "[3/4] Staging ..." -ForegroundColor Cyan
$stageRoot = Join-Path $repo "artifacts\$Tag"
$inner     = Join-Path $stageRoot 'HaloCheckpointManager_Harcs_Build'
if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $inner | Out-Null

foreach ($k in $FROM_PUBLISH.Keys) {
    Copy-Item (Join-Path $pub $k) (Join-Path $inner $FROM_PUBLISH[$k]) -Force
}
foreach ($f in $FROM_BUILD) {
    $src = Join-Path $out $f
    if (-not (Test-Path $src)) { Fail "missing build output: $f" }
    Copy-Item $src (Join-Path $inner $f) -Force
}
foreach ($f in $FROM_REPO) {
    Copy-Item (Join-Path $repo $f) (Join-Path $inner $f) -Force
}

# refuse to ship anything unexpected - the build folder also contains the user's Saves, Logs and a
# multi-GB Havok cache, none of which may ever reach a zip
$expected = @($FROM_PUBLISH.Values) + $FROM_BUILD + $FROM_REPO
$staged   = Get-ChildItem $inner -Recurse
$extra    = $staged | Where-Object { $expected -notcontains $_.Name }
if ($extra) { Fail ("unexpected files staged: " + (($extra | ForEach-Object { $_.Name }) -join ', ')) }
if ($staged.Count -ne $expected.Count) { Fail "staged $($staged.Count) files, expected $($expected.Count)" }

# ---- THE SHIPPED CONFIG MUST NOT SWITCH ON EXPERIMENTAL FEATURES -----------------------------------
# HCMInternalConfig.xml is copied from the build folder - i.e. the developer's OWN saved settings. V5.10.19 and
# V5.10.20 shipped halo3TheaterInterpToggle=1 that way, so everyone who downloaded them got the experimental,
# halo3.dll-patching Theater Interpolation Fix switched on. Force the listed keys off in the STAGED copy only (the
# run folder keeps the developer's settings), and warn about any other feature toggle that is on in the staged
# config while its code default is off, so it is a decision rather than an accident.
$SHIP_OFF = @('halo3TheaterInterpToggle', 'uncapRenderSectionsToggle', 'havokBroadphaseBypassToggle', 'mpRevertRepairToggle')
$cfgPath  = Join-Path $inner 'HCMInternalConfig.xml'
$cfgBytes = [IO.File]::ReadAllBytes($cfgPath)
$cfgBom   = $cfgBytes.Length -ge 3 -and $cfgBytes[0] -eq 0xEF -and $cfgBytes[1] -eq 0xBB -and $cfgBytes[2] -eq 0xBF
$cfgText  = [IO.File]::ReadAllText($cfgPath)
foreach ($k in $SHIP_OFF) {
    if ($cfgText -match "<$k>1</$k>") {
        $cfgText = $cfgText.Replace("<$k>1</$k>", "<$k>0</$k>")
        Write-Host "      shipped config: forced $k off" -ForegroundColor Yellow
    }
}
[IO.File]::WriteAllText($cfgPath, $cfgText, (New-Object System.Text.UTF8Encoding($cfgBom)))
foreach ($k in $SHIP_OFF) {
    if ([IO.File]::ReadAllText($cfgPath) -match "<$k>1</$k>") { Fail "shipped config still enables $k" }
}
$settingsSrc = Get-Content (Join-Path $repo 'HCMInternal\SettingsStateAndEvents.h') -Raw
$defaultOff  = [regex]::Matches($settingsSrc, 'BinarySetting<bool>>\s+(\w+Toggle)\s*=\s*std::make_shared<BinarySetting<bool>>\s*\(\s*false') |
    ForEach-Object { $_.Groups[1].Value }
$onAnyway = @($defaultOff | Where-Object { $cfgText -match "<$_>1</$_>" })
if ($onAnyway) {
    Write-Host ("      NOTE: shipped config turns ON these default-off toggles (your saved settings): " + ($onAnyway -join ', ')) -ForegroundColor Yellow
}

# ---- EVERY SHIPPED BINARY MUST CARRY THE SAME VERSION ---------------------------------------------
# Directory.Build.props is the single source of truth: it sets the managed exe's version directly, and
# updateVersionInfo.ps1 rewrites FILEVERSION in every HCM*.rc from it. But an .rc edit only reaches the
# binary when that target is REBUILT, so a partial build ships a mixed set - which is exactly what
# happened when this script built HCMInternal alone. A user reporting a bug against "5.10.18" should not
# be running three DLLs from two different releases.
$propsFile = Join-Path $repo 'Directory.Build.props'
if (-not (Test-Path $propsFile)) { Fail "Directory.Build.props not found - cannot verify versions" }
$m = [regex]::Match((Get-Content $propsFile -Raw), '<VersionPrefix>\s*([0-9]+(?:\.[0-9]+){3})\s*</VersionPrefix>')
if (-not $m.Success) { Fail "could not read a 4-part <VersionPrefix> from Directory.Build.props" }
$wantVersion = $m.Groups[1].Value
Write-Host ("      expected version: {0}" -f $wantVersion) -ForegroundColor DarkGray

$versionMismatches = @()
foreach ($f in (Get-ChildItem $inner -Recurse -Include *.exe, *.dll)) {
    $vi  = [Diagnostics.FileVersionInfo]::GetVersionInfo($f.FullName)
    $got = "$($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart)"
    Write-Host ("        {0,-28} {1}" -f $f.Name, $got) -ForegroundColor DarkGray
    if ($got -ne $wantVersion) { $versionMismatches += "$($f.Name) is $got" }
}
if ($versionMismatches) {
    Fail ("these binaries do not carry version {0}: {1}. Their .rc was updated but the target was not " -f $wantVersion, ($versionMismatches -join ', ')) `
        + "rebuilt - build HCMInternal, HCMInterproc AND HCMSpeedhack, or drop -SkipBuild."
}

Write-Host "[4/4] Zipping ..." -ForegroundColor Cyan
$zip = Join-Path $stageRoot '1-HaloCheckpointManager_Harcs_Build_Windows-x64.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# ⚠ Entry names are written by hand with '/' separators. Compress-Archive and .NET Framework's
# CreateFromDirectory emit BACKSLASHES, which unzip(1) on Linux turns into one file with a literal
# backslash in its name. This bit a release already.
$fs = [IO.File]::Open($zip, 'CreateNew')
$ar = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
Get-ChildItem $inner -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($stageRoot.Length).TrimStart('\', '/') -replace '\\', '/'
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($ar, $_.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
}
$ar.Dispose(); $fs.Dispose()

# ---- verify what we just wrote -------------------------------------------------------------------
$z = [IO.Compression.ZipFile]::OpenRead($zip)
$bad = @($z.Entries | Where-Object { $_.FullName -match '\\' }).Count
$exeEntry = $z.Entries | Where-Object { $_.FullName -match 'HaloCheckpointManager\.exe$' }
$entryCount = $z.Entries.Count
$exeLen = if ($exeEntry) { $exeEntry.Length } else { 0 }
$z.Dispose()
if ($bad -gt 0)  { Fail "$bad zip entries use backslash separators" }
if (-not $exeEntry) { Fail "HaloCheckpointManager.exe missing from the zip" }
if ($exeLen -lt $EXE_MIN_BYTES) { Fail ("zipped exe is only {0:N0} bytes" -f $exeLen) }

# ---- copy the release to the output folder ---------------------------------------------------------
# The staged files go there as a runnable install, plus the zip. Anything already in the folder that is not part of
# the release (Saves, Logs, caches) is left alone, and an existing HCMInternalConfig.xml is never overwritten - the
# shipped one is your build-folder config, not necessarily what you play with.
if ($OutputDir) {
    Write-Host "[out] $OutputDir" -ForegroundColor Cyan
    if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null }
    Get-ChildItem $inner -File | ForEach-Object {
        $dst = Join-Path $OutputDir $_.Name
        if ($_.Name -eq 'HCMInternalConfig.xml' -and (Test-Path $dst)) {
            Write-Host "      kept your HCMInternalConfig.xml" -ForegroundColor DarkGray
            return
        }
        Copy-Item $_.FullName $dst -Force
        Write-Host "      $($_.Name)" -ForegroundColor DarkGray
    }
    Copy-Item $zip (Join-Path $OutputDir (Split-Path $zip -Leaf)) -Force
    Write-Host "      $(Split-Path $zip -Leaf)" -ForegroundColor DarkGray
}

Write-Host "`nOK" -ForegroundColor Green
Write-Host ("  {0}" -f $zip)
Write-Host ("  {0:N0} bytes, {1} entries, exe {2:N0} bytes" -f (Get-Item $zip).Length, $entryCount, $exeLen)
if ($OutputDir) { Write-Host ("  copied to {0}" -f $OutputDir) }
Write-Host "`nUpload with:" -ForegroundColor Cyan
Write-Host "  gh release create $Tag `"$zip`" --repo harctheshark/HaloCheckpointManager---Harcs-Build --title `"...`" --notes-file ..."
