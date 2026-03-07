# Player

基于 **FFmpeg 7.1 + SDL3** 构建的多线程音视频播放器，使用 C++17 编写，支持软解和 NVIDIA NVDEC 硬件加速解码。

---

## 目录

- [功能特性](#功能特性)
- [依赖环境](#依赖环境)
- [构建方法](#构建方法)
- [使用方法](#使用方法)
- [键盘操作](#键盘操作)
- [项目结构](#项目结构)
- [架构设计](#架构设计)
- [模块说明](#模块说明)
- [AV 同步机制](#av-同步机制)
- [线程模型](#线程模型)

---

## 功能特性

- 播放主流视频格式（MP4、MKV、AVI、MOV 等）
- 软解（CPU）和 NVIDIA NVDEC 硬件加速解码
- 音画同步（以音频时钟为基准）
- 变速播放（0.25x ～ 3.0x），音调不变（atempo 滤镜）
- 实时 Seek（快进/快退），支持关键帧精确跳转
- 音量调节，OSD 信息叠加层
- 窗口自适应缩放（保持原始宽高比，自动补黑边）
- 支持 YUV420P / NV12 / 其他格式（libswscale 自动转换）

---

## 依赖环境

| 依赖 | 版本 | 说明 |
|------|------|------|
| FFmpeg | 7.1（库版本 61） | 解封装、解码、滤镜、重采样、格式转换 |
| SDL3 | 最新稳定版 | 窗口、渲染器、音频设备 |
| CMake | ≥ 3.20 | 构建系统 |
| C++ 编译器 | C++17 支持 | MinGW-w64 / MSVC |

预编译的 FFmpeg 和 SDL3 库已放置于 `3rdparty/` 目录，无需额外安装。

---

## 构建方法

```bash
# 在项目根目录执行
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

构建完成后，可执行文件 `Player.exe` 以及所需 DLL 均位于 `build/` 目录。

---

## 使用方法

```bash
# 基本用法（软件解码）
Player.exe <视频文件路径>

# 启用 NVIDIA 硬件加速（需要 NVDEC 支持的 GPU）
Player.exe --hw <视频文件路径>

# 指定初始音量（0～100，默认 80）
Player.exe --vol 60 <视频文件路径>

# 显示帮助
Player.exe --help
```

---

## 键盘操作

| 按键 | 功能 |
|------|------|
| `Space` | 暂停 / 继续播放 |
| `Esc` | 退出 |
| `→` 右方向键 | 快进 10 秒 |
| `←` 左方向键 | 快退 10 秒 |
| `↑` 上方向键 | 音量 +5% |
| `↓` 下方向键 | 音量 -5% |
| `Tab` | 循环切换播放倍速（0.25x → 0.5x → 0.75x → 1.0x → 1.5x → 2.0x → 2.5x → 3.0x → 0.25x …） |
| `Left Shift` | 切换 OSD 信息叠加层显示/隐藏 |
| 鼠标滚轮 | 音量 ±3% |

---

## 项目结构

```
Player/
├── src/                    # 源代码
│   ├── main.cpp            # 程序入口，命令行解析
│   ├── Config.h            # 全局配置常量
│   ├── Clock.h             # AV 同步时钟
│   ├── PacketQueue.h       # 线程安全压缩包队列
│   ├── FrameQueue.h        # 线程安全解码帧队列
│   ├── Demuxer.h/cpp       # 解封装模块
│   ├── VideoDecoder.h/cpp  # 视频解码模块
│   ├── AudioDecoder.h/cpp  # 音频解码 + 重采样模块
│   ├── VideoRenderer.h/cpp # 视频渲染模块
│   ├── AudioOutput.h/cpp   # 音频输出模块
│   ├── OSD.h/cpp           # 屏幕信息叠加层
│   ├── Font8x8.h           # 8×8 点阵字体数据
│   └── Player.h/cpp        # 顶层控制器，事件循环
├── 3rdparty/               # 预编译第三方库
│   ├── ffmpeg/             # FFmpeg 头文件、静态库、DLL
│   │   ├── include/
│   │   ├── lib/
│   │   └── bin/
│   └── sdl3/               # SDL3 头文件、静态库、DLL
│       ├── include/
│       ├── lib/
│       └── bin/
├── build/                  # 构建输出目录
├── .vscode/                # VSCode 调试配置
│   ├── launch.json         # 调试启动配置（软解/硬解两个配置）
│   └── tasks.json          # 构建任务
└── CMakeLists.txt          # CMake 构建脚本
```

---

## 架构设计

### 整体架构

播放器采用**生产者-消费者**多线程模型，由 4 条线程协同工作：

```
┌─────────────────────────────────────────────────────────┐
│                      主线程（Main Thread）                │
│  SDL 事件循环 → tryRenderVideoFrame() → updateOSD() → present()  │
└────────────────────────┬────────────────────────────────┘
                         │ 读取 FrameQueue
                         │
┌────────────┐   PacketQueue   ┌──────────────────┐
│  Demuxer   │ ─── 视频包 ───▶ │  VideoDecoder    │ ─▶ FrameQueue
│  Thread    │                 │  Thread          │
│            │ ─── 音频包 ───▶ │  AudioDecoder    │ ─▶ AudioOutput ─▶ SDL 音频设备
└────────────┘   PacketQueue   └──────────────────┘
                                         │
                                   Clock.set(PTS)
                                         │
                              主线程读取 Clock.get()
                              决定视频帧的显示时机
```

### 数据流

```
媒体文件
  │
  ▼
Demuxer（解封装）
  ├─▶ VideoPacketQueue ─▶ VideoDecoder ─▶ FrameQueue ─▶ VideoRenderer ─▶ 窗口
  └─▶ AudioPacketQueue ─▶ AudioDecoder ─▶ AudioOutput ─▶ SDL 音频设备
                                               │
                                         Clock.set(当前 PTS)
```

---

## 模块说明

### Config.h — 全局配置

集中管理所有可调参数：

| 常量 | 默认值 | 说明 |
|------|--------|------|
| `VIDEO_PACKET_QUEUE_SIZE` | 128 | 视频包队列容量 |
| `AUDIO_PACKET_QUEUE_SIZE` | 256 | 音频包队列容量 |
| `VIDEO_FRAME_QUEUE_SIZE` | 8 | 解码帧队列容量 |
| `AUDIO_SAMPLE_RATE` | 44100 | 音频采样率 |
| `AUDIO_CHANNELS` | 2 | 声道数（立体声） |
| `AV_SYNC_THRESHOLD_MIN` | 0.04s | AV 同步最小容差 |
| `AV_SYNC_THRESHOLD_MAX` | 0.10s | AV 同步最大容差 |
| `FRAME_DROP_THRESHOLD` | 0.5s | 帧丢弃阈值 |
| `SPEED_LEVELS` | 8 档 | 0.25x 到 3.0x |

---

### Clock.h — AV 同步时钟

以音频播放位置为基准的主时钟，线程安全。

- `set(pts)` — 音频线程每次推送 PCM 时调用，记录当前媒体时间
- `get()` — 视频线程读取，通过 `pts + 已流逝时间 × speed` 插值推算当前位置
- `seek(pts)` — Seek 后将时钟重置到目标位置
- `pause() / resume()` — 暂停时冻结时钟，恢复时继续走动
- `setSpeed(speed)` — 变速后调整推算速率

---

### PacketQueue.h / FrameQueue.h — 线程安全缓冲队列

- `push()` — 生产者调用，队列满时阻塞等待
- `pop()` — 消费者调用，队列空时阻塞等待
- `peek()` — 非破坏性地查看队首元素（仅 FrameQueue，用于 AV 同步判断）
- `abort() / resetAbort()` — 中止/恢复阻塞操作，用于退出和 Seek

---

### Demuxer — 解封装模块

后台线程持续从文件读取压缩包（`AVPacket`），按流索引分发到视频包队列和音频包队列。

- Seek 请求由主线程通过 `requestSeek(seconds)` 发起，在解封装线程内执行 `av_seek_frame()`，避免跨线程访问 `AVFormatContext`

---

### VideoDecoder — 视频解码模块

后台线程从视频包队列取出压缩包，调用 FFmpeg 解码为 `AVFrame`，推入帧队列。

- **硬件加速**：尝试初始化 NVDEC（`h264_cuvid` / `hevc_cuvid` 等），失败则自动回退软解
- **Seek 优化**：
  - `flush()` — 设置 `flush_pending_` 标志，由解码线程自行调用 `avcodec_flush_buffers()`，避免跨线程操作 codec context
  - `setSkipUntilPts(pts)` — Seek 后在解码线程内丢弃早于目标时间的帧，不经过帧队列，消除 vsync 阻塞延迟

---

### AudioDecoder — 音频解码模块

后台线程解码音频包，通过 `libswresample` 重采样到 S16 立体声 44100Hz，推送给 `AudioOutput`。

- **变速播放**：通过 FFmpeg `atempo` 滤镜实现变速不变调
  - 速度 = 1.0x 时直接重采样，不建立滤镜图
  - 其他速度时构建 `aresample=out_sample_fmt=fltp,atempo=<speed>` 滤镜图
  - 速度切换时在滤镜 drain 循环内检查 `speed_changed_` 标志，提前退出，防止旧速度音频在 flush 后继续推送导致的电流声

---

### VideoRenderer — 视频渲染模块

在主线程运行，管理 SDL3 窗口和渲染器。

- 支持原生 YUV420P 和 NV12 纹理，其他格式通过 `libswscale` 先转换为 YUV420P
- `renderFrame(frame)` — 上传帧数据到 GPU 纹理并渲染
- `blitLastFrame()` — 不上传新数据，仅重绘上一帧（用于暂停状态和 Seek 等待期）
- `calcDstRect()` — 计算保持宽高比的目标矩形（信箱/柱箱黑边）

---

### AudioOutput — 音频输出模块

接收解码线程推送的 PCM 数据，写入 SDL3 音频流。

- 每次 `pushData()` 根据当前缓冲字节数估算实际播放位置，更新主时钟：
  ```
  clock = end_pts - buffered_bytes / bytes_per_second × speed
  ```
- **速度切换时钟精度**：记录切换瞬间的旧速度缓冲字节数（`trans_bytes_`），在混合窗口期分别计算新旧速度字节对应的媒体时长，避免时钟跳变
- **Seek 后过滤**：`setMinPts(pts)` 丢弃关键帧到目标时间段的音频帧，防止时钟被拖回

---

### OSD — 屏幕信息叠加层

使用内嵌 8×8 点阵字体，通过 SDL3 逐像素绘制，无需外部字体库。

显示内容（按 `Left Shift` 切换）：

```
PLAYING  00:01:23 / 00:10:30
FPS: 60.0   Video: h264  Audio: aac
1920×1080   Bitrate: 4096 Kbps
Vol: 80%    Speed: 1.00x   SW
```

---

### Player — 顶层控制器

协调所有模块，在主线程运行 SDL 事件循环。

**Seek 流程**：
1. 计算目标时间，调用 `demuxer_->requestSeek(target)`
2. 冲刷视频/音频解码器（设置 flush 标志）
3. 清空 SDL 音频缓冲，设置 `setMinPts(target)` 过滤旧音频
4. 设置 `setSkipUntilPts(target)` 让视频解码线程内部丢帧
5. 设置 `seek_pending_ = true`：下一帧到达时跳过 AV 同步检查，直接渲染并将时钟重新锚定到实际解码位置（防止 av_seek_frame 延迟期间时钟超前导致的级联丢帧）

---

## AV 同步机制

```
音频线程每次 pushData() 时：
  Clock.set(end_pts - buffered_seconds)  ← 当前正在播放的音频位置

主线程 tryRenderVideoFrame() 时：
  diff = frame.pts - Clock.get()

  diff < -FRAME_DROP_THRESHOLD(-0.5s)  → 批量丢弃过时帧
  diff > sync_threshold(0.04~0.10s)    → 帧太早，等下一个渲染周期
  否则                                  → 渲染此帧
```

Seek 后首帧特殊处理（`seek_pending_` 标志）：
- 跳过时钟比较，直接渲染第一个到达的帧
- 用该帧的真实 PTS 重新锚定时钟，消除 av_seek_frame 延迟造成的时钟超前问题
