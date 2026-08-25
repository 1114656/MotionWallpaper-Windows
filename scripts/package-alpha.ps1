param(
    [string]$Version,
    [string]$BuildDirectory,
    [string]$ArtifactDirectory
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $Version = (Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw).Trim()
}
if ($Version -notmatch '^\d+\.\d+\.\d+-alpha\.\d+$') {
    throw "Alpha release candidate version '$Version' is not in the expected 0.0.0-alpha.0 form."
}

if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $projectRoot 'build'
}
if (-not $ArtifactDirectory) {
    $ArtifactDirectory = Join-Path $projectRoot 'artifacts'
}

$buildRoot = [IO.Path]::GetFullPath($BuildDirectory)
$artifactRoot = [IO.Path]::GetFullPath($ArtifactDirectory)
$entryPoint = Join-Path $buildRoot 'MotionWallpaper.exe'
if (-not (Test-Path -LiteralPath $entryPoint -PathType Leaf)) {
    throw "Published application was not found at $entryPoint. Run scripts\build-native.ps1 first."
}

$archiveName = "MotionWallpaper-v$Version-windows-x64.zip"
$archivePath = Join-Path $artifactRoot $archiveName
$checksumPath = "$archivePath.sha256"
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$stagingRoot = Join-Path $temporaryBase ("MotionWallpaper-package-" + [guid]::NewGuid().ToString('N'))
$payloadRoot = Join-Path $stagingRoot ("MotionWallpaper-v$Version-windows-x64")

function Get-RelativePath {
    param(
        [Parameter(Mandatory)][string]$BasePath,
        [Parameter(Mandatory)][string]$TargetPath
    )
    $baseFullPath = [IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
    $targetFullPath = [IO.Path]::GetFullPath($TargetPath)
    $relative = ([Uri]$baseFullPath).MakeRelativeUri([Uri]$targetFullPath).ToString()
    [Uri]::UnescapeDataString($relative).Replace('/', '\')
}

try {
    New-Item -ItemType Directory -Path $payloadRoot -Force | Out-Null
    $excludedRoots = @('Wallpapers', 'Config')
    $excludedExtensions = @('.exp', '.iobj', '.ipdb', '.lib', '.pdb', '.log')

    Get-ChildItem -LiteralPath $buildRoot -Recurse -Force -File | ForEach-Object {
        $relativePath = Get-RelativePath $buildRoot $_.FullName
        $topLevel = ($relativePath -split '[\\/]', 2)[0]
        if ($topLevel -in $excludedRoots -or $_.Extension.ToLowerInvariant() -in $excludedExtensions) {
            return
        }
        $destination = Join-Path $payloadRoot $relativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    }

    New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
    Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $checksumPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -LiteralPath $payloadRoot -DestinationPath $archivePath -CompressionLevel Optimal

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        $prefix = "MotionWallpaper-v$Version-windows-x64/"
        $requiredEntries = @(
            'MotionWallpaper.exe',
            'motionwallpaper-agent.exe',
            'motionwallpaper-renderer.exe',
            'portable.mode',
            'LICENSE.txt',
            'THIRD_PARTY_NOTICES.md',
            'Tools/ffmpeg/ffmpeg.exe',
            'Tools/ffmpeg/FFmpeg-NOTICE.txt',
            'Tools/ffmpeg/LICENSE-FFmpeg.txt'
        )
        foreach ($required in $requiredEntries) {
            if (($prefix + $required) -notin $entries) {
                throw "Package is missing required entry: $required"
            }
        }

        $forbidden = @($entries | Where-Object {
            $_ -like ($prefix + 'Wallpapers/*') -or
            $_ -like ($prefix + 'Config/*') -or
            $_ -match '(?i)\.(pdb|lib|exp|log)$' -or
            $_ -match '(^|/)(tmp|output|artifacts)/'
        })
        if ($forbidden.Count -gt 0) {
            throw "Package contains forbidden entries: $($forbidden -join ', ')"
        }

        $supportedResourceLanguages = @('zh-CN', 'en-us')
        $unsupportedLanguageResources = @($entries | Where-Object { $_ -match '(?i)\.mui$' } | ForEach-Object {
            $relative = $_.Substring($prefix.Length)
            ($relative -split '/', 2)[0]
        } | Where-Object { $_ -notin $supportedResourceLanguages } | Select-Object -Unique)
        if ($unsupportedLanguageResources.Count -gt 0) {
            throw "Package contains unsupported language resources: $($unsupportedLanguageResources -join ', ')"
        }
    } finally {
        $archive.Dispose()
    }

    $hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText($checksumPath, "$hash  $archiveName`r`n", [Text.UTF8Encoding]::new($false))
    Write-Host "Validated alpha release candidate archive: $archivePath"
    Write-Host "SHA-256: $hash"
} finally {
    $resolvedStaging = [IO.Path]::GetFullPath($stagingRoot)
    if ($resolvedStaging.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedStaging) -like 'MotionWallpaper-package-*') {
        Remove-Item -LiteralPath $resolvedStaging -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        throw "Refusing to remove unexpected staging path: $resolvedStaging"
    }
}
