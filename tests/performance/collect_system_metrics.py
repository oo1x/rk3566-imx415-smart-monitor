#!/usr/bin/env python3
"""Board-local 1 Hz process and RK3566 resource sampler."""
from __future__ import print_function

import argparse
import csv
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time


UNAVAILABLE = "Unavailable"
RUNNING = True


def _stop(_signum, _frame):
    global RUNNING
    RUNNING = False


def _read(path, default=UNAVAILABLE):
    try:
        with open(path, "r") as handle:
            return handle.read().strip()
    except (IOError, OSError):
        return default


def _number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _proc_ticks(path):
    text = _read(path, "")
    close = text.rfind(")")
    if close < 0:
        return None
    fields = text[close + 2:].split()
    if len(fields) < 13:
        return None
    try:
        return int(fields[11]) + int(fields[12])
    except ValueError:
        return None


def _status_value(pid, key):
    text = _read("/proc/%d/status" % pid, "")
    match = re.search(r"^%s:\s+([0-9]+)" % re.escape(key), text, re.MULTILINE)
    return match.group(1) if match else UNAVAILABLE


def _pss_kb(pid):
    text = _read("/proc/%d/smaps_rollup" % pid, "")
    match = re.search(r"^Pss:\s+([0-9]+)", text, re.MULTILINE)
    return match.group(1) if match else UNAVAILABLE


def _thread_ticks(pid):
    values = {}
    task_root = "/proc/%d/task" % pid
    try:
        tids = os.listdir(task_root)
    except OSError:
        return values
    for tid in tids:
        ticks = _proc_ticks(os.path.join(task_root, tid, "stat"))
        if ticks is not None:
            name = _read(os.path.join(task_root, tid, "comm"), "thread").replace(":", "_")
            values["%s:%s" % (tid, name)] = ticks
    return values


def _npu_debug_load():
    path = "/sys/kernel/debug/rknpu/load"
    text = _read(path, "")
    if not text:
        try:
            process = subprocess.Popen(
                ["sudo", "-n", "cat", path], stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, universal_newlines=True)
            text, _ = process.communicate(timeout=2)
            if process.returncode != 0:
                text = ""
        except Exception:
            text = ""
    match = re.search(r"(?:NPU\s+load[^0-9]*|:\s*)([0-9]+(?:\.[0-9]+)?)%", text, re.I)
    return match.group(1) if match else UNAVAILABLE


def _temperature_c():
    value = _number(_read("/sys/class/thermal/thermal_zone0/temp"))
    return value / 1000.0 if value is not None else UNAVAILABLE


def _network():
    text = _read("/proc/net/dev", "")
    preferred = None
    for line in text.splitlines():
        if ":" not in line:
            continue
        interface, payload = line.split(":", 1)
        interface = interface.strip()
        if interface == "lo":
            continue
        fields = payload.split()
        if len(fields) >= 16:
            value = (fields[1], fields[3], fields[9], fields[11])
            if interface == "eth0":
                return value
            if preferred is None:
                preferred = value
    return preferred or (UNAVAILABLE,) * 4


def _queue_status(path):
    values = {}
    text = _read(path, "") if path else ""
    for token in text.replace("\n", " ").split():
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return values


def _cpu_percent(current_ticks, previous_ticks, elapsed, tick_hz):
    if current_ticks is None or previous_ticks is None or elapsed <= 0:
        return UNAVAILABLE
    if current_ticks < previous_ticks:
        return UNAVAILABLE
    return (current_ticks - previous_ticks) / float(tick_hz) / elapsed * 100.0


def collect(args):
    fields = [
        "sample", "monotonic_s", "pid_alive", "process_cpu_pct",
        "thread_cpu_pct_json", "rss_kb", "pss_kb", "cpu_governor",
        "cpu_frequency_khz", "npu_governor", "npu_load_pct",
        "npu_frequency_hz", "temperature_c", "infer_queue_depth", "infer_busy",
        "infer_submitted", "infer_skipped", "infer_completed", "eth_rx_packets",
        "eth_rx_dropped", "eth_tx_packets", "eth_tx_dropped", "disk_free_bytes",
    ]
    tick_hz = os.sysconf("SC_CLK_TCK")
    started = time.monotonic()
    previous_time = None
    previous_process = None
    previous_threads = {}
    sample = 0
    status = "complete"
    exit_code = 0

    output_dir = os.path.dirname(os.path.abspath(args.output))
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    with open(args.output, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        handle.flush()
        while RUNNING:
            now = time.monotonic()
            alive = os.path.isdir("/proc/%d" % args.pid)
            if not alive:
                status = "process_exited"
                break
            process_ticks = _proc_ticks("/proc/%d/stat" % args.pid)
            threads = _thread_ticks(args.pid)
            elapsed = (now - previous_time) if previous_time is not None else 0.0
            thread_cpu = {}
            for key, ticks in threads.items():
                value = _cpu_percent(ticks, previous_threads.get(key), elapsed, tick_hz)
                if value != UNAVAILABLE:
                    thread_cpu[key] = value
            queue = _queue_status(args.queue_status)
            rx_packets, rx_dropped, tx_packets, tx_dropped = _network()
            free_bytes = shutil.disk_usage(output_dir).free
            row = {
                "sample": sample,
                "monotonic_s": "%.6f" % now,
                "pid_alive": 1,
                "process_cpu_pct": _cpu_percent(process_ticks, previous_process, elapsed, tick_hz),
                "thread_cpu_pct_json": json.dumps(thread_cpu, separators=(",", ":"), sort_keys=True),
                "rss_kb": _status_value(args.pid, "VmRSS"),
                "pss_kb": _pss_kb(args.pid),
                "cpu_governor": _read("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"),
                "cpu_frequency_khz": _read("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq"),
                "npu_governor": _read("/sys/class/devfreq/fde40000.npu/governor"),
                "npu_load_pct": _npu_debug_load(),
                "npu_frequency_hz": _read("/sys/class/devfreq/fde40000.npu/cur_freq"),
                "temperature_c": _temperature_c(),
                "infer_queue_depth": queue.get("infer_queue_depth", UNAVAILABLE),
                "infer_busy": queue.get("infer_busy", UNAVAILABLE),
                "infer_submitted": queue.get("infer_submitted", UNAVAILABLE),
                "infer_skipped": queue.get("infer_skipped", UNAVAILABLE),
                "infer_completed": queue.get("infer_completed", UNAVAILABLE),
                "eth_rx_packets": rx_packets,
                "eth_rx_dropped": rx_dropped,
                "eth_tx_packets": tx_packets,
                "eth_tx_dropped": tx_dropped,
                "disk_free_bytes": free_bytes,
            }
            writer.writerow(row)
            handle.flush()
            if free_bytes < args.min_free_bytes:
                status = "disk_below_limit"
                exit_code = 3
                break
            previous_time = now
            previous_process = process_ticks
            previous_threads = threads
            sample += 1
            if args.max_duration_sec and now - started >= args.max_duration_sec:
                status = "duration_reached"
                break
            deadline = now + args.interval
            while RUNNING and time.monotonic() < deadline:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                time.sleep(min(0.1, remaining))

    status_path = args.status or (args.output + ".status.json")
    with open(status_path, "w") as handle:
        json.dump({
            "status": status,
            "samples": sample,
            "pid": args.pid,
            "clock_ticks_per_second": tick_hz,
            "ended_monotonic_s": time.monotonic(),
        }, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return exit_code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--status")
    parser.add_argument("--queue-status")
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--max-duration-sec", type=float, default=0.0)
    parser.add_argument("--min-free-bytes", type=int, default=256 * 1024 * 1024)
    args = parser.parse_args(argv)
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    return collect(args)


if __name__ == "__main__":
    sys.exit(main())
