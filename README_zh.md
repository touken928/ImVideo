# imvideo

**跨平台 Dear ImGui 视频播放器扩展库。**  
FFmpeg 解码 + miniaudio 输出 + OpenGL 纹理上传。

## 作为库引入（vcpkg）

在项目的 `vcpkg.json` 中添加 `imvideo` 依赖，并通过 overlay port
指向克隆仓库中的 `ports/` 目录：

```json
{
  "dependencies": ["imvideo"],
  "vcpkg-configuration": {
    "overlay-ports": [ "./path/to/imvideo/ports" ]
  }
}
```

然后在 CMake 中 `find_package` 并链接：

```cmake
find_package(imvideo CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE imvideo::imvideo)
```

**从 GitHub 快速开始：**

```bash
git clone https://github.com/touken928/imvideo.git
```

将 `"overlay-ports": [ "./imvideo/ports" ]` 加入你的 `vcpkg-configuration`
（或 `vcpkg.json`）即可。

### 最小使用示例

```cpp
#include <imvideo/imvideo.h>

imvideo::Player player;

if (!player.Open("video.mp4")) {
    fprintf(stderr, "错误: %s\n", player.LastError());
    return;
}
player.Play();

// 在渲染循环内（需有当前 GL 上下文）：
player.Update();

// 在 ImGui 窗口内：
imvideo::Video(player);    // 完整控件：画面 + 时间轴 + 按钮
imvideo::Image(player, ImVec2(640, 360));  // 仅画面
```

## 构建示例（IMV）

`examples/imv/` 是一个独立的 CMake + vcpkg 项目，用于演示库的功能：

```bash
cd examples/imv

cmake --preset default
cmake --build --preset default

./build/default/imv /path/to/video.mp4
```

## 公共 API

所有符号位于 `imvideo` 命名空间。

### `Player` 类

| 方法 | 说明 |
|------|------|
| `Open(path)` | 打开视频文件，失败返回 `false` |
| `Close()` | 关闭并释放所有资源 |
| `Play()` | 开始或恢复播放 |
| `Pause()` | 暂停在当前位置 |
| `Stop()` | 停止并回到开头 |
| `Seek(s)` | 跳转到 `s` 秒处 |
| `Position()` | 当前播放位置（秒），由墙钟驱动 |
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

## ⚠ 重要：OpenGL 上下文要求

`Update()` 和 `Player` 析构函数**需要有当前 OpenGL 上下文**。
必须从拥有 GL 上下文的同一线程调用（通常是主渲染/UI 线程）。
库不会自行创建或管理 GL 上下文。

## 项目结构

```
├── CMakeLists.txt              # 库构建
├── vcpkg.json                  # 库依赖（ffmpeg, imgui, miniaudio）
├── ports/imvideo/              # vcpkg overlay port（供消费者使用）
├── cmake/                      # CMake 辅助模块
├── include/imvideo/imvideo.h   # 公共 API（单头文件）
├── src/                        # 库实现
└── examples/imv/               # 独立演示（自带 vcpkg + CMake）
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

MIT
