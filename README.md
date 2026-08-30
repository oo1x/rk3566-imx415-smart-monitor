# RK3566 IMX415 智能监控视频采集与 RTSP 推流

基于 RK3566 + IMX415 的端侧链路：Sensor 采集、V4L2 取帧、YOLO11n 异步推理、RGA 叠框、MPP H.264、RTSP 推流。

当前默认代码在分支 `latest-pending`。`main` 上仍是较早的 1080p30 / 2 buffer / 每 3 帧同步推理说明，不要和本分支混用。

## 当前默认配置

```text
采集 / 编码     1920×1080 NV12 @ 60 fps
V4L2            4 个 mmap buffer，VIDIOC_EXPBUF 导出 dma-buf fd
视频通路        同一 fd：叠框 RGA 原地画框 → MPP 编码（零拷贝）
AI 策略         latest-pending
拷贝节拍        仅帧 0 / 4 / 8 … 将 NV12 拷进 pending（YOLO_INFER_INTERVAL=4）
中间帧          只叠框和编码，不拷给 AI
NPU 忙          到期帧覆盖唯一 pending；推理结束后立刻用 pending
NPU 空          到期帧直接开始推理
YOLO 输入       缩放 RGA：NV12 1080p → RGB 640×640 letterbox → rknn_run
复用框          非推理帧叠最近一次检测结果
编码            H.264 Baseline / CAVLC，CBR 4 Mbps，GOP 60
IDR             约每秒 MPP_ENC_SET_IDR_FRAME（不单靠 rc:gop）
RTSP            rtsp://<board-ip>:8554/live ，正式测试用 TCP interleaved
```

## 三线程

```text
采集线程   DQBUF → 把最新帧指针交给视频线程
           若视频还没拿走上一帧，上一帧 QBUF 还给 ISP

视频线程   到期帧：memcpy → pending
           叠框 RGA（写 V4L2 NV12）
           MPP 编码同一 dma-buf fd
           QBUF 还给 ISP
           RTSP 发送 H.264

AI 线程    pending 与 work 换指针
           缩放 RGA → RKNN 输入 dma-buf（RGB 640×640）
           rknn_run → 发布 boxes + Frame ID
```

两条 RGA 不是同一次调用：叠框在视频线程改 1080p NV12；缩放大图在 AI 线程写 640×640 RGB。

## 三块内存

| Buffer | 格式 | 归属 |
|---|---|---|
| V4L2 ×4 | NV12 1080p dma-buf | ISP ⇄ 应用（DQBUF～QBUF）；叠框 RGA 与 MPP 共用同一 fd |
| pending / work / scratch | NV12 1080p CPU | 仅到期帧拷入；忙则覆盖 pending |
| RKNN input | RGB 640×640 dma-buf | 缩放写出，只给 `rknn_run` |

## 运行参数

默认即 `latest-pending` + 每 4 帧拷一次。可用环境变量覆盖：

```text
PERF_AI_ENABLED=1
PERF_SUBMIT_POLICY=latest_pending   # 或 latest_if_idle / always_queue / disabled
PERF_INFER_INTERVAL=4
PERF_REUSE_BOXES=1
PERF_INFER_QUEUE_LIMIT=8            # 仅 always_queue 过载保护
```

`latest_if_idle`：到期帧若 NPU 忙则跳过，不存 pending（S4 实测用的是这个，间隔为 5）。

## 功能链路

```text
IMX415 RAW10 1944×1096
  → MIPI CSI-2
  → RKISP crop 1920×1080 NV12
  → V4L2 4× DMA-BUF
      ├─ 视频线程：叠框 RGA → MPP H.264 → RTSP
      └─ 到期帧拷贝 → AI 线程：缩放 RGA → YOLO11n → 发布框
```

## 目录

```text
sensor_driver/          IMX415 V4L2 subdev、overlay、DPHY 参考
rtsp_streaming/         V4L2、MPP、RTSP；imx415_rtsp.c 为无 YOLO 推流
yolo_integration/       主程序 imx415_yolo_rtsp.cpp、RGA overlay、RKNN
docs/                   讲解文档；标了日期的是历史基线，当前以本 README 为准
```

## 编译

交叉编译依赖 RKNN Runtime、RGA、MPP、sysroot。`yolo_integration/Makefile` 里是原开发机路径，换环境先改。

```bash
cd yolo_integration
make clean
make
```

无 YOLO 的基础推流：

```bash
cd rtsp_streaming
make -f Makefile.imx415
```

## 运行

```bash
cd /home/cat/latency_test
LD_LIBRARY_PATH=. \
  ./imx415_yolo_rtsp /dev/video0 /home/cat/latency_test/model/yolo11.rknn
```

拉流：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop \
  -rtsp_transport tcp rtsp://192.168.1.20:8554/live
```

模型 `yolo11.rknn` MD5：`91faf3f5526db7ecfed3a61a99a3ef75`。

## 已测数字与本分支的关系

本分支当前默认（`latest-pending` + 方案 A，300 s，2026-08-30）：

```text
采集 60.000 fps
编码输出 60.000 fps
检测更新 15.000 fps
板端视频丢帧 0
```

交接说明（优化前 / 改了什么 / 最终结果，给 Codex 读）：  
[docs/CODEX_NOTE_SCHEME_A_ENCODE_60FPS_20260830.md](docs/CODEX_NOTE_SCHEME_A_ENCODE_60FPS_20260830.md)

下列数字来自 **7 月正式 S4**：1080p60、4 buffer、`latest_if_idle`、每 5 帧触发，**不是** 本分支默认的 `latest-pending` / 每 4 帧。不要把下面的 fps 说成本分支当前结果。

```text
采集 60.000 fps
编码输出约 58.75 fps
检测更新约 11.75 fps
采集时间戳 → 编码输出 P95 约 14.90 ms（板内，不是显示端到端）
```

S0/S4 曾做 3×300 s RTSP TCP 拉流，解码错误与 RTP 序号空洞为 0。那是可播放性修复（FU-A 发送游标、有理数时间戳、周期 IDR）的回归，与 AI pending 策略无关。

旧文档里的 1080p30、2 个 V4L2 buffer、每 3 帧同步推理、约 71 ms「端到端」，都是 2026-07 之前的基线。

## 文档

- [docs/CODEX_NOTE_SCHEME_A_ENCODE_60FPS_20260830.md](docs/CODEX_NOTE_SCHEME_A_ENCODE_60FPS_20260830.md) 方案 A：编码 60 fps 交接说明（Codex 先读这篇）
- [docs/CODE_WALKTHROUGH.md](docs/CODE_WALKTHROUGH.md) 模块讲解（已按当前分支更新链路说明）
- [docs/PROJECT_REPORT.md](docs/PROJECT_REPORT.md) 项目长文；文首标明历史口径
- `docs/*2026-07-10.md`、`LATENCY_BASELINE.md` 为当时 perf / 延迟快照，不是当前默认策略
