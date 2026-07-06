# RK3566 IMX415 智能监控视频采集与推流系统项目说明文档

## 1. 项目概述

本项目面向智慧社区智能监控场景，基于 RK3566 嵌入式平台和 IMX415 摄像头，构建端侧视频采集、AI 推理、H.264 硬件编码与 RTSP 推流链路，实现视频数据从 Sensor 到网络侧稳定输出。

项目整体可抽象为：

```text
IMX415 Sensor
  -> MIPI CSI-2
  -> RKISP / V4L2
  -> 应用层取帧
  -> RGA 图像预处理
  -> RKNN / NPU YOLO11n 推理
  -> 检测框与警戒线叠加
  -> MPP H.264 硬件编码
  -> RTSP 推流
  -> 局域网 ffplay 拉流预览
```

项目定位为完整监控系统中的端侧开发部分。服务端转发、客户端播放和业务平台可作为组内其他模块，本人主要负责 RK3566 端侧开发，包括摄像头驱动适配、V4L2 采集链路、AI 模型部署、H.264 编码、RTSP 推流和低延迟优化。

## 2. 项目职责边界

本人负责内容：

- IMX415 摄像头驱动适配与调试。
- V4L2 采集链路打通。
- RK3566 上的视频帧获取、格式处理和时间戳统计。
- YOLO11n RKNN 模型端侧部署。
- RGA 图像预处理接入。
- MPP H.264 硬件编码接入。
- RTSP 推流链路实现。
- ffplay 局域网拉流验证。
- 端到端延迟打点、瓶颈分析和优化。

非本人负责内容：

- 服务端公网转发。
- 用户端 App / Web 客户端预览。
- 云端存储和告警平台。
- 多用户权限系统。

这些模块可作为完整智慧社区监控项目背景存在，但简历和面试中应明确本人负责的是端侧视频链路。

## 3. 硬件与软件环境

硬件平台：

- 主控：RK3566。
- 摄像头：IMX415。
- 视频输入：MIPI CSI-2。
- NPU：RK3566 内置 NPU。
- 编码：Rockchip MPP 硬件 H.264 编码器。

软件环境：

- Ubuntu 18.04 作为交叉编译环境。
- LubanCat / RK3566 Linux 系统。
- V4L2 摄像头采集接口。
- RKNN Runtime / RKNN Toolkit2。
- RGA 图像加速库。
- Rockchip MPP 编码库。
- RTSP demo 推流库。
- ffplay 作为局域网拉流验证工具。

## 4. 代码模块结构

当前整理后的项目核心代码集中在：

```text
rk3566_imx415_smart_monitor_project
```

主要模块：

```text
rtsp_streaming/
  v4l2_capture.c / h      V4L2 摄像头采集
  mpp_encoder.c / h       MPP H.264 编码
  rtsp_demo.c             RTSP 服务与发送
  rtp_enc.c               RTP 打包
  stream_queue.c          RTSP 内部流队列

yolo_integration/
  imx415_yolo_rtsp.cpp    主程序，串联采集、YOLO、编码、推流
  rknn_yolo11/            YOLO11 RKNN 推理代码
  rknn_utils/             RGA / 图像处理工具
```

摄像头驱动相关代码位于 LubanCat kernel 中的 IMX415 驱动文件，核心是 `imx415.c`，负责 sensor 上电、寄存器配置、MIPI 输出、V4L2 subdev 注册和格式协商。

## 5. Sensor 驱动链路

IMX415 驱动属于 V4L2 subdev 驱动，主要职责是让 sensor 能被 RKISP 识别和调度。

核心流程：

```text
设备树匹配 IMX415
  -> probe 初始化
  -> 获取 clk / gpio / regulator
  -> 上电时序
  -> 写 sensor 寄存器
  -> 配置 MIPI CSI-2 输出
  -> 注册 v4l2_subdev
  -> 暴露 pad / format / stream 控制接口
```

关键点：

- 通过 V4L2 subdev 框架接入 RKISP。
- 通过 pad ops 配置媒体链路格式。
- 通过 v4l2_ctrl 支持曝光、增益等控制。
- 通过 sensor 寄存器配置 RAW10 输出。
- 通过 dphy / mipi 参数保证链路稳定。

驱动侧的目标是让 IMX415 能稳定输出 `1920x1080@30fps` 图像，并被 RKISP mainpath 输出为应用层可读的 NV12 视频帧。

## 6. V4L2 采集模块

应用层通过 V4L2 从 RKISP mainpath 取帧。

采集流程：

```text
open /dev/video0
  -> VIDIOC_QUERYCAP
  -> VIDIOC_S_FMT 设置 NV12 1920x1080
  -> VIDIOC_REQBUFS 申请 buffer
  -> mmap 映射 buffer
  -> VIDIOC_EXPBUF 导出 DMA-BUF fd
  -> VIDIOC_QBUF 入队
  -> VIDIOC_STREAMON 开流
  -> select 等待帧
  -> VIDIOC_DQBUF 取帧
  -> 使用 / 交接 buffer
  -> VIDIOC_QBUF 归还 buffer
```

当前采集格式：

```text
分辨率：1920x1080
帧率：30fps
像素格式：NV12
V4L2 buffer 数量：2
```

采集优化：

- 将 V4L2 buffer 数量从 4 个减少到 2 个。
- 减少应用层取到旧帧的概率。
- 将采集到应用层取帧延迟从约 83 到 86 ms 降到约 25 到 26 ms。

## 7. YOLO11n 端侧部署

项目使用 RK 官方 YOLO11n 模型，转换为 RKNN 后部署在 RK3566 NPU 上。

模型确认结果：

```text
源模型：yolo11n.onnx
RKNN 模型：yolo11.rknn
模型大小：约 4.5 MB
MD5：91faf3f5526db7ecfed3a61a99a3ef75
```

当前模型已经是 YOLO11 系列中最轻量的 `yolo11n`，不是 `yolo11s` 或 `yolo11m`。

YOLO 推理流程：

```text
NV12 1920x1080 输入帧
  -> RGA letterbox 预处理
  -> 转成 640x640 RGB888
  -> RKNN/NPU 执行 YOLO11n
  -> 输出 NC1HWC2 格式 tensor
  -> 转换为后处理格式
  -> 检测框解码
  -> 阈值过滤
  -> NMS
  -> 输出 person 检测结果
```

当前单次真实 YOLO 推理细分：

```text
RGA 预处理：约 6 ms
RKNN/NPU 真推理：约 72 到 74 ms
输出格式转换：约 5 ms
后处理：约 1 ms
完整单次真实推理：约 85 到 88 ms
```

为了降低每帧平均耗时，当前采用推理降频策略：

```text
每 3 帧执行一次真实 YOLO 推理
其余帧复用最近一次检测结果
```

这样可以将 YOLO 每帧平均耗时降低到约 28 到 29 ms。

## 8. RGA 预处理

RGA 是 Rockchip Raster Graphic Acceleration，用于 2D 图像硬件加速。

本项目中 RGA 用于：

- NV12 转 RGB888。
- 1920x1080 缩放到 640x640。
- letterbox 灰边填充。

当前代码路径：

```text
inference_yolo11_model
  -> convert_image_with_letterbox
    -> convert_image
      -> convert_image_rga
        -> improcess
```

RGA 加速的是 YOLO 输入预处理，不是 NPU 模型推理本身。当前 RGA 预处理耗时约 6 ms，不是最大瓶颈。

## 9. MPP H.264 编码

项目使用 Rockchip MPP 进行 H.264 硬件编码。

编码流程：

```text
NV12 视频帧
  -> 设置 MppFrame
  -> 设置输入 buffer
  -> 提交 MPP 编码任务
  -> 等待硬件编码完成
  -> 取出 H.264 packet
  -> 送入 RTSP
```

当前编码参数：

```text
编码格式：H.264
分辨率：1920x1080
帧率：30fps
码率模式：CBR
profile：Baseline
熵编码：CAVLC
```

编码优化：

- 去除上层已经拷贝到 MPP buffer 后的重复 memcpy。
- 使用 Baseline + CAVLC 降低编码复杂度。
- MPP 编码耗时从约 45 到 46 ms 降到约 12 ms。

## 10. RTSP 推流模块

RTSP 模块负责将 H.264 裸流打包为 RTP，并通过 RTSP 服务输出。

推流地址：

```text
rtsp://192.168.1.20:8554/live
```

测试方式：

```text
ffplay -fflags nobuffer -flags low_delay -framedrop rtsp://192.168.1.20:8554/live
```

RTSP 优化：

- 保留低延迟发送策略。
- 避免过小 RTP 包队列导致关键帧不完整。
- 之前将 RTP 包队列减小后出现下半屏灰色不刷新，最终恢复足够队列容量。

## 11. 多线程低延迟流水线

项目从串行主循环优化为多线程链路。

最初串行结构：

```text
DQBUF -> YOLO -> memcpy -> QBUF -> 画框 -> MPP 编码 -> RTSP
```

多线程帧池版本：

```text
采集线程：
V4L2 DQBUF -> 拷贝到应用层帧池 -> QBUF

处理线程：
取最新帧 -> YOLO -> MPP -> RTSP
```

该版本工程结构清晰，但因为多一次帧池拷贝和线程等待，端到端约 72 ms。

最终推荐多线程版本：V4L2 buffer 直通交接。

```text
采集线程：
V4L2 DQBUF -> 直接交接 V4L2 buffer 指针、buf_index、dma_fd

处理线程：
直接读取 V4L2 buffer 做 YOLO 输入
  -> 拷贝到 MPP buffer
  -> 立即 QBUF 归还 V4L2 buffer
  -> 在 MPP buffer 上画框
  -> MPP 编码
  -> RTSP 推流
```

该版本省掉一次应用层帧池整帧拷贝，端到端稳定约 71 ms。

## 12. DMA-BUF 使用情况

V4L2 buffer 已导出 DMA-BUF fd，当前用于：

- RGA / YOLO 预处理输入。
- 后续具备导入 MPP 的条件。

当前编码侧仍采用：

```text
V4L2 buffer -> memcpy 到 MPP buffer -> MPP 编码
```

原因是需要在视频帧上画检测框和警戒线。如果直接将 V4L2 buffer 导入 MPP 编码，会影响画框叠加和 V4L2 buffer 周转。

后续更完整的 DMA-BUF 全链路方向：

```text
V4L2 DMA-BUF
  -> RGA blit 到编码 DMA-BUF
  -> RGA / CPU 叠加检测框
  -> MPP 直接导入编码 DMA-BUF
```

当前 `memcpy_to_mpp` 约 2 ms，继续做 DMA-BUF 全链路主要收益是降低 CPU 占用和提升工程完整性，对端到端延迟的收益预计有限。

## 13. 延迟优化过程

原始基线：

```text
采集到应用层取帧：约 83 到 86 ms
YOLO 每帧推理：约 79 ms
MPP 编码：约 45 到 46 ms
端到端总延迟：约 213 到 216 ms
```

主要优化步骤：

| 优化项 | 优化前 | 优化后 | 效果 |
| --- | ---: | ---: | --- |
| V4L2 buffer 数量 4 -> 2 | 83-86 ms | 25-35 ms | 显著降低旧帧排队 |
| YOLO 每 3 帧推理一次 | 每帧约 79 ms | 每帧平均约 28-29 ms | 降低平均 NPU 占用 |
| MPP 编码参数优化 | 45-46 ms | 12 ms | 显著降低编码耗时 |
| RTSP 队列修正 | 曾出现画面下半屏异常 | 正常 | 保证关键帧完整 |
| 多线程帧池低延迟版 | 80-82 ms | 72 ms | 降低旧帧等待 |
| V4L2 buffer 直通交接 | 72 ms | 71 ms | 省掉帧池拷贝 |

当前推荐版本实测：

```text
采集到应用层取帧：25 到 26 ms
YOLO 每帧平均：28 到 29 ms
YOLO 单次真实推理：85 到 88 ms
MPP 编码：12 ms
DQBUF 到 RTSP 发送：45 ms
摄像头采集到 RTSP 发送：约 71 ms
```

总体效果：

```text
原始端到端：约 213 到 216 ms
当前端到端：约 71 ms
总延迟降低：约 140 ms 以上
```

## 14. NPU 频率与 RKNN Profiling 分析

当前确认 NPU devfreq 节点：

```text
/sys/class/devfreq/fde40000.npu
```

默认状态：

```text
cur_freq = 600000000
max_freq = 900000000
governor = rknpu_ondemand
load = 100@600000000Hz
```

说明 YOLO 推理时 NPU 已经满负载，但默认只运行在 600 MHz，没有达到最高 900 MHz。

临时切换 performance 后：

```text
governor = performance
cur_freq = 900000000
max_freq = 900000000
load = 100@900000000Hz
```

这可以解释 RK 官方 benchmark 和项目实测的差距：

```text
项目实测 rknn_run：约 72 到 74 ms @ 600 MHz
理论满频估算：72 / 1.5 = 48 ms 左右
RK 官方 YOLO11n benchmark：约 48.5 ms
```

因此当前 YOLO11n 推理慢于官网，很大概率不是模型版本错误，而是 NPU 默认频率未跑满。

注意：板子在 900 MHz performance 状态下出现 SSH 断连，结合当前电源为 5V2A，而板子要求 5V3A，建议更换 5V3A 电源后再进行长时间满频测试和 RKNN profiling。

RKNN profiling 方向：

```text
rknn_init 增加 RKNN_FLAG_COLLECT_PERF_MASK
rknn_query 使用 RKNN_QUERY_PERF_RUN
rknn_query 使用 RKNN_QUERY_PERF_DETAIL
```

profiling 可用于确认：

- 每层耗时。
- 是否存在 CPU fallback。
- NPU 执行瓶颈层。
- RKNN runtime 真实推理耗时。

## 15. 当前瓶颈判断

当前主要瓶颈：

```text
YOLO11n 单次真实推理
```

其中最主要的是：

```text
RKNN/NPU 真推理：约 72 到 74 ms @ 600 MHz
```

链路其他部分：

- 采集到应用层取帧已经接近 30fps 帧周期范围。
- MPP 编码已降到约 12 ms。
- RTSP 发送在板端内部统计中不是主要瓶颈。
- memcpy_to_mpp 约 2 ms，继续优化收益有限。

因此后续若要继续明显降低延迟，优先级应为：

1. 更换 5V3A 电源，保证 NPU 可以稳定 900 MHz 满频。
2. 进行 RKNN profiling，确认是否存在 CPU fallback。
3. 尝试 YOLO11n 416x416 或 320x320 输入尺寸。
4. 若不强制 YOLO11，评估 YOLOv5n / YOLOv6n。
5. 使用真实监控场景数据重新 INT8 量化校准，提升场景精度稳定性。

## 16. 量化说明

量化是将模型中的浮点计算压缩为低比特整数计算。

典型变化：

```text
FP32 / FP16 -> INT8
```

量化作用：

- 减小模型体积。
- 降低内存带宽。
- 提高 NPU 推理效率。
- 降低功耗。

当前模型已经是 INT8 量化模型，日志中可见：

```text
type=INT8
qnt_type=AFFINE
```

因此项目不是“未量化模型”，而是已经使用 INT8。后续量化优化更多是：

- 检查是否所有算子都在 NPU 上执行。
- 使用真实监控画面作为 calibration dataset。
- 减少量化带来的精度损失。
- 确认输入预处理和转换配置一致。

量化本身不一定继续大幅提速，但可以提升模型在真实监控场景下的检测稳定性。

## 17. 测试与验证

板端运行示例：

```text
cd /home/cat/latency_test
LD_LIBRARY_PATH=/home/cat/latency_test ./imx415_yolo_rtsp_latency /dev/video0 /home/cat/latency_test/model/yolo11.rknn
```

Ubuntu / PC 侧拉流：

```text
ffplay -fflags nobuffer -flags low_delay -framedrop rtsp://192.168.1.20:8554/live
```

延迟日志示例：

```text
[latency][threaded_handoff_yolo]
  capture_ts_to_dqbuf:     25
  yolo_actual_infer:       33/100
  rknn_inference_once:     85-88
  yolo_rknn_run:           72-74
  memcpy_to_mpp:            2
  mpp_encode_total:        12
  capture_ts_to_rtsp_tx:   71
```

## 18. 已解决问题

1. 摄像头采集延迟高。

原因：V4L2 buffer 过多导致应用层取到旧帧。

解决：`BUF_COUNT` 从 4 调整为 2。

2. MPP 编码耗时高。

原因：存在重复 memcpy，编码参数复杂度较高。

解决：去除冗余拷贝，使用 Baseline + CAVLC。

3. RTSP 拉流下半屏灰色。

原因：RTP 包队列过小，1080p IDR 帧被截断。

解决：恢复足够 RTP 包队列容量。

4. YOLO 推理拖慢整体链路。

原因：YOLO11n 单次真实推理较重。

解决：每 3 帧推理一次，其余帧复用检测结果。

5. 多线程帧池仍有等待。

原因：应用层帧池存在排队和整帧拷贝。

解决：改为 V4L2 buffer 直通交接。

6. 官网 YOLO11n 性能与实测差异大。

原因：NPU 默认 600 MHz，未跑到 900 MHz 满频。

解决方向：更换 5V3A 电源后测试 performance governor 和 RKNN profiling。

## 19. 项目亮点

- 打通 IMX415 Sensor 到 RTSP 网络输出的完整端侧视频链路。
- 基于 V4L2 subdev 理解 sensor 驱动适配流程。
- 使用 V4L2 mmap + DMA-BUF 获取摄像头帧。
- 基于 RGA 实现 YOLO 输入预处理。
- 基于 RKNN Runtime 部署 YOLO11n INT8 模型。
- 基于 MPP 实现 H.264 硬件编码。
- 基于 RTSP 实现局域网低延迟预览。
- 对采集、推理、编码、RTSP 全链路进行分阶段打点。
- 将端到端延迟从约 213 到 216 ms 优化到约 71 ms。
- 定位 NPU 频率未满频导致实测性能低于官方 benchmark。
