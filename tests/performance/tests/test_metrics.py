from __future__ import print_function

import csv
import json
import os
import shutil
import sys
import tempfile
import unittest

PERF_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PERF_DIR not in sys.path:
    sys.path.insert(0, PERF_DIR)

from analyze_metrics import analyze_run


FRAME_FIELDS = [
    "event", "frame_id", "capture_ts_us", "rga_pre_start_us", "rga_pre_end_us",
    "infer_submit_us", "infer_start_us", "infer_end_us", "post_end_us",
    "overlay_start_us", "overlay_end_us", "encode_submit_us", "encode_output_us",
    "detection_source_frame_id", "triggered", "skipped", "skip_reason",
    "infer_queue_depth",
]


def frame(event, frame_id, capture, **values):
    row = {field: 0 for field in FRAME_FIELDS}
    row.update({"event": event, "frame_id": frame_id, "capture_ts_us": capture,
                "skip_reason": ""})
    row.update(values)
    return row


class MetricsTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="rk3566_metrics_")

    def tearDown(self):
        shutil.rmtree(self.root)

    def write_fixture(self):
        metadata = {
            "scenario": {"scenario": "TEST", "expected_queue_stop": False, "thresholds": {}},
            "run": 1,
            "measurement": {
                "board_monotonic_start_s": 10.0,
                "board_monotonic_end_s": 12.0,
                "requested_duration_sec": 2.0,
            },
            "environment": {"clock_ticks_per_second": 100},
        }
        with open(os.path.join(self.root, "metadata.json"), "w") as handle:
            json.dump(metadata, handle)
        rows = [
            frame("capture", 1, 10100000),
            frame("processed", 1, 10100000, overlay_start_us=10101000,
                  overlay_end_us=10102000, encode_submit_us=10102000,
                  encode_output_us=10106000, detection_source_frame_id=1,
                  triggered=1, skipped=0, skip_reason="async_submit", infer_queue_depth=1),
            frame("inference", 1, 10100000, rga_pre_start_us=10112000,
                  rga_pre_end_us=10114000, infer_submit_us=10110000,
                  infer_start_us=10115000, infer_end_us=10125000,
                  post_end_us=10128000, detection_source_frame_id=1,
                  triggered=1, skipped=0, skip_reason="async_complete", infer_queue_depth=0),
            frame("capture", 2, 10600000),
            frame("drop", 2, 10600000, skip_reason="latest_frame_replaced"),
            frame("capture", 3, 11100000),
            frame("processed", 3, 11100000, overlay_start_us=11101000,
                  overlay_end_us=11103000, encode_submit_us=11103000,
                  encode_output_us=11108000, detection_source_frame_id=1,
                  triggered=1, skipped=1, skip_reason="npu_busy", infer_queue_depth=0),
            frame("capture", 4, 11600000),
            frame("processed", 4, 11600000, overlay_start_us=11601000,
                  overlay_end_us=11603000, encode_submit_us=11603000,
                  encode_output_us=11609000, detection_source_frame_id=1,
                  triggered=0, skipped=0, skip_reason="interval_not_due", infer_queue_depth=0),
        ]
        with open(os.path.join(self.root, "frame_metrics.csv"), "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=FRAME_FIELDS)
            writer.writeheader()
            writer.writerows(rows)
        system_fields = [
            "sample", "monotonic_s", "process_cpu_pct", "rss_kb", "pss_kb",
            "npu_load_pct", "npu_frequency_hz", "temperature_c", "infer_queue_depth",
            "eth_rx_dropped", "eth_tx_dropped",
        ]
        with open(os.path.join(self.root, "system_metrics.csv"), "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=system_fields)
            writer.writeheader()
            writer.writerow({"sample": 0, "monotonic_s": 10.1, "process_cpu_pct": 20,
                             "rss_kb": 1000, "pss_kb": 800, "npu_load_pct": 40,
                             "npu_frequency_hz": 600000000, "temperature_c": 50,
                             "infer_queue_depth": 1, "eth_rx_dropped": 0,
                             "eth_tx_dropped": 0})
            writer.writerow({"sample": 1, "monotonic_s": 11.1, "process_cpu_pct": 30,
                             "rss_kb": 1200, "pss_kb": 900, "npu_load_pct": 60,
                             "npu_frequency_hz": 600000000, "temperature_c": 52,
                             "infer_queue_depth": 0, "eth_rx_dropped": 0,
                             "eth_tx_dropped": 0})

    def test_formulas_and_separate_drop_types(self):
        self.write_fixture()
        summary = analyze_run(self.root)
        self.assertEqual("PASS", summary["status"])
        self.assertAlmostEqual(2.0, summary["fps"]["capture"])
        self.assertAlmostEqual(1.5, summary["fps"]["output"])
        self.assertAlmostEqual(0.5, summary["detection"]["update_fps"])
        self.assertAlmostEqual(25.0, summary["drops"]["video_drop_pct"])
        self.assertEqual(1, summary["inference"]["skipped_attempts"])
        self.assertAlmostEqual(50.0, summary["inference"]["skip_pct_of_triggers"])
        self.assertEqual({"npu_busy": 1}, summary["inference"]["skip_reason_counts"])
        self.assertAlmostEqual(2.0, summary["latency_ms"]["inference_queue_wait"]["p95"])
        self.assertAlmostEqual(10.0, summary["latency_ms"]["npu_run"]["p95"])
        self.assertAlmostEqual(50.0, summary["system"]["npu_load_pct"]["mean"])
        self.assertEqual(0, summary["integrity"]["stage_order_violations"])

    def test_missing_raw_file_cannot_pass(self):
        with open(os.path.join(self.root, "system_metrics.csv"), "w") as handle:
            handle.write("sample,monotonic_s\n")
        summary = analyze_run(self.root)
        self.assertEqual("FAIL", summary["status"])
        self.assertFalse(summary["validation"]["passed"])


if __name__ == "__main__":
    unittest.main()
