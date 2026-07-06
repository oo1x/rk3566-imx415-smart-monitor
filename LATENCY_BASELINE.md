# Pipeline Latency Baseline 使用说明

本项目已在应用层加入 pipeline 分段耗时统计，不需要修改 IMX415 驱动，也不需要重新编译内核。统计口径是从 V4L2 DQBUF 后应用层拿到这一帧开始，到 RTSP 发送完成为止。

## 基础 RTSP 链路

源码：

```text
rtsp_streaming/imx415_rtsp.c
```

编译：

```bash
cd rtsp_streaming
make -f Makefile.imx415
```

运行：

```bash
./imx415_rtsp /dev/video0 1920 1080
```

每 100 帧输出一次：

```text
[latency][rtsp]
  wait_v4l2_select+dqbuf: 等待并取出 V4L2 buffer
  capture_ts_to_dqbuf:    V4L2 buffer timestamp 到应用层 DQBUF 完成
  mpp_encode:             MPP H.264 编码
  v4l2_qbuf:              归还 V4L2 buffer
  rtsp_tx_video:          RTSP/RTP 发送
  rtsp_do_event:          RTSP 事件处理
  e2e_dqbuf_to_rtsp_tx:   DQBUF 完成到 RTSP 发送完成
  loop_total_with_wait:   包含等待取帧的整轮循环耗时
```

## YOLO 检测推流链路

源码：

```text
yolo_integration/imx415_yolo_rtsp.cpp
```

编译：

```bash
cd yolo_integration
make
```

运行：

```bash
./imx415_yolo_rtsp /dev/video0 /mnt/nfs/rknn_yolo11_demo/model/yolo11.rknn
```

每 100 帧输出一次：

```text
[latency][yolo]
  wait_v4l2_select+dqbuf: 等待并取出 V4L2 buffer
  capture_ts_to_dqbuf:    V4L2 buffer timestamp 到应用层 DQBUF 完成
  rknn_inference:         RKNN YOLO11 推理与后处理
  memcpy_draw:            NV12 memcpy + 检测框/警戒线绘制
  mpp_encode:             MPP H.264 编码
  v4l2_qbuf:              归还 V4L2 buffer
  rtsp_tx_video:          RTSP/RTP 发送
  rtsp_do_event:          RTSP 事件处理
  e2e_dqbuf_to_rtsp_tx:   DQBUF 完成到 RTSP 发送完成
  loop_total_with_wait:   包含等待取帧的整轮循环耗时
```

## 优化判断

如果 `rknn_inference` 高：优先做抽帧推理、模型输入尺寸对比、person-only 后处理裁剪。

如果 `memcpy_draw` 高：优先减少拷贝、复用 buffer、考虑 RGA/OSD 叠加。

如果 `mpp_encode` 高：检查编码分辨率、码率、GOP、是否走 DMA-BUF 或 fallback memcpy。

如果 `rtsp_tx_video` 高：检查网络、RTP 包发送、客户端数量和码率。

如果 `wait_v4l2_select+dqbuf` 高：通常表示应用处理速度低于采集帧率，或者等待下一帧；需要结合 fps 判断。

## 关于驱动时间戳

当前统计是应用层基线。如果要精确到 sensor SOF/EOF 或 CSI/ISP 内部时间，需要在驱动或 media pipeline 中读取硬件/内核 timestamp，再传到应用层。这一步才需要修改驱动并重编内核。
