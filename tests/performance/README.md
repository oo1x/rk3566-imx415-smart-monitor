# RK3566 + IMX415 + YOLO11n 性能测试脚本

这套脚本用一条命令执行 S0–S5，负责预检、预热、正式测量、资源采集、停止、清理、统计和归档。它只使用 Python 标准库以及系统已有的 `ssh`、`scp`、`ffmpeg`，不包含 Agent、Web 服务或外部 Python 依赖。

## 当前状态

- 统一执行器、板端 1 Hz 采集器、统计器、Markdown 报告器和人工样本单元测试已实现。
- S0、S3、S4 已分别完成三轮正式测试；S1/S2 各三轮按队列保护规则得到 `EXPECTED_ABORT`。
- S4（每 5 帧、`latest_if_idle`）已根据三轮结果选为稳定策略。
- `scenarios.json` 已固定正式测试二进制 MD5 `fbc5c50fc6fc1b76482353a0b430d9cc`。
- S5 只完成一轮 1800 秒正式测试，后续轮次按用户要求停止；不能宣称完成三轮长稳重复性验收。

## 环境前提

执行位置是 Ubuntu 18.04 虚拟机，而不是 Windows 主机。虚拟机需要：

- Python 3、OpenSSH 客户端、FFmpeg；
- 能以密钥方式登录 `cat@192.168.1.20`；
- 板端有 Python 3，并允许 `sudo -n` 设置 CPU/NPU governor、读取 RKNN debugfs 负载；
- 板端 `/dev/video0`、模型和测试二进制路径与 `scenarios.json` 一致；
- 输入画面、镜头位置和光照在重复轮次之间保持不变。

## 一条命令执行

在项目根目录执行：

```bash
python3 tests/performance/run_benchmark.py \
  --scenario S3 --duration 300 --repeat 3 --output tests/performance/results/
```

只做预检，不启动测试：

```bash
python3 tests/performance/run_benchmark.py --scenario S3 --preflight-only
```

如果确认板端只有上次遗留的 `./imx415_yolo_rtsp*`，可明确要求脚本先停止它：

```bash
python3 tests/performance/run_benchmark.py --scenario S3 --stop-existing
```

默认遇到已有进程会退出，不会误杀未知任务。Ctrl+C 会停止本轮 FFmpeg、板端应用和采集器，已产生的文件仍会回收并标记 `INTERRUPTED`。

## 场景

| 场景 | 触发和提交行为 | 默认正式时长 | 重点 |
|---|---|---:|---|
| S0 | AI 关闭 | 300 s × 3 | 视频基线、CPU、丢帧 |
| S1 | 每帧尝试，允许排队 | 60 s × 3 | 队列到安全上限立即停止并留证 |
| S2 | 每 3 帧提交，允许排队 | 60 s × 3 | 积压和过期任务风险 |
| S3 | 每 3 帧检查，仅 NPU 空闲时取最新帧 | 300 s × 3 | 队列有界、检测率、框年龄 |
| S4 | 每 5 帧检查，仅 NPU 空闲时取最新帧 | 300 s × 3 | 资源和检测新鲜度折中 |
| S5 | 选定策略长稳 | 1800 s × 3 | 温升、内存、丢帧、队列稳定 |

S1/S2 预热阶段只运行视频链路，正式测量起点才激活 AI，防止队列在预热阶段提前触发保护。`always_queue` 的队列是有安全容量的真实帧队列；达到 `infer_queue_limit` 会写入 `queue_limit_stop` 证据并停止，不允许无界消耗内存。S3/S4 的 `latest_if_idle` 在 NPU 忙或已有待处理项时跳过本次推理，但视频仍持续编码并复用最近检测结果。

## 每轮产物

```text
results/<时间>_<场景>/runN/
  metadata.json
  frame_metrics.csv
  system_metrics.csv
  stdout.log
  stderr.log
  ffmpeg_progress.txt
  ffmpeg_stderr.log
  collector_status.json
  summary.json
  performance_report.md
```

批次根目录还有 `preflight.json`、`aggregate_summary.json` 和三轮汇总 `performance_report.md`。所有报告数字都能回到两个原始 CSV 和 `summary.json` 中的公式。

## 统计口径

- 采集 FPS：正式测量窗内 `capture` 行数 ÷ 实际测量秒数。
- 编码输入/输出 FPS：正式测量窗内 `processed` 行数 ÷ 实际测量秒数。
- 新检测更新率：正式测量窗内完成的 `inference` 行数 ÷ 秒数；重复使用旧框不计。
- 视频丢帧率：`drop` 行数 ÷ `capture` 行数；推理主动跳过单独统计。
- 框年龄：输出帧 `overlay_end` 减去 `detection_source_frame_id` 对应源帧的采集时间。
- P50/P95/P99：最近秩法，排序后取 `ceil(p×N)-1`。
- CPU：相邻样本进程 tick 差 ÷ `CLK_TCK` ÷ 时间差；单核满载为 100%。
- NPU：优先读取 `/sys/kernel/debug/rknpu/load`。不可读就写 `Unavailable`，不会把已证实无效的 `devfreq/load` 当利用率。
- 采集→编码输出：板内同一个 `CLOCK_MONOTONIC` 的内部延迟，不等于客户端显示端到端延迟。

## 校验和失败规则

以下情况不会标成 Pass：关键文件缺失、测量时长不足、逐阶段时间逆序、Frame ID 重复、处理帧找不到采集帧、系统采样不足/中断超过 2.5 秒、应用或采集器意外退出、磁盘低于保护线、场景阈值失败。S1/S2 的预期队列保护停止标成 `EXPECTED_ABORT`，只有确实出现 `queue_limit_stop` 才成立。

运行人工样本测试：

```bash
python3 -m unittest discover -s tests/performance/tests -v
```

## 埋点开关 A/B

`detailed_trace` 由 `scenarios.json` 控制。正式 S0–S5 必须为 `true` 才能生成完整逐帧指标。埋点开/关 A/B 应复制一个短场景，其他参数完全不变，只切换该字段；关闭埋点后的运行只比较客户端 FPS、系统资源和稳定性，逐帧延迟应报告 `Unavailable`，不得与完整埋点报告混算。

## 修改场景

只编辑 `scenarios.json`：

- `duration_sec`、`warmup_sec`、`repeat` 控制时间和轮次；
- `ai_enabled`、`trigger_interval` 控制 AI 与触发频率；
- `submit_policy` 只能是 `disabled`、`always_queue`、`latest_if_idle`；
- `reuse_boxes` 控制是否把最近结果叠加到后续视频帧；
- `infer_queue_limit` 是 S1/S2 的内存/积压保护线；
- `thresholds` 只放有明确依据的判定值，不得为了 Pass 临时放宽。
- 需要完全替换默认阈值（例如 S1/S2 或埋点关闭 A/B）时设置 `inherit_default_thresholds: false`。

修改或重编测试二进制后，必须同时更新 `expected_binary_md5`，并在报告中保留实际板端 MD5。
