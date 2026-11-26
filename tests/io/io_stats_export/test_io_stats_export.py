"""I20 real-writer, byte-preservation and benchmark environment acceptance tests.

For the separate origin/main comparison, pass --baseline-tool pointing to an
origin/main iowt_write_timing_tests built with the same Release/HDF5 settings.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time


FIXTURE = Path(__file__).resolve().parent / "fixture"
DIGESTS = {
    "example_np2.json": "038f7970ae8d3f2f2bc2a78eaf88dc13ef7551ea11bb7d0a9b7ee7174594915f",
    "check.py": "ddfc51ab349dd304cd716dba5b620dc44aa384accce2a86c9620423008e538a8",
}
TIME_KEYS = ("write_step_seconds", "finalize_seconds", "last_write_step_seconds")
INPUT_KEYS = {"ranks", *TIME_KEYS, "write_step_count", "bytes_written", "peak_rss_bytes"}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def frozen_fixtures():
    for name, expected in DIGESTS.items():
        actual = hashlib.sha256((FIXTURE / name).read_bytes()).hexdigest()
        require(actual == expected, f"frozen fixture SHA-256 mismatch: {name}: {actual}")


def run(command):
    completed = subprocess.run([str(arg) for arg in command], text=True, capture_output=True)
    require(
        completed.returncode == 0,
        f"command failed ({completed.returncode}): {command}\n"
        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
    )
    return completed.stdout


def check_file(path):
    output = run([sys.executable, FIXTURE / "check.py", path])
    require("OK" in output, f"frozen checker did not accept {path}: {output}")


def generate(tool, stackup, directory, helper=True):
    command = [tool, "generate", stackup, directory]
    if not helper:
        command.append("--no-helper")
    output = run(command)
    snapshots = [json.loads(line) for line in output.splitlines() if line.startswith("{")]
    require(len(snapshots) == 1, f"expected one helper-input snapshot: {output}")
    require(set(snapshots[0]) == INPUT_KEYS, "captured helper inputs must contain all seven fields")
    return snapshots[0]


def validate_generated(directory, inputs):
    file = directory / "io_stats.json"
    check_file(file)
    exported = json.loads(file.read_bytes())
    require(set(exported) == {"schema", *INPUT_KEYS}, "I20 must export exactly eight keys")
    require(exported["schema"] == "chipcae.io_stats/1.0", "unexpected schema")
    for key in INPUT_KEYS:
        if key in TIME_KEYS:
            require(
                struct.pack("!d", exported[key]) == struct.pack("!d", inputs[key]),
                f"{key} changed double bits between helper input and JSON",
            )
        else:
            require(type(exported[key]) is int, f"{key} must be an integer token")
            require(exported[key] == inputs[key], f"{key} differs from helper input")
    steps = sorted((directory / "out").glob("step-*.vtkhdf"))
    require([p.name for p in steps] == ["step-0.vtkhdf", "step-1.vtkhdf"], "expected two real steps")
    size = (directory / "manifest.json").stat().st_size + sum(p.stat().st_size for p in steps)
    require(exported["bytes_written"] == size, "bytes must equal manifest + step sizes exactly")
    require(exported["write_step_count"] == 2, "real writer must report two steps")
    require(exported["ranks"] == 1, "serial export must have ranks=1")
    require(not (directory / "io_stats.json.tmp").exists(), "temporary export remains")


def fixture_test(tool, stackup):
    require(run([sys.executable, FIXTURE / "check.py"]).strip() == "OK", "checker self-test failed")
    with tempfile.TemporaryDirectory(prefix="iose-fixture-") as temporary:
        directory = Path(temporary) / "run"
        inputs = generate(tool, stackup, directory)
        validate_generated(directory, inputs)


def inventory(directory):
    return {p.relative_to(directory).as_posix() for p in directory.rglob("*") if p.is_file()}


def compare_bytes(directories):
    files = ["manifest.json", "out/step-0.vtkhdf", "out/step-1.vtkhdf"]
    for name in files:
        digests = [hashlib.sha256((p / name).read_bytes()).hexdigest() for p in directories]
        require(len(set(digests)) == 1, f"whole-file SHA-256 differs for {name}: {digests}")
    inventories = [inventory(p) - {"io_stats.json"} for p in directories]
    require(all(files == inventories[0] for files in inventories), f"inventories changed: {inventories}")
    require(
        inventories[0] == {"stackup.json", "manifest.json", "out/step-0.vtkhdf", "out/step-1.vtkhdf"},
        f"unexpected generated file inventory: {inventories[0]}",
    )


def writer_bytes_test(tool, stackup, baseline_tool=None):
    with tempfile.TemporaryDirectory(prefix="iose-bytes-") as temporary:
        root = Path(temporary)
        first, second, disabled = (root / name for name in ("first", "second", "disabled"))
        inputs = generate(tool, stackup, first)
        validate_generated(first, inputs)
        # Deliberately cross HDF5's historical whole-second timestamp boundary.
        boundary = int(time.time())
        while int(time.time()) == boundary:
            time.sleep(0.01)
        time.sleep(0.05)
        inputs = generate(tool, stackup, second)
        require(int(time.time()) > boundary, "generation must cross a whole-second boundary")
        validate_generated(second, inputs)
        generate(tool, stackup, disabled, helper=False)
        require(not (disabled / "io_stats.json").exists(), "disabled helper must not export")
        require(not (disabled / "io_stats.json.tmp").exists(), "disabled helper left tmp")
        directories = [first, second, disabled]
        if baseline_tool:
            baseline = root / "origin-main"
            run([baseline_tool, "generate", stackup, baseline])
            directories.append(baseline)
        compare_bytes(directories)


def bench_environment():
    """No /proc dependency: use host tools on Linux and macOS, outside timed work."""
    if sys.platform == "darwin":
        output = run(["vm_stat"])
        page_size = int(re.search(r"page size of (\d+) bytes", output).group(1))
        pages = []
        for label in ("Pages free", "Pages inactive", "Pages speculative"):
            match = re.search(rf"{label}:\s+(\d+)", output)
            require(match is not None, f"vm_stat missing {label}")
            pages.append(int(match.group(1)))
        available = sum(pages) * page_size
    else:
        output = run(["free", "-b"])
        memory = next(line for line in output.splitlines() if line.startswith("Mem:"))
        available = int(memory.split()[-1])
    nproc = int(run(["nproc"]).strip()) if shutil.which("nproc") else os.cpu_count()
    load = os.getloadavg()[0]
    print(f"bench environment: available={available} bytes, load={load}, nproc={nproc}", file=sys.stderr)
    require(available >= 3_000_000_000, "measurement invalid: MemAvailable < 3 GB; rerun later")
    require(load <= 2 * nproc, "measurement invalid: load > 2*nproc; rerun later")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("fixture", "writer_bytes", "bench_environment"))
    parser.add_argument("--tool", type=Path)
    parser.add_argument("--stackup", type=Path)
    parser.add_argument("--baseline-tool", type=Path, default=os.environ.get("IOSE_BASELINE_TOOL"))
    args = parser.parse_args()
    frozen_fixtures()
    if args.mode == "bench_environment":
        bench_environment()
        return
    binary_dir = Path(os.environ["CHIPCAE_BINARY_DIR"]).resolve()
    require(binary_dir.is_dir(), "CHIPCAE_BINARY_DIR must name the configured build")
    require(args.tool is not None and args.tool.is_file(), "CMake-supplied iose_tool missing")
    require(args.tool.resolve().is_relative_to(binary_dir), "tool must belong to CHIPCAE_BINARY_DIR")
    require(args.stackup is not None and args.stackup.is_file(), "CMake-supplied stackup missing")
    if args.mode == "fixture":
        fixture_test(args.tool, args.stackup)
    else:
        writer_bytes_test(args.tool, args.stackup, args.baseline_tool)


if __name__ == "__main__":
    main()
