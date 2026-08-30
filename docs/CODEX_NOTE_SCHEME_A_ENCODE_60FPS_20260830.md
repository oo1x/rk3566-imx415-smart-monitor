# Codex 交接说明：方案 A 把编码输出做到 60 fps

给后续 Codex / 讲解用。先读本文，再对照 `yolo_integration/imx415_yolo_rtsp.cpp`。  
不要把下面的数字说成 7 月正式 S4（那是 `latest_if_idle`、每 5 帧）。  
不要把 2026-08-29 的 15 fps 失败说成应用改成了 4K15。

- 仓库：`https://github.com/oo1x/rk3566-imx415-smart-monitor`
- 分支：`latest-pending`
- 本文路径：`docs/CODEX_NOTE_SCHEME_A_ENCODE_60FPS_20260830.md`
- 代码改动文件：`yolo_integration/imx415_yolo_rtsp.cpp`（只动 `handoff_get_fresh()`）

---

## 1. 一句话

采集本来就是 1080p60。编码少约 1.1 fps，是因为编码线程取下一帧时把槽里已经到的最新帧扔掉了。方案 A 改成「槽里有帧就编」。300 秒复测：采集 60、编码 60、AI 新检测 15、板端视频丢帧 0。

---

## 2. 优化前是什么样

### 2.1 当时已经在树上的策略（不要和方案 A 混）

`latest-pending` 在方案 A 之前已经有两层 Cursor 改动，**都没有改 Sensor 驱动，也没有改成 4K15**：

| 提交 | 做什么 |
|---|---|
| `d9521b1` | latest-pending：NPU 忙时覆盖唯一 pending，不排队 |
| `8da8601` | 只在第 0、4、8… 帧 memcpy 给 YOLO；中间帧只叠框+编码 |
| `c17027e` | 文档：把上述策略写成当前默认 |

采集宏一直是 `1920×1080 @ 60`，`BUF_COUNT=4`。  
Sensor 1080p60 真实 raw 是 `1944×1096@60`，ISP crop 到 `1920×1080`。驱动默认 mode 是 `3864×2192@15`。应用只 `S_FMT` `/dev/video0`，不切 Sensor。板子若停在 4K15，应用只能拿到 15 fps。这是板端 mode，不是这份代码改的。

### 2.2 方案 A 之前、同一策略、1080p60 已测数字

二进制：`imx415_yolo_rtsp_latest_pending_20260830`  
MD5：`a319b008668c809f43d315b2d6df7a12`

| 轮次 | 采集 | 编码输出 | 客户端窗口 | 检测更新 | 视频丢帧 |
|---|---:|---:|---:|---:|---:|
| 冒烟 30s | 60.000 | 59.000 | 58.733 | 14.767 | 1.667% |
| 正式 300s | 60.000 | 58.873 | 58.840 | 14.720 | 338 / 1.878% |
| 长测 600s | 60.000 | 58.890 | 58.900 | 14.722 | 666 / 1.850% |

对照（不要当成本分支结果）：

- 7 月正式 S4：`latest_if_idle`、每 5 帧；编码约 58.65–58.80，检测约 11.75
- 2026-08-29 失败冒烟：Sensor 停在 `3864×2192@15`，采集/编码 15.000

### 2.3 根因（优化前）

板端视频丢帧全部是 `latest_frame_replaced`，发生在采集线程和编码线程之间的 **1 个 ready 槽**，不在 Sensor，也不在 NPU。

600 秒里 666 次丢帧：

- 589/589 次窗口内近似 IDR 后面都丢掉了下一张
- 丢掉的是序号 `N`：已经采集、还没进 MPP
- 紧接着编码 `N+1`
- 「上一帧 MPP 编码 > 16.7 ms」只有 11 次；丢帧前一帧编码中位数约 12.6 ms

结论：每秒强制 IDR（`MPP_ENC_SET_IDR_FRAME`，`frame_count % 60 == 0`）会让编码线程整圈（编码 + 大包 `rtsp_tx` + `rtsp_do_event`）偶发超过 16.7 ms。下一张此时已经在 ready 里。

**真正误伤在 `handoff_get_fresh()`：** 取下一帧前先 `handoff_drop_ready_locked()`，把槽里已有的最新帧扔掉，再等下一张。采集侧覆盖是合理的 latest-only；编码侧再扔一次没有「更新」价值，只少编一帧。

V4L2 持有只到 MPP 输出，不到 RTSP。DMA-BUF 路径：叠框 → `mpp_encoder_encode` → `handoff_release_frame` → 然后才 `rtsp_tx_video`。RTSP 不占 V4L2，但和编码在同一条线程，发完 IDR 大包才会再 `get_fresh`。

优化前伪代码：

```c
/* handoff_get_fresh 旧逻辑 */
handoff_drop_ready_locked(q);   /* 槽里有帧也扔掉 */
while (!q->has_ready)
    wait();
take q->ready;
```

---

## 3. 这次改了什么

只改 `handoff_get_fresh()`：去掉取帧前的 `handoff_drop_ready_locked(q)`。  
槽里有帧就马上编；空了才等采集。

```c
/* 方案 A：槽里已有最新帧则直接拿走 */
while (g_running && !q->has_ready)
    wait();
take q->ready;
```

采集线程仍可覆盖未拿走的 ready（只留最新）。  
AI 仍是 latest-pending、每 4 帧拷贝。  
每秒 IDR 没动。

讲解时可以说：P 帧整圈多半 < 16.7 ms，回来时槽是空的，行为和以前一样。只有 IDR 晚归那一下，下一张已经在槽里，现在直接编掉，把超时吸收掉，不再空等 16.7 ms。

---

## 4. 没改什么

- `sensor_driver/imx415.c`、分辨率、帧率宏
- latest-pending 策略、`YOLO_INFER_INTERVAL=4`
- MPP IDR / GOP / 码率
- `rtsp_tx` 线程模型、V4L2 `BUF_COUNT=4`
- 没有做 2 槽 ready，也没有把发送拆出编码线程

---

## 5. 优化后最终结果

二进制：`imx415_yolo_rtsp_latest_pending_a_20260830`  
MD5：`1e9edbb18c39a1f8f4bce254cb6df399`  
测量：300 s，预热 60 s，`latest_pending`，间隔 4，1080p60，状态 **PASS**

| 指标 | 数值 | 原始计数 / 口径 |
|---|---:|---|
| 采集 | **60.000** | 18000 `capture` / 300 s |
| 编码输出 | **60.000** | 18000 `processed` / 300 s |
| 最低 1 秒采集 / 编码 | 60.000 / 59.000 | 整秒窗口最小计数 |
| 客户端窗口 | 59.963 | FFmpeg 17989 帧 / 300 s 墙钟 |
| 客户端活动 | 60.020 | 17989 / PTS 299.717 s |
| AI 新检测 | **15.000** | 4500 次推理完成 / 300 s（60/4） |
| 推理触发 / 跳过 | 4500 / 0 | 占输出 25% |
| 板端视频丢帧 | **0 / 0%** | `latest_frame_replaced` 次数为 0 |
| FFmpeg `drop_frames` | 0 | 解码器未报丢帧 |
| 完整性 | 全 0 | frame_id 跳跃、时间戳回退、关联失败均为 0 |
| MPP 编码 P50 / 最大 | 12.301 / 19.522 ms | IDR 仍可能 > 16.7 ms，但不再因此扔下一张 |
| NPU / 温度峰值 | 66% / 61.1 °C | 与改前同一量级 |

板端报告：`C:\Users\oo1\Desktop\IMX415\latest_pending_1080p60_20260830\scheme_a_300s_performance_report.md`  
Guest：`/home/oo1/latest_pending_results_20260830/measure_a_300s/20260829_202137_S4`

方案 A 之后还没有再跑 10 分钟。300 秒已经是满 60 编码、0 板端丢帧。

---

## 6. 客户端少的 11 帧不是播放丢帧

18000 编码 vs 17989 收到，差 11 帧（约 183 ms）。

- 板端 `video_drop_events = 0`
- FFmpeg `drop_frames = 0`
- `client_active = 60.020`：锁流后按 PTS 就是 60

这 11 帧在测量窗两端：正式计时一开始就拉 FFmpeg（握手 + 等下一张 IDR），到点 SIGINT 时网上最后几帧来不及计入。7 月 S0/S4 也是 `client_active ≈ 60.02`、`client_window` 略低于板端。

讲解「有没有做到 60」：板端看采集/编码；播放稳不稳看 `client_active` 和 `ffmpeg_drop_frames`。不要把 `client_window` 说成客户端一直在丢。

---

## 7. 三条线程（当前）

```text
采集线程   DQBUF → 写入唯一 ready 槽
           若编码还没拿走上一张：覆盖（latest_frame_replaced）并 QBUF 旧帧

编码线程   get_fresh：槽里有就拿，没有才等          ← 方案 A
           仅 frame_idx % 4 == 0 时 memcpy 给 YOLO pending
           RGA 叠框（写同一 V4L2 NV12）
           MPP 编码同一 dma-buf
           QBUF 还 V4L2
           rtsp_tx_video + rtsp_do_event
           约每 60 个编码帧强制 IDR

AI 线程    pending / work 换指针
           NPU 忙：到期帧覆盖 pending
           NPU 空：立刻推理
           发布 boxes，非推理帧复用
```

---

## 8. 讲解时不要混的口径

1. 4K15 不是这份代码改的，是 Sensor 默认 mode。
2. 7 月 S4 的 58.6 / 11.75 是每 5 帧 + `latest_if_idle`，不是本分支默认。
3. `8da8601` 解决的是「每帧 3 MB memcpy 给 AI」。方案 A 解决的是「编码取帧误扔 ready」。
4. V4L2 不持有到 RTSP；RTSP 只让编码线程晚一点回到 `get_fresh`。
5. 方案 A 之后还没做 10 分钟长稳，只有 300 秒满 60。

---

## 9. 拉流

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop \
  -rtsp_transport tcp rtsp://192.168.1.20:8554/live
```
