# 架构说明

## 产品不变量

1. 导入源文件时逐字节复制，不对源文件转码、缩放或覆盖。
2. 自动平衡和低功耗文件属于可删除的派生副本；用户也可以在至少保留一个可播放副本时单独删除源文件。
3. 媒体库始终位于程序可执行文件旁的 `Wallpapers` 目录。
4. 常驻 Agent 只负责策略，不承载 WinUI，也不解码视频帧。
5. 桌面、动态屏保和冻结画面复用同一条 Renderer 时间线。
6. `Win+L` 会停止 Renderer，不能被当作闲置屏保处理。

## 原生项目

- `MotionWallpaper.App`：WinUI 3 组合根、页面和界面事件处理。
- `SettingsStore`：规范化设置持久化与 Agent 通知。
- `MediaLibrary`：分组、后台单次导入、重复内容复用、重命名、排序、移动和回收站删除。加载媒体库时不会隐式执行破坏性的重复分组合并。
- `ThumbnailGenerator`：受尺寸限制的视频帧和静态图片封面生成。
- `MotionWallpaper.Core`：供 App、Agent 和集成测试共用的媒体操作、缩略图和转码静态库。
- `MotionWallpaper.Common`：配置模型、Windows JSON、路径、ID、策略决策、命名事件和 Win32 句柄所有权。
- `MotionWallpaper.Agent`：单实例、事件驱动的常驻策略进程。
- `MotionWallpaper.Renderer`：Media Foundation、D3D11、DXGI 与 DirectComposition 呈现。
- `MotionWallpaper.Tests`：配置、状态策略、媒体库和协议回归测试。

`MotionWallpaper.exe` 是唯一面向用户的可执行文件。设置窗口关闭后 Agent 继续驻留；只有需要播放媒体或保留冻结帧时 Renderer 才存在。

关闭设置窗口会保存待处理修改并只退出 WinUI 进程。从 Agent 托盘菜单选择“退出”时，会广播命名退出事件，让设置进程先保存并正常关闭，再停止 Renderer 和 Agent，最终不保留任何 MotionWallpaper 进程。

## 存储结构

```text
<程序目录>\
├─ MotionWallpaper.exe
├─ motionwallpaper-agent.exe
├─ motionwallpaper-renderer.exe
├─ Wallpapers\Groups\{group-id}\
│  ├─ group.json
│  └─ Videos\{media-id}\
│     ├─ source.<原扩展名>          # 可由用户单独删除
│     ├─ poster.png                # 删除源文件后继续保留
│     ├─ metadata.json             # 名称、类型、修订号和副本状态
│     └─ Variants\
│        ├─ balanced-*.mp4
│        └─ power-*.mp4
└─ Config\
   ├─ settings.json               # 仅设置应用写入
   └─ runtime.json                # 活动媒体，仅 Agent 写入
```

如果两个性能档位生成的内容与规格完全一致，可以通过硬链接共享一个物理文件。界面仍显示两个逻辑档位，但容量统计按物理文件去重。

删除源文件后保留 `poster.png`、`metadata.json` 和已有副本。原画模式不可选择，也不能重新生成副本；最后一个可播放副本受到保护，避免留下无法播放的媒体条目。

设置、运行状态和元数据都先写入唯一临时文件，再通过原子替换提交。按路径划分的跨进程互斥锁负责串行化替换，媒体元数据带有单调递增修订号。

设置始终以完整、规范化对象保存，包括空选择值。读取时仍兼容旧的 `selectedVideoId` 字段，并在下次加载时迁移到 `selectedMediaId`；废弃字段不会继续写回。

## 运行流程

```text
WinUI 修改 -> 原子保存设置 -> 命名事件 -> Agent 计算目标状态
                                         -> Renderer 命令 + 类型化 ACK
                                         -> 发布已确认的 runtime.json
```

设置变化会通过命名事件立即唤醒 Agent。会话、显示器电源、显示拓扑、Explorer、前台窗口、最小化和顶层窗口位置事件也会触发策略计算。

覆盖检测会枚举所有可见、未最小化且未被 DWM 隐藏的顶层窗口，而不仅检查焦点窗口。进入和退出全屏无需等待 500 ms 桌面兜底轮询；重复位置事件会合并。锁定会话检查使用 1 秒兜底。

状态归约器的固定优先级为：

1. 显示器关闭或会话锁定；
2. 闲置动态屏保；
3. 桌面播放关闭或没有有效媒体；
4. 全屏窗口覆盖；
5. 用户活动时冻结；
6. 正常播放。

Renderer 在冻结、暂停、循环以及桌面/屏保宿主切换时保留最后提交帧。普通暂停会停止帧调度，但保留已经暂停的解码器，避免恢复时从关键帧高速追帧。

冻结边界还会按完整显示分辨率捕获到一张 DirectComposition 表面。Windows 报告低内存压力时，DWM 先原子接管该表面，再释放 Media Engine 和双缓冲交换链。恢复时在保留表面后创建新交换链、定位到记录时间并提交首帧，最后才切换视觉层。深度压缩不会降低分辨率或帧率。

同一 GPU 上的多个显示器由一个 Renderer 共享一条解码链和一个 D3D11 设备，但每块显示器拥有独立、原生尺寸的 DirectComposition 视觉层和交换链。跨显卡显示器使用不同 Renderer，避免隐式跨适配器复制。

## 解码路径

播放视频前，Renderer 会读取原生视频子类型，并枚举匹配编码格式的硬件 Media Foundation Decoder Transform：

- `auto`：探测成功时请求硬件路径，否则明确回退到软件/WARP 路径；
- `hardware`：没有兼容硬件解码器时直接报告不可用；
- `software`：始终使用软件/WARP 路径。

Renderer 将决策报告给 Agent，由 Agent 写入 `Config/runtime.json` 供界面显示。`IMFMediaEngine` 不公开最终使用的解码器变换标识，因此“硬件”只表示检测到匹配硬件 MFT 并请求了硬件/DXGI 路径，不代表厂商级遥测。

驱动 Trim 只在闲置深度压缩后执行。正常路径不会强制清空进程工作集，以免快速恢复时产生缺页卡顿。

播放期间使用基于 Media Foundation 时间戳的高精度单次调度：在下一帧预计到达前唤醒，帧未准备好时仅执行短重试；非播放状态完全停止调度。

## 命令与确认

Renderer 命令携带单调递增修订号。目标状态 ACK 与轻量控制 ACK 分开跟踪，避免时钟开关等控制命令误满足首帧切换等待。

Agent 只在目标状态获得确认后更新 `runtime.json`，因此界面不会把尚未确认的选择标成当前壁纸。快速连续选择采用最终请求生效，过期确认不能覆盖新状态。

## 验证

`scripts\build-native.ps1` 会恢复依赖、构建、运行定时器、Renderer 子进程协议、双进程配置、损坏恢复、媒体库、状态策略和 UI 绑定测试，然后在保留 `Wallpapers` 与 `Config` 的前提下镜像应用负载。

性能候选版还应覆盖以下人工测试：

- 1080p/4K H.264 与 HEVC 长时间播放；
- 重复循环、快速选择和快速开关；
- `Alt + Tab`、桌面/屏保切换和锁定/解锁；
- 多显示器和跨显卡布局；
- CPU、GPU Video Decode、工作集、句柄数、磁盘读取和丢帧统计。
