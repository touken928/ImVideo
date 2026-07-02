<p align="center">
  <h1 align="center">ImVideo</h1>
</p>

<p align="center">
  <strong>跨平台 Dear ImGui 视频播放器扩展库：FFmpeg 解码、miniaudio 音频输出、OpenGL 纹理上传——单头文件、静态链接友好。</strong>
</p>

<p align="center">
  <a href="../README.md">English</a>
</p>

<p align="center">
  <a href="https://en.cppreference.com/w/cpp/20"><img src="https://img.shields.io/badge/c++-20-blue.svg?style=for-the-badge&logo=c%2B%2B" alt="C++20"></a>
  <a href="https://cmake.org/"><img src="https://img.shields.io/badge/cmake-3.20+-064F8C.svg?style=for-the-badge&logo=cmake" alt="CMake 3.20+"></a>
  <a href="../LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg?style=for-the-badge" alt="Apache-2.0"></a>
</p>

<p align="center">
  <img src="images/implayer-screenshot.png" alt="ImPlayer 截图" width="900">
</p>

## 特性

- **FFmpeg 后端** — 基于 avcodec / avformat / swscale / swresample 的解封装、解码、缩放
- **miniaudio** — 低延迟 PCM 环形缓冲音频输出
- **OpenGL 纹理** — 帧数据直接上传为 OpenGL 纹理，无缝嵌入 ImGui
- **单头文件** — `#include <imvideo/imvideo.h>` 即可使用
- **静态链接** — vcpkg 构建产物为自包含的静态库
- **跨平台** — macOS arm64 + Windows x86-64（MinGW 交叉编译）

## 库概览

```cpp
#include <imvideo/imvideo.h>

imvideo::Player player;

// 从本地文件路径打开：
if (!player.Open(std::filesystem::path("video.mp4"))) {
    fprintf(stderr, "错误: %s\n", player.LastError());
    return;
}
// 或从网络 URL (http/https/rtsp/rtmp)：
// player.OpenUrl("http://example.com/stream.m3u8");
// player.Open(std::string_view{"https://example.com/video.mp4"});   // 自动识别
// player.Open(std::string_view{"file:///home/user/video.mp4"});     // 自动识别

player.Play();

// 在渲染循环内（需有当前 GL 上下文）：
player.Update();

// 在 ImGui 窗口内：
imvideo::Video(player);    // 完整控件：画面 + 时间轴 + 按钮
imvideo::Image(player, ImVec2(640, 360));  // 仅画面
```

## 如何引入（通过 vcpkg overlay）

本项目不提供预制的 vcpkg port，而是通过
[`examples/implayer/`](../examples/implayer/) 展示推荐的使用方式 ——
**vcpkg overlay port** 是在本地项目中使用自定义库的标准做法。

`examples/implayer/` 中内置了 overlay port 示例 `overlay/imvideo/`，直接指向本地
源码目录。如需从 GitHub 拉取，将 portfile 中的 `SOURCE_PATH` 替换为
`vcpkg_from_github(touken928/imvideo …)` 并相应调整 `vcpkg.json` 引用即可，
无需其他配置。直接参考这些文件来为自己的项目编写 overlay port。

### 快速体验演示

```bash
cd examples/implayer
cmake --preset macos
cmake --build --preset macos
./build/macos/ImPlayer /path/to/video.mp4
# 或
./build/macos/ImPlayer rtsp://127.0.0.1:8554/imvideo
```

### 本地网络流测试

可使用 `streamlab/Makefile` 将一个本地文件临时暴露成 `http`、`https`、
`rtsp`、`rtmp` 地址，方便手工验证：

```bash
cd streamlab
make all INPUT=../oceans.mp4
```

各协议目标、变量和验证方式见 `streamlab/README.md`。

## 公共 API

所有符号位于 `imvideo` 命名空间。

### `Player` 类

| 方法 | 说明 |
|------|------|
| `Open(path)` | 打开本地视频文件，失败返回 `false` |
| `Open(source)` | 打开本地路径或 URL（自动识别 `file://`、`http://`、`https://`、`rtsp://`、`rtmp://`） |
| `OpenUrl(url)` | 显式打开 URL；不支持的协议会返回清晰错误 |
| `Close()` | 关闭并释放所有资源 |
| `Play()` | 开始或恢复播放 |
| `Pause()` | 暂停在当前位置 |
| `Stop()` | 停止并回到开头 |
| `Seek(s)` | 跳转到 `s` 秒处 |
| `Position()` | 当前播放位置（秒），墙钟驱动 |
| `Duration()` | 总时长（秒），未知时返回 0 |
| `IsPlaying()` | 是否正在播放 |
| `SetVolume(v)` | 音量 0–1 |
| `SetSpeed(s)` | 播放速度（≥ 0.0625），有音频时锁定 1.0 |
| `Update()` | **每帧必须在有 GL 上下文的线程中调用** |
| `Texture()` | 当前帧的 `ImTextureID` |
| `VideoSize()` | 视频原始分辨率 |
| `LastError()` | 可读错误信息 |

### 自由函数

| 函数 | 说明 |
|------|------|
| `Image(player, size)` | 将当前帧绘制为静态图片 |
| `Video(player, size)` | 绘制完整视频控件（画面 + 控制栏） |

## 项目结构

```
├── CMakeLists.txt              # 库构建
├── include/imvideo/imvideo.h   # 公共 API（单头文件）
├── src/                        # 库实现
└── examples/implayer/          # 独立演示 + vcpkg overlay 参考
    ├── src/                    # 演示源码（main, app）
    ├── overlay/imvideo/        # 本地路径 overlay port
    ├── CMakePresets.json       # macos / macos-release / mingw-static
    └── toolchains/             # 平台工具链与链接配置
```

## 依赖项

| 包 | 作用 |
|----|------|
| FFmpeg | 解封装 + 解码（avcodec, avformat, swscale, swresample） |
| Dear ImGui | 公共 API 依赖（`imgui.h`） |
| miniaudio | 音频输出（PCM 环形缓冲 → 回调） |
| OpenGL | 纹理上传（系统库） |

*GLFW 和 ImGui 后端仅演示需要，库本身不依赖它们。*

## 协议

Apache-2.0
