# RK3566 IMX415 智能监控视频采集与 RTSP 推流系统

本项目面向智慧社区智能监控场景，基于 RK3566 与 IMX415 摄像头构建端侧视频链路，实现从 Sensor 采集、V4L2 取帧、YOLO11n RKNN 推理、H.264 硬件编码到 RTSP 推流的完整流程。

项目当前重点在端侧开发，服务端转发和客户端预览可作为完整监控系统的其他模块。本仓库主要保留 RK3566 端侧相关源码和说明文档。

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
```

## 当前实现方案

### 摄像头驱动

IMX415 通过 V4L2 subdev 接入 RKISP，驱动负责上电时序、寄存器配置、MIPI CSI-2 输出、pad format 协商和 stream 控制。

当前目标输出：

```text
1920x1080@30fps
NV12
/dev/video0
```

### V4L2 采集

应用层使用 V4L2 mmap buffer 取帧，并导出 DMA-BUF fd，便于 RGA / MPP 等硬件模块使用。

采集优化将 V4L2 buffer 数量从 4 个减少到 2 个，降低旧帧排队。

### YOLO11n 端侧部署

当前模型为 RK 官方 YOLO11n INT8 RKNN 模型。

```text
yolo11.rknn
MD5: 91faf3f5526db7ecfed3a61a99a3ef75
```

推理策略：

```text
每 3 帧执行一次真实 YOLO 推理
其余帧复用最近一次检测结果
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
ffplay -fflags nobuffer -flags low_delay -framedrop rtsp://192.168.1.20:8554/live
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
LD_LIBRARY_PATH=/home/cat/latency_test ./imx415_yolo_rtsp_latency /dev/video0 /home/cat/latency_test/model/yolo11.rknn
```

PC / Ubuntu 拉流：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop rtsp://192.168.1.20:8554/live
```

## 延迟优化结果

原始基线：

```text
端到端延迟：约 213-216 ms
```

当前推荐版本：

```text
端到端延迟：约 71 ms
```

关键优化：

- V4L2 buffer 数量从 4 调整为 2。
- YOLO11n 每 3 帧推理一次，其余帧复用检测结果。
- MPP H.264 编码改为低复杂度配置。
- 去除冗余 memcpy。
- RTSP RTP 包队列恢复合理大小，避免关键帧不完整。
- 将串行链路改为采集线程与处理/编码/推流线程分离。
- 多线程版本进一步采用 V4L2 buffer 直通交接，减少应用层帧池拷贝和旧帧等待。

当前典型耗时：

```text
采集到应用层取帧：25-26 ms
YOLO 每帧平均：28-29 ms
YOLO 单次真实推理：85-88 ms
MPP 编码：12 ms
Sensor timestamp 到 RTSP 发送：约 71 ms
```

## NPU 频率分析

当前发现 RK3566 NPU 默认运行在 600 MHz：

```text
cur_freq = 600000000
max_freq = 900000000
governor = rknpu_ondemand
load = 100@600000000Hz
```

临时切换 performance 后可到 900 MHz，但当前 5V2A 电源下稳定性不足。后续建议更换 5V3A 电源后进行满频测试和 RKNN profiling。

## 文档

完整项目说明见：

```text
docs/PROJECT_REPORT.md
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
