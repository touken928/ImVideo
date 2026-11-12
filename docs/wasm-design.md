# WebAssembly 核心库支持

核心保留实验性的 Emscripten 静态库、FFmpeg 软件解码、pthreads 和 WebGL 2 支持。
WASM 示例已移除；浏览器宿主、文件导入、页面控件和音频设备由应用自行提供。

## 构建

激活 `profiles/emscripten` 固定的 emsdk 4.0.10，安装 Conan 2、CMake、Ninja 和
pkg-config 后，从仓库根目录创建包：

```sh
source /path/to/emsdk/emsdk_env.sh
conan create . -pr:b default -pr:h profiles/emscripten -b missing
```

宿主通过 Conan 安装该包，再使用 `find_package(imvideo CONFIG REQUIRED)` 和
`imvideo::imvideo`。包会传播 pthread、异常模型和 WebGL 2 链接要求；宿主负责设置
线程池、内存预算、虚拟文件系统导出及自己的 JS 绑定。所有依赖使用相同 SDK 和线程 ABI。

该 profile 设置 `tools.build:skip_test=True`；原生 CTest 不能在交叉编译目标上执行。
跳过原生测试不等于验证了浏览器运行时，应用需要自行验证其浏览器宿主。

## 输入与线程

- `Source::file()` 指向 Emscripten 虚拟文件系统；宿主先准备文件再调用核心。
- 核心浏览器构建拒绝直接 URL/RTSP 输入。下载、CORS、取消和输入大小限制属于宿主。
- Player 的阻塞控制和析构在工作线程执行，Renderer 在拥有当前 WebGL context 的线程执行。
  不要在浏览器主线程等待解码线程，也不要将所有 getter 假定为可跨 open/close 并发调用。
- 解码和显示帧上限为 2,073,600 像素；该预算不保证任意编码参数的媒体都能装入宿主内存。

## 显示与音频

`Options::gpu_color_conversion` 默认关闭。开启后，偶数宽高、8 位 YUV420P/YUVJ420P、
BT.601/BT.709 且无需 SAR 重采样的帧可由 Renderer 上传平面并完成 GPU 转色和旋转。
其他输入保留 CPU 转换。`Renderer::texture()` 始终提供当前 context 中的 RGBA 纹理名称。

宿主在 context 丢失后调用 `Renderer::invalidate()`，恢复后重新上传有效帧；正常释放
图形资源时仍需有效 context。Renderer 保存并恢复上传和离屏绘制涉及的宿主 GL 状态。

音频设备继续由调用方的 `AudioSink` 提供。Web Audio/AudioWorklet 的异步设备准备、
用户激活和共享 PCM 缓冲属于宿主。`clock_seconds()` 应反映实际消费的媒体样本，欠载
补零不推进媒体时钟；seek、变速和关闭须正确取消等待并清除旧代次 PCM。
`AudioSink::drain()` 用于排出设备适配层的重采样尾部。

## 部署

浏览器宿主需提供 WebGL 2、SharedArrayBuffer 和跨源隔离，并正确部署完整运行时产物。
本地使用 localhost，生产环境使用 HTTPS 和以下响应头：

```http
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

媒体格式、内存峰值、音画同步、像素与颜色、GL 状态恢复及 context 丢失恢复均应在
应用实际使用的浏览器中验证。核心模块的职责和资源管理见 [架构说明](architecture.md)。
