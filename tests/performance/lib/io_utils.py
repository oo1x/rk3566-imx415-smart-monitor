from __future__ import print_function

import json
import os
import tempfile
from datetime import datetime


UNAVAILABLE = "Unavailable"


def ensure_dir(path):
    if path and not os.path.isdir(path):
        os.makedirs(path)
    return path


def load_json(path):
    with open(path, "r") as handle:
        return json.load(handle)


def dump_json_atomic(path, value):
    directory = os.path.dirname(os.path.abspath(path))
    ensure_dir(directory)
    fd, temporary = tempfile.mkstemp(prefix=".tmp_", suffix=".json", dir=directory)
    try:
        with os.fdopen(fd, "w") as handle:
            json.dump(value, handle, ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def read_key_values(path):
    values = {}
    if not path or not os.path.isfile(path):
        return values
    with open(path, "r") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def local_timestamp():
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def iso_now():
    return datetime.now().astimezone().isoformat()
