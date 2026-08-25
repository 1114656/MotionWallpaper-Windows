param(
    [switch]$SkipPublish
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $projectRoot 'native\MotionWallpaper.Native.sln'
$outputDirectory = Join-Path $projectRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

function Get-PublishedProcesses {
    param([string[]]$Names)
    $publishedRoot = [IO.Path]::GetFullPath($outputDirectory).TrimEnd('\') + '\'
    Get-Process -Name $Names -ErrorAction SilentlyContinue | Where-Object {
        try {
            $processPath = [IO.Path]::GetFullPath($_.Path)
            $processPath.StartsWith($publishedRoot, [StringComparison]::OrdinalIgnoreCase)
        } catch { $false }
    }
}

function Get-PortableRelativePath {
    param(
        [Parameter(Mandatory)][string]$BasePath,
        [Parameter(Mandatory)][string]$TargetPath
    )
    # IO.Path.GetRelativePath is unavailable in Windows PowerShell 5.1.
    # Uri.MakeRelativeUri keeps the build entry point usable from both the
    # inbox shell and PowerShell 7 without maintaining two publish scripts.
    $baseFullPath = [IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
    $targetFullPath = [IO.Path]::GetFullPath($TargetPath)
    $relative = ([Uri]$baseFullPath).MakeRelativeUri([Uri]$targetFullPath).ToString()
    [Uri]::UnescapeDataString($relative).Replace('/', '\')
}

$publishedProcesses = @(Get-PublishedProcesses @('MotionWallpaper', 'motionwallpaper-agent', 'motionwallpaper-renderer'))
$appWasRunning = [bool]($publishedProcesses | Where-Object ProcessName -EQ 'MotionWallpaper')
$agentWasRunning = [bool]($publishedProcesses | Where-Object ProcessName -EQ 'motionwallpaper-agent')

# Some portable Build Tools layouts leave non-existent optional ATL/VS entries
# in LIB. Roslyn reports those inherited paths as warnings even though this
# solution does not use them.
if ($env:LIB) {
    $env:LIB = (($env:LIB -split ';') | Where-Object { $_ -and (Test-Path -LiteralPath $_) }) -join ';'
}

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio with Desktop development with C++ and WinUI tools is required. vswhere.exe was not found.'
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    throw 'MSBuild was not found. Install the Desktop development with C++ workload.'
}

& $msbuild $solution -t:Restore -m
if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed with exit code $LASTEXITCODE" }

& $msbuild $solution -t:Build -p:Configuration=Release -p:Platform=x64 -m
if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE" }

$testExecutable = Join-Path $projectRoot 'native\x64\Release\MotionWallpaper.Tests\MotionWallpaper.Tests.exe'
& $testExecutable
if ($LASTEXITCODE -ne 0) { throw "Native tests failed with exit code $LASTEXITCODE" }

if ($SkipPublish) {
    Write-Host 'Native build and tests completed successfully; publish was skipped.'
    return
}

$nativeOutput = Join-Path $projectRoot 'native\x64\Release\MotionWallpaper.App'
$nativeExecutable = Join-Path $nativeOutput 'MotionWallpaper.exe'
if (-not (Test-Path -LiteralPath $nativeExecutable)) {
    throw "Native executable was not found at $nativeExecutable"
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
@(Get-PublishedProcesses @('MotionWallpaper', 'motionwallpaper-agent', 'motionwallpaper-renderer')) |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
$preservedRoots = @('Wallpapers', 'Config')
$developmentOnlyExtensions = @(
    '.exp',
    '.iobj',
    '.ipdb',
    '.lib',
    '.pdb'
)
$payloadFiles = @(Get-ChildItem -LiteralPath $nativeOutput -Recurse -File | Where-Object {
    $relativePath = Get-PortableRelativePath $nativeOutput $_.FullName
    $topLevel = ($relativePath -split '[\\/]', 2)[0]
    $extension = $_.Extension.ToLowerInvariant()
    $topLevel -notin $preservedRoots -and $extension -notin $developmentOnlyExtensions
})
$payloadPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$payloadFiles | ForEach-Object {
    $relativePath = Get-PortableRelativePath $nativeOutput $_.FullName
    [void]$payloadPaths.Add($relativePath)
    $destination = Join-Path $outputDirectory $relativePath
    $existing = Get-Item -LiteralPath $destination -ErrorAction SilentlyContinue
    if (-not $existing -or $existing.Length -ne $_.Length -or $existing.LastWriteTimeUtc -ne $_.LastWriteTimeUtc) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    }
}

# Mirror only application payload files. User media and settings are preserved.
Get-ChildItem -LiteralPath $outputDirectory -Recurse -File | ForEach-Object {
    $relativePath = Get-PortableRelativePath $outputDirectory $_.FullName
    $topLevel = ($relativePath -split '[\\/]', 2)[0]
    if ($topLevel -notin $preservedRoots -and -not $payloadPaths.Contains($relativePath)) {
        Remove-Item -LiteralPath $_.FullName -Force
    }
}
Get-ChildItem -LiteralPath $outputDirectory -Recurse -Directory |
    Sort-Object { $_.FullName.Length } -Descending |
    ForEach-Object {
        $relativePath = Get-PortableRelativePath $outputDirectory $_.FullName
        $topLevel = ($relativePath -split '[\\/]', 2)[0]
        if ($topLevel -notin $preservedRoots -and -not (Get-ChildItem -LiteralPath $_.FullName -Force)) {
            Remove-Item -LiteralPath $_.FullName -Force
        }
    }

# Windows App SDK self-contained output includes satellite resources for every
# translated WinUI language. MotionWallpaper currently supports Simplified
# Chinese with English fallback, so keep only those two resource directories.
$supportedResourceLanguages = @('zh-CN', 'en-us')
Get-ChildItem -LiteralPath $outputDirectory -Directory | ForEach-Object {
    $resourceFiles = @(Get-ChildItem -LiteralPath $_.FullName -Recurse -File)
    $isSatelliteLanguageDirectory = $resourceFiles.Count -gt 0 -and
        @($resourceFiles | Where-Object { $_.Extension -ine '.mui' }).Count -eq 0
    if ($isSatelliteLanguageDirectory -and $_.Name -notin $supportedResourceLanguages) {
        Remove-Item -LiteralPath $_.FullName -Recurse -Force
    }
}

# The ZIP remains genuinely portable. The installer deliberately excludes this
# marker so installed builds store mutable data under LocalAppData instead.
[IO.File]::WriteAllText((Join-Path $outputDirectory 'portable.mode'), "portable`r`n", [Text.UTF8Encoding]::new($false))

# Keep the optional optimization backend inside the installed application.
# Downloads and extraction stay in the workspace Codex cache, not in the repo.
& (Join-Path $PSScriptRoot 'prepare-ffmpeg.ps1') -Destination (Join-Path $outputDirectory 'Tools\ffmpeg')

# Ship the project and third-party terms with every locally published payload.
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination (Join-Path $outputDirectory 'LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $outputDirectory 'THIRD_PARTY_NOTICES.md') -Force

# Normalize the user-facing executable name for Explorer.
$publishedApp = Get-ChildItem -LiteralPath $outputDirectory -File | Where-Object { $_.Name -ieq 'MotionWallpaper.exe' } | Select-Object -First 1
if ($publishedApp.Name -cne 'MotionWallpaper.exe') {
    $caseTemporary = Join-Path $outputDirectory 'MotionWallpaper.casefix.exe'
    Move-Item -LiteralPath $publishedApp.FullName -Destination $caseTemporary -Force
    Move-Item -LiteralPath $caseTemporary -Destination (Join-Path $outputDirectory 'MotionWallpaper.exe') -Force
}

if ($appWasRunning) {
    Start-Process -FilePath (Join-Path $outputDirectory 'MotionWallpaper.exe')
} elseif ($agentWasRunning) {
    Start-Process -FilePath (Join-Path $outputDirectory 'motionwallpaper-agent.exe') -WindowStyle Hidden
}

Write-Host "Built native MotionWallpaper into $outputDirectory"
