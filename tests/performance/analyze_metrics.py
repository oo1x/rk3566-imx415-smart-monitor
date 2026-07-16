#!/usr/bin/env python3
"""Analyze one benchmark run using only Python's standard library."""
from __future__ import print_function

import argparse
import csv
import json
import os
import sys
from collections import Counter

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from lib.io_utils import UNAVAILABLE, dump_json_atomic, load_json, read_key_values
from lib.statistics_utils import as_float, as_int, metric_stats, min_aligned_window_rate


def _read_csv(path):
    if not os.path.isfile(path):
        return []
    with open(path, "r", newline="") as handle:
        return list(csv.DictReader(handle))


def _number(row, *names):
    for name in names:
        if name in row:
            value = as_float(row.get(name))
            if value is not None:
                return value
    return None


def _integer(row, *names):
    value = _number(row, *names)
    return int(value) if value is not None else None


def _delta_ms(row, end_names, start_names):
    end = _number(row, *end_names)
    start = _number(row, *start_names)
    if end is None or start is None or end <= 0 or start <= 0 or end < start:
        return None
    return (end - start) / 1000.0


def _measurement_window(run_dir, metadata):
    measurement = metadata.get("measurement", {}) if metadata else {}
    start_s = as_float(measurement.get("board_monotonic_start_s"))
    end_s = as_float(measurement.get("board_monotonic_end_s"))
    requested = as_float(measurement.get("requested_duration_sec"))

    legacy = read_key_values(os.path.join(run_dir, "measurement_window.txt"))
    if start_s is None:
        start_s = as_float(legacy.get("board_monotonic_start_s"))
    if requested is None:
        requested = as_float(legacy.get("requested_duration_s"))
    if end_s is None:
        legacy_end = as_float(legacy.get("board_monotonic_end_s"))
        if start_s is not None and requested is not None:
            end_s = start_s + requested
        else:
            end_s = legacy_end
    if requested is None and start_s is not None and end_s is not None:
        requested = end_s - start_s
    if start_s is None or end_s is None or end_s <= start_s:
        raise ValueError("measurement window is missing or invalid")
    return start_s * 1000000.0, end_s * 1000000.0, requested


def _parse_ffmpeg_progress(path):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            if "=" in line:
                key, value = line.rstrip("\n").split("=", 1)
                values[key] = value
    return values


def _event_timestamp(row):
    event = row.get("event", "")
    if event == "inference":
        return _number(row, "infer_end_us", "post_end_us", "capture_ts_us")
    return _number(row, "capture_ts_us")


def _within(rows, start_us, end_us):
    selected = []
    for row in rows:
        timestamp = _event_timestamp(row)
        if timestamp is not None and start_us <= timestamp < end_us:
            selected.append(row)
    return selected


def _monotonic_violations(rows, field):
    previous = None
    violations = 0
    for row in rows:
        value = _number(row, field)
        if value is None or value <= 0:
            continue
        if previous is not None and value < previous:
            violations += 1
        previous = value
    return violations


def _stage_order_violations(rows):
    violations = 0
    for row in rows:
        if row.get("event") == "inference":
            names = (
                "capture_ts_us", "infer_submit_us", "rga_pre_start_us",
                "rga_pre_end_us", "infer_start_us", "infer_end_us", "post_end_us",
            )
        elif row.get("event") == "processed":
            names = (
                "capture_ts_us", "overlay_start_us", "overlay_end_us",
                "encode_submit_us", "encode_output_us",
            )
        else:
            continue
        values = []
        for name in names:
            value = _number(row, name)
            if value is not None and value > 0:
                values.append(value)
        if any(values[index] > values[index + 1] for index in range(len(values) - 1)):
            violations += 1
    return violations


def _analyze_system(rows, start_us, end_us, metadata, warnings):
    start_s, end_s = start_us / 1000000.0, end_us / 1000000.0
    window = [row for row in rows
              if (_number(row, "monotonic_s") is not None and
                  start_s <= _number(row, "monotonic_s") < end_s)]
    result = {
        "samples": len(window),
        "process_cpu_pct": metric_stats([]),
        "rss_kb": metric_stats([]),
        "pss_kb": metric_stats([]),
        "npu_load_pct": metric_stats([]),
        "npu_frequency_hz": metric_stats([]),
        "temperature_c": metric_stats([]),
        "queue_depth": metric_stats([]),
        "max_sample_gap_sec": UNAVAILABLE,
        "thread_cpu_peak_pct": {},
        "network_drop_delta": {"rx": UNAVAILABLE, "tx": UNAVAILABLE},
    }
    if not window:
        return result

    direct_cpu = [_number(row, "process_cpu_pct") for row in window]
    direct_cpu = [value for value in direct_cpu if value is not None]
    if direct_cpu:
        result["process_cpu_pct"] = metric_stats(direct_cpu)
    elif len(window) >= 2:
        ticks_per_second = as_float(metadata.get("environment", {}).get("clock_ticks_per_second"))
        if ticks_per_second is None:
            ticks_per_second = 100.0
            warnings.append("CPU tick rate absent; legacy replay uses documented RK3566 HZ=100")
        cpu_values = []
        previous = None
        for row in window:
            timestamp = _number(row, "monotonic_s")
            user = _number(row, "proc_utime_ticks")
            system = _number(row, "proc_stime_ticks")
            if timestamp is None or user is None or system is None:
                continue
            current = (timestamp, user + system)
            if previous and current[0] > previous[0] and current[1] >= previous[1]:
                cpu_values.append((current[1] - previous[1]) / ticks_per_second /
                                  (current[0] - previous[0]) * 100.0)
            previous = current
        result["process_cpu_pct"] = metric_stats(cpu_values)

    result["rss_kb"] = metric_stats([_number(row, "rss_kb") for row in window])
    result["pss_kb"] = metric_stats([_number(row, "pss_kb") for row in window])
    debug_npu = [_number(row, "npu_debug_load_pct", "npu_load_pct") for row in window]
    result["npu_load_pct"] = metric_stats(debug_npu)
    result["npu_frequency_hz"] = metric_stats([
        _number(row, "npu_freq_hz", "npu_frequency_hz") for row in window
    ])
    temperatures = []
    for row in window:
        value = _number(row, "temperature_c")
        if value is None:
            millic = _number(row, "soc_temp_millic")
            value = millic / 1000.0 if millic is not None else None
        temperatures.append(value)
    result["temperature_c"] = metric_stats(temperatures)
    result["queue_depth"] = metric_stats([_number(row, "infer_queue_depth", "queue_depth")
                                            for row in window])

    timestamps = [_number(row, "monotonic_s") for row in window]
    gaps = [timestamps[index] - timestamps[index - 1]
            for index in range(1, len(timestamps)) if timestamps[index] >= timestamps[index - 1]]
    result["max_sample_gap_sec"] = max(gaps) if gaps else UNAVAILABLE

    thread_peaks = {}
    for row in window:
        encoded = row.get("thread_cpu_pct_json", "")
        if not encoded or encoded == UNAVAILABLE:
            continue
        try:
            values = json.loads(encoded)
        except ValueError:
            continue
        for thread_id, value in values.items():
            numeric = as_float(value)
            if numeric is not None:
                thread_peaks[thread_id] = max(thread_peaks.get(thread_id, 0.0), numeric)
    result["thread_cpu_peak_pct"] = thread_peaks

    first, last = window[0], window[-1]
    for label, field in (("rx", "eth_rx_dropped"), ("tx", "eth_tx_dropped")):
        initial, final = _number(first, field), _number(last, field)
        if initial is not None and final is not None and final >= initial:
            result["network_drop_delta"][label] = final - initial
    return result


def _evaluate_thresholds(summary, thresholds):
    failures = []
    checks = (
        ("min_capture_fps", summary["fps"]["capture"], lambda value, limit: value >= limit),
        ("min_output_fps", summary["fps"]["output"], lambda value, limit: value >= limit),
        ("max_video_drop_pct", summary["drops"]["video_drop_pct"], lambda value, limit: value <= limit),
        ("max_inference_queue", summary["inference"]["max_queue_depth"], lambda value, limit: value <= limit),
        ("max_temperature_c", summary["system"]["temperature_c"]["max"], lambda value, limit: value <= limit),
        ("max_rss_mb", summary["system"]["rss_kb"]["max"], lambda value, limit: value <= limit * 1024.0),
    )
    for name, value, predicate in checks:
        if name not in thresholds:
            continue
        if value == UNAVAILABLE:
            failures.append("threshold %s cannot be evaluated" % name)
        elif not predicate(float(value), float(thresholds[name])):
            failures.append("threshold %s failed: observed=%s limit=%s" %
                            (name, value, thresholds[name]))
    return failures


def analyze_run(run_dir):
    run_dir = os.path.abspath(run_dir)
    metadata_path = os.path.join(run_dir, "metadata.json")
    metadata = load_json(metadata_path) if os.path.isfile(metadata_path) else {}
    frame_path = os.path.join(run_dir, "frame_metrics.csv")
    if not os.path.isfile(frame_path):
        frame_path = os.path.join(run_dir, "raw.csv")
    system_path = os.path.join(run_dir, "system_metrics.csv")
    if not os.path.isfile(system_path):
        system_path = os.path.join(run_dir, "system.csv")

    errors, warnings = [], []
    for path, label in ((frame_path, "frame metrics"), (system_path, "system metrics")):
        if not os.path.isfile(path):
            errors.append("missing %s: %s" % (label, os.path.basename(path)))
    if errors:
        return {
            "schema_version": 1,
            "status": "FAIL",
            "validation": {"passed": False, "errors": errors, "warnings": warnings},
            "source_files": {"metadata": metadata_path, "frame_metrics": frame_path,
                             "system_metrics": system_path},
        }

    try:
        start_us, end_us, requested_duration = _measurement_window(run_dir, metadata)
    except ValueError as exc:
        return {
            "schema_version": 1,
            "status": "FAIL",
            "validation": {"passed": False, "errors": [str(exc)], "warnings": warnings},
            "source_files": {"metadata": metadata_path, "frame_metrics": frame_path,
                             "system_metrics": system_path},
        }
    duration = (end_us - start_us) / 1000000.0
    trace_enabled = bool(metadata.get("scenario", {}).get("detailed_trace", True))
    rows = _read_csv(frame_path)
    window = _within(rows, start_us, end_us)
    capture = [row for row in window if row.get("event") == "capture"]
    processed = [row for row in window if row.get("event") == "processed"]
    inference = [row for row in window if row.get("event") == "inference"]
    drops = [row for row in window if row.get("event") == "drop"]

    capture_ts = [_number(row, "capture_ts_us") for row in capture]
    capture_ts = [value for value in capture_ts if value is not None]
    output_ts = [_number(row, "encode_output_us") for row in processed]
    output_ts = [value for value in output_ts if value is not None]

    all_capture_map = {}
    for row in rows:
        if row.get("event") != "capture":
            continue
        frame_id = _integer(row, "frame_id")
        timestamp = _number(row, "capture_ts_us")
        if frame_id is not None and timestamp is not None and frame_id not in all_capture_map:
            all_capture_map[frame_id] = timestamp
    frame_capture_map = {}
    capture_ids = []
    duplicate_capture_ids = 0
    for row in capture:
        frame_id = _integer(row, "frame_id")
        timestamp = _number(row, "capture_ts_us")
        if frame_id is None:
            continue
        capture_ids.append(frame_id)
        if frame_id in frame_capture_map:
            duplicate_capture_ids += 1
        elif timestamp is not None:
            frame_capture_map[frame_id] = timestamp
    frame_id_jumps = sum(1 for index in range(1, len(capture_ids))
                         if capture_ids[index] != capture_ids[index - 1] + 1)
    processed_association_failures = sum(
        1 for row in processed if _integer(row, "frame_id") not in all_capture_map
    )

    box_age = []
    detection_association_failures = 0
    for row in processed:
        source_id = _integer(row, "detection_source_frame_id")
        endpoint = _number(row, "overlay_end_us", "encode_output_us")
        if not source_id:
            continue
        source_capture = all_capture_map.get(source_id)
        if source_capture is None or endpoint is None:
            detection_association_failures += 1
        elif endpoint >= source_capture:
            box_age.append((endpoint - source_capture) / 1000.0)

    triggered = []
    skipped = []
    skip_reasons = Counter()
    result_reuse = 0
    previous_detection_source = 0
    for row in processed:
        trigger_value = _integer(row, "triggered")
        if trigger_value is None:
            trigger_value = _integer(row, "infer_triggered") or 0
        skipped_value = _integer(row, "skipped") or 0
        if trigger_value:
            triggered.append(row)
        if skipped_value:
            skipped.append(row)
            skip_reasons[row.get("skip_reason") or "unspecified"] += 1
        source_id = _integer(row, "detection_source_frame_id") or 0
        if source_id and source_id == previous_detection_source:
            result_reuse += 1
        if source_id:
            previous_detection_source = source_id

    inference_queue_rows = [row for row in window if row.get("event") in ("processed", "inference", "inference_skip")]
    all_queue_depths = [_number(row, "infer_queue_depth", "queue_depth") for row in inference_queue_rows]
    all_queue_depths = [value for value in all_queue_depths if value is not None]
    queue_stop_detected = any((row.get("skip_reason") or "") in
                              ("queue_limit_stop", "queue_limit_reached") for row in window)

    client_progress = _parse_ffmpeg_progress(os.path.join(run_dir, "ffmpeg_progress.txt"))
    client_frames = as_int(client_progress.get("frame"))
    client_pts_seconds = as_float(client_progress.get("out_time_ms"))
    if client_pts_seconds is not None:
        client_pts_seconds /= 1000000.0

    summary = {
        "schema_version": 1,
        "scenario": metadata.get("scenario", {}).get("scenario", metadata.get("scenario", UNAVAILABLE)),
        "run": metadata.get("run", UNAVAILABLE),
        "measurement": {
            "board_monotonic_start_s": start_us / 1000000.0,
            "board_monotonic_end_s": end_us / 1000000.0,
            "actual_duration_sec": duration,
            "requested_duration_sec": requested_duration if requested_duration is not None else UNAVAILABLE,
        },
        "fps": {
            "capture": (len(capture) / duration) if trace_enabled else UNAVAILABLE,
            "encode_input": (len(processed) / duration) if trace_enabled else UNAVAILABLE,
            "output": (len(processed) / duration) if trace_enabled else UNAVAILABLE,
            "capture_min_1s": (min_aligned_window_rate(capture_ts, start_us, end_us)
                               if trace_enabled else UNAVAILABLE),
            "output_min_1s": (min_aligned_window_rate(output_ts, start_us, end_us)
                              if trace_enabled else UNAVAILABLE),
            "client_window": (client_frames / duration) if client_frames is not None else UNAVAILABLE,
            "client_active": ((client_frames / client_pts_seconds)
                              if client_frames is not None and client_pts_seconds else UNAVAILABLE),
        },
        "detection": {
            "completed_results": len(inference),
            "update_fps": (len(inference) / duration) if trace_enabled else UNAVAILABLE,
            "reused_output_frames": result_reuse,
            "box_age_ms": metric_stats(box_age),
        },
        "latency_ms": {
            "rga_preprocess": metric_stats([
                _delta_ms(row, ("rga_pre_end_us",), ("rga_pre_start_us",)) for row in inference
            ]),
            "inference_queue_wait": metric_stats([
                _delta_ms(row, ("rga_pre_start_us",), ("infer_submit_us",)) for row in inference
            ]),
            "npu_run": metric_stats([
                _delta_ms(row, ("infer_end_us",), ("infer_start_us",)) for row in inference
            ]),
            "postprocess": metric_stats([
                _delta_ms(row, ("post_end_us",), ("infer_end_us",)) for row in inference
            ]),
            "overlay": metric_stats([
                _delta_ms(row, ("overlay_end_us",), ("overlay_start_us",)) for row in processed
            ]),
            "encode": metric_stats([
                _delta_ms(row, ("encode_output_us",), ("encode_submit_us",)) for row in processed
            ]),
            "capture_to_overlay": metric_stats([
                _delta_ms(row, ("overlay_end_us",), ("capture_ts_us",)) for row in processed
            ]),
            "capture_to_encode_output": metric_stats([
                _delta_ms(row, ("encode_output_us",), ("capture_ts_us",)) for row in processed
            ]),
        },
        "inference": {
            "trigger_attempts": len(triggered),
            "trigger_pct_of_output": (100.0 * len(triggered) / len(processed)) if processed else 0.0,
            "skipped_attempts": len(skipped),
            "skip_pct_of_triggers": (100.0 * len(skipped) / len(triggered)) if triggered else 0.0,
            "skip_reason_counts": dict(skip_reasons),
            "max_queue_depth": max(all_queue_depths) if all_queue_depths else UNAVAILABLE,
            "queue_limit_stop_detected": queue_stop_detected,
        },
        "drops": {
            "captured_frames": len(capture),
            "video_drop_events": len(drops),
            "video_drop_pct": (100.0 * len(drops) / len(capture)) if capture else UNAVAILABLE,
            "inference_skips_are_counted_separately": True,
        },
        "integrity": {
            "frame_timestamp_monotonic_violations": _monotonic_violations(capture, "capture_ts_us"),
            "inference_timestamp_monotonic_violations": _monotonic_violations(inference, "infer_end_us"),
            "stage_order_violations": _stage_order_violations(window),
            "duplicate_capture_frame_ids": duplicate_capture_ids,
            "capture_frame_id_jumps": frame_id_jumps,
            "processed_capture_association_failures": processed_association_failures,
            "detection_source_association_failures": detection_association_failures,
        },
        "client": {
            "frames": client_frames if client_frames is not None else UNAVAILABLE,
            "pts_seconds": client_pts_seconds if client_pts_seconds is not None else UNAVAILABLE,
            "ffmpeg_drop_frames": as_int(client_progress.get("drop_frames"))
                                   if as_int(client_progress.get("drop_frames")) is not None else UNAVAILABLE,
            "progress_end": client_progress.get("progress", UNAVAILABLE),
        },
        "formulas": {
            "capture_fps": "measurement-window capture rows / actual_duration_sec",
            "output_fps": "measurement-window processed rows / actual_duration_sec",
            "detection_update_fps": "completed inference rows / actual_duration_sec; reused boxes excluded",
            "video_drop_pct": "drop rows / capture rows * 100; inference skips excluded",
            "box_age_ms": "overlay_end of output frame - capture timestamp of detection_source_frame_id",
            "inference_queue_wait_ms": "rga_pre_start - infer_submit; excludes RGA preprocessing",
            "percentile": "nearest-rank: sorted_values[ceil(p*N)-1]",
            "process_cpu_pct": "delta(user+system ticks) / CLK_TCK / elapsed * 100; one core = 100%",
        },
        "source_files": {
            "metadata": os.path.basename(metadata_path),
            "frame_metrics": os.path.basename(frame_path),
            "system_metrics": os.path.basename(system_path),
        },
    }

    system_rows = _read_csv(system_path)
    summary["system"] = _analyze_system(system_rows, start_us, end_us, metadata, warnings)

    expected_queue_stop = bool(metadata.get("scenario", {}).get("expected_queue_stop", False))
    if trace_enabled and not capture:
        errors.append("no capture samples in measurement window")
    if trace_enabled and not processed:
        errors.append("no processed samples in measurement window")
    if not trace_enabled:
        warnings.append("detailed trace disabled; per-frame FPS and latency are Unavailable by design")
    if requested_duration is not None and duration + 0.5 < requested_duration:
        if not (expected_queue_stop and queue_stop_detected):
            errors.append("measurement duration shorter than requested")
    if expected_queue_stop and not queue_stop_detected:
        errors.append("expected queue-growth stop was not observed")
    for field, count in summary["integrity"].items():
        if count and field not in ("capture_frame_id_jumps",):
            errors.append("integrity check failed: %s=%s" % (field, count))
    if summary["integrity"]["capture_frame_id_jumps"]:
        warnings.append("capture frame ID jumps observed; inspect raw rows")

    if expected_queue_stop and queue_stop_detected:
        minimum_system_samples = 0
        if summary["system"]["samples"] == 0:
            warnings.append("expected queue abort was shorter than the 1 Hz collector period; formal-window system metrics are Unavailable")
    else:
        minimum_system_samples = max(2, int(duration * 0.8))
        if summary["system"]["samples"] < minimum_system_samples:
            errors.append("insufficient system samples: %s < %s" %
                          (summary["system"]["samples"], minimum_system_samples))
    gap = summary["system"]["max_sample_gap_sec"]
    if gap != UNAVAILABLE and gap > 2.5:
        errors.append("system collector gap exceeded 2.5s: %.3fs" % gap)

    thresholds = metadata.get("scenario", {}).get("thresholds", {})
    errors.extend(_evaluate_thresholds(summary, thresholds))
    status = "EXPECTED_ABORT" if expected_queue_stop and queue_stop_detected and not errors else (
        "PASS" if not errors else "FAIL"
    )
    summary["status"] = status
    summary["validation"] = {"passed": not errors, "errors": errors, "warnings": warnings}
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--output", help="default: RUN_DIR/summary.json")
    args = parser.parse_args(argv)
    output = args.output or os.path.join(args.run_dir, "summary.json")
    summary = analyze_run(args.run_dir)
    dump_json_atomic(output, summary)
    print("%s: %s" % (output, summary.get("status")))
    return 0 if summary.get("status") in ("PASS", "EXPECTED_ABORT") else 2


if __name__ == "__main__":
    sys.exit(main())
