# 第三方软件声明

MotionWallpaper 的 MIT License 仅适用于本仓库的原创源代码。第三方组件继续适用各自的许可证条款。

## FFmpeg

可选的性能副本后端调用未经修改的 FFmpeg 可执行文件和动态链接库，来源为 BtbN Windows x64 LGPL shared build，按照 GNU LGPL v3 授权。二进制负载会在 `Tools/ffmpeg` 中保留对应许可证和声明。

- 项目主页：https://ffmpeg.org/
- 构建分发：https://github.com/BtbN/FFmpeg-Builds
- 仓库内声明：`third_party/FFmpeg-NOTICE.txt`

## Microsoft Windows 组件

项目通过 NuGet 恢复以下组件：

- Microsoft.WindowsAppSDK.Foundation 1.8.260803002；
- Microsoft.WindowsAppSDK.InteractiveExperiences 1.8.260708001；
- Microsoft.WindowsAppSDK.WinUI 1.8.260803003；
- Microsoft.Windows.CppWinRT 3.0.260715.1；
- Microsoft.Windows.SDK.BuildTools 10.0.28000.2526。

Windows App SDK 组件适用其 NuGet 包中附带的 Microsoft 许可证；C++/WinRT 使用 MIT License；SDK Build Tools 仅用于构建。

项目已不再引用 Windows App SDK 2.x Engineering Preview。固定使用的 Windows App SDK 1.8 组件包含可分发代码，其分发权仍以包内 Microsoft 许可证为准。发布时必须保留这些许可证文件，并完成 `docs/RELEASING.md` 中的检查。
