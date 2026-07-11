# YOLO and RTSP Performance Optimization Notes

Date: 2026-07-10

This document records the baseline implementation, perf hotspots, optimization changes, and follow-up directions for the RK3566 + IMX415 + YOLO11 + RTSP demo.

## 1. Baseline Implementation

Baseline entry file:

```text
yolo_integration/imx415_yolo_rtsp.cpp
```

Baseline pipeline:

```text
V4L2 DQBUF
-> YOLO preprocess
-> rknn_run
-> NC1HWC2_i8_to_NCHW_i8
-> YOLO postprocess / NMS
-> copy V4L2 NV12 to MPP buffer
-> draw boxes in MPP buffer
-> MPP H.264 encode
-> RTSP send
-> V4L2 QBUF
```

The RKNN part already uses `rknn_create_mem` and `rknn_set_io_mem` to bind input and output tensor buffers. It is not the old `rknn_inputs_set` / `rknn_outputs_get` path.

However, the baseline still has two clear CPU costs:

1. Convert RKNN native output from `NC1HWC2` to `NCHW` after each inference.
2. Copy each `1920x1080 NV12` frame from the V4L2 buffer to the MPP frame buffer.

## 2. Baseline Perf Hotspots

User-provided key perf hotspots:

```text
inference_yolo11_model        45.34%
rknn_run                      23.64%
NC1HWC2_i8_to_NCHW_i8         13.27%
copy_nv12_v4l2_to_mpp          6.66%
post_process / process_i8      4.60%
convert_image_with_letterbox   4.32%
rtsp_tx_video                  3.87%
```

Conclusion:

1. First priority: remove RKNN output layout conversion.
2. Second priority: remove or reduce full-frame `V4L2 -> MPP` memcpy.
3. Lower priority: RTSP NALU search and socket send path, because their baseline ratio is smaller.

## 3. Optimization 1: Native RKNN Output Postprocess

Changed files:

```text
yolo_integration/rknn_yolo11/yolo11_zero_copy.cc
yolo_integration/rknn_yolo11/postprocess.cc
yolo_integration/rknn_yolo11/postprocess.h
```

Baseline path:

```text
output_mems[i]->virt_addr
-> allocate outputs[i].buf
-> NC1HWC2_i8_to_NCHW_i8
-> post_process()
-> release outputs[i].buf
```

Optimized path:

```text
output_mems[i]->virt_addr
-> post_process_native()
-> read RKNN native layout directly
```

The new `post_process_native()` reads `app_ctx->output_mems[i]->virt_addr` directly. When the RKNN output format is `RKNN_TENSOR_NC1HWC2`, the element access is:

```text
c1 = c / C2
c2 = c % C2
offset = (((c1 * H + h) * W + w) * C2 + c2)
value = base[offset]
```

After this change, `inference_yolo11_model()` no longer allocates temporary output buffers for every frame and no longer calls `NC1HWC2_i8_to_NCHW_i8()`.

Expected result:

1. `NC1HWC2_i8_to_NCHW_i8` disappears or drops sharply in perf.
2. `yolo_output_convert` becomes `0 ms` in latency logs.
3. `post_process_native` may become more visible, but total CPU time should drop.

## 4. Optimization 2: Prefer V4L2 DMA-BUF for MPP Encode

Changed file:

```text
yolo_integration/imx415_yolo_rtsp.cpp
```

Baseline path:

```cpp
uint8_t *frm_ptr = (uint8_t *)mpp_buffer_get_ptr(enc->frm_buf);
memcpy(frm_ptr, frm.data, width * height * 3 / 2);
handoff_release_frame(q, frm.buf_index);
draw_detections(frm_ptr, width, height, &od_results);
mpp_encoder_encode(enc, frm_ptr, ..., -1, ...);
```

Optimized path:

```text
startup:
V4L2 EXPBUF exports DMA-BUF fd
-> mpp_encoder_setup_dmabuf()
-> MPP imports V4L2 DMA-BUF

per frame:
if DMA-BUF is available:
    draw boxes in the V4L2 buffer
    encode the V4L2 DMA-BUF directly
    QBUF after MPP encode is done
else:
    fallback to the memcpy path
```

Important notes:

1. In the DMA-BUF path, do not QBUF before MPP encode is done.
2. If the buffer is returned too early, ISP/V4L2 may reuse it while MPP is still reading it.
3. The current implementation calls `handoff_release_frame()` only after MPP encode completes.
4. If DMA-BUF import fails, the program falls back to the memcpy path.

Expected result:

1. `copy_nv12_v4l2_to_mpp` / `memcpy` disappears or drops sharply in perf.
2. `memcpy_to_mpp` becomes close to `0 ms` in latency logs.

## 5. Current Perf Result After Optimization

Latest test command:

```bash
sudo perf record -F 99 -e cpu-clock -p <PID> --call-graph dwarf -- sleep 60
sudo perf report --stdio > perf_report_dwarf_after.txt
```

Sampling summary:

```text
duration: 60 seconds
event: cpu-clock
frequency: 99 Hz
samples: 1673
perf.data: 13.708 MB
```

Key hotspots after optimization:

| Hotspot | Ratio | Meaning |
| --- | ---: | --- |
| `el0_svc` / `el0_svc_handler` | 28.15% | Kernel syscall entry parent node |
| `__arm64_sys_ioctl` -> `vfs_ioctl` | 15.48% | ioctl parent path |
| `librknnrt.so std::vector<int>::_M_fill_insert` | 17.81% | RKNN runtime internal CPU cost |
| `librknnrt.so std::vector<bool>::_M_assign_aux` | 17.10% | RKNN runtime internal CPU cost |
| `post_process_native` | 11.60% | YOLO postprocess in project code |
| `librknnrt.so regex _M_assertion` | 8.19% | RKNN runtime internal symbol |
| `__rknpu_gem_sync_ioctl` | 6.58% | RKNPU GEM buffer sync |
| `rknpu_gem_sync_ioctl` | 6.10% | RKNPU buffer sync path |
| `__pi___clean_dcache_area_poc` | 6.87% self / 4.78% in QBUF path | Cache clean |
| `v4l2_ioctl` -> `v4l_qbuf` -> `vb2_core_qbuf` | 5.74% / 5.20% / 5.14% | V4L2 QBUF path |
| `__arm64_sys_sendto` -> `tcp_sendmsg` | 6.04% / 5.68% | RTSP over TCP send path |
| `tcp_sendmsg_locked` -> `tcp_write_xmit` | 4.60% / 3.53% | TCP transmit path |
| `memchr` | 4.42% | H.264/H.265 NALU start-code search |
| `_raw_spin_unlock_irq` | 3.17% | Kernel lock / interrupt related path |

Notes:

1. `el0_svc`, `ioctl`, and `vfs_ioctl` are parent paths. They include RKNPU sync, V4L2 QBUF, and other kernel operations.
2. `NC1HWC2_i8_to_NCHW_i8` is no longer visible above 3%.
3. `copy_nv12_v4l2_to_mpp` is no longer visible above 3%.
4. This confirms that the RKNN layout conversion and V4L2-to-MPP memcpy hotspots were removed.

## 6. Remaining Hotspots

### 6.1 YOLO postprocess

Current project-code hotspot:

```text
post_process_native  11.60%
```

This function includes:

1. Decode model output.
2. Dequantize scores / box values.
3. Filter by confidence threshold.
4. Restore coordinates from letterbox space.
5. Run NMS.

Possible follow-up optimization:

1. If only `person` is needed, filter non-person classes earlier.
2. Reduce the number of boxes entering NMS.
3. Raise `BOX_THRESH` if the demo scene allows it.
4. Cache box area values to reduce repeated NMS calculation.

### 6.2 RKNPU sync

Related hotspots:

```text
__rknpu_gem_sync_ioctl  6.58%
rknpu_gem_sync_ioctl    6.10%
```

This is related to RKNN runtime and RKNPU buffer synchronization. Application-level code has limited control here.

Possible follow-up optimization:

1. Keep NPU governor in performance mode during benchmark.
2. Avoid recreating RKNN input/output memory per frame.
3. Keep the current zero-copy RKNN memory path.

### 6.3 V4L2 QBUF and cache clean

Related hotspots:

```text
v4l_qbuf                    5.20%
vb2_core_qbuf               5.14%
__pi___clean_dcache_area_poc 4.78% in QBUF path
```

This is the cost of returning DMA buffers to V4L2 and cleaning cache when needed.

Important point:

```text
Do not QBUF too early in the DMA-BUF encode path.
```

Returning the buffer early can cause ISP/V4L2 to reuse the same buffer while MPP is still encoding it.

### 6.4 RTSP NALU search

Related hotspot:

```text
memchr  4.42%
```

This is the optimized NALU start-code search path. The old byte-by-byte loop was replaced by `memchr`, so this result means the optimized search path is active.

Further optimization direction:

1. Do not use multi-threaded NALU search for this low-latency pipeline.
2. A better approach is to avoid scanning when possible.
3. If MPP packet segment information is reliable, use segment metadata to reduce full H.264 stream scanning.

## 7. Verification Checklist

After deployment, compare these perf symbols:

```text
NC1HWC2_i8_to_NCHW_i8
copy_nv12_v4l2_to_mpp
memcpy
post_process_native
rknpu_gem_sync_ioctl
v4l_qbuf
memchr
tcp_sendmsg
```

Expected result:

1. `NC1HWC2_i8_to_NCHW_i8` should not appear as a major hotspot.
2. `copy_nv12_v4l2_to_mpp` should not appear as a major hotspot.
3. `post_process_native` may become the main application-level hotspot.
4. RKNPU sync, V4L2 QBUF, cache clean, and TCP send are expected system-level costs.

Latency logs to check:

1. `yolo_output_convert` should be `0 ms`.
2. `memcpy_to_mpp` should be close to `0 ms`.
3. `e2e_dqbuf_to_rtsp_tx` should stay lower than the baseline.
4. `capture_ts_to_rtsp_tx` should stay lower than the baseline.

## 8. Interview Summary

A concise way to describe this work:

```text
I profiled the RK3566 IMX415 YOLO RTSP pipeline with perf and found two clear CPU hotspots: RKNN output layout conversion and full-frame NV12 memcpy before MPP encode.

For RKNN, I removed the NC1HWC2-to-NCHW conversion and added native-output postprocess, so the postprocess reads RKNN zero-copy output memory directly.

For encoding, I exported V4L2 buffers as DMA-BUF fds and imported them into MPP, so MPP can encode the ISP output buffer directly without copying the whole NV12 frame.

After optimization, the previous conversion and memcpy hotspots disappeared from perf. The remaining major costs are YOLO postprocess, RKNN/RKNPU synchronization, V4L2 QBUF cache clean, and TCP send.
```

