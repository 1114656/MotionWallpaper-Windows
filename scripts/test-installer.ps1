[CmdletBinding()]
param(
    [string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if (-not $InstallerPath) {
    $InstallerPath = Join-Path $root "artifacts\MotionWallpaper-v$version-setup-windows-x64.exe"
}
$InstallerPath = [IO.Path]::GetFullPath($InstallerPath)
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw "安装器不存在：$InstallerPath"
}

$running = @(Get-Process -Name 'MotionWallpaper', 'motionwallpaper-agent', 'motionwallpaper-renderer' -ErrorAction SilentlyContinue)
if ($running) {
    throw '安装器测试前请先退出正在运行的 MotionWallpaper。'
}

$uninstallRoots = @(
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
)
$existingInstall = @(Get-ItemProperty -Path $uninstallRoots -ErrorAction SilentlyContinue | Where-Object {
    $_.DisplayName -eq 'MotionWallpaper' -or $_.PSChildName -like '*F2984836-8AAC-4A5E-B137-69472F784A32*'
})
if ($existingInstall) {
    throw '检测到已安装的 MotionWallpaper。为避免覆盖用户安装，已取消冒烟测试。'
}

$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$testRoot = Join-Path $temporaryBase ("MotionWallpaper-installer-test-" + [guid]::NewGuid().ToString('N'))
$installRoot = Join-Path $testRoot 'custom install path'
$installLog = Join-Path $testRoot 'install.log'
$uninstaller = Join-Path $installRoot 'unins000.exe'

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    $install = Start-Process -FilePath $InstallerPath -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=chinesesimp',
        "/DIR=`"$installRoot`"", "/LOG=`"$installLog`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($install.ExitCode -ne 0) { throw "静默安装失败，退出码：$($install.ExitCode)" }

    $appRoot = Join-Path $installRoot 'App'
    foreach ($required in @('MotionWallpaper.exe', 'motionwallpaper-agent.exe', 'motionwallpaper-renderer.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $appRoot $required) -PathType Leaf)) {
            throw "安装后缺少必需文件：$required"
        }
    }
    foreach ($forbidden in @('portable.mode', 'Config', 'Wallpapers')) {
        if (Test-Path -LiteralPath (Join-Path $appRoot $forbidden)) {
            throw "安装目录出现不应存在的项目：$forbidden"
        }
    }

    $muiRoots = @(Get-ChildItem -LiteralPath $appRoot -Recurse -File -Filter '*.mui' | ForEach-Object {
        $_.FullName.Substring($appRoot.Length + 1).Split('\')[0]
    } | Sort-Object -Unique)
    $unexpectedLanguages = @($muiRoots | Where-Object { $_ -notin @('zh-CN', 'en-us') })
    if ($unexpectedLanguages) {
        throw "安装负载包含多余语言：$($unexpectedLanguages -join ', ')"
    }
    if (@($muiRoots | Where-Object { $_ -in @('zh-CN', 'en-us') }).Count -ne 2) {
        throw "安装负载缺少中英文资源：$($muiRoots -join ', ')"
    }

    if (-not (Test-Path -LiteralPath $uninstaller -PathType Leaf)) {
        throw '安装后缺少卸载程序。'
    }
    $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -WindowStyle Hidden -Wait -PassThru
    if ($uninstall.ExitCode -ne 0) { throw "静默卸载失败，退出码：$($uninstall.ExitCode)" }
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $installRoot) {
        throw "卸载后仍残留安装目录：$installRoot"
    }

    Write-Host "安装器冒烟测试通过：中英文资源、干净目录、自定义路径和卸载均正常。"
} finally {
    if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
        Start-Process -FilePath $uninstaller -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
        ) -WindowStyle Hidden -Wait | Out-Null
    }
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if ($resolved.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolved) -like 'MotionWallpaper-installer-test-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        throw "拒绝清理意外的测试目录：$resolved"
    }
}
