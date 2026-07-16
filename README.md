# RK3566 IMX415 智能监控视频采集与 RTSP 推流系统

本项目面向智慧社区智能监控场景，基于 RK3566 与 IMX415 摄像头构建端侧视频链路，实现从 Sensor 采集、V4L2 取帧、YOLO11n RKNN 推理、H.264 硬件编码到 RTSP 推流的完整流程。

项目当前重点在端侧开发，服务端转发和客户端预览可作为完整监控系统的其他模块。本仓库主要保留 RK3566 端侧相关源码和说明文档。

`s4-stable-1080p60` 分支对应 2026-07-15 完成三轮实测的 S4 稳定版本：1080p60、每 5 帧触发一次推理、`latest_if_idle` 异步提交、复用最新检测框。完整配置、运行命令、实测数据和已知限制见 [`docs/S4_STABLE_1080P60_2026-07-16.md`](docs/S4_STABLE_1080P60_2026-07-16.md)。

## 功能链路

```text
IMX415 Sensor
  -> MIPI CSI-2
  -> RKISP / V4L2
  -> V4L2 DQBUF 取帧
  -> RGA letterbox 预处理
  -> RKNN / NPU YOLO11n 推理
  -> 检测框与警戒线叠加
  -> MPP H.264 硬件编码
  -> RTSP 推流
  -> ffplay 局域网预览
```

## 目录结构

```text
sensor_driver/
  imx415.c                         IMX415 V4L2 subdev 驱动
  overlays/                        LubanCat / RK356x IMX415 设备树 overlay
  dphy_reference/                  DPHY 调试参考源码

rtsp_streaming/
  v4l2_capture.c / h               V4L2 采集封装
  mpp_encoder.c / h                MPP H.264 编码封装
  rtsp_demo.c                      RTSP 服务与推流
  imx415_rtsp.c                    基础 IMX415 RTSP 推流入口
  Makefile.imx415                  基础推流编译脚本

yolo_integration/
  imx415_yolo_rtsp.cpp             YOLO11n + H.264 + RTSP 主程序
  rknn_yolo11/                     RKNN YOLO11 推理与后处理
  rknn_utils/                      RGA / 图像处理工具
  Makefile                         YOLO 推流版本编译脚本

docs/
  PROJECT_REPORT.md                完整项目说明文档

tests/performance/
  run_benchmark.py                 S0-S5 自动化执行器
  collect_system_metrics.py        板端资源采集器
  analyze_metrics.py               统一统计与校验
```

## 当前实现方案

### 摄像头驱动

IMX415 通过 V4L2 subdev 接入 RKISP，驱动负责上电时序、寄存器配置、MIPI CSI-2 输出、pad format 协商和 stream 控制。

当前目标输出：

```text
1920x1080@60fps
NV12
/dev/video0
```

### V4L2 采集

应用层使用 V4L2 mmap buffer 取帧，并导出 DMA-BUF fd，便于 RGA / MPP 等硬件模块使用。

S4 稳定版本使用 4 个 V4L2 mmap/DMA-BUF，以覆盖采集、RGA 叠加和 MPP 编码期间的缓冲生命周期；视频交接仍采用“最新帧优先”，不建立无界旧帧队列。

### YOLO11n 端侧部署

当前模型为 RK 官方 YOLO11n INT8 RKNN 模型。

```text
yolo11.rknn
MD5: 91faf3f5526db7ecfed3a61a99a3ef75
```

推理策略：

```text
每 5 帧形成一次 YOLO 推理触发
仅在 NPU 空闲时提交最新帧（latest_if_idle）
视频线程不等待推理，持续复用最近一次检测结果
```

### RGA 预处理

RGA 用于 YOLO 输入预处理：

```text
NV12 1920x1080 -> RGB888 640x640 letterbox
```

### MPP H.264 编码

使用 Rockchip MPP 硬件编码 H.264。当前使用 Baseline + CAVLC 降低编码复杂度，并去除冗余 memcpy。

### RTSP 推流

板端启动 RTSP server，推流路径：

```text
rtsp://<board-ip>:8554/live
```

拉流示例：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop -rtsp_transport tcp rtsp://192.168.1.20:8554/live
```

## 编译说明

本项目依赖 RK3566 / LubanCat 交叉编译环境和 Rockchip 相关 SDK：

- RKNN Runtime
- RGA
- Rockchip MPP
- TurboJPEG
- stb_image
- aarch64-linux-gnu 交叉编译工具链
- 与板端匹配的 sysroot

基础 RTSP 推流：

```bash
cd rtsp_streaming
make -f Makefile.imx415
```

YOLO 推流：

```bash
cd yolo_integration
make
```

说明：`yolo_integration/Makefile` 中保留了原开发环境路径，迁移环境时需要修改 sysroot、RKNN Model Zoo、MPP、RGA 等路径。

## 运行示例

板端运行 YOLO + RTSP：

```bash
cd /home/cat/latency_test
env \
  LD_LIBRARY_PATH=. \
  PERF_AI_ENABLED=1 \
  PERF_INFER_INTERVAL=5 \
  PERF_SUBMIT_POLICY=latest_if_idle \
  PERF_REUSE_BOXES=1 \
  PERF_DETAILED_TRACE=0 \
  PERF_INFER_QUEUE_LIMIT=8 \
  ./imx415_yolo_rtsp_1080p60_scripted_test \
  /dev/video0 \
  /home/cat/latency_test/model/yolo11.rknn
```

PC / Ubuntu 拉流：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop -rtsp_transport tcp rtsp://192.168.1.20:8554/live
```

## S4 正式测试结果

S4 在固定场景下完成 3 轮、每轮 300 秒测试，三轮均通过：

```text
V4L2 采集                 60.000 fps
板端编码输出              58.746 fps
RTSP 客户端窗口           58.624 fps
检测结果更新              11.748 fps
最大推理队列              1
框年龄 P95                150.301 ms
板内采集到编码输出 P95     14.902 ms
纯 NPU rknn_run P95        58.403 ms
```

这里的 14.902 ms 是板内同一单调时钟下的采集到编码输出，不是客户端屏幕端到端延迟；本轮没有跨设备同步时钟或可见时间码，因此不再沿用历史文档中的约 71 ms 作为正式端到端结论。

关键改动包括异步推理、最新帧空闲提交、DMA-BUF 编码、RTSP 非阻塞发送游标修复、60 fps 有理数 RTP 时间戳和每秒周期 IDR。详细证据与限制见 S4 稳定版本说明。

## NPU 频率分析

正式 S4 测试将 NPU 固定在 600 MHz userspace governor：

```text
cur_freq = 600000000
max_freq = 900000000
governor = userspace
S4 平均负载 = 49.03%
```

此前临时切换 900 MHz 时在 5V2A 电源下出现稳定性不足，因此 S4 正式结论只对应 600 MHz。后续若更换 5V3A 电源，需要重新进行满频测试和 RKNN profiling。

## 文档

完整项目说明见：

```text
docs/PROJECT_REPORT.md
docs/S4_STABLE_1080P60_2026-07-16.md
```

其中包含驱动链路、V4L2 采集、YOLO11n 部署、RGA、MPP、RTSP、多线程优化、DMA-BUF、NPU 频率、量化说明、测试验证和简历表述建议。

代码模块讲解与流程图见：

```text
docs/CODE_WALKTHROUGH.md
```

其中按 Sensor 驱动、设备树、V4L2 采集、YOLO 端侧部署、MPP 编码、RTSP 推流和低延迟优化拆分说明每部分做了什么、对应哪些代码、面试时应该怎么讲。

## 后续方向

- 更换 5V3A 电源，验证 NPU 900 MHz 满频性能。
- 开启 RKNN profiling，确认是否存在 CPU fallback。
- 尝试 YOLO11n 416x416 / 320x320 输入尺寸。
- 评估 YOLOv5n / YOLOv6n 在 RK3566 上的推理性能。
- 使用真实监控画面重新做 INT8 calibration。
- 进一步探索 RGA blit 到编码 DMA-BUF，减少 CPU 整帧拷贝。
