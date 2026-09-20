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

.PARAMETER Tag
    Release tag, e.g. V5.10.18. Used only to name the staging folder.

.PARAMETER SkipBuild
    Skip the HCMInternal native build (use when it is already current and the game is closed).

.EXAMPLE
    .\make-release.ps1 -Tag V5.10.18
    gh release create V5.10.18 .\artifacts\V5.10.18\1-HaloCheckpointManager_Harcs_Build_Windows-x64.zip ...
#>
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [switch]$SkipBuild
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
$blockers = Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -match 'halo|HCM|HaloCheckpoint|MCC' }
if ($blockers) {
    Fail ("close these first (they lock HCMInternal.dll): " + (($blockers | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', '))
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "vswhere not found" }
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (-not $msbuild) { Fail "MSBuild not found" }

# ---- 1. native build -----------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "[1/4] Building HCMInternal ..." -ForegroundColor Cyan
    & $msbuild (Join-Path $repo 'HaloCheckpointManager.sln') /t:HCMInternal /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { Fail "HCMInternal build failed" }
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

Write-Host "`nOK" -ForegroundColor Green
Write-Host ("  {0}" -f $zip)
Write-Host ("  {0:N0} bytes, {1} entries, exe {2:N0} bytes" -f (Get-Item $zip).Length, $entryCount, $exeLen)
Write-Host "`nUpload with:" -ForegroundColor Cyan
Write-Host "  gh release create $Tag `"$zip`" --repo harctheshark/HaloCheckpointManager---Harcs-Build --title `"...`" --notes-file ..."
