# MotionWallpaper 原生工程

本目录包含 C++/WinRT + WinUI 3 设置应用、原生策略 Agent、Media Foundation Renderer、公共基础设施和原生测试。

```text
<程序目录>\
├─ MotionWallpaper.exe
├─ motionwallpaper-agent.exe
├─ motionwallpaper-renderer.exe
├─ Wallpapers\Groups\...\
└─ Config\
   ├─ settings.json
   └─ runtime.json
```

`MotionWallpaper.exe` 是唯一面向用户的可执行文件。Agent 和 Renderer 是内部辅助程序：设置窗口关闭后 Agent 继续驻留；需要播放视频、静态图片或保留冻结画面时 Renderer 才运行。

现有分组、设置和导入媒体会在原位置继续使用。

## 构建要求

- Visual Studio 2026 Build Tools，或包含兼容 MSVC 的 Visual Studio
- 使用 C++ 的桌面开发
- Windows 应用开发 / WinUI 工具
- Windows 10/11 SDK 10.0.19041 或更高版本

执行 `scripts\build-native.ps1`。NuGet 只恢复项目所需的稳定依赖：

- Microsoft.WindowsAppSDK.Foundation 1.8.260803002
- Microsoft.WindowsAppSDK.InteractiveExperiences 1.8.260708001
- Microsoft.WindowsAppSDK.WinUI 1.8.260803003
- Microsoft.Windows.CppWinRT 3.0.260715.1
- Microsoft.Windows.SDK.BuildTools 10.0.28000.2526

可执行文件会发布到现有 `build` 目录，以保留固定的 `Wallpapers` 媒体库。发布前脚本会运行 `MotionWallpaper.Tests.exe`。

项目已经移除 Windows App SDK 2.x Engineering Preview 引用。为控制自包含负载大小，没有引入未使用的 AI、ML、Widgets 和 DWrite 组件。分发二进制负载前请阅读 `../THIRD_PARTY_NOTICES.md` 和 `../docs/RELEASING.md`。

`scripts\build-native.ps1` 通过 `vswhere` 查找 Visual Studio 工具链，恢复依赖、构建全部原生项目，并将自包含 WinUI 运行时发布到可执行文件旁。

传入 `-SkipPublish` 可以只恢复、构建和运行测试，不生成应用负载或下载 FFmpeg；CI 使用该模式。
