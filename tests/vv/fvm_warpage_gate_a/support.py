# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Test-runner resource accounting; no numerical gate implementation lives here."""

import json
import math
from pathlib import Path
import re
import resource
import shlex
import subprocess
import sys
import time

GIB = 1024**3


def available_memory_bytes():
    if sys.platform.startswith("linux"):
        text = Path("/proc/meminfo").read_text(encoding="utf-8")
        match = re.search(r"^MemAvailable:\s+(\d+)\s+kB$", text, re.MULTILINE)
        assert match, "资源不足，不跑：MemAvailable unavailable"
        return int(match[1]) * 1024
    if sys.platform == "darwin":
        text = subprocess.check_output(["vm_stat"], text=True)
        page_size = int(subprocess.check_output(["sysctl", "-n", "hw.pagesize"], text=True))
        pages = []
        for name in ("free", "inactive", "speculative"):
            match = re.search(rf"^Pages {name}:\s+(\d+)\.$", text, re.MULTILINE)
            assert match, f"资源不足，不跑：vm_stat missing {name}"
            pages.append(int(match[1]))
        assert page_size > 0, "invalid page size"
        return sum(pages) * page_size
    raise AssertionError("平台不支持")


def memory_preflight():
    available = available_memory_bytes()
    print(f"MemAvailable_bytes={available} MemAvailable_MiB={available / 1024**2:.17g}",
          flush=True)
    assert available >= 4 * GIB, "资源不足，不跑"
    return available


def rss_mib(raw):
    assert math.isfinite(raw) and raw >= 0, "invalid peak RSS"
    if sys.platform.startswith("linux"):
        return raw / 1024
    if sys.platform == "darwin":
        return raw / 1024**2
    raise AssertionError("平台不支持")


def require_ordinary_resources(measurement, *, reference=False):
    assert math.isfinite(measurement["rss_mib"]), "nonfinite peak RSS"
    assert 0 <= measurement["rss_mib"] <= 3072, "资源超限：peak RSS >3072 MiB"
    assert math.isfinite(measurement["wall_s"]), "nonfinite wall time"
    assert measurement["wall_s"] >= 0, "invalid wall time"
    if reference:
        assert measurement["wall_s"] <= 600, "参考未收敛：wall_s >600"


def measured_solver(command, cwd, *, reference=False):
    """Use a fresh supervisor so RUSAGE_CHILDREN contains this launch alone."""
    command = [str(arg) for arg in command]
    output = Path(cwd) / "measurement.json"
    args = [sys.executable, str(Path(__file__).resolve()), str(output),
            "600" if reference else "0", *command]
    print("COMMAND " + shlex.join(command), flush=True)
    supervisor = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    print(supervisor.stdout, end="", flush=True)
    print(supervisor.stderr, end="", file=sys.stderr, flush=True)
    assert supervisor.returncode == 0, "measurement supervisor failed"
    measurement = json.loads(output.read_text(encoding="utf-8"))
    print(f"wall_s={measurement['wall_s']:.17g} peak_RSS_MiB={measurement['rss_mib']:.17g}",
          flush=True)
    require_ordinary_resources(measurement, reference=reference)
    assert measurement["returncode"] == 0, f"solver failure: {measurement}"
    return measurement


def _supervise(output, timeout, command):
    start = time.monotonic()
    try:
        result = subprocess.run(command, timeout=timeout or None)
        code = result.returncode
    except subprocess.TimeoutExpired:
        code = 124
    measurement = dict(command=command, returncode=code, wall_s=time.monotonic() - start,
                       rss_mib=rss_mib(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss))
    Path(output).write_text(json.dumps(measurement, allow_nan=False), encoding="utf-8")


if __name__ == "__main__":
    _supervise(sys.argv[1], int(sys.argv[2]), sys.argv[3:])
