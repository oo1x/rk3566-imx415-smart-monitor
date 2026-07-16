from __future__ import print_function

import math

from .io_utils import UNAVAILABLE


def as_float(value):
    try:
        if value is None or value == "" or value == UNAVAILABLE:
            return None
        return float(value)
    except (TypeError, ValueError):
        return None


def as_int(value):
    number = as_float(value)
    return int(number) if number is not None else None


def nearest_rank(values, fraction):
    clean = sorted(float(value) for value in values if value is not None)
    if not clean:
        return UNAVAILABLE
    index = max(0, int(math.ceil(fraction * len(clean))) - 1)
    return clean[index]


def metric_stats(values):
    clean = [float(value) for value in values if value is not None]
    if not clean:
        return {
            "samples": 0,
            "mean": UNAVAILABLE,
            "p50": UNAVAILABLE,
            "p95": UNAVAILABLE,
            "p99": UNAVAILABLE,
            "max": UNAVAILABLE,
        }
    return {
        "samples": len(clean),
        "mean": sum(clean) / len(clean),
        "p50": nearest_rank(clean, 0.50),
        "p95": nearest_rank(clean, 0.95),
        "p99": nearest_rank(clean, 0.99),
        "max": max(clean),
    }


def min_aligned_window_rate(timestamps_us, start_us, end_us, window_sec=1):
    window_us = int(window_sec * 1000000)
    complete = int((end_us - start_us) // window_us)
    if complete <= 0:
        return UNAVAILABLE
    counts = [0] * complete
    for timestamp in timestamps_us:
        if start_us <= timestamp < start_us + complete * window_us:
            index = int((timestamp - start_us) // window_us)
            counts[index] += 1
    return min(counts) / float(window_sec)


def numeric_summary(values):
    clean = [float(value) for value in values if value != UNAVAILABLE and value is not None]
    if not clean:
        return {"mean": UNAVAILABLE, "min": UNAVAILABLE, "max": UNAVAILABLE}
    return {"mean": sum(clean) / len(clean), "min": min(clean), "max": max(clean)}
