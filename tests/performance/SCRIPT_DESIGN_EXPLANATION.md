# 脚本设计说明

## 1. 为什么这样拆

这套工具有四条清楚的边界：

1. `run_benchmark.py` 只负责流程和进程生命周期；
2. `collect_system_metrics.py` 只负责板端每秒资源快照；
3. `analyze_metrics.py` 只读取原始文件并计算；
4. `generate_report.py` 只把结构化结论翻译成 Markdown。

因此，执行失败不会偷偷改变统计公式，统计代码修改也不会改变被测业务行为。所有模块只使用 Python 标准库，便于在 Ubuntu 18.04 上直接阅读和修改。

## 2. 一轮数据如何流动

执行器先加载 `scenarios.json`，检查客机命令、板端设备/模型/二进制、磁盘、已有进程和 MD5，然后固定 CPU/NPU governor。板端应用启动后写逐帧 CSV 和每秒更新一次的队列状态文件；板端采集器根据应用 PID 读取 `/proc`、sysfs/debugfs 和队列状态。预热结束后，执行器记录板端单调时钟起点并启动 FFmpeg 拉流。正式时长结束或保护条件触发后，它按 TERM→等待→KILL 的顺序清理，只回收自己记录的 PID。

原始文件回收到每轮目录后，分析器用 `metadata.json` 中的单调时钟边界过滤预热数据。报告器不重新计算，只读取 `summary.json`，防止报告口径和 JSON 不一致。

## 3. 被测程序的必要测试开关

业务链路没有被重构，只增加测试行为所必需的运行时开关：

- `PERF_AI_ENABLED`：是否允许推理；
- `PERF_INFER_INTERVAL`：每几帧形成一次推理触发；
- `PERF_SUBMIT_POLICY`：关闭、允许排队、仅空闲提交最新帧；
- `PERF_REUSE_BOXES`：是否在后续输出帧复用最近结果；
- `PERF_INFER_QUEUE_LIMIT`：真实排队场景的安全上限；
- `PERF_AI_START_FILE`：S1/S2 在预热结束后才激活 AI；
- `PERF_METRICS_CSV`、`PERF_DETAILED_TRACE`：逐事件埋点及开关；
- `PERF_QUEUE_STATUS_FILE`：以 1 Hz 左右向系统采集器暴露队列快照。

逐帧 CSV 使用 1 MiB 用户态缓冲，不逐帧 `flush`，正常收到 SIGINT/SIGTERM 时统一刷新关闭。队列状态文件每秒最多更新一次，用临时文件加 `rename`，采集器不会读到半行。

## 4. 三种提交策略

`disabled` 不触发推理。`always_queue` 为每次符合间隔的触发复制一份 NV12 帧到固定容量环形队列；队列达到保护线时记录 `queue_limit_stop` 并停机，这样既能真实证明积压，又不会无界占用约 3.11 MiB/帧的内存。`latest_if_idle` 只有 `busy=0` 且队列为空才接收当前最新帧，否则记录 `npu_busy`，不保存过期任务。

工作线程从环形队列取帧，保留源 Frame ID、源采集时间和真正提交时间。由此可分别计算排队时间、RGA 预处理、RKNN/NPU、后处理和框年龄。

## 5. 系统采集的降级顺序

进程/线程 CPU 来自 `/proc/PID/stat` 和 `/proc/PID/task/*/stat`；RSS 来自 `status`，PSS 来自 `smaps_rollup`。NPU 利用率只认可 RKNN debugfs 的百分比，读取失败写 `Unavailable`；频率仍可独立读取 devfreq。所有字段都保留原始行，报告不会把缺值填成零。

采集器每行立即刷新，因为频率只有 1 Hz，开销远低于逐帧刷新，同时能在异常退出时保住已完成样本。采集器还记录磁盘剩余量；低于保护线会非零退出，执行器据此终止测试。

## 6. 指标与校验

分析器先保留整个文件用于 Frame ID 关联，再只对正式测量窗计数。P50/P95/P99 统一用最近秩，避免各脚本插值方法不同。最低 1 秒 FPS 从正式起点划分完整 1 秒窗口，不使用滑动挑选最好窗口。

完整性检查覆盖：事件时间单调、阶段先后、采集 Frame ID 重复/跳变、处理帧与采集帧关联、检测源帧关联、正式时长、系统采样数和最大采样间隔。视频 `drop` 与推理 `skipped` 永远分列。

## 7. 异常处理

应用 PID、采集器 PID 和 FFmpeg PID 都由执行器单独保存。任一核心进程意外退出会立即停止当前轮，不继续产生看似完整的数据。Ctrl+C 或异常进入同一个 `finally` 清理路径；即使分析失败，已有 CSV、日志和失败原因仍会保留。已有未知 IMX415 进程默认只报告不终止，只有明确传 `--stop-existing` 才停止。
