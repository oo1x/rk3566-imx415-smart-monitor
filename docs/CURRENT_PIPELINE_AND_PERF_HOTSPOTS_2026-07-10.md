# RK3566 + IMX415 + YOLO11 + RTSP 当前链路与 perf 热点整理

更新时间：2026-07-10

本文档记录当前项目版本的视频链路、每个节点采用的实现方案，以及最近一次 `perf` 抓到的主要热点函数。当前测试程序为：

```text
/home/cat/latency_test/imx415_yolo_rtsp_perfopt
```

对应源码入口：

```text
yolo_integration/imx415_yolo_rtsp.cpp
```

## 1. 当前整体链路

```text
IMX415 Sensor
-> MIPI CSI-2 / D-PHY
-> RKISP 输入与 ISP 处理
-> /dev/video0 输出 NV12
-> V4L2 mmap + DMA-BUF 取帧
-> 采集线程 DQBUF，保留最新帧
-> 处理线程 YOLO11n 推理
-> 画检测框
-> MPP H.264 硬件编码
-> RTSP/RTP 打包发送
-> PC 端 ffplay 拉流预览
```

当前应用层宏配置为：

```text
分辨率：1920x1080
应用编码/推流帧率：30 fps
V4L2 buffer 数量：2
YOLO 推理间隔：每 3 帧推理 1 次，其余帧复用最近一次检测结果
编码格式：H.264 Baseline + CAVLC
RTSP 地址：rtsp://192.168.1.20:8554/live
```

说明：底层 Sensor / CSI / ISP 已调通过 1080p60 RAW 出帧链路，但当前 `imx415_yolo_rtsp_perfopt` 这版应用侧宏仍是 `CAPTURE_FPS=30`，也就是当前 perf 数据对应的是 1080p30 编码推流版本。

## 2. 各节点当前方案

| 节点 | 当前方案 | 关键点 |
| --- | --- | --- |
| IMX415 Sensor | 驱动配置 1080p 裁剪模式，实际 raw 输入尺寸为 1944x1096，ISP crop 到 1920x1080 | 解决过尺寸不一致导致的 MIPI / ISP size error |
| MIPI CSI-2 / D-PHY | 通过设备树和 sensor mode 配置 lane、link frequency、raw bpp | 之前的 MIPI error 本质是 sensor 输出尺寸、CSI、ISP 输入 crop 没对齐 |
| RKISP / rkaiq | 使用 1080p 对应 IQ 文件，当前演示重点是画面稳定 | 曾出现 AE/AWB 异常、LSC 导致条纹，最终通过调整 IQ/关闭异常 LSC 路径解决 |
| V4L2 采集 | `/dev/video0` 输出 NV12，使用 `VIDIOC_REQBUFS` 申请 mmap buffer | 应用层直接拿 ISP 后的 NV12，不再自己做 RAW 转换 |
| DMA-BUF 导出 | 对每个 V4L2 mmap buffer 执行 `VIDIOC_EXPBUF` 导出 fd | 后续 MPP 可以直接 import V4L2 buffer |
| 采集线程 | 单独线程执行 `select + DQBUF`，只保留最新帧 | 如果处理线程来不及处理，旧帧会被丢弃，减少排队延迟 |
| 帧交接 | 通过 `handoff_queue` 交接 `data / size / buf_index / dma_fd / timestamp` | 当前是低延迟优先，不追求每帧都处理 |
| YOLO 前处理 | `convert_image_with_letterbox()` 将 NV12 转换/缩放/letterbox 到模型输入 | 使用 RKNN zero-copy input buffer 作为目标 buffer |
| YOLO 推理 | RKNN Runtime 调用 `rknn_run()`，模型为 YOLO11n RKNN | 推理本身主要跑在 NPU，CPU 侧主要看到 runtime 和同步开销 |
| YOLO 输出 | 不再把 RKNN native output 从 NC1HWC2 转 NCHW | 当前 `post_process_native()` 直接按 RKNN native layout 读取输出 |
| YOLO 后处理 | `post_process_native()` 完成 decode、阈值过滤、NMS | 当前 perf 中该函数已成为可见 CPU 热点 |
| 画框 | 直接在 NV12 的 Y 平面画检测框 | 已删除警戒线，只保留目标框 |
| MPP 输入 | 优先使用 V4L2 DMA-BUF 直接导入 MPP | 避免每帧 1920x1080 NV12 整帧 memcpy |
| QBUF 时机 | DMA-BUF 路径下，MPP 编码完成后再 QBUF 归还 V4L2 buffer | 避免 buffer 提前归还后被 ISP 覆盖，造成撕裂/花屏 |
| MPP 编码 | Rockchip MPP H.264 硬件编码，CBR，Baseline，关闭 CABAC | 当前目标是低延迟，不是最高压缩率 |
| RTSP 打包 | MPP 输出 H.264 packet 后，RTSP 模块扫描 NALU 并 RTP 分包 | NALU 搜索已从逐字节扫描优化为 `memchr` 批量搜索 |
| 网络发送 | RTSP over TCP 发送给 ffplay | perf 中能看到 `sendto / tcp_sendmsg / tcp_write_xmit` |

## 3. 当前应用层数据流

### 3.1 采集线程

```text
select 等待 /dev/video0
-> VIDIOC_DQBUF 取出 V4L2 buffer
-> 如果队列里已有旧帧，先 QBUF 丢掉旧帧
-> 保存最新帧的 buf_index、dma_fd、mmap 地址、V4L2 timestamp
-> 通知处理线程
```

这部分的目标是减少旧帧堆积。它不是完整缓存所有帧，而是保留最新帧，适合智能监控预览这种低延迟场景。

### 3.2 处理线程

```text
取最新帧
-> 每 3 帧执行一次 YOLO11n 推理
-> 非推理帧复用上一帧检测框
-> 在当前 NV12 buffer 上画框
-> MPP 直接使用对应 buf_index 的 DMA-BUF 编码
-> 编码完成后 QBUF 归还 V4L2 buffer
-> RTSP 发送 H.264
```

这部分的关键是：DMA-BUF 路径下不能提前 QBUF。因为 MPP 编码器正在读这块 V4L2 buffer，如果提前归还给驱动，ISP 可能复用该 buffer 写入下一帧，画面就可能出现撕裂、上下半帧错乱、颜色块等问题。

## 4. 当前已完成的主要优化

| 优化点 | 基线做法 | 当前做法 | 效果 |
| --- | --- | --- | --- |
| V4L2 buffer 数量 | 多 buffer 容易堆旧帧 | `BUF_COUNT=2` | 降低采集排队延迟 |
| 采集与处理 | 单线程串行处理 | 采集线程 + 处理线程 | 减少 DQBUF 被 YOLO/编码阻塞 |
| YOLO 推理频率 | 每帧都推理 | 每 3 帧推理 1 次 | 降低平均 CPU/NPU 压力 |
| RKNN 输出处理 | NC1HWC2 转 NCHW 后再后处理 | `post_process_native()` 直接读 native output | `NC1HWC2_i8_to_NCHW_i8` 热点消失 |
| V4L2 到 MPP | 每帧 memcpy 到 MPP buffer | V4L2 DMA-BUF 直接 import 到 MPP | `copy_nv12_v4l2_to_mpp` 热点消失 |
| MPP 编码参数 | 复杂编码配置 | Baseline + CAVLC | 降低编码等待时间 |
| RTSP NALU 查找 | 逐字节扫描 | `memchr` 批量搜索候选 0 字节 | 降低码流扫描 CPU 开销 |

## 5. 当前 perf 热点结果

最近一次采样命令：

```bash
sudo perf record -F 99 -e cpu-clock -p <PID> --call-graph dwarf -- sleep 60
sudo perf report --stdio > perf_report_dwarf_after.txt
```

采样结果：

```text
采样时长：60 秒
采样频率：99 Hz
采样事件：cpu-clock
样本数量：1673 samples
perf.data 大小：13.708 MB
```

注意：下面表格按“实际含义”整理，父节点和子节点不会简单相加。例如 `el0_svc / ioctl / vfs_ioctl` 是系统调用父路径，里面包含 RKNPU 同步、V4L2 QBUF、网络发送等多个子路径。

| 热点/路径 | 占比 | 含义 | 判断 |
| --- | ---: | --- | --- |
| `el0_svc` / `el0_svc_handler` | 28.15% | 用户态进入内核态的总入口 | 父节点，不是单一瓶颈 |
| `__arm64_sys_ioctl` -> `vfs_ioctl` | 15.48% | ioctl 总路径 | 包含 RKNPU、V4L2 等多个 ioctl |
| `librknnrt.so std::vector<int>::_M_fill_insert` | 17.81% | RKNN runtime 内部 CPU 开销 | 可能是 runtime 内部调度/缓冲/符号显示，不是项目代码 |
| `librknnrt.so std::vector<bool>::_M_assign_aux` | 17.10% | RKNN runtime 内部 CPU 开销 | 同上 |
| `post_process_native` | 11.60% | YOLO 后处理，直接读 RKNN native output | 当前应用层主要可优化点之一 |
| `librknnrt.so regex _M_assertion` | 8.19% | RKNN runtime 内部符号 | 可视为 RKNN runtime 侧开销 |
| `drm_ioctl` -> `__rknpu_gem_sync_ioctl` | 6.58% | RKNPU buffer 同步/cache 同步 | 与 NPU/RKNN 内存同步相关 |
| `rknpu_gem_sync_ioctl` | 6.10% | RKNPU GEM 同步 | 模型推理链路的一部分 |
| `__pi___clean_dcache_area_poc` | 6.87% self / 4.78% in QBUF path | cache clean | 主要来自 V4L2 QBUF / DMA buffer 同步 |
| `v4l2_ioctl` -> `v4l_qbuf` -> `vb2_core_qbuf` | 5.74% / 5.20% / 5.14% | V4L2 buffer 归还路径 | DMA-BUF/QBUF 带来的内核开销 |
| `__arm64_sys_sendto` -> `tcp_sendmsg` | 6.04% / 5.68% | RTSP over TCP 发送 | 网络发送路径开销 |
| `tcp_sendmsg_locked` -> `tcp_write_xmit` | 4.60% / 3.53% | TCP 写出 | 与码率、RTP 包数量、TCP 发送相关 |
| `memchr` | 4.42% | RTSP NALU 起始码批量搜索 | 说明当前使用的是 `memchr` 优化版，不是旧的逐字节扫描 |
| `_raw_spin_unlock_irq` | 3.17% | 内核锁/中断路径 | 系统调度和驱动路径附带开销 |

## 6. perf 结果解读

### 6.1 已经优化成功的点

旧版热点中比较明显的两个函数：

```text
NC1HWC2_i8_to_NCHW_i8
copy_nv12_v4l2_to_mpp
```

在当前 `perf_hotspots_ge3_after.txt` 中已经没有作为 3% 以上热点出现。这说明：

```text
RKNN 输出 layout 转换热点已基本消除；
V4L2 -> MPP 整帧 memcpy 热点已基本消除；
```

也就是说，本轮优化确实把两个明确的 CPU 热点打掉了。

### 6.2 当前最大的应用层热点

当前项目代码里最明确的热点是：

```text
post_process_native  11.60%
```

它来自 YOLO 后处理，包括：

```text
遍历模型输出
反量化 / decode
置信度过滤
坐标还原
NMS
```

如果后面继续优化应用层代码，优先看这里。可选方向：

```text
只保留 person 类别，提前过滤非 person 输出；
限制进入 NMS 的候选框数量；
提高置信度阈值，减少低质量候选框；
优化 NMS 数据结构，减少重复计算；
```

### 6.3 当前系统/驱动热点

当前内核侧热点主要有三类：

```text
RKNPU buffer 同步：rknpu_gem_sync_ioctl
V4L2 buffer 归还：v4l_qbuf / vb2_core_qbuf / cache clean
TCP 发送：sendto / tcp_sendmsg / tcp_write_xmit
```

这些不完全是“代码写得慢”，而是实时视频链路必然会碰到的系统调用、buffer 同步和网络发送成本。

## 7. 后续优化优先级

### 优先级 1：YOLO 后处理

原因：`post_process_native` 是当前最明确的项目代码热点。

建议：

```text
如果项目只需要检测人，则在 decode 阶段提前过滤非 person 类；
减少 NMS 输入候选框数量；
将 box 坐标、面积、score 做结构化缓存，降低 NMS 重复计算；
```

### 优先级 2：RTSP NALU 分包

原因：`memchr` 仍有 4.42%，但已经比逐字节扫描合理。

建议：

```text
不建议做多线程并行搜索；
更好的方向是利用 MPP packet segment 信息，尽量避免每帧重复扫描完整 H.264 码流；
如果拿不到可靠 segment 信息，再保留 memchr 批量搜索方案。
```

### 优先级 3：网络发送与码率

原因：TCP 发送路径占比约 5% 到 6%。

建议：

```text
适当调整 H.264 码率；
控制 RTP 包数量；
必要时测试 UDP 拉流，但演示稳定性上 TCP 更直观。
```

### 优先级 4：RKNPU / V4L2 同步开销

原因：这部分属于驱动和 DMA buffer 同步路径，应用层可控空间有限。

建议：

```text
保持 NPU performance governor；
保持 DMA-BUF 路径；
避免频繁重新申请/释放 RKNN 和 MPP buffer；
不要提前 QBUF 正在被 MPP 使用的 V4L2 buffer。
```

## 8. 面试表述建议

可以这样讲当前版本：

```text
我这部分主要负责端侧视频链路。Sensor 侧完成 IMX415 1080p 模式适配，
ISP 输出 NV12 后，应用层通过 V4L2 mmap 获取帧，并导出 DMA-BUF 给 MPP，
减少 V4L2 到编码器之间的整帧拷贝。

AI 部分使用 RKNN 部署 YOLO11n，前处理通过 letterbox 适配模型输入，
推理后直接读取 RKNN native output 做后处理，避免 NC1HWC2 到 NCHW 的额外转换。

编码推流部分使用 Rockchip MPP 做 H.264 硬件编码，再通过 RTSP/RTP 推流。
为了降低延迟，我把采集和处理拆成两个线程，只保留最新帧，并调整 QBUF 时机，
保证 DMA-BUF 被 MPP 编码完成后再归还给 V4L2，避免 buffer 复用导致画面异常。

后续通过 perf 定位热点，确认原来的 layout 转换和整帧 memcpy 热点已经消失；
当前主要热点转移到 YOLO 后处理、RKNPU buffer 同步、V4L2 QBUF cache clean 和 TCP 发送路径。
```

