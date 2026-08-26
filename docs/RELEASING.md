# 发布清单

MotionWallpaper 目前处于 Alpha 阶段。WinUI 依赖固定在稳定的 Windows App SDK 1.8 维护版本。公开 Alpha 安装器和便携包必须经过 CI、内容、安装/卸载和校验和验证，并明确标注为未签名预发布软件。

当前候选版本从仓库根目录的 `VERSION` 读取。单 EXE 安装器由 `scripts\build-installer.ps1` 生成，便携包由 `scripts\package-alpha.ps1` 生成；版本标签推送成功后，`Alpha Release` 工作流会重新构建、测试、打包并创建公开预发布版。

## 源码预发布

1. 确认 `git status --short` 干净。
2. 执行 `scripts\build-native.ps1 -SkipPublish`，要求零错误且全部原生测试通过。
3. 检查暂存差异中是否包含设置、日志、媒体、绝对本机路径、凭据、生成文件或二进制文件。
4. 确认包含 `LICENSE`、`SECURITY.md`、`CONTRIBUTING.md` 和 `THIRD_PARTY_NOTICES.md`。
5. 在 GitHub 仓库设置中启用私密漏洞报告。
6. 推送提交并等待 Native CI 通过。
7. 创建类似 `v0.1.0-alpha.1` 的预发布标签；标签必须与 `VERSION` 完全一致。

## 公开 Alpha 候选版

1. 完整执行 `scripts\build-native.ps1` 发布流程。
2. 执行 `scripts\package-alpha.ps1` 和 `scripts\build-installer.ps1 -SkipBuild`，要求两种分发物验证通过。
3. 确认两种负载只包含简体中文和英文资源，且不包含 `Wallpapers`、`Config`、日志、调试符号和本机测试证据。
4. 静默安装到临时自定义目录，验证程序、设置、日志与壁纸数据集中位于 `App`，随后测试旧数据迁移、升级和卸载。
5. 确认卸载会删除用户壁纸和设置，且不会留下旧 LocalAppData 目录或开机启动注册表值。
6. 上传安装 EXE、便携 ZIP 及匹配的 `.sha256` 文件，并将 GitHub Release 标记为 Pre-release。
7. 明确标注为未签名 Alpha 软件，提醒用户注意 SmartScreen；安装版卸载会清除全部数据，便携版升级时需保留 `Wallpapers` 与 `Config`。

## 未来稳定二进制版本

在附加可运行程序包前必须完成：

1. 确认锁定依赖中没有 Preview 或 Engineering Preview 版本。
2. 重新审计全部 NuGet 依赖和随附工具的许可证。
3. 在干净电脑上完整执行 `scripts\build-native.ps1` 发布流程。
4. 确认负载包含 `LICENSE.txt`、`THIRD_PARTY_NOTICES.md`，以及 `Tools/ffmpeg` 下的 FFmpeg 声明和 LGPL 许可证。
5. 测试安装、升级、卸载、媒体保留、多显示器变化、睡眠、锁定/解锁、关闭显示器恢复和 Explorer 重启。
6. 扫描最终压缩包并公开 SHA-256 校验值。
7. 完成代码签名，并验证 Windows SmartScreen 与杀毒软件误报情况。
