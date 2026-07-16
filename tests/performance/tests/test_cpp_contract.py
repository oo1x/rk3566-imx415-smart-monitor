from __future__ import print_function

import os
import unittest


PERF_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT_DIR = os.path.dirname(os.path.dirname(PERF_DIR))
PROJECT_CPP_PATH = os.path.join(PROJECT_DIR, "yolo_integration", "imx415_yolo_rtsp.cpp")
BUNDLED_CPP_PATH = os.path.join(os.path.dirname(PERF_DIR), "source_evidence",
                                "imx415_yolo_rtsp.cpp")
CPP_PATH = PROJECT_CPP_PATH if os.path.exists(PROJECT_CPP_PATH) else BUNDLED_CPP_PATH


def argument_counts(text, function_name):
    counts = []
    marker = function_name + "("
    offset = 0
    while True:
        start = text.find(marker, offset)
        if start < 0:
            break
        index = start + len(marker)
        depth = 1
        commas = 0
        quoted = None
        escaped = False
        while index < len(text) and depth:
            char = text[index]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quoted:
                    quoted = None
            elif char in ("'", '"'):
                quoted = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            elif char == "," and depth == 1:
                commas += 1
            index += 1
        counts.append(commas + 1)
        offset = index
    return counts


class CppMetricsContractTest(unittest.TestCase):
    def test_metrics_rows_match_schema(self):
        with open(CPP_PATH, "r", encoding="utf-8", errors="replace") as handle:
            source = handle.read()
        self.assertEqual([18, 18, 18, 18, 18], argument_counts(source, "metrics_row"))
        self.assertIn("overlay_start_us,overlay_end_us", source)
        self.assertIn("triggered,skipped,skip_reason,infer_queue_depth", source)

    def test_old_single_pending_fields_are_gone(self):
        with open(CPP_PATH, "r", encoding="utf-8", errors="replace") as handle:
            source = handle.read()
        for old_name in ("has_pending", "pending_seq", "pending_capture_ts_us", "replaced_count"):
            self.assertNotIn(old_name, source)


if __name__ == "__main__":
    unittest.main()
