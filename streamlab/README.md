# streamlab

用 `make` 在本地启动 `http`、`https`、`rtsp`、`rtmp` 测试流。

## 依赖

- `ffmpeg`
- `mediamtx`
- `openssl`
- `python3`（仅用于本地 `http/https` 静态服务）

## 快速开始

```bash
cd streamlab
make all INPUT=../oceans.mp4
```

启动后地址默认是：

- `http://127.0.0.1:8080/oceans.mp4`
- `https://127.0.0.1:8443/oceans.mp4`
- `rtsp://127.0.0.1:8554/imvideo`
- `rtmp://127.0.0.1:1935/imvideo`

## 常用目标

```bash
make mediamtx INPUT=../oceans.mp4
make rtsp     INPUT=../oceans.mp4
make rtmp     INPUT=../oceans.mp4
make http     INPUT=../oceans.mp4
make https    INPUT=../oceans.mp4
make stop
make clean
```

## 可定制变量

```bash
make rtsp INPUT=../oceans.mp4 STREAM_NAME=demo RTSP_PORT=9554
make rtmp INPUT=../oceans.mp4 STREAM_NAME=demo RTMP_PORT=2935
```

## 验证

```bash
make probe-rtsp INPUT=../oceans.mp4
make probe-rtmp INPUT=../oceans.mp4
```

## 用示例播放器打开

```bash
examples/implayer/build/macos/ImPlayer rtsp://127.0.0.1:8554/imvideo
examples/implayer/build/macos/ImPlayer rtmp://127.0.0.1:1935/imvideo
```
