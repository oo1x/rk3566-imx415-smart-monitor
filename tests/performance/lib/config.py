from __future__ import print_function

import copy

from .io_utils import load_json


VALID_POLICIES = ("disabled", "always_queue", "latest_if_idle")
REQUIRED_FIELDS = (
    "scenario",
    "duration_sec",
    "warmup_sec",
    "repeat",
    "ai_enabled",
    "trigger_interval",
    "submit_policy",
    "reuse_boxes",
    "detailed_trace",
    "thresholds",
)


def _merge(base, override):
    result = copy.deepcopy(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = _merge(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


def load_scenario(path, scenario_name, duration=None, repeat=None, warmup=None):
    document = load_json(path)
    selected = None
    for item in document.get("scenarios", []):
        if item.get("scenario") == scenario_name:
            selected = item
            break
    if selected is None:
        raise ValueError("unknown scenario: %s" % scenario_name)

    scenario = _merge(document.get("defaults", {}), selected)
    if selected.get("inherit_default_thresholds") is False:
        scenario["thresholds"] = copy.deepcopy(selected.get("thresholds", {}))
    if duration is not None:
        scenario["duration_sec"] = duration
    if repeat is not None:
        scenario["repeat"] = repeat
    if warmup is not None:
        scenario["warmup_sec"] = warmup
    validate_scenario(scenario)
    return document.get("testbed", {}), scenario


def validate_scenario(scenario):
    missing = [field for field in REQUIRED_FIELDS if field not in scenario]
    if missing:
        raise ValueError("scenario missing fields: %s" % ", ".join(missing))
    if scenario["submit_policy"] not in VALID_POLICIES:
        raise ValueError("unsupported submit_policy: %s" % scenario["submit_policy"])
    for field in ("duration_sec", "repeat"):
        if int(scenario[field]) <= 0:
            raise ValueError("%s must be positive" % field)
    if int(scenario["warmup_sec"]) < 0:
        raise ValueError("warmup_sec must not be negative")
    if scenario["ai_enabled"] and int(scenario["trigger_interval"]) <= 0:
        raise ValueError("AI scenario trigger_interval must be positive")
    if not scenario["ai_enabled"] and scenario["submit_policy"] != "disabled":
        raise ValueError("AI-disabled scenario must use disabled policy")
