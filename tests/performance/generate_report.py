#!/usr/bin/env python3
"""Generate traceable Markdown reports from summary JSON files."""
from __future__ import print_function

import argparse
import glob
import json
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from lib.io_utils import UNAVAILABLE, load_json
from lib.statistics_utils import numeric_summary


def _fmt(value, digits=3):
    if value is None or value == UNAVAILABLE:
        return UNAVAILABLE
    if isinstance(value, bool):
        return "是" if value else "否"
    if isinstance(value, (int, float)):
        return ("%%.%df" % digits) % value
    return str(value)


def _write(path, lines):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines).rstrip() + "\n")


def render_run(run_dir, output=None):
    summary = load_json(os.path.join(run_dir, "summary.json"))
    metadata_path = os.path.join(run_dir, "metadata.json")
    metadata = load_json(metadata_path) if os.path.isfile(metadata_path) else {}
    scenario = summary.get("scenario", UNAVAILABLE)
    lines = [
        "# %s 性能测试报告" % scenario,
        "",
        "状态：`%s`。本报告只使用同目录原始 CSV 自动计算；`Unavailable` 表示没有可靠采样，不作估算。" %
        summary.get("status", UNAVAILABLE),
        "",
        "## 测试配置",
        "",
        "- 轮次：%s" % _fmt(summary.get("run"), 0),
        "- 正式测量：%s 秒（请求 %s 秒）" % (
            _fmt(summary.get("measurement", {}).get("actual_duration_sec")),
            _fmt(summary.get("measurement", {}).get("requested_duration_sec"))),
        "- AI：%s；触发间隔：%s；提交策略：`%s`；复用检测框：%s" % (
            _fmt(metadata.get("scenario", {}).get("ai_enabled", UNAVAILABLE)),
            _fmt(metadata.get("scenario", {}).get("trigger_interval", UNAVAILABLE), 0),
            metadata.get("scenario", {}).get("submit_policy", UNAVAILABLE),
            _fmt(metadata.get("scenario", {}).get("reuse_boxes", UNAVAILABLE))),
        "- 二进制 MD5：`%s`" % metadata.get("environment", {}).get("binary_md5", UNAVAILABLE),
        "- RTSP：`%s`，传输：`%s`" % (
            metadata.get("testbed", {}).get("rtsp_url", UNAVAILABLE),
            metadata.get("testbed", {}).get("rtsp_transport", UNAVAILABLE)),
        "",
        "## 核心结果",
        "",
        "| 指标 | 数值 | 原始口径 |",
        "|---|---:|---|",
        "| 采集 FPS | %s | capture 行数 / 正式时长 |" % _fmt(summary.get("fps", {}).get("capture")),
        "| 编码输入/输出 FPS | %s / %s | processed 行数 / 正式时长 |" % (
            _fmt(summary.get("fps", {}).get("encode_input")), _fmt(summary.get("fps", {}).get("output"))),
        "| 最低 1 秒采集/输出 FPS | %s / %s | 从测量起点对齐的完整 1 秒窗口最小计数 |" % (
            _fmt(summary.get("fps", {}).get("capture_min_1s")),
            _fmt(summary.get("fps", {}).get("output_min_1s"))),
        "| 客户端窗口/活动 FPS | %s / %s | FFmpeg 帧数÷测量窗 / 帧数÷PTS跨度 |" % (
            _fmt(summary.get("fps", {}).get("client_window")),
            _fmt(summary.get("fps", {}).get("client_active"))),
        "| 新检测更新率 | %s FPS | inference 完成事件；复用框不计 |" %
        _fmt(summary.get("detection", {}).get("update_fps")),
        "| 视频丢帧 | %s / %s%% | drop 事件 / capture 事件；与推理跳过分开 |" % (
            _fmt(summary.get("drops", {}).get("video_drop_events"), 0),
            _fmt(summary.get("drops", {}).get("video_drop_pct"))),
        "| 推理跳过 | %s / %s%% | skipped=1 / triggered=1 |" % (
            _fmt(summary.get("inference", {}).get("skipped_attempts"), 0),
            _fmt(summary.get("inference", {}).get("skip_pct_of_triggers"))),
        "| 最大推理队列 | %s | infer_queue_depth 最大值 |" %
        _fmt(summary.get("inference", {}).get("max_queue_depth"), 0),
        "| CPU 平均/峰值 | %s%% / %s%% | 单核=100%% |" % (
            _fmt(summary.get("system", {}).get("process_cpu_pct", {}).get("mean")),
            _fmt(summary.get("system", {}).get("process_cpu_pct", {}).get("max"))),
        "| RSS 平均/峰值 | %s / %s KiB | /proc/PID/status |" % (
            _fmt(summary.get("system", {}).get("rss_kb", {}).get("mean")),
            _fmt(summary.get("system", {}).get("rss_kb", {}).get("max"))),
        "| NPU 平均/峰值 | %s%% / %s%% | debugfs rknpu/load；不可读则 Unavailable |" % (
            _fmt(summary.get("system", {}).get("npu_load_pct", {}).get("mean")),
            _fmt(summary.get("system", {}).get("npu_load_pct", {}).get("max"))),
        "| 温度平均/峰值 | %s / %s °C | thermal_zone0 |" % (
            _fmt(summary.get("system", {}).get("temperature_c", {}).get("mean")),
            _fmt(summary.get("system", {}).get("temperature_c", {}).get("max"))),
        "",
        "## 延迟分布（毫秒）",
        "",
        "| 阶段 | 平均 | P50 | P95 | P99 | 最大 | 样本 |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    labels = (
        ("rga_preprocess", "RGA 预处理"),
        ("inference_queue_wait", "推理排队"),
        ("npu_run", "RKNN/NPU 运行"),
        ("postprocess", "后处理"),
        ("overlay", "Overlay"),
        ("encode", "MPP 编码"),
        ("capture_to_overlay", "采集→Overlay 完成"),
        ("capture_to_encode_output", "采集→编码输出"),
    )
    for key, label in labels:
        stats = summary.get("latency_ms", {}).get(key, {})
        lines.append("| %s | %s | %s | %s | %s | %s | %s |" % (
            label, _fmt(stats.get("mean")), _fmt(stats.get("p50")),
            _fmt(stats.get("p95")), _fmt(stats.get("p99")),
            _fmt(stats.get("max")), _fmt(stats.get("samples"), 0)))
    box = summary.get("detection", {}).get("box_age_ms", {})
    lines.extend([
        "| 检测框年龄 | %s | %s | %s | %s | %s | %s |" % (
            _fmt(box.get("mean")), _fmt(box.get("p50")), _fmt(box.get("p95")),
            _fmt(box.get("p99")), _fmt(box.get("max")), _fmt(box.get("samples"), 0)),
        "",
        "## 跳过原因与完整性",
        "",
        "推理跳过原因：`%s`。" % json.dumps(
            summary.get("inference", {}).get("skip_reason_counts", {}), ensure_ascii=False, sort_keys=True),
        "",
        "完整性检查：`%s`。" % json.dumps(
            summary.get("integrity", {}), ensure_ascii=False, sort_keys=True),
        "",
        "验证错误：`%s`。" % json.dumps(
            summary.get("validation", {}).get("errors", []), ensure_ascii=False),
        "",
        "验证警告：`%s`。" % json.dumps(
            summary.get("validation", {}).get("warnings", []), ensure_ascii=False),
        "",
        "## 可追溯文件",
        "",
        "- `metadata.json`：配置、环境、测量边界和二进制身份。",
        "- `frame_metrics.csv`：逐帧/逐推理原始埋点。",
        "- `system_metrics.csv`：每秒资源原始采样。",
        "- `stdout.log`、`stderr.log`：被测程序原始输出。",
        "- `summary.json`：本报告使用的结构化统计和公式。",
        "",
        "注意：采集→编码输出是板内同一单调时钟延迟，不等于客户端显示端到端延迟。",
    ])
    output = output or os.path.join(run_dir, "performance_report.md")
    _write(output, lines)
    return output


def _path_value(document, path):
    value = document
    for part in path.split("."):
        if not isinstance(value, dict):
            return UNAVAILABLE
        value = value.get(part, UNAVAILABLE)
    return value


def render_batch(batch_dir, output=None):
    paths = sorted(glob.glob(os.path.join(batch_dir, "run*", "summary.json")))
    summaries = [load_json(path) for path in paths]
    metrics = (
        ("fps.capture", "采集 FPS"),
        ("fps.output", "输出 FPS"),
        ("detection.update_fps", "检测更新 FPS"),
        ("drops.video_drop_pct", "视频丢帧率 %"),
        ("inference.skip_pct_of_triggers", "推理跳过率 %"),
        ("latency_ms.capture_to_encode_output.p95", "采集→编码输出 P95 ms"),
        ("detection.box_age_ms.p95", "框年龄 P95 ms"),
        ("system.process_cpu_pct.mean", "进程 CPU 平均 %"),
        ("system.rss_kb.max", "RSS 峰值 KiB"),
        ("system.temperature_c.max", "温度峰值 °C"),
    )
    scenario = summaries[0].get("scenario", UNAVAILABLE) if summaries else UNAVAILABLE
    lines = [
        "# %s 重复测试汇总" % scenario,
        "",
        "保留所有轮次，不选择最好结果。范围按各轮 summary.json 计算。",
        "",
        "| 指标 | 均值 | 最小 | 最大 |",
        "|---|---:|---:|---:|",
    ]
    aggregate = {"scenario": scenario, "runs": [], "metrics": {}}
    for summary in summaries:
        aggregate["runs"].append({"run": summary.get("run"), "status": summary.get("status")})
    for path, label in metrics:
        values = [_path_value(summary, path) for summary in summaries]
        stats = numeric_summary(values)
        aggregate["metrics"][path] = stats
        lines.append("| %s | %s | %s | %s |" % (
            label, _fmt(stats["mean"]), _fmt(stats["min"]), _fmt(stats["max"])))
    lines.extend(["", "轮次状态：`%s`。" % json.dumps(aggregate["runs"], ensure_ascii=False)])
    aggregate_path = os.path.join(batch_dir, "aggregate_summary.json")
    with open(aggregate_path, "w", encoding="utf-8") as handle:
        json.dump(aggregate, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")
    output = output or os.path.join(batch_dir, "performance_report.md")
    _write(output, lines)
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--run-dir")
    group.add_argument("--batch-dir")
    parser.add_argument("--output")
    args = parser.parse_args(argv)
    output = render_run(args.run_dir, args.output) if args.run_dir else render_batch(args.batch_dir, args.output)
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
