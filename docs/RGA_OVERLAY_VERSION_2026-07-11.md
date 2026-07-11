# RGA Overlay Version Notes

Date: 2026-07-11

Branch:

```text
rga-overlay
```

Baseline tag:

```text
cpu-overlay-stable-2026-07-11
```

## 1. Goal

The CPU overlay version draws detection boxes by writing the NV12 image buffer directly from the CPU. This is simple and stable, but perf shows cost in the V4L2 QBUF and cache-clean path after CPU writes the DMA buffer.

The RGA overlay version moves box drawing from CPU pixel loops to RGA `imfill()` calls.

## 2. CPU Overlay Path

```text
V4L2 DQBUF
-> YOLO preprocess
-> RKNN inference
-> YOLO postprocess
-> CPU writes box pixels into NV12 Y plane
-> MPP encodes the same V4L2 DMA-BUF
-> QBUF after MPP encode
```

Main cost pattern:

```text
CPU writes DMA buffer
-> cache clean before hardware reuse
-> visible in v4l_qbuf / vb2_core_qbuf / __pi___clean_dcache_area_poc
```

## 3. RGA Overlay Path

```text
V4L2 DQBUF
-> YOLO preprocess
-> RKNN inference
-> YOLO postprocess
-> RGA fills four thin rectangles for each box
-> MPP encodes the same V4L2 DMA-BUF
-> QBUF after MPP encode
```

For each box:

```text
top edge    -> imfill()
bottom edge -> imfill()
left edge   -> imfill()
right edge  -> imfill()
```

The current version only uses simple NV12 white/dark boxes. It avoids text rendering and complex alpha blending.

## 4. Implementation Details

Changed file:

```text
yolo_integration/imx415_yolo_rtsp.cpp
```

New helpers:

```text
rga_overlay_init()
rga_overlay_deinit()
rga_draw_rect_nv12()
rga_fill_rect()
draw_detections_rga()
draw_detections_cpu()
```

Runtime behavior:

```text
if V4L2 DMA-BUF path is active:
    try RGA draw first
    if RGA draw fails, fallback to CPU draw
else:
    use CPU draw
```

This keeps the demo stable even if a specific RGA driver version rejects the fill parameters.

## 5. Hotspot-Oriented Design

The implementation avoids these common costs:

1. No per-frame overlay buffer allocation.
2. No full-frame copy for drawing.
3. No ARGB overlay clear per frame.
4. No text rendering.
5. No extra V4L2 buffer ownership change.

If `LIBRGA_IM2D_HANDLE` is enabled by the build, the code imports V4L2 DMA-BUF fds once and reuses RGA handles. Otherwise it uses `wrapbuffer_fd()` for the current frame.

## 6. Expected Perf Changes

Expected improvements:

```text
CPU draw loop cost should drop.
draw_overlay latency may drop when boxes exist.
```

Possible remaining costs:

```text
rga_ioctl / rga2_blit / rga2_get_dma_info may increase.
v4l_qbuf / cache clean may not disappear completely.
```

Reason:

```text
The frame buffer is still a shared DMA-BUF used by ISP, RGA, MPP, and V4L2.
Some synchronization cost is expected.
```

## 7. Test Commands

Build on Ubuntu:

```bash
cd /home/oo1/Desktop/rk3566_imx415_smart_monitor_project/yolo_integration
make clean
make TARGET=imx415_yolo_rtsp_rga_overlay
```

Deploy to board:

```bash
cp imx415_yolo_rtsp_rga_overlay /home/oo1/Desktop/cat_nfs/imx415_yolo_rtsp_rga_overlay
```

Run on board:

```bash
cd /home/cat/latency_test
LD_LIBRARY_PATH=. ./imx415_yolo_rtsp_rga_overlay /dev/video0 /home/cat/latency_test/model/yolo11.rknn
```

Pull stream on Ubuntu:

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop -rtsp_transport tcp rtsp://192.168.1.20:8554/live
```

Perf capture:

```bash
PID=$(pidof imx415_yolo_rtsp_rga_overlay)
sudo perf record -F 99 -e cpu-clock -p $PID --call-graph dwarf -- sleep 60
sudo perf report --stdio > perf_report_rga_overlay.txt
```

Important symbols to compare:

```text
draw_overlay latency log
rga_ioctl
rga2_blit
rga2_get_dma_info
v4l_qbuf
__pi___clean_dcache_area_poc
post_process_native
rknpu_gem_sync_ioctl
tcp_sendmsg
```

## 8. Risk Points

1. RGA fill color packing for NV12 may vary by librga version.
2. RGA rectangles should stay even-aligned for NV12.
3. RGA must finish before MPP encode starts.
4. MPP encode must finish before QBUF returns the buffer to V4L2.

The current code uses synchronous `imfill()` calls and keeps the existing "encode first, QBUF later" ownership rule.
