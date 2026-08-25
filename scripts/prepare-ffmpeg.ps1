param(
    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $projectRoot
$cacheRoot = Join-Path $workspaceRoot 'Codex\MotionWallpaper-ffmpeg'
$assetName = 'ffmpeg-n8.1.2-44-g7c533d0f86-win64-lgpl-shared-8.1.zip'
$headers = @{ 'User-Agent' = 'MotionWallpaper-build' }
$expectedHash = '7b32983c242dd73d43c20836582572e9927986f8cb112aafd62aa9978f4be645'
$downloadUrl = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-08-23-13-03/' + $assetName
$notice = Join-Path $projectRoot 'third_party\FFmpeg-NOTICE.txt'

function Copy-FfmpegPackage {
    param([string]$PackageRoot)
    $bin = Join-Path $PackageRoot 'bin'
    $license = Join-Path $PackageRoot 'LICENSE.txt'
    if (-not (Test-Path -LiteralPath (Join-Path $bin 'ffmpeg.exe')) -or -not (Test-Path -LiteralPath $license)) {
        throw 'The FFmpeg package is incomplete.'
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $bin -File | Where-Object {
        $_.Extension -ieq '.dll' -or $_.Name -ieq 'ffmpeg.exe'
    } | Copy-Item -Destination $Destination -Force
    Copy-Item -LiteralPath $license -Destination (Join-Path $Destination 'LICENSE-FFmpeg.txt') -Force
    Copy-Item -LiteralPath $notice -Destination $Destination -Force

    $publishedFfmpeg = Join-Path $Destination 'ffmpeg.exe'
    $encoders = (& $publishedFfmpeg -hide_banner -encoders 2>&1 | Out-String)
    foreach ($requiredEncoder in @('hevc_nvenc', 'hevc_qsv', 'hevc_amf', 'libkvazaar')) {
        if ($encoders -notmatch [Regex]::Escape($requiredEncoder)) {
            throw "The verified FFmpeg package does not provide required encoder '$requiredEncoder'."
        }
    }
    $filters = (& $publishedFfmpeg -hide_banner -filters 2>&1 | Out-String)
    if ($filters -notmatch 'scale_cuda') {
        throw "The verified FFmpeg package does not provide required filter 'scale_cuda'."
    }
}

New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
$verifiedCacheRoot = Join-Path $cacheRoot $expectedHash
$cachedPackage = if (Test-Path -LiteralPath $verifiedCacheRoot) {
    Get-ChildItem -LiteralPath $verifiedCacheRoot -Directory | Where-Object {
        Test-Path -LiteralPath (Join-Path $_.FullName 'bin\ffmpeg.exe')
    } | Select-Object -First 1
}
if ($cachedPackage) {
    Copy-FfmpegPackage $cachedPackage.FullName
    return
}

$archive = Join-Path $cacheRoot $assetName
$validArchive = (Test-Path -LiteralPath $archive) -and
    ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expectedHash)
if (-not $validArchive) {
    $download = "$archive.download"
    Invoke-WebRequest -Uri $downloadUrl -OutFile $download -Headers $headers
    $actualHash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        [IO.File]::Delete($download)
        throw 'The downloaded FFmpeg archive failed SHA-256 verification.'
    }
    Move-Item -LiteralPath $download -Destination $archive -Force
}

$expanded = Join-Path $cacheRoot $expectedHash
if (-not (Test-Path -LiteralPath $expanded)) {
    New-Item -ItemType Directory -Path $expanded | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
}
$package = Get-ChildItem -LiteralPath $expanded -Directory | Select-Object -First 1
if (-not $package) { throw 'The FFmpeg archive did not contain a package directory.' }
Copy-FfmpegPackage $package.FullName
