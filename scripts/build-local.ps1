# Build retro-hub for THIS Windows machine into a FLAT hub prefix, for local
# use: what a port's framework points RETRO_HUB at to run or package a title
# app (docs/RELEASES.md, "Title-app mode"). Linux / macOS: scripts/build-local.sh.
#
# Not a release: the release is .github/workflows/release.yml. The prefix holds
#   retro-hub.exe, its DLLs (SDL3, curl, zlib, ...), retro-core-runner.exe,
#   retcomm-portable.exe (the portable stub the title-app kit wraps a title in),
#   retcomm.png, fonts\, platforms\, controllers\, setup\, LICENSE,
#   packaging\title\ (the kit) and packaging\common\.
#
# Dependencies come from vcpkg (vcpkg.json), as CI does: $env:VCPKG_ROOT or
# $env:RETCOMM_VCPKG when set, else a checkout at the CI baseline under
# <repo>\.cache\vcpkg (cloned and bootstrapped on first use).
#
# The prefix is checked before its path is printed: retro-hub.exe --version
# must say `title_app 1` and list --package, the runner must answer --version.
# The LAST line written is always
#   RETRO_HUB=<absolute path to retro-hub.exe>
#
# usage: scripts/build-local.ps1 [-Debug] [-Out DIR] [-Build DIR]
#          [-Runtime <Retro-Runtime checkout>] [-Jobs N] [-Help]
param(
    [switch]$Debug,
    [string]$Out = "",
    [string]$Build = "",
    [string]$Runtime = "",
    [int]$Jobs = 0,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2

function Fail([string]$Message) { [Console]::Error.WriteLine("build-local: $Message"); exit 1 }
function Say([string]$Message) { [Console]::Error.WriteLine("build-local: $Message") }

if ($Help) {
    @"
usage: scripts/build-local.ps1 [options]

Build retro-hub.exe (and a retro-core-runner.exe) into a flat hub prefix for
this machine, and print its path last, as RETRO_HUB=<absolute path>.

  -Debug          Debug build (default: Release)
  -Out DIR        the flat prefix (default: <repo>\out\local\windows-<arch>\)
  -Build DIR      CMake build directory (default: <repo>\build-local[-debug])
  -Runtime DIR    build the runner from this Retro-Runtime checkout (its
                  scripts\build-local.ps1 when it has one) instead of the
                  submodule's
  -Jobs N         parallel build jobs (default: all CPUs)
  -Help           this text
"@ | Write-Output
    exit 0
}

function Get-AbsolutePath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments | ForEach-Object { [Console]::Error.WriteLine($_) }
    if ($LASTEXITCODE -ne 0) { Fail "$Exe $($Arguments -join ' ') failed (exit $LASTEXITCODE)" }
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Config = if ($Debug) { "Debug" } else { "Release" }
$Arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x86_64" }
$Triplet = if ($Arch -eq "arm64") { "arm64-windows" } else { "x64-windows" }
$Platform = "windows-$Arch"
if (-not $Build) { $Build = Join-Path $Root ("build-local" + $(if ($Debug) { "-debug" } else { "" })) }
$Build = Get-AbsolutePath $Build
if (-not $Out) { $Out = Join-Path $Root "out\local\$Platform" }
$Out = Get-AbsolutePath $Out
if ($Out -eq $Build) { Fail "-Out and -Build must differ" }
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
if ($Runtime) {
    $Runtime = Get-AbsolutePath $Runtime
    if (-not (Test-Path (Join-Path $Runtime "CMakeLists.txt")) -or -not (Test-Path (Join-Path $Runtime "runner"))) {
        Fail "-Runtime ${Runtime}: not a Retro-Runtime checkout"
    }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail "cmake not found" }
if (-not (Test-Path (Join-Path $Root "third_party\Retro-Runtime\CMakeLists.txt"))) {
    Fail "third_party\Retro-Runtime is empty: run 'git submodule update --init' in $Root"
}
New-Item -ItemType Directory -Force -Path $Out, $Build | Out-Null
$Mark = Join-Path $Out ".retro-hub-local"
if ((Get-ChildItem -LiteralPath $Out -Force) -and -not (Test-Path $Mark)) {
    Fail "-Out $Out is not empty and was not made by build-local.ps1; pick another directory"
}

# ---- vcpkg, as CI bootstraps it ---------------------------------------------------
$Vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } elseif ($env:RETCOMM_VCPKG) { $env:RETCOMM_VCPKG } else { Join-Path $Root ".cache\vcpkg" }
if (-not (Test-Path (Join-Path $Vcpkg "scripts\buildsystems\vcpkg.cmake"))) {
    $baseline = (Get-Content -Raw (Join-Path $Root "vcpkg.json") | ConvertFrom-Json)."builtin-baseline"
    Say "cloning vcpkg at $baseline into $Vcpkg"
    Invoke-Checked git @("clone", "https://github.com/microsoft/vcpkg.git", $Vcpkg)
    Invoke-Checked git @("-C", $Vcpkg, "checkout", $baseline)
}
if (-not (Test-Path (Join-Path $Vcpkg "vcpkg.exe"))) {
    Invoke-Checked (Join-Path $Vcpkg "bootstrap-vcpkg.bat") @("-disableMetrics")
}

# ---- configure and build ------------------------------------------------------------
$commit = ""
try {
    $commit = (& git -C $Root rev-parse HEAD 2>$null)
    & git -C $Root diff --quiet HEAD -- 2>$null
    if ($LASTEXITCODE -ne 0) { $commit += "-dirty" }
} catch { $commit = "" }
$generator = if ($env:CMAKE_GENERATOR) { $env:CMAKE_GENERATOR } else { "Visual Studio 17 2022" }
$cmakeArgs = @("-S", $Root, "-B", $Build, "-G", $generator,
    "-DCMAKE_TOOLCHAIN_FILE=$Vcpkg\scripts\buildsystems\vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=$Triplet",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DRETCOMM_COMMIT=$commit")
if ($generator -like "Visual Studio*") { $cmakeArgs += @("-A", $(if ($Arch -eq "arm64") { "ARM64" } else { "x64" })) }
Say "$Platform, $Config, build $Build"
Invoke-Checked cmake $cmakeArgs
Invoke-Checked cmake @("--build", $Build, "--config", $Config, "--parallel", "$Jobs")
$Stage = Join-Path $Build "build-local-stage"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
Invoke-Checked cmake @("--install", $Build, "--config", $Config, "--prefix", $Stage)
$StageBin = Join-Path $Stage "bin"
if (-not (Test-Path (Join-Path $StageBin "retro-hub.exe"))) { Fail "retro-hub.exe was not built (it needs SDL3 and Dear ImGui)" }

# ---- the runner: the submodule's, or -Runtime's --------------------------------------
$RunnerSrc = Join-Path $StageBin "retro-core-runner.exe"
$RunnerFrom = "the Retro-Runtime submodule"
if ($Runtime) {
    $rtOut = Join-Path $Build "runtime-out"
    $rtBuild = Join-Path $Build "runtime-build"
    $rtScript = Join-Path $Runtime "scripts\build-local.ps1"
    if (Test-Path $rtScript) {
        Say "building the runner with $rtScript"
        $rtArgs = @("-Out", $rtOut, "-Build", $rtBuild, "-Jobs", "$Jobs")
        if ($Debug) { $rtArgs += "-Debug" }
        $lines = @(& $rtScript @rtArgs)
        if ($LASTEXITCODE -ne 0) { Fail "$rtScript failed" }
        $last = [string]($lines | Select-Object -Last 1)
        if (-not $last.StartsWith("RETRO_CORE_RUNNER=")) { Fail "$rtScript printed no RETRO_CORE_RUNNER= line" }
        $RunnerSrc = $last.Substring("RETRO_CORE_RUNNER=".Length)
    } else {
        Say "building the runner from $Runtime with cmake (it has no scripts\build-local.ps1)"
        Invoke-Checked cmake @("-S", $Runtime, "-B", $rtBuild, "-G", $generator,
            "-DCMAKE_TOOLCHAIN_FILE=$Vcpkg\scripts\buildsystems\vcpkg.cmake",
            "-DVCPKG_TARGET_TRIPLET=$Triplet", "-DCMAKE_BUILD_TYPE=$Config")
        Invoke-Checked cmake @("--build", $rtBuild, "--config", $Config, "--parallel", "$Jobs", "--target", "retro-core-runner")
        if (Test-Path $rtOut) { Remove-Item -Recurse -Force $rtOut }
        Invoke-Checked cmake @("--install", $rtBuild, "--config", $Config, "--prefix", $rtOut)
        $RunnerSrc = Join-Path $rtOut "bin\retro-core-runner.exe"
    }
    $RunnerFrom = $Runtime
}
if (-not (Test-Path $RunnerSrc)) { Fail "no retro-core-runner.exe at $RunnerSrc" }

# ---- the flat prefix ----------------------------------------------------------------
Get-ChildItem -LiteralPath $Out -Force | Remove-Item -Recurse -Force
New-Item -ItemType File -Path $Mark | Out-Null
Copy-Item (Join-Path $StageBin "retro-hub.exe") $Out
Copy-Item $RunnerSrc (Join-Path $Out "retro-core-runner.exe")
if (-not (Test-Path (Join-Path $StageBin "retcomm-portable.exe"))) { Fail "retcomm-portable.exe was not built (the title-app kit needs it)" }
Copy-Item (Join-Path $StageBin "retcomm-portable.exe") $Out
# DLLs: what CMake installed beside the exes (TARGET_RUNTIME_DLLS), and the
# vcpkg bin directory, which also holds the transitive ones (curl -> zlib).
Get-ChildItem -LiteralPath $StageBin -Filter "*.dll" | ForEach-Object { Copy-Item $_.FullName $Out -Force }
foreach ($d in @((Join-Path $Build "vcpkg_installed\$Triplet\bin"), (Join-Path $Build "$Config"))) {
    if (Test-Path $d) { Get-ChildItem -LiteralPath $d -Filter "*.dll" | ForEach-Object { Copy-Item $_.FullName $Out -Force } }
}
$Assets = Join-Path $Stage "share\retcomm"
Copy-Item (Join-Path $Assets "retcomm.png") $Out
foreach ($kind in @("fonts", "platforms", "controllers", "setup")) {
    Copy-Item -Recurse (Join-Path $Assets $kind) (Join-Path $Out $kind)
}
New-Item -ItemType Directory -Force -Path (Join-Path $Out "packaging") | Out-Null
foreach ($kit in @("title", "common")) {
    Copy-Item -Recurse (Join-Path $Assets "packaging\$kit") (Join-Path $Out "packaging\$kit")
}
Copy-Item (Join-Path $Root "LICENSE") $Out

# ---- check the prefix, then print where it is -------------------------------------
function Get-VersionReport([string]$Exe) {
    $file = Join-Path $Build ("version-" + [guid]::NewGuid().ToString("N") + ".txt")
    # A GUI-subsystem exe: Start-Process -Wait with redirected output.
    $p = Start-Process -FilePath $Exe -ArgumentList "--version" -NoNewWindow -Wait -PassThru -RedirectStandardOutput $file
    $text = if (Test-Path $file) { Get-Content -Raw $file } else { "" }
    Remove-Item $file -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { Fail "$Exe --version failed (exit $($p.ExitCode))" }
    return $text
}
$Hub = Join-Path $Out "retro-hub.exe"
$report = Get-VersionReport $Hub
if ($report -notmatch "(?m)^title_app 1\r?$") { Fail "$Hub --version has no 'title_app 1'" }
if ($report -notmatch "(?m)^direct_mode_flags .*--package") { Fail "$Hub --version lists no --package" }
$runnerReport = Get-VersionReport (Join-Path $Out "retro-core-runner.exe")
if ($runnerReport -notmatch "(?m)^game_package 1\r?$") { Fail "retro-core-runner.exe cannot load a game package (no 'game_package 1')" }

Say "$Platform $Config flat hub prefix in $Out"
Say "runner from $RunnerFrom"
Write-Output $report.TrimEnd()
Write-Output "RETRO_HUB=$Hub"
