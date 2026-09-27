# Windows packaging helpers shared by the launcher (packaging/windows/
# package.ps1) and the title-app kit (packaging/title/build-title-app.ps1).
# Dot-source this file; it defines functions only.
#
#   Initialize-Signing / Sign-Files / Get-InnoSignArgs / Remove-SigningCertificate
#       Authenticode, from the environment (a CI secret), never from the repo:
#         WINDOWS_SIGN_PFX_BASE64    PKCS#12 certificate, base64
#         WINDOWS_SIGN_PFX_PASSWORD  its password (may be empty)
#         WINDOWS_SIGN_TIMESTAMP_URL optional RFC 3161 server
#         WINDOWS_SIGN_DESCRIPTION   optional text for the file properties / UAC UI
#       Without a certificate every Sign-* call is a no-op (with a notice).
#   New-PortableExe -Stub -StageDir -OutExe
#       The single portable .exe: [stub][zip of StageDir][uint64 size]["RCM1"]
#       (src/portable/portable_trailer.hpp). Entry names use forward slashes.
#   Expand-PortableExe -Exe -Dest
#       Reads the trailer back and unpacks the payload (packaging gates).

$script:SignTool = $null
$script:SignThumb = $null
$script:SignTs = if ($env:WINDOWS_SIGN_TIMESTAMP_URL) { $env:WINDOWS_SIGN_TIMESTAMP_URL } else { "http://timestamp.digicert.com" }
$script:SignDesc = if ($env:WINDOWS_SIGN_DESCRIPTION) { $env:WINDOWS_SIGN_DESCRIPTION } else { "Retro Launcher" }

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

function New-PortableExe {
    param(
        [Parameter(Mandatory = $true)][string]$Stub,
        [Parameter(Mandatory = $true)][string]$StageDir,
        [Parameter(Mandatory = $true)][string]$OutExe
    )
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stage = (Resolve-Path $StageDir).Path.TrimEnd('\', '/')
    $payload = [System.IO.Path]::GetTempFileName()
    try {
        # Entry names with '/' (Compress-Archive on Windows PowerShell 5.1
        # writes '\'), relative to the stage root.
        Remove-Item $payload -Force
        $zip = [System.IO.Compression.ZipFile]::Open($payload, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            Get-ChildItem -Path $stage -Recurse -File | ForEach-Object {
                $rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, $_.FullName, $rel, [System.IO.Compression.CompressionLevel]::Optimal)
            }
        } finally {
            $zip.Dispose()
        }
        if (Test-Path $OutExe) { Remove-Item $OutExe -Force }
        $stubBytes = [System.IO.File]::ReadAllBytes($Stub)
        $zipBytes = [System.IO.File]::ReadAllBytes($payload)
        $fs = [System.IO.File]::Open($OutExe, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
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
        Write-Host "Portable exe: $OutExe (stub $($stubBytes.Length) + payload $($zipBytes.Length))"
    } finally {
        Remove-Item $payload -Force -ErrorAction SilentlyContinue
    }
}

# The payload of an UNSIGNED portable exe (trailer = the last 12 bytes). A
# signed exe carries its certificate table after the trailer; the gates run
# before signing, so they never need the PE walk portable_trailer.hpp does.
function Expand-PortableExe {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string]$Dest
    )
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $bytes = [System.IO.File]::ReadAllBytes($Exe)
    $n = $bytes.Length
    if ($n -lt 12 -or [Text.Encoding]::ASCII.GetString($bytes, $n - 4, 4) -ne "RCM1") {
        throw "$Exe has no RCM1 trailer"
    }
    $size = [BitConverter]::ToUInt64($bytes, $n - 12)
    $start = $n - 12 - [int64]$size
    if ($start -lt 0) { throw "${Exe}: payload size $size exceeds the file" }
    $ms = New-Object System.IO.MemoryStream(, $bytes)
    $ms.Position = $start
    $sub = New-Object System.IO.MemoryStream
    $buf = New-Object byte[] 1048576
    $left = [int64]$size
    while ($left -gt 0) {
        $got = $ms.Read($buf, 0, [int][Math]::Min($buf.Length, $left))
        if ($got -le 0) { throw "${Exe}: short payload" }
        $sub.Write($buf, 0, $got)
        $left -= $got
    }
    $sub.Position = 0
    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    $archive = New-Object System.IO.Compression.ZipArchive($sub, [System.IO.Compression.ZipArchiveMode]::Read)
    try {
        [System.IO.Compression.ZipFileExtensions]::ExtractToDirectory($archive, $Dest)
    } finally {
        $archive.Dispose()
    }
}
