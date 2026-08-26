[CmdletBinding()]
param(
    [string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if (-not $InstallerPath) {
    $InstallerPath = Join-Path $root "artifacts\MotionWallpaper-v$version-setup-windows-x64.exe"
}
$InstallerPath = [IO.Path]::GetFullPath($InstallerPath)
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw "安装器不存在：$InstallerPath"
}

$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$testRoot = Join-Path $temporaryBase ("MotionWallpaper-installer-test-" + [guid]::NewGuid().ToString('N'))
$installRoot = Join-Path $testRoot 'custom install path'
$legacyRoot = Join-Path $testRoot 'legacy-data'
$installLog = Join-Path $testRoot 'install.log'
$uninstaller = Join-Path $installRoot 'unins000.exe'
$compiler = Join-Path $root '.tools\InnoSetup\ISCC.exe'
$installerScript = Join-Path $root 'installer\MotionWallpaper.iss'
$smokeInstaller = Join-Path $testRoot 'MotionWallpaper-installer-smoke.exe'

function Get-AssociatedIconHash([string]$Path) {
    $icon = [Drawing.Icon]::ExtractAssociatedIcon($Path)
    if ($null -eq $icon) { throw "无法读取程序图标：$Path" }
    $bitmap = $icon.ToBitmap()
    $stream = [IO.MemoryStream]::new()
    try {
        $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha256.ComputeHash($stream.ToArray()))).Replace('-', '')
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
        $bitmap.Dispose()
        $icon.Dispose()
    }
}

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $legacyRoot 'Config') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Config\settings.json'), '{"version":1}')
    [IO.File]::WriteAllText((Join-Path $legacyRoot 'Wallpapers\Groups\legacy-group\group.json'), '{"version":1}')

    & $compiler "/DMyAppVersion=$version" '/DMyAppId={{1D39CB35-4E75-46D0-B117-934E57415E50}' `
        "/DLegacyDataRoot=$legacyRoot" '/DInstallerSmokeTest=1' "/O$testRoot" '/FMotionWallpaper-installer-smoke' $installerScript
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $smokeInstaller -PathType Leaf)) {
        throw "隔离测试安装器编译失败，退出码：$LASTEXITCODE"
    }

    $install = Start-Process -FilePath $smokeInstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=chinesesimp',
        '/NOCLOSEAPPLICATIONS',
        "/DIR=`"$installRoot`"", "/LOG=`"$installLog`""
    ) -WindowStyle Hidden -Wait -PassThru
    if ($install.ExitCode -ne 0) { throw "静默安装失败，退出码：$($install.ExitCode)" }

    $appRoot = Join-Path $installRoot 'App'
    foreach ($required in @('MotionWallpaper.exe', 'motionwallpaper-agent.exe', 'motionwallpaper-renderer.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $appRoot $required) -PathType Leaf)) {
            throw "安装后缺少必需文件：$required"
        }
    }
    $appIconHash = Get-AssociatedIconHash (Join-Path $appRoot 'MotionWallpaper.exe')
    $agentIconHash = Get-AssociatedIconHash (Join-Path $appRoot 'motionwallpaper-agent.exe')
    if ($appIconHash -ne $agentIconHash) {
        throw '常驻 Agent 没有使用与主程序相同的托盘图标资源。'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $appRoot 'portable.mode') -PathType Leaf)) {
        throw '安装负载缺少单目录数据标记 portable.mode。'
    }
    foreach ($migrated in @('Config\settings.json', 'Wallpapers\Groups\legacy-group\group.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $appRoot $migrated) -PathType Leaf)) {
            throw "安装器没有迁移旧数据：$migrated"
        }
    }
    $legacyRemainders = @(Get-ChildItem -LiteralPath $legacyRoot -Force -ErrorAction SilentlyContinue)
    if ($legacyRemainders.Count -gt 0) {
        throw "迁移成功后仍残留旧数据：$($legacyRemainders.Name -join ', ')"
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
    New-Item -ItemType Directory -Path (Join-Path $appRoot 'Config') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $appRoot 'Wallpapers\Groups\installer-smoke-test') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $appRoot 'Config\agent.log'), "smoke test`r`n")
    [IO.File]::WriteAllText((Join-Path $appRoot 'Wallpapers\Groups\installer-smoke-test\group.json'), '{}')
    $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -WindowStyle Hidden -Wait -PassThru
    if ($uninstall.ExitCode -ne 0) { throw "静默卸载失败，退出码：$($uninstall.ExitCode)" }
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $installRoot) {
        throw "卸载后仍残留安装目录：$installRoot"
    }

    Write-Host "安装器冒烟测试通过：中英文资源、单目录数据、自定义路径和完整卸载均正常。"
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
