[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
$build = Join-Path $root 'build'
$artifacts = Join-Path $root 'artifacts'
$script = Join-Path $root 'installer\MotionWallpaper.iss'
$compiler = Join-Path $root '.tools\InnoSetup\ISCC.exe'

if ($version -notmatch '^\d+\.\d+\.\d+-alpha\.\d+$') {
    throw "VERSION 格式无效：$version"
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-native.ps1')
    if ($LASTEXITCODE -ne 0) { throw "原生发布构建失败，退出码：$LASTEXITCODE" }
}

$required = @(
    'MotionWallpaper.exe',
    'motionwallpaper-agent.exe',
    'motionwallpaper-renderer.exe',
    'LICENSE.txt',
    'THIRD_PARTY_NOTICES.md'
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $name) -PathType Leaf)) {
        throw "安装负载缺少必需文件：$name"
    }
}

$allowedLanguages = @('zh-CN', 'en-us')
$unexpectedLanguages = @(Get-ChildItem -LiteralPath $build -Directory | Where-Object {
    $files = @(Get-ChildItem -LiteralPath $_.FullName -Recurse -File -ErrorAction SilentlyContinue)
    $files.Count -gt 0 -and @($files | Where-Object { $_.Extension -ne '.mui' }).Count -eq 0 -and
        $_.Name -notin $allowedLanguages
})
if ($unexpectedLanguages) {
    throw "安装负载包含多余语言目录：$($unexpectedLanguages.Name -join ', ')"
}

if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'install-inno-tool.ps1')
}
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw '未找到 Inno Setup 命令行编译器。'
}

New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
& $compiler "/DMyAppVersion=$version" $script
if ($LASTEXITCODE -ne 0) { throw "安装器编译失败，退出码：$LASTEXITCODE" }

$installer = Join-Path $artifacts "MotionWallpaper-v$version-setup-windows-x64.exe"
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "安装器未生成：$installer"
}

$checksum = "$installer.sha256"
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($installer))" | Set-Content -LiteralPath $checksum -Encoding ascii

Write-Host "安装器已生成：$installer"
Write-Host "SHA-256：$hash"
