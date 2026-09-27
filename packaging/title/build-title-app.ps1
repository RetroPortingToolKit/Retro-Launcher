# Package one title as a local, double-clickable Windows app: a single portable
# .exe (the launcher's portable stub + a zip of the hub, the runner and the
# title payload + the RCM1 trailer). Linux / macOS: build-title-app.sh.
#
# The title-app kit (docs/RELEASES.md, "Title-app mode"). It runs from a flat
# hub prefix -- <hub dir>\packaging\title\ -- and needs nothing from the
# launcher's source tree. The payload unpacks beside the exe, into
# <exe dir>\<id>-data\app\, and the hub keeps its state in <exe dir>\<id>-data\
# (src/portable/win_portable_main.cpp, title apps).
#
# The app is a LOCAL build: the game package holds ROM-derived generated code
# and is never published (recomp-ai-rules SHIPPING.md §1); the ROM is never in
# it.
#
# Gates, each failing the build (SHIPPING.md §4), as build-title-app.sh:
#   1. the payload allowlist (MANIFEST.txt exact, no ROM/disc extension, no N64
#      ROM header magic, no links), and the magic scan over the whole payload;
#   2. retro-hub.exe and retro-core-runner.exe --version, read back from the
#      payload unpacked OUT OF THE BUILT .exe;
#   3. retro-hub.exe --check-title from that unpacked payload, in a clean
#      directory, with RETCOMM_PORTABLE_EXE set as the stub sets it: the data
#      dir must be <exe dir>\<id>-data.
#
# The last line written is `app <absolute path>`.
#
# usage: build-title-app.ps1 -Title <payload dir> -Runner <retro-core-runner.exe>
#          -Out <dir> [-HubDir <flat hub prefix>] [-Icon <png>] [-Version <v>]
#          [-Stub <retcomm-portable.exe>]
#
# -Icon is accepted for symmetry and not applied: the exe's icon is the stub's
# own resource (assets/retcomm.ico); re-branding it needs a resource editor.
param(
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Runner,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$HubDir = "",
    [string]$Icon = "",
    [string]$Version = "",
    [string]$Stub = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2

function Fail([string]$Message) {
    [Console]::Error.WriteLine("build-title-app: $Message")
    exit 1
}
function Say([string]$Message) { [Console]::Error.WriteLine("build-title-app: $Message") }

# Every path absolute, against the directory this was run from (PowerShell's
# location, which .NET's current directory does not follow).
function Get-AbsolutePath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

$Kit = $PSScriptRoot
$Common = Join-Path $Kit "..\common"
if (-not (Test-Path (Join-Path $Common "portable.ps1"))) { Fail "$Common\portable.ps1 missing (a partial kit?)" }
. (Join-Path $Common "portable.ps1")

$Title = Get-AbsolutePath $Title
if ((Test-Path $Title -PathType Leaf) -and (Split-Path $Title -Leaf) -eq "title.json") { $Title = Split-Path $Title -Parent }
if (-not (Test-Path $Title -PathType Container)) { Fail "-Title ${Title}: not a directory" }
$Runner = Get-AbsolutePath $Runner
if (-not (Test-Path $Runner -PathType Leaf)) { Fail "-Runner ${Runner}: not a file" }
if (-not $HubDir) { $HubDir = Join-Path $Kit "..\.." }
$HubDir = Get-AbsolutePath $HubDir
$HubExe = Join-Path $HubDir "retro-hub.exe"
if (-not (Test-Path $HubExe -PathType Leaf)) {
    Fail "$HubExe not found (-HubDir must be a flat hub prefix: scripts/build-local.ps1 output)"
}
if (-not $Stub) { $Stub = Join-Path $HubDir "retcomm-portable.exe" }
$Stub = Get-AbsolutePath $Stub
if (-not (Test-Path $Stub -PathType Leaf)) { Fail "portable stub $Stub not found (-Stub, or retcomm-portable.exe in the hub prefix)" }
if ($Icon) { Say "-Icon is not applied on Windows (the exe carries the stub's icon)" }
$Out = Get-AbsolutePath $Out
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# ---- title.json ----------------------------------------------------------------
try {
    $meta = Get-Content -Raw -LiteralPath (Join-Path $Title "title.json") | ConvertFrom-Json
} catch {
    Fail "title.json: $($_.Exception.Message)"
}
# StrictMode: a key title.json leaves out is $null here, not an error.
function Get-Prop($Object, [string]$Name) {
    $p = $Object.PSObject.Properties[$Name]
    if ($p) { return $p.Value }
    return $null
}
if ((Get-Prop $meta "schema") -ne 1) { Fail "title.json: schema must be 1" }
$Id = [string](Get-Prop $meta "id")
if ($Id -notmatch '^[a-z0-9_-]+$') { Fail "title.json: id '$Id' must match [a-z0-9_-]+" }
if (-not (Get-Prop $meta "core")) { Fail "title.json: 'core' is required" }
$Name = [string](Get-Prop $meta "name")
if (-not $Name) { $Name = $Id }
$HasPackage = [bool](Get-Prop $meta "package")
if (-not $Version) { $Version = [string](Get-Prop $meta "version") }
if (-not $Version) { $Version = "0.0.0" }
if ($Version -notmatch '^[A-Za-z0-9._+-]+$') { Fail "version '$Version': letters, digits and . _ + - only" }
# Spaces in the name are kept; characters Windows refuses in a file name are not.
$FileName = ($Name -replace '[\\/:*?"<>|]', '-')
$Artifact = Join-Path $Out "$FileName-$Version-windows-x64.exe"

# ---- gate 1: the payload allowlist ------------------------------------------------
$RomExt = '\.(z64|n64|v64|rom|bin|iso|cue|chd|sfc|smc|gba|gb|gbc|nds|md|gen|sms|nes)$'
function Test-RomMagic([string]$Path) {
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $b = New-Object byte[] 4
        if ($fs.Read($b, 0, 4) -lt 4) { return $false }
    } finally { $fs.Close() }
    $hex = ($b | ForEach-Object { $_.ToString("x2") }) -join ""
    return $hex -in @("80371240", "37804012", "40123780")
}
# Every file under $Dir: no ROM magic; in the payload (or a title\ inside the
# app) no ROM extension and no link either. The contract's list includes .md
# (Mega Drive), so a Markdown file in the payload is refused too.
function Assert-NoRoms([string]$Dir, [string]$What) {
    $bad = @()
    $root = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\')
    foreach ($f in Get-ChildItem -LiteralPath $root -Recurse -Force -File) {
        $rel = $f.FullName.Substring($root.Length + 1)
        $inTitle = ($What -eq "payload") -or ("\$rel" -like "*\title\*")
        if ($inTitle -and ($f.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            $bad += "  ${rel}: a link (the payload holds regular files only)"
        } elseif ($inTitle -and $rel.ToLowerInvariant() -match $RomExt) {
            $bad += "  ${rel}: a ROM/disc image extension"
        } elseif (Test-RomMagic $f.FullName) {
            $bad += "  ${rel}: begins with an N64 ROM header"
        }
    }
    if ($bad.Count) { Fail ("refusing to package: the $What holds a ROM or something that may be one:`n" + ($bad -join "`n")) }
}
function Get-ManifestEntries {
    $entries = @()
    foreach ($line in Get-Content -LiteralPath (Join-Path $Title "MANIFEST.txt")) {
        $line = $line.Trim("`r")
        if (-not $line) { continue }
        if ([System.IO.Path]::IsPathRooted($line) -or ("/$line/" -match '/\.\.?/')) {
            Fail "MANIFEST.txt: '$line' is not a plain relative path"
        }
        if ($line -in @("title.json", "MANIFEST.txt")) { Fail "MANIFEST.txt lists $line, which it must not" }
        $entries += $line.Replace('\', '/')
    }
    return $entries | Sort-Object -Unique
}
foreach ($f in @("title.json", "MANIFEST.txt")) {
    if (-not (Test-Path (Join-Path $Title $f) -PathType Leaf)) { Fail "$Title\${f}: missing" }
}
Assert-NoRoms $Title "payload"
$Manifest = @(Get-ManifestEntries)
foreach ($e in $Manifest) {
    $p = Join-Path $Title $e
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { Fail "MANIFEST.txt lists $e, which is not in the payload" }
}
$titleRoot = (Resolve-Path -LiteralPath $Title).Path.TrimEnd('\')
$actual = @(Get-ChildItem -LiteralPath $titleRoot -Recurse -Force -File | ForEach-Object {
    $_.FullName.Substring($titleRoot.Length + 1).Replace('\', '/')
} | Where-Object { $_ -notin @("title.json", "MANIFEST.txt") })
$extra = @($actual | Where-Object { $_ -notin $Manifest })
if ($extra.Count) { Fail ("refusing to package: not in MANIFEST.txt:`n" + ($extra -join "`n")) }
Say "payload ${Title}: $($Manifest.Count) listed file(s), allowlist ok"

# ---- stage: the hub (an allowlist of what a flat prefix holds), runner, title ------
$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("build-title-app-" + [guid]::NewGuid().ToString("N"))
$Stage = Join-Path $Work "stage"
try {
    New-Item -ItemType Directory -Force -Path $Stage | Out-Null
    Copy-Item -LiteralPath $HubExe -Destination $Stage
    Get-ChildItem -LiteralPath $HubDir -Filter "*.dll" -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $Stage }
    foreach ($f in @("retcomm.png", "LICENSE")) {
        $p = Join-Path $HubDir $f
        if (Test-Path $p) { Copy-Item -LiteralPath $p -Destination $Stage }
    }
    foreach ($kind in @("fonts", "platforms", "controllers", "setup", "licenses")) {
        $p = Join-Path $HubDir $kind
        if (Test-Path $p -PathType Container) { Copy-Item -LiteralPath $p -Destination (Join-Path $Stage $kind) -Recurse }
    }
    if (-not (Test-Path (Join-Path $Stage "fonts\LatoLatin-Regular.ttf"))) { Fail "$HubDir\fonts\LatoLatin-Regular.ttf missing (not a complete hub prefix)" }
    Copy-Item -LiteralPath $Runner -Destination (Join-Path $Stage "retro-core-runner.exe")
    # exe_dir\title\: where the hub finds its title.
    $StageTitle = Join-Path $Stage "title"
    New-Item -ItemType Directory -Force -Path $StageTitle | Out-Null
    foreach ($f in @("title.json", "MANIFEST.txt") + $Manifest) {
        $dst = Join-Path $StageTitle $f
        New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
        Copy-Item -LiteralPath (Join-Path $Title $f) -Destination $dst
    }
    Assert-NoRoms $Stage "app"

    # Signing (when a certificate is configured): what the player runs, before
    # it is packed, as packaging/windows/package.ps1 does.
    if (-not $env:WINDOWS_SIGN_DESCRIPTION) { $script:SignDesc = $Name }
    Initialize-Signing
    Sign-Files (Get-ChildItem $Stage -Include "*.exe", "*.dll" -Recurse | ForEach-Object FullName)
    New-PortableExe -Stub $Stub -StageDir $Stage -OutExe $Artifact

    # ---- the gates, on the payload read back out of the built exe --------------
    $Check = Join-Path $Work "check"
    Expand-PortableExe -Exe $Artifact -Dest $Check
    $checkTitle = (Resolve-Path (Join-Path $Check "title")).Path.TrimEnd('\')
    $got = @(Get-ChildItem -LiteralPath $checkTitle -Recurse -File | ForEach-Object {
        $_.FullName.Substring($checkTitle.Length + 1).Replace('\', '/') } | Sort-Object)
    $want = @((@("title.json", "MANIFEST.txt") + $Manifest) | Sort-Object -Unique)
    if (($want -join "`n") -ne ($got -join "`n")) { Fail "the exe's title\ differs from the payload" }
    foreach ($f in $want) {
        $a = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $checkTitle $f)).Hash
        $b = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Title $f)).Hash
        if ($a -ne $b) { Fail "the exe's title\$f is not byte-identical to the payload's" }
    }
    Assert-NoRoms $Check "packaged exe"

    function Invoke-Captured([string]$Exe, [string[]]$Arguments, [string]$Cwd, [hashtable]$Vars) {
        $outFile = Join-Path $Work ("out-" + [guid]::NewGuid().ToString("N") + ".txt")
        $errFile = "$outFile.err"
        $saved = @{}
        foreach ($k in @("RETRO_HUB_APP", "RETRO_HUB_REEXEC", "RETRO_CORE_RUNNER", "RETCOMM_PORTABLE_EXE",
                         "RETCOMM_HOME", "APPIMAGE", "APPDIR", "RETRO_TITLE_STATE_DIR")) {
            $saved[$k] = [Environment]::GetEnvironmentVariable($k)
            [Environment]::SetEnvironmentVariable($k, $null)
        }
        foreach ($k in $Vars.Keys) { [Environment]::SetEnvironmentVariable($k, $Vars[$k]) }
        try {
            # retro-hub.exe is a GUI-subsystem program: Start-Process -Wait
            # with redirected output is what reliably waits for it and keeps
            # what it prints.
            $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -WorkingDirectory $Cwd -NoNewWindow `
                -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
            $text = if (Test-Path $outFile) { Get-Content -Raw $outFile } else { "" }
            $err = if (Test-Path $errFile) { Get-Content -Raw $errFile } else { "" }
            return @{ Code = $p.ExitCode; Out = $text; Err = $err }
        } finally {
            foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
            foreach ($k in $Vars.Keys) { if (-not $saved.ContainsKey($k)) { [Environment]::SetEnvironmentVariable($k, $null) } }
        }
    }
    function Get-Field([string]$Report, [string]$Key) {
        foreach ($l in ($Report -split "`r?`n")) { if ($l.StartsWith("$Key ")) { return $l.Substring($Key.Length + 1) } }
        return ""
    }
    $clean = Join-Path $Work "clean-cwd"
    New-Item -ItemType Directory -Force -Path $clean | Out-Null
    $hubV = Invoke-Captured (Join-Path $Check "retro-hub.exe") @("--version") $clean @{}
    $runV = Invoke-Captured (Join-Path $Check "retro-core-runner.exe") @("--version") $clean @{}
    if ($hubV.Code -ne 0) { Fail "gate: the packaged retro-hub.exe --version failed: $($hubV.Err)" }
    if ($runV.Code -ne 0) { Fail "gate: the packaged retro-core-runner.exe --version failed: $($runV.Err)" }
    if ((Get-Field $hubV.Out "title_app") -notmatch '^[1-9][0-9]*$') { Fail "gate: the packaged retro-hub.exe has no 'title_app' in --version" }
    if ($HasPackage -and (Get-Field $runV.Out "game_package") -ne "1") { Fail "gate: the packaged runner cannot load a game package (no 'game_package 1')" }
    $HubVersion = Get-Field $hubV.Out "version"
    $RunnerVersion = Get-Field $runV.Out "version"
    Say "gate: packaged retro-hub $HubVersion (title_app $(Get-Field $hubV.Out 'title_app')); retro-core-runner $RunnerVersion"

    $chk = Invoke-Captured (Join-Path $Check "retro-hub.exe") @("--check-title") $clean @{ RETCOMM_PORTABLE_EXE = $Artifact }
    if ($chk.Code -ne 0) { Fail "gate: --check-title failed in the packaged app:`n$($chk.Out)$($chk.Err)" }
    if (Get-ChildItem -LiteralPath $clean -Force) { Fail "gate: --check-title wrote into its working directory" }
    $norm = { param($p) ([string]$p).Replace('/', '\').TrimEnd('\').ToLowerInvariant() }
    if ((& $norm (Get-Field $chk.Out "title")) -ne (& $norm (Join-Path $checkTitle "title.json"))) {
        Fail "gate: the packaged hub resolved title '$(Get-Field $chk.Out 'title')', not its own"
    }
    $wantData = Join-Path $Out "$Id-data"
    if ((& $norm (Get-Field $chk.Out "data_dir")) -ne (& $norm $wantData)) {
        Fail "gate: data dir '$(Get-Field $chk.Out 'data_dir')' is not beside the exe ($wantData)"
    }
    Say "gate: --check-title ok from a clean directory; data dir beside the exe"

    # Signed AFTER the payload is appended (portable_trailer.hpp finds the
    # trailer before the certificate table), and after the gates, which read
    # the trailer at the end of the file.
    Sign-Files @($Artifact)
    Remove-SigningCertificate
} finally {
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}

Say "$Name $Version ($Id): hub $HubVersion, runner $RunnerVersion"
Write-Output "app $Artifact"
