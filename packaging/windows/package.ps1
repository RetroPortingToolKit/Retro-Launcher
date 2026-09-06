# Package RetComM Windows portable single-exe and Inno Setup installer.
#
# Usage:
#   packaging/windows/package.ps1 -Prefix out -Version 0.1.1 [-VcpkgBin path] [-Arch x64] [-PortableStub path]
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$VcpkgBin = "",
    [string]$Arch = "x64",
    [string]$PortableStub = "",
    [string]$InnoSetup = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")

# --- Authenticode signing (optional) ------------------------------------------
# Windows 11 Smart App Control allows an exe only when it is signed or its exact
# hash already has reputation, so unsigned releases are blocked outright. The
# certificate comes from the environment (a CI secret), never from the repo:
#   WINDOWS_SIGN_PFX_BASE64    PKCS#12 certificate, base64
#   WINDOWS_SIGN_PFX_PASSWORD  its password (may be empty)
#   WINDOWS_SIGN_TIMESTAMP_URL optional RFC 3161 server
#   WINDOWS_SIGN_DESCRIPTION   optional text for the file properties / UAC UI
# Without a certificate every Sign-* call is a no-op and packaging proceeds
# unsigned (with a notice). With one, a signing failure stops the package.
# The certificate is imported into the user store for the run and signed by
# thumbprint, so no password ever appears on a command line or in ISCC's log.
$script:SignTool = $null
$script:SignThumb = $null
$script:SignTs = if ($env:WINDOWS_SIGN_TIMESTAMP_URL) { $env:WINDOWS_SIGN_TIMESTAMP_URL } else { "http://timestamp.digicert.com" }
$script:SignDesc = if ($env:WINDOWS_SIGN_DESCRIPTION) { $env:WINDOWS_SIGN_DESCRIPTION } else { "RetComM Launcher" }

function Find-SignTool {
    if ($env:SIGNTOOL -and (Test-Path $env:SIGNTOOL)) { return $env:SIGNTOOL }
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($kits in @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")) {
        if (-not (Test-Path $kits)) { continue }
        $hit = Get-ChildItem -Path $kits -Directory -Filter "10.*" -ErrorAction SilentlyContinue |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" } |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($hit) { return $hit }
    }
    return $null
}

function Initialize-Signing {
    if (-not $env:WINDOWS_SIGN_PFX_BASE64) {
        Write-Host "Code signing skipped: WINDOWS_SIGN_PFX_BASE64 not set (binaries ship unsigned)"
        if ($env:GITHUB_ACTIONS) { Write-Host "::notice::Windows code signing skipped: no certificate secret" }
        return
    }
    $script:SignTool = Find-SignTool
    if (-not $script:SignTool) { throw "A signing certificate is configured but signtool.exe was not found (install the Windows SDK or set SIGNTOOL)" }
    $pfx = Join-Path ([System.IO.Path]::GetTempPath()) ("retcomm-sign-" + [guid]::NewGuid().ToString("N") + ".pfx")
    try {
        [System.IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:WINDOWS_SIGN_PFX_BASE64))
        $pass = if ($env:WINDOWS_SIGN_PFX_PASSWORD) {
            ConvertTo-SecureString -String $env:WINDOWS_SIGN_PFX_PASSWORD -AsPlainText -Force
        } else { New-Object System.Security.SecureString }
        $cert = Import-PfxCertificate -FilePath $pfx -CertStoreLocation Cert:\CurrentUser\My -Password $pass -Exportable:$false
        $script:SignThumb = $cert.Thumbprint
    } finally {
        Remove-Item $pfx -Force -ErrorAction SilentlyContinue
    }
    Write-Host "Code signing enabled: $($cert.Subject) (timestamp $script:SignTs)"
}

function Remove-SigningCertificate {
    if ($script:SignThumb) {
        Remove-Item ("Cert:\CurrentUser\My\" + $script:SignThumb) -Force -ErrorAction SilentlyContinue
        $script:SignThumb = $null
    }
}

function Sign-Files([string[]]$Files) {
    if (-not $script:SignThumb) { return }
    foreach ($f in $Files) {
        if (-not (Test-Path $f)) { continue }
        $ok = $false
        # Timestamp servers are the flaky part; retry a few times per file.
        for ($attempt = 1; $attempt -le 4 -and -not $ok; $attempt++) {
            & $script:SignTool sign /fd SHA256 /td SHA256 /tr $script:SignTs /sha1 $script:SignThumb /d $script:SignDesc $f 2>&1 | Out-Null
            if ($LASTEXITCODE -eq 0) {
                & $script:SignTool verify /pa /q $f 2>&1 | Out-Null
                if ($LASTEXITCODE -eq 0) { $ok = $true }
            }
            if (-not $ok) { Start-Sleep -Seconds (5 * $attempt) }
        }
        if (-not $ok) { throw "Code signing failed: $f" }
        Write-Host "  signed: $f"
    }
}

# ISCC's SignTool= directive: the command Inno runs for setup.exe and the
# uninstaller ($f = file). Empty when signing is off; setup.iss then omits it.
function Get-InnoSignArgs {
    if (-not $script:SignThumb) { return @() }
    $cmd = "`$q$($script:SignTool)`$q sign /fd SHA256 /td SHA256 /tr $($script:SignTs) /sha1 $($script:SignThumb) /d `$q$($script:SignDesc)`$q `$f"
    return @("/Srcsign=$cmd", "/DSignToolName=rcsign")
}

Initialize-Signing
$OutDir = Join-Path $Root "dist"
$Stage = Join-Path $OutDir "windows-stage"
# Friendly name for the desktop / unzipped portable stub (spaces OK).
$PortableExeName = "RetComM Launcher.exe"
# Release-page asset: zip wrapping that exe (stable name, no version).
$PortableZipName = "RetComM-Launcher-portable-windows.zip"

Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$PrefixBin = Join-Path $Prefix "bin"
Copy-Item (Join-Path $PrefixBin "retcomm.exe") $Stage
Copy-Item (Join-Path $PrefixBin "retcomm-hub.exe") $Stage

# Prefer DLLs already installed beside the exes (CMake TARGET_RUNTIME_DLLS).
# Note: TARGET_RUNTIME_DLLS often misses *transitive* deps (e.g. zlib behind
# libcurl), so we always harvest from vcpkg installed/bin as well.
Get-ChildItem -Path $PrefixBin -Filter "*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName $Stage -Force
}

function Copy-RuntimeDlls([string]$Dir, [switch]$AllDlls) {
    if (-not $Dir -or -not (Test-Path $Dir)) { return 0 }
    $count = 0
    if ($AllDlls) {
        Get-ChildItem -Path $Dir -Filter "*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $Stage -Force
            $count++
        }
        return $count
    }
    $patterns = @(
        "SDL3.dll",
        "libcurl.dll",
        "libcurl-d.dll",
        "zlib1.dll",
        "zlibd1.dll",
        "zlib.dll",
        "z.dll",
        "libssl*.dll",
        "libcrypto*.dll",
        "libssl-*.dll",
        "libcrypto-*.dll",
        "nghttp2.dll",
        "libssh2.dll",
        "brotlicommon.dll",
        "brotlidec.dll",
        "brotlienc.dll",
        "fmt.dll",
        "legacy.dll",
        "libomp*.dll"
    )
    foreach ($pat in $patterns) {
        Get-ChildItem -Path $Dir -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $Stage -Force
            $count++
        }
    }
    return $count
}

function Test-StagedDll([string[]]$Names) {
    foreach ($n in $Names) {
        if (Test-Path (Join-Path $Stage $n)) { return $true }
        if (Get-ChildItem $Stage -Filter $n -ErrorAction SilentlyContinue) { return $true }
    }
    return $false
}

# Always merge from every known location. Skipping the vcpkg installed tree when
# build/Release already has SDL3 was leaving zlib (curl transitive) out of the
# installer — clean machines then fail with "zlib1.dll / z.dll was not found".
$searchRoots = [System.Collections.Generic.List[string]]::new()
if ($VcpkgBin) { [void]$searchRoots.Add($VcpkgBin) }
@(
    (Join-Path $Root "build\Release"),
    (Join-Path $Root "build\RelWithDebInfo"),
    (Join-Path $Root "vcpkg_installed\x64-windows\bin"),
    (Join-Path $Root "retcomm-vcpkg\installed\x64-windows\bin"),
    (Join-Path $Root "build\vcpkg_installed\x64-windows\bin"),
    (Join-Path $env:RETCOMM_VCPKG "installed\x64-windows\bin")
) | ForEach-Object {
    if ($_ -and (Test-Path $_)) { [void]$searchRoots.Add($_) }
}

$copied = 0
foreach ($dir in ($searchRoots | Select-Object -Unique)) {
    # Full harvest from vcpkg package bins (small; covers curl→zlib→…).
    $isVcpkgPkgBin = ($dir -match '[/\\]installed[/\\]x64-windows[/\\]bin$')
    $n = Copy-RuntimeDlls $dir -AllDlls:$isVcpkgPkgBin
    if ($n -gt 0) {
        Write-Host "Harvested $n DLL(s) from $dir"
        $copied += $n
    }
}

if (-not (Test-StagedDll @("SDL3.dll"))) {
    $hit = Get-ChildItem -Path (Join-Path $Root "build") -Recurse -Filter "SDL3.dll" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($hit) {
        Write-Host "Discovered SDL3.dll at $($hit.DirectoryName)"
        Copy-RuntimeDlls $hit.DirectoryName | Out-Null
        Copy-RuntimeDlls $hit.DirectoryName -AllDlls | Out-Null
    }
}

# MSVC/vcpkg ships zlib1.dll; some MinGW layouts use z.dll / zlib.dll.
$hasZlib = Test-StagedDll @("zlib1.dll", "zlibd1.dll", "z.dll", "zlib.dll")
$hasCurl = Test-StagedDll @("libcurl.dll", "libcurl-d.dll", "libcurl*.dll")
$hasSdl = Test-StagedDll @("SDL3.dll")

if (-not $hasSdl) {
    throw "SDL3.dll not bundled — cannot ship a Windows release without it"
}
if (-not $hasCurl) {
    throw "libcurl DLL not bundled — cannot ship a Windows release without it"
}
if (-not $hasZlib) {
    throw @"
zlib runtime DLL not bundled (need zlib1.dll from vcpkg, or z.dll/zlib.dll).
libcurl depends on zlib; clean machines fail with 'z.dll / zlib1.dll was not found'.
Searched: $($searchRoots -join '; ')
Staged so far: $((Get-ChildItem $Stage -Filter '*.dll' | ForEach-Object Name) -join ', ')
"@
}

# If an import expects z.dll but we only have zlib1.dll, provide an alias copy.
# (Harmless when unused; unblocks mismatched import names.)
$zlib1 = Join-Path $Stage "zlib1.dll"
$zDll = Join-Path $Stage "z.dll"
if ((Test-Path $zlib1) -and -not (Test-Path $zDll)) {
    Copy-Item $zlib1 $zDll -Force
    Write-Host "Aliased zlib1.dll -> z.dll for import-name compatibility"
}
if ((Test-Path $zDll) -and -not (Test-Path $zlib1)) {
    Copy-Item $zDll $zlib1 -Force
    Write-Host "Aliased z.dll -> zlib1.dll for import-name compatibility"
}

# Installer channel marker (self-update picks the windows-*-setup.exe asset).
@'
{
  "schema_version": 1,
  "channel": "installer"
}
'@ | Set-Content -Path (Join-Path $Stage "channel.json") -Encoding utf8

$IcoSrc = Join-Path $Root "assets\retcomm.ico"
if (Test-Path $IcoSrc) {
    Copy-Item $IcoSrc (Join-Path $Stage "retcomm.ico") -Force
}
if (Test-Path (Join-Path $Root "assets\retcomm.png")) {
    Copy-Item (Join-Path $Root "assets\retcomm.png") (Join-Path $Stage "retcomm.png") -Force
}
# Hub UI fonts next to the exes (fonts/LatoLatin-*.ttf).
$FontsSrc = Join-Path $Prefix "share\retcomm\fonts"
if (-not (Test-Path (Join-Path $FontsSrc "LatoLatin-Regular.ttf"))) {
    $FontsSrc = Join-Path $Root "assets\fonts"
}
$FontsRegular = Join-Path $FontsSrc "LatoLatin-Regular.ttf"
if (-not (Test-Path $FontsRegular)) {
    throw "Hub fonts missing (expected LatoLatin-Regular.ttf under share/retcomm/fonts or assets/fonts)"
}
$FontsDst = Join-Path $Stage "fonts"
New-Item -ItemType Directory -Force -Path $FontsDst | Out-Null
Copy-Item (Join-Path $FontsSrc "*") $FontsDst -Force
if (-not (Test-Path (Join-Path $FontsDst "LatoLatin-Regular.ttf"))) {
    throw "Failed to stage hub fonts into $FontsDst"
}

# Hub platform controller icons next to the exes (platforms/psx.png, …).
$PlatSrc = Join-Path $Prefix "share\retcomm\platforms"
if (-not (Test-Path (Join-Path $PlatSrc "psx.png"))) {
    $PlatSrc = Join-Path $Root "assets\platforms"
}
if (-not (Test-Path (Join-Path $PlatSrc "psx.png"))) {
    throw "Hub platform icons missing (expected psx.png under share/retcomm/platforms or assets/platforms)"
}
$PlatDst = Join-Path $Stage "platforms"
New-Item -ItemType Directory -Force -Path $PlatDst | Out-Null
Copy-Item (Join-Path $PlatSrc "*.png") $PlatDst -Force
if (-not (Test-Path (Join-Path $PlatDst "psx.png"))) {
    throw "Failed to stage hub platform icons into $PlatDst"
}

# PSX DualShock art for the Gamepads configure mapper (controllers/pad_*.png).
$CtrlSrc = Join-Path $Prefix "share\retcomm\controllers"
if (-not (Test-Path (Join-Path $CtrlSrc "pad_analog.png"))) {
    $CtrlSrc = Join-Path $Root "assets\controllers"
}
if (-not (Test-Path (Join-Path $CtrlSrc "pad_analog.png"))) {
    throw "Hub PSX pad art missing (expected pad_analog.png under share/retcomm/controllers or assets/controllers)"
}
$CtrlDst = Join-Path $Stage "controllers"
New-Item -ItemType Directory -Force -Path $CtrlDst | Out-Null
Copy-Item (Join-Path $CtrlSrc "pad_*.png") $CtrlDst -Force
if (-not (Test-Path (Join-Path $CtrlDst "pad_analog.png"))) {
    throw "Failed to stage hub PSX pad art into $CtrlDst"
}

# First-run setup path cards (Easy / Advanced).
$SetupSrc = Join-Path $Prefix "share\retcomm\setup"
if (-not (Test-Path (Join-Path $SetupSrc "setup_easy_rocket.png"))) {
    $SetupSrc = Join-Path $Root "assets\setup"
}
if (-not (Test-Path (Join-Path $SetupSrc "setup_easy_rocket.png"))) {
    throw "Hub setup card art missing (expected setup_easy_rocket.png under share/retcomm/setup or assets/setup)"
}
$SetupDst = Join-Path $Stage "setup"
New-Item -ItemType Directory -Force -Path $SetupDst | Out-Null
Copy-Item (Join-Path $SetupSrc "*.png") $SetupDst -Force
foreach ($n in @("setup_easy_rocket.png", "setup_advanced_wrench.png")) {
    if (-not (Test-Path (Join-Path $SetupDst $n))) {
        throw "Failed to stage hub setup card art ($n) into $SetupDst"
    }
}

# Everything the player runs is signed before it is packed anywhere: the
# hub, the CLI, and the DLLs beside them. Third-party DLLs (SDL3, curl, …)
# get our signature too; Smart App Control checks them as well.
Sign-Files (Get-ChildItem $Stage -Include "*.exe", "*.dll" -Recurse | ForEach-Object FullName)

# --- Portable: friendly-named stub+payload exe, wrapped in a release zip ---
if (-not $PortableStub) {
    $candidates = @(
        (Join-Path $PrefixBin "retcomm-portable.exe"),
        (Join-Path $Root "build\Release\retcomm-portable.exe"),
        (Join-Path $Root "build\retcomm-portable.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $PortableStub = $c; break }
    }
}
if (-not $PortableStub -or -not (Test-Path $PortableStub)) {
    Write-Warning "retcomm-portable.exe not found — skipping portable zip"
} else {
    $PayloadZip = Join-Path $OutDir "windows-portable-payload.zip"
    if (Test-Path $PayloadZip) { Remove-Item $PayloadZip -Force }
    # Payload should not include the installer channel marker as authoritative;
    # the stub writes channel.json on extract. Still fine if present.
    Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $PayloadZip

    $OutPortableExe = Join-Path $OutDir $PortableExeName
    if (Test-Path $OutPortableExe) { Remove-Item $OutPortableExe -Force }

    $stubBytes = [System.IO.File]::ReadAllBytes($PortableStub)
    $zipBytes = [System.IO.File]::ReadAllBytes($PayloadZip)
    $fs = [System.IO.File]::Open($OutPortableExe, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $fs.Write($stubBytes, 0, $stubBytes.Length)
        $fs.Write($zipBytes, 0, $zipBytes.Length)
        $len = [BitConverter]::GetBytes([uint64]$zipBytes.Length)
        $fs.Write($len, 0, 8)
        $magic = [Text.Encoding]::ASCII.GetBytes("RCM1")
        $fs.Write($magic, 0, 4)
    } finally {
        $fs.Close()
    }
    Remove-Item $PayloadZip -Force -ErrorAction SilentlyContinue
    Write-Host "Portable exe: $OutPortableExe (stub $($stubBytes.Length) + payload $($zipBytes.Length))"
    # Signed AFTER the payload is appended: the certificate table lands after
    # the RCM1 trailer, and the stub (portable_trailer.hpp) looks for the
    # trailer just before that table when it is not at the very end.
    Sign-Files @($OutPortableExe)

    # Release asset: flat zip root = "RetComM Launcher.exe" (no nested folder).
    # Compress-Archive with a full path can store a parent segment; ZipFile does not.
    $OutPortableZip = Join-Path $OutDir $PortableZipName
    if (Test-Path $OutPortableZip) { Remove-Item $OutPortableZip -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::Open(
        $OutPortableZip,
        [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $OutPortableExe, $PortableExeName,
            [System.IO.Compression.CompressionLevel]::Optimal)
    } finally {
        $zip.Dispose()
    }
    Write-Host "Portable zip: $OutPortableZip (entry: $PortableExeName)"
}

# --- Inno Setup installer ---
function Find-ISCC([string]$Hint) {
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $paths = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "${env:LocalAppData}\Programs\Inno Setup 6\ISCC.exe"
    )
    foreach ($p in $paths) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$iscc = Find-ISCC $InnoSetup
if (-not $iscc) {
    Write-Warning "Inno Setup (ISCC.exe) not found — skipping windows setup.exe"
} elseif (-not (Test-Path (Join-Path $Stage "retcomm.ico"))) {
    Write-Warning "assets/retcomm.ico missing from stage — skipping windows setup.exe"
} else {
    $iss = Join-Path $PSScriptRoot "setup.iss"
    $stageAbs = (Resolve-Path $Stage).Path
    $outAbs = (Resolve-Path $OutDir).Path
    $signArgs = Get-InnoSignArgs
    & $iscc `
        "/DMyAppVersion=$Version" `
        "/DStageDir=$stageAbs" `
        "/DOutputDir=$outAbs" `
        "/DArch=$Arch" `
        @signArgs `
        $iss
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC failed with exit code $LASTEXITCODE"
    }
    Write-Host "Installer: (see OutputBaseFilename under $outAbs)"
}

Remove-SigningCertificate

Get-ChildItem $Stage | ForEach-Object { Write-Host ("  staged: " + $_.Name) }
Get-ChildItem $OutDir -File | ForEach-Object { Write-Host ("  dist: " + $_.Name) }
