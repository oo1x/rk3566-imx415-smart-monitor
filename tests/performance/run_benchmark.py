#!/usr/bin/env python3
"""One-command S0-S5 benchmark orchestrator for the Ubuntu test VM."""
from __future__ import print_function

import argparse
import json
import os
import shlex
import shutil
import signal
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from analyze_metrics import analyze_run
from generate_report import render_batch, render_run
from lib.config import load_scenario
from lib.io_utils import UNAVAILABLE, dump_json_atomic, ensure_dir, iso_now, local_timestamp


class BenchmarkError(RuntimeError):
    pass


class Remote(object):
    def __init__(self, target):
        self.target = target
        self.ssh_base = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=6", target]
        self.scp_base = ["scp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=6"]

    def run(self, command, timeout=30, check=True):
        last = None
        for attempt in range(1, 4):
            try:
                process = subprocess.run(
                    self.ssh_base + [command], stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, universal_newlines=True, timeout=timeout)
            except subprocess.TimeoutExpired as exc:
                last = exc
                if attempt < 3:
                    time.sleep(2)
                    continue
                raise BenchmarkError("remote command timed out: %s" % command)
            if process.returncode != 255 or attempt == 3:
                if check and process.returncode != 0:
                    raise BenchmarkError("remote command failed rc=%d: %s" %
                                         (process.returncode, process.stderr.strip()))
                return process
            last = process
            time.sleep(2)
        raise BenchmarkError("remote command failed: %s" % last)

    def copy_to(self, local_path, remote_path):
        command = self.scp_base + [local_path, "%s:%s" % (self.target, remote_path)]
        self._copy(command, "copy to board")

    def copy_from(self, remote_path, local_path, required=True):
        command = self.scp_base + ["%s:%s" % (self.target, remote_path), local_path]
        try:
            self._copy(command, "copy from board")
            return True
        except BenchmarkError:
            if required:
                raise
            return False

    @staticmethod
    def _copy(command, label):
        for attempt in range(1, 4):
            try:
                process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         universal_newlines=True, timeout=45)
            except subprocess.TimeoutExpired:
                process = None
            if process is not None and process.returncode == 0:
                return
            if attempt < 3:
                time.sleep(2)
        detail = process.stderr.strip() if process is not None else "timeout"
        raise BenchmarkError("%s failed: %s" % (label, detail))


def _quote(value):
    return shlex.quote(str(value))


def _command_exists(name):
    return shutil.which(name) is not None


def _remote_float(remote, command):
    output = remote.run(command, timeout=15).stdout.strip().split()
    if not output:
        raise BenchmarkError("remote numeric command returned no value")
    return float(output[0])


def _parse_key_values(text):
    values = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def preflight(remote, testbed, scenario, stop_existing=False, set_governors=True):
    for command in ("ssh", "scp", "ffmpeg"):
        if not _command_exists(command):
            raise BenchmarkError("required guest command not found: %s" % command)
    free = shutil.disk_usage(os.getcwd()).free
    if free < int(testbed.get("minimum_local_free_bytes", 0)):
        raise BenchmarkError("guest disk free space is below configured minimum")

    root = testbed["remote_root"]
    binary_path = root.rstrip("/") + "/" + testbed["binary"]
    model = testbed["model"]
    device = testbed["device"]
    python = testbed.get("remote_python", "python3")
    existing = remote.run(
        "pgrep -af '^\\./imx415_yolo_rtsp' 2>/dev/null || true", check=True).stdout.strip()
    if existing and stop_existing:
        remote.run("pkill -TERM -f '^\\./imx415_yolo_rtsp' 2>/dev/null || true; sleep 3", check=True)
        existing = remote.run(
            "pgrep -af '^\\./imx415_yolo_rtsp' 2>/dev/null || true", check=True).stdout.strip()
    if existing:
        raise BenchmarkError("board already has an imx415_yolo_rtsp process: %s" % existing)

    required_checks = " && ".join([
        "test -x %s" % _quote(binary_path),
        "test -r %s" % _quote(model),
        "test -c %s" % _quote(device),
        "command -v %s >/dev/null" % _quote(python),
    ])
    check_command = required_checks + " || exit 11; " + " ; ".join([
        "printf 'binary_md5='; md5sum %s | awk '{print $1}'" % _quote(binary_path),
        "printf 'model_md5='; md5sum %s | awk '{print $1}'" % _quote(model),
        "printf 'clock_ticks_per_second='; getconf CLK_TCK",
        "printf 'remote_free_kb='; df -Pk %s | awk 'NR==2{print $4}'" % _quote(root),
        "printf 'cpu_governor='; cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null || echo Unavailable",
        "printf 'npu_governor='; cat /sys/class/devfreq/fde40000.npu/governor 2>/dev/null || echo Unavailable",
        "printf 'npu_frequency_hz='; cat /sys/class/devfreq/fde40000.npu/cur_freq 2>/dev/null || echo Unavailable",
        "printf 'npu_debug_readable='; sudo -n test -r /sys/kernel/debug/rknpu/load && echo yes || echo no",
    ])
    values = _parse_key_values(remote.run(check_command, timeout=45).stdout)
    expected_md5 = testbed.get("expected_binary_md5")
    if expected_md5 and expected_md5 != "TO_BE_FILLED_AFTER_BUILD" and values.get("binary_md5") != expected_md5:
        raise BenchmarkError("binary MD5 mismatch: board=%s expected=%s" %
                             (values.get("binary_md5"), expected_md5))
    if not expected_md5 or expected_md5 == "TO_BE_FILLED_AFTER_BUILD":
        raise BenchmarkError("scenarios.json expected_binary_md5 must be filled after the scripted binary is built")
    remote_free_kb = float(values.get("remote_free_kb", 0))
    if remote_free_kb * 1024 < int(testbed.get("minimum_remote_free_bytes", 0)):
        raise BenchmarkError("board disk free space is below configured minimum")

    if set_governors:
        setup = " ; ".join([
            "echo %s | sudo -n tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null" %
            _quote(testbed.get("cpu_governor", "performance")),
            "echo %s | sudo -n tee /sys/class/devfreq/fde40000.npu/governor >/dev/null" %
            _quote(testbed.get("npu_governor", "userspace")),
            "echo %s | sudo -n tee /sys/class/devfreq/fde40000.npu/userspace/set_freq >/dev/null" %
            _quote(testbed.get("npu_frequency_hz", 600000000)),
        ])
        remote.run(setup, timeout=20)
        values.update(_parse_key_values(remote.run(" ; ".join([
            "printf 'cpu_governor='; cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
            "printf 'npu_governor='; cat /sys/class/devfreq/fde40000.npu/governor",
            "printf 'npu_frequency_hz='; cat /sys/class/devfreq/fde40000.npu/cur_freq",
        ])).stdout))

    v4l2 = remote.run(
        "v4l2-ctl -d %s --get-fmt-video 2>&1; v4l2-ctl -d /dev/v4l-subdev3 --get-subdev-fps 2>&1" %
        _quote(device), timeout=20, check=False).stdout
    values["v4l2_configuration"] = v4l2.strip()
    values["guest_free_bytes"] = free
    values["binary_path"] = binary_path
    values["model_path"] = model
    return values


def _pid_alive(remote, pid):
    if not pid:
        return False
    return remote.run("kill -0 %d 2>/dev/null" % pid, timeout=10, check=False).returncode == 0


def _terminate_remote_pid(remote, pid, timeout_sec):
    if not pid:
        return
    remote.run("kill -TERM %d 2>/dev/null || true" % pid, timeout=10, check=False)
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if not _pid_alive(remote, pid):
            return
        time.sleep(0.5)
    remote.run("kill -KILL %d 2>/dev/null || true" % pid, timeout=10, check=False)


def _read_remote_pid(remote, path):
    process = remote.run("cat %s 2>/dev/null" % _quote(path), timeout=10, check=False)
    try:
        return int(process.stdout.strip())
    except ValueError:
        return None


def _write_measurement_window(path, start_s, end_s, requested, guest_start, guest_end):
    with open(path, "w") as handle:
        handle.write("board_monotonic_start_s=%.6f\n" % start_s)
        handle.write("board_monotonic_end_s=%.6f\n" % end_s)
        handle.write("guest_epoch_start_s=%.6f\n" % guest_start)
        handle.write("guest_epoch_end_s=%.6f\n" % guest_end)
        handle.write("requested_duration_s=%s\n" % requested)


def _read_queue_status(remote, queue_status):
    process = remote.run("cat %s 2>/dev/null || true" % _quote(queue_status),
                         timeout=10, check=False)
    return _parse_key_values(process.stdout.replace(" ", "\n"))


def _queue_depth(remote, queue_status):
    values = _read_queue_status(remote, queue_status)
    try:
        return int(values.get("infer_queue_depth"))
    except (TypeError, ValueError):
        return None


def _wait_warmup(remote, app_pid, collector_pid, seconds, expected_queue_stop):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not _pid_alive(remote, app_pid):
            raise BenchmarkError("application exited during warmup")
        if not _pid_alive(remote, collector_pid):
            raise BenchmarkError("system collector exited during warmup")
        if expected_queue_stop:
            # S1/S2 must defer AI until the formal measurement starts.
            pass
        time.sleep(min(2.0, max(0.0, deadline - time.monotonic())))


def _remote_paths(remote_prefix):
    return {
        "frame": remote_prefix + "_frame_metrics.csv",
        "system": remote_prefix + "_system_metrics.csv",
        "stdout": remote_prefix + "_stdout.log",
        "stderr": remote_prefix + "_stderr.log",
        "collector_stdout": remote_prefix + "_collector_stdout.log",
        "collector_stderr": remote_prefix + "_collector_stderr.log",
        "collector_status": remote_prefix + "_collector_status.json",
        "queue_status": remote_prefix + "_queue_status.txt",
        "app_pidfile": remote_prefix + "_app.pid",
        "collector_pidfile": remote_prefix + "_collector.pid",
        "ai_start_file": remote_prefix + "_ai_start",
    }


def _launch_run(remote, testbed, scenario, run_dir, remote_collector, remote_prefix):
    root = testbed["remote_root"]
    binary = testbed["binary"]
    paths = _remote_paths(remote_prefix)
    frame_remote, system_remote = paths["frame"], paths["system"]
    stdout_remote, stderr_remote = paths["stdout"], paths["stderr"]
    collector_stdout, collector_stderr = paths["collector_stdout"], paths["collector_stderr"]
    collector_status, queue_status = paths["collector_status"], paths["queue_status"]
    app_pidfile, collector_pidfile = paths["app_pidfile"], paths["collector_pidfile"]
    ai_start_file = paths["ai_start_file"]
    cleanup_list = " ".join(_quote(path) for path in paths.values())
    remote.run("rm -f %s" % cleanup_list, timeout=15)

    environment = {
        "LD_LIBRARY_PATH": ".",
        "PERF_METRICS_CSV": frame_remote,
        "PERF_QUEUE_STATUS_FILE": queue_status,
        "PERF_AI_ENABLED": "1" if scenario["ai_enabled"] else "0",
        "PERF_INFER_INTERVAL": str(scenario["trigger_interval"]),
        "PERF_SUBMIT_POLICY": scenario["submit_policy"],
        "PERF_REUSE_BOXES": "1" if scenario["reuse_boxes"] else "0",
        "PERF_DETAILED_TRACE": "1" if scenario["detailed_trace"] else "0",
        "PERF_INFER_QUEUE_LIMIT": str(scenario.get("infer_queue_limit", 8)),
    }
    if scenario.get("defer_ai_until_measurement"):
        environment["PERF_AI_START_FILE"] = ai_start_file
    env_command = " ".join("%s=%s" % (key, _quote(value)) for key, value in sorted(environment.items()))
    application = "cd %s && exec env %s ./%s %s %s" % (
        _quote(root), env_command, _quote(binary), _quote(testbed["device"]), _quote(testbed["model"]))
    launch = "nohup sh -c %s >%s 2>%s </dev/null & echo $! >%s" % (
        _quote(application), _quote(stdout_remote), _quote(stderr_remote), _quote(app_pidfile))
    remote.run(launch, timeout=15)
    app_pid = _read_remote_pid(remote, app_pidfile)
    if not app_pid:
        raise BenchmarkError("application PID was not created")
    deadline = time.monotonic() + int(testbed.get("startup_timeout_sec", 30))
    while time.monotonic() < deadline:
        if not _pid_alive(remote, app_pid):
            raise BenchmarkError("application exited before RTSP became ready")
        ready = remote.run("ss -ltn 2>/dev/null | grep -q ':8554 ' || nc -z 127.0.0.1 8554",
                           timeout=10, check=False)
        if ready.returncode == 0:
            break
        time.sleep(1)
    else:
        raise BenchmarkError("RTSP port did not become ready")

    max_duration = int(scenario["warmup_sec"]) + int(scenario["duration_sec"]) + 120
    collector_command = (
        "nohup %s %s --pid %d --output %s --status %s --queue-status %s "
        "--max-duration-sec %d --min-free-bytes %d >%s 2>%s </dev/null & echo $! >%s" % (
            _quote(testbed.get("remote_python", "python3")), _quote(remote_collector), app_pid,
            _quote(system_remote), _quote(collector_status), _quote(queue_status), max_duration,
            int(testbed.get("minimum_remote_free_bytes", 0)), _quote(collector_stdout),
            _quote(collector_stderr), _quote(collector_pidfile)))
    remote.run(collector_command, timeout=15)
    collector_pid = _read_remote_pid(remote, collector_pidfile)
    if not collector_pid:
        raise BenchmarkError("collector PID was not created")
    return app_pid, collector_pid, paths, environment


def _run_ffmpeg(remote, testbed, scenario, run_dir, app_pid, collector_pid, queue_status):
    progress_path = os.path.join(run_dir, "ffmpeg_progress.txt")
    stdout_path = os.path.join(run_dir, "ffmpeg_stdout.log")
    stderr_path = os.path.join(run_dir, "ffmpeg_stderr.log")
    command = [
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
        "-rtsp_transport", testbed.get("rtsp_transport", "tcp"),
        "-i", testbed["rtsp_url"], "-map", "0:v:0", "-an",
        "-progress", progress_path,
        "-f", "null", "-",
    ]
    queue_limit = int(scenario.get("infer_queue_limit", 8))
    expected_stop = bool(scenario.get("expected_queue_stop"))
    observed_stop = False
    with open(stdout_path, "w") as stdout_handle, open(stderr_path, "w") as stderr_handle:
        process = subprocess.Popen(command, stdout=stdout_handle, stderr=stderr_handle)
        started = time.monotonic()
        formal_deadline = started + float(scenario["duration_sec"])
        stopped_at_deadline = False
        while process.poll() is None:
            now = time.monotonic()
            if now >= formal_deadline:
                stopped_at_deadline = True
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                break
            time.sleep(0.1)
        elapsed = time.monotonic() - started
        if not stopped_at_deadline and elapsed + 0.5 < float(scenario["duration_sec"]):
            if expected_stop:
                depth = _queue_depth(remote, queue_status)
                observed_stop = depth is not None and depth >= queue_limit
                if not observed_stop and not _pid_alive(remote, app_pid):
                    observed_stop = True
            if not observed_stop:
                raise BenchmarkError("FFmpeg exited before the requested wall-clock duration")
        return process.returncode, observed_stop, elapsed


def execute_run(remote, testbed, scenario, batch_dir, run_number, preflight_values):
    run_dir = ensure_dir(os.path.join(batch_dir, "run%d" % run_number))
    safe_name = os.path.basename(batch_dir).replace("-", "_")
    remote_prefix = "/tmp/imx415_benchmark_%s_run%d" % (safe_name, run_number)
    remote_collector = "/tmp/imx415_collect_system_metrics.py"
    remote.copy_to(os.path.join(SCRIPT_DIR, "collect_system_metrics.py"), remote_collector)
    remote.run("chmod 755 %s" % _quote(remote_collector), timeout=10)

    metadata = {
        "schema_version": 1,
        "created_at": iso_now(),
        "scenario": scenario,
        "run": run_number,
        "testbed": testbed,
        "environment": dict(preflight_values),
        "state": "starting",
        "runner": {"script": "run_benchmark.py", "python": sys.version},
    }
    dump_json_atomic(os.path.join(run_dir, "metadata.json"), metadata)
    app_pid = collector_pid = None
    paths = _remote_paths(remote_prefix)
    failure = None
    interrupted = False
    try:
        app_pid, collector_pid, paths, environment = _launch_run(
            remote, testbed, scenario, run_dir, remote_collector, remote_prefix)
        metadata["environment_variables"] = environment
        metadata["processes"] = {"app_pid": app_pid, "collector_pid": collector_pid}
        metadata["state"] = "warmup"
        dump_json_atomic(os.path.join(run_dir, "metadata.json"), metadata)
        _wait_warmup(remote, app_pid, collector_pid, float(scenario["warmup_sec"]),
                     bool(scenario.get("expected_queue_stop")))
        if scenario.get("defer_ai_until_measurement"):
            activation = remote.run(
                "cut -d' ' -f1 /proc/uptime; touch %s" % _quote(paths["ai_start_file"]),
                timeout=15)
            activation_output = activation.stdout.strip().split()
            if not activation_output:
                raise BenchmarkError("AI activation did not return a board monotonic start")
            start_s = float(activation_output[0])
        else:
            start_s = _remote_float(remote, "cut -d' ' -f1 /proc/uptime")
        guest_start = time.time()
        metadata["state"] = "measuring"
        metadata["measurement"] = {
            "board_monotonic_start_s": start_s,
            "requested_duration_sec": scenario["duration_sec"],
            "guest_epoch_start_s": guest_start,
        }
        dump_json_atomic(os.path.join(run_dir, "metadata.json"), metadata)
        ffmpeg_rc, observed_stop, runner_elapsed = _run_ffmpeg(
            remote, testbed, scenario, run_dir, app_pid, collector_pid, paths["queue_status"])
        guest_end = time.time()
        queue_state = _read_queue_status(remote, paths["queue_status"])
        try:
            final_queue_depth = int(queue_state.get("infer_queue_depth"))
        except (TypeError, ValueError):
            final_queue_depth = None
        if (scenario.get("expected_queue_stop") and final_queue_depth is not None and
                final_queue_depth >= int(scenario.get("infer_queue_limit", 8))):
            observed_stop = True
        if scenario.get("expected_queue_stop") and observed_stop:
            stop_us = None
            try:
                stop_us = float(queue_state.get("monotonic_us"))
            except (TypeError, ValueError):
                pass
            if stop_us is not None and stop_us / 1000000.0 >= start_s:
                measured_elapsed = min(runner_elapsed, stop_us / 1000000.0 - start_s)
            else:
                measured_elapsed = runner_elapsed
        else:
            measured_elapsed = float(scenario["duration_sec"])
        end_s = start_s + measured_elapsed
        metadata["measurement"].update({
            "board_monotonic_end_s": end_s,
            "actual_duration_sec": measured_elapsed,
            "guest_epoch_end_s": guest_end,
            "ffmpeg_return_code": ffmpeg_rc,
            "queue_stop_observed_by_runner": observed_stop,
            "runner_elapsed_sec": runner_elapsed,
            "final_queue_status": queue_state,
        })
        _write_measurement_window(os.path.join(run_dir, "measurement_window.txt"),
                                  start_s, end_s, scenario["duration_sec"], guest_start, guest_end)
        metadata["state"] = "collecting"
    except KeyboardInterrupt:
        interrupted = True
        failure = "interrupted by user"
    except Exception as exc:
        failure = str(exc)
    finally:
        if app_pid is None:
            app_pid = _read_remote_pid(remote, paths["app_pidfile"])
        if collector_pid is None:
            collector_pid = _read_remote_pid(remote, paths["collector_pidfile"])
        _terminate_remote_pid(remote, app_pid, int(testbed.get("shutdown_timeout_sec", 10)))
        _terminate_remote_pid(remote, collector_pid, 3)
        if paths:
            have_measurement = "measurement" in metadata
            copies = (
                ("frame", "frame_metrics.csv", have_measurement),
                ("system", "system_metrics.csv", have_measurement),
                ("stdout", "stdout.log", False),
                ("stderr", "stderr.log", False),
                ("collector_stdout", "collector_stdout.log", False),
                ("collector_stderr", "collector_stderr.log", False),
                ("collector_status", "collector_status.json", False),
                ("queue_status", "queue_status.txt", False),
            )
            for key, name, required in copies:
                try:
                    remote.copy_from(paths[key], os.path.join(run_dir, name), required=required)
                except Exception as exc:
                    if required and failure is None:
                        failure = str(exc)
        metadata["state"] = "interrupted" if interrupted else ("failed" if failure else "complete")
        metadata["failure"] = failure
        metadata["ended_at"] = iso_now()
        dump_json_atomic(os.path.join(run_dir, "metadata.json"), metadata)

    summary = analyze_run(run_dir)
    if failure:
        summary.setdefault("validation", {}).setdefault("errors", []).append(failure)
        summary["validation"]["passed"] = False
        summary["status"] = "INTERRUPTED" if interrupted else "FAIL"
    dump_json_atomic(os.path.join(run_dir, "summary.json"), summary)
    render_run(run_dir)
    if interrupted:
        raise KeyboardInterrupt()
    if summary.get("status") not in ("PASS", "EXPECTED_ABORT"):
        raise BenchmarkError("run%d failed; evidence preserved in %s" % (run_number, run_dir))
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", required=True, choices=("S0", "S1", "S2", "S3", "S4", "S5"))
    parser.add_argument("--duration", type=int)
    parser.add_argument("--repeat", type=int)
    parser.add_argument("--warmup", type=int)
    parser.add_argument("--output", default=os.path.join(SCRIPT_DIR, "results"))
    parser.add_argument("--config", default=os.path.join(SCRIPT_DIR, "scenarios.json"))
    parser.add_argument("--stop-existing", action="store_true",
                        help="stop a pre-existing ./imx415_yolo_rtsp process before testing")
    parser.add_argument("--no-governor-setup", action="store_true")
    parser.add_argument("--preflight-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        testbed, scenario = load_scenario(
            args.config, args.scenario, duration=args.duration, repeat=args.repeat, warmup=args.warmup)
        output_root = ensure_dir(os.path.abspath(args.output))
        batch_dir = ensure_dir(os.path.join(output_root, "%s_%s" % (local_timestamp(), args.scenario)))
        remote = Remote(testbed["ssh_target"])
        values = preflight(remote, testbed, scenario, stop_existing=args.stop_existing,
                           set_governors=not args.no_governor_setup)
        with open(os.path.join(batch_dir, "preflight.json"), "w", encoding="utf-8") as handle:
            json.dump(values, handle, ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
        if args.preflight_only:
            print("preflight passed: %s" % batch_dir)
            return 0
        for run_number in range(1, int(scenario["repeat"]) + 1):
            print("[%s] run %d/%d" % (scenario["scenario"], run_number, scenario["repeat"]))
            execute_run(remote, testbed, scenario, batch_dir, run_number, values)
        render_batch(batch_dir)
        print("benchmark complete: %s" % batch_dir)
        return 0
    except KeyboardInterrupt:
        print("benchmark interrupted; partial evidence was preserved", file=sys.stderr)
        return 130
    except Exception as exc:
        print("benchmark failed: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
