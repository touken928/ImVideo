# imvideo

**跨平台 Dear ImGui 视频播放器扩展库。**

`imvideo` 提供一套极简的、单头文件的 API，让你能在 ImGui 窗口中像使用普通控件一样嵌入视频播放。

- **解码** — FFmpeg 解封装 + 软件解码，独立工作线程。
- **视频** — swscale RGBA 转换 → 有界线程安全队列 → 渲染线程 OpenGL 纹理上传（通过 `Update()`）。
- **音频** — swresample → float PCM → 无锁环形缓冲 → miniaudio 回调节点（独立音频线程）。
  播放位置使用单调墙钟驱动，音频欠载或设备状态不会冻结视频。
- **稳定模型** — 解码受有界缓冲节流，播放中不会提前跑完整个文件；暂停/停止/seek 可中断。
- **倍速** — 有音频时锁定为 1.0 倍（v0.1 暂未实现音频变速）。

---

## 构建

### 前置要求

- **CMake ≥ 3.20**
- **C++20 编译器**（Clang 14+、GCC 11+、MSVC 2022+）
- **Ninja**（推荐）或 Make
- **[vcpkg](https://github.com/microsoft/vcpkg)**，并设置 `VCPKG_ROOT` 环境变量

### 库 + 演示（项目根目录）

```bash
git clone <仓库地址> imvideo
cd imvideo

cmake -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build

# 运行演示播放器
./build/examples/imv/imv /path/to/video.mp4
```

### 独立构建演示（独立的 vcpkg 项目）

`examples/imv/` 是一个完整的独立 CMake 项目，拥有自己的预设。

```bash
cd examples/imv

cmake --preset default
cmake --build --preset default

./build/default/imv /path/to/video.mp4
```

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `IMVIDEO_BUILD_DEMO` | `ON` | 构建 `imv` 演示应用 |

### 手动构建

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

---

## API 示例

```cpp
#include <imvideo/imvideo.h>

imvideo::Player player;

// 打开文件
if (!player.Open("video.mp4")) {
    fprintf(stderr, "错误: %s\n", player.LastError());
    return;
}

// 开始播放
player.Play();

// 在渲染循环内（需要有当前 GL 上下文）：
player.Update();                // 弹出解码帧 + 上传纹理

// 在 ImGui 窗口内：
imvideo::Video(player);         // 完整控件：画面 + 时间轴 + 按钮

// 或者仅显示画面：
imvideo::Image(player, ImVec2(640, 360));
```

---

## 公共 API

所有符号位于 `imvideo` 命名空间下。

### `Player` 类

| 方法 | 说明 |
|------|------|
| `Open(path)` | 打开视频文件，失败返回 `false` |
| `Close()` | 关闭并释放所有资源 |
| `Play()` | 开始或恢复播放 |
| `Pause()` | 暂停在当前位置 |
| `Stop()` | 停止并回到开头 |
| `Seek(s)` | 跳转到 `s` 秒处 |
| `Position()` | 当前播放位置（秒），由单调墙钟驱动 |
| `Duration()` | 总时长（秒），未知时返回 0 |
| `IsPlaying()` | 是否正在播放 |
| `SetVolume(v)` | 音量 0–1 |
| `SetSpeed(s)` | 播放速度（≥ 0.0625），有音频时锁定为 1.0 |
| `Update()` | **每帧必须在有 GL 上下文的线程中调用** |
| `Texture()` | 当前帧的 `ImTextureID` |
| `VideoSize()` | 视频原始分辨率 |
| `LastError()` | 可读错误信息 |

### 自由函数

| 函数 | 说明 |
|------|------|
| `Image(player, size)` | 将当前帧绘制为静态图片 |
| `Video(player, size)` | 绘制完整视频控件（画面 + 控制栏） |

---

## ⚠ 渲染线程 / OpenGL 说明

**`Update()` 和 `Player` 析构函数需要有当前 OpenGL 上下文。**
它们会创建、更新和销毁内部纹理。必须从拥有 GL 上下文的同一线程调用
（通常是主渲染/UI 线程）。库不会自行创建上下文，也不会调用
`glfwMakeContextCurrent` 或类似函数。

解码工作线程和音频回调运行在独立线程上，不涉及 OpenGL。

---

## 演示功能（`imv`）

- **顶部原生工具栏**：`Open...`、`About`
- **底部控制栏**：▶ Play / || Pause / ■ Stop、音量滑块、全宽进度条、时间显示
- **居中视频**：自动缩放适应窗口，保持宽高比，水平和垂直居中
- **原生文件对话框**：通过操作系统文件选择器打开视频
- **拖拽播放**：直接将文件拖入窗口
- **命令行参数**：`imv file1.mp4 file2.mkv`
- **音频播放**：支持带音轨的视频文件

---

## 项目结构

```
├── CMakeLists.txt              # 顶层库构建
├── vcpkg.json                  # vcpkg manifest（所有依赖）
├── cmake/
│   ├── FindFFMPEG.cmake        # FFmpeg 查找模块
│   └── imvideoConfig.cmake.in  # 包配置模板
├── include/imvideo/
│   └── imvideo.h               # 公共 API（单头文件）
├── src/                        # 库实现
│   ├── player_impl.hpp
│   ├── player.cpp
│   ├── video.cpp
│   ├── audio_engine.hpp / .cpp
│   ├── ring_buffer.hpp
│   └── video_frame.hpp
└── examples/imv/               # 独立演示播放器（自带 vcpkg + CMake）
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── vcpkg.json
    ├── main.cpp / app.hpp / app.cpp
    ├── file_dialog.hpp / _mac.mm / _linux.cpp / _win.cpp
    └── overlay/imvideo/        # imvideo 的 vcpkg overlay port
```

---

## 依赖（通过 vcpkg）

| 包 | 说明 |
|----|------|
| FFmpeg | `avcodec`, `avformat`, `avutil`, `swscale`, `swresample` |
| Dear ImGui | `glfw-binding`, `opengl3-binding`, `docking-experimental` |
| GLFW3 | 窗口 / OpenGL 上下文（仅演示需要） |
| OpenGL | 系统 OpenGL 库 |
| miniaudio | 音频输出（回调读取 PCM 环形缓冲） |

---

## 版本历史

- **v0.1** — 多线程解码、miniaudio 音频输出、稳定墙钟播放时机、有界队列、有音频时速度锁定、播放结束处理。
