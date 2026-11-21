# SPDX-License-Identifier: Apache-2.0
"""Run the synthetic F-1130 acceptance manifest and retain JUnit evidence."""

import argparse
import json
import math
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


_NAMES = (
    "f1130_uncut_cli_e2e",
    "f1130_uncut_cli_records",
    "chain_ipc_uncut_chain",
    "odbcli_odbpp_cli_e2e",
)
_KEYS = (
    "CHIPCAE_ENABLE_ECAD",
    "CHIPCAE_ENABLE_MFEM",
    "CHIPCAE_ENABLE_MPI",
    "CHIPCAE_ENABLE_HDF5",
)
_REPO = Path(__file__).resolve().parents[2]


def _nonempty(value):
    if not value.strip():
        raise argparse.ArgumentTypeError("must not be empty")
    return value


def _ranks(value):
    tokens = value.split(",")
    if any(token not in ("1", "2") for token in tokens):
        raise argparse.ArgumentTypeError("ranks must be a nonempty comma-separated list of 1 and 2")
    return [int(token) for token in tokens]


def _arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=_nonempty)
    parser.add_argument("--out", required=True, type=_nonempty)
    parser.add_argument("--ranks", type=_ranks, default=[1, 2])
    parser.add_argument("--allow-missing-backend", action="store_true")
    return parser.parse_args(argv)


def _cmake_bool(value):
    if value is None:
        return False
    upper = value.upper()
    if upper in ("", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"):
        return False
    if upper.endswith("-NOTFOUND"):
        return False
    if upper in ("ON", "YES", "TRUE", "Y"):
        return True
    try:
        number = float(value)
        return math.isfinite(number) and number != 0
    except ValueError:
        return False


def _cache(build):
    values = dict.fromkeys(_KEYS)
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([^:#/][^:=]*):[^=]+=(.*)", line)
        if match and match[1] in values:
            values[match[1]] = match[2]
    return values


def _run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, check=False,
                          encoding="utf-8", errors="replace", **kwargs)


def _output(process):
    return "stdout:\n" + (process.stdout or "") + "\nstderr:\n" + (process.stderr or "")


def _discover(build, report):
    process = _run(["ctest", "--test-dir", str(build), "-N"])
    report["ctest_listing"] = _output(process)
    report["ctest_listing_returncode"] = process.returncode
    names = set(re.findall(r"^\s*Test\s+#\d+:\s+([^\r\n]+?)\s*$",
                           process.stdout or "", re.MULTILINE))
    errors = []
    if process.returncode:
        errors.append(f"CTest discovery returned {process.returncode}")
    errors.extend(f"CTest discovery missing exact test name: {name}"
                  for name in _NAMES if name not in names)
    return errors


def _diagnostic(element):
    # Attribute values and text remain literal, including multiline skip messages.
    attributes = "\n".join(f"{key}={value}" for key, value in element.attrib.items())
    return f"<{element.tag}>\n{attributes}\n{''.join(element.itertext())}"


def _classify(case):
    tags = {child.tag for child in case}
    if tags & {"failure", "error"}:
        status = "Failed"
    elif "skipped" in tags:
        status = "Skipped"
    elif case.get("status") != "run":
        status = "Not Run"
    else:
        status = "Passed"
    try:
        elapsed = float(case.get("time", ""))
        if not math.isfinite(elapsed) or elapsed < 0:
            elapsed = None
    except ValueError:
        elapsed = None
    details = ["testcase attributes:\n" + "\n".join(
        f"{key}={value}" for key, value in case.attrib.items())]
    details.extend(_diagnostic(child) for child in case)
    return {
        "name": case.get("name"), "status": status, "time_s": elapsed,
        "skip_or_fail_message": "\n\n".join(details), "junit_status": case.get("status"),
    }


def _junit(path, report):
    try:
        raw = path.read_text(encoding="utf-8")
        root = ET.fromstring(raw)
    except (OSError, UnicodeError, ET.ParseError) as exc:
        message = f"Cannot read JUnit {path}: {exc}"
        report["junit_error"] = message
        for entry in report["tests"]:
            entry["skip_or_fail_message"] = message
        return False
    report["junit_xml"] = raw
    cases = list(root.iter("testcase"))
    for index, name in enumerate(_NAMES):
        matches = [case for case in cases if case.get("name") == name]
        if len(matches) != 1:
            detail = f"Expected one JUnit result for {name}; found {len(matches)}"
            if matches:
                detail += "\n\n" + "\n\n".join(_diagnostic(case) for case in matches)
            report["tests"][index]["skip_or_fail_message"] = detail
        else:
            report["tests"][index] = _classify(matches[0])
    return True


def _cell(value):
    return str(value).replace("\\", "\\\\").replace("|", "\\|").replace(
        "\r", "&#13;").replace("\n", "<br>")


def _fence(value, language="text"):
    longest = max((len(match[0]) for match in re.finditer(r"`+", value)), default=0)
    fence = "`" * max(3, longest + 1)
    return f"{fence}{language}\n{value}\n{fence}\n"


def _markdown(report):
    lines = ["# F-1130 rehearsal", "", f"Overall: {report['overall']}", "",
             "| name | status | time_s | skip_or_fail_message |",
             "| --- | --- | --- | --- |"]
    for entry in report["tests"]:
        lines.append("| " + " | ".join(_cell(entry[key]) for key in
                     ("name", "status", "time_s", "skip_or_fail_message")) + " |")
    lines.extend(["", "## Command", "", _fence(shlex.join(report["ctest_command"])),
                  "## Discovery", "", _fence(report["ctest_listing"]),
                  "## Full diagnostics", ""])
    for entry in report["tests"]:
        lines.extend([f"### {entry['name']}", "", _fence(entry["skip_or_fail_message"])])
    # Including the complete report also keeps all provenance and optional evidence
    # identical between formats; the table above is always in manifest order.
    lines.extend(["## Complete report", "",
                  _fence(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False), "json")])
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    """Return 0 for PASS, 1 for unsuccessful execution, 2 for preflight errors."""
    try:
        args = _arguments(argv)
    except SystemExit as exc:
        return exc.code
    start = time.monotonic()
    report = {
        "commit": None, "build_dir": args.build_dir,
        "backends": dict.fromkeys(_KEYS), "mpiexec": None, "ranks": args.ranks,
        "allow_missing_backend": args.allow_missing_backend, "preflight_errors": [],
        "ctest_listing": "", "ctest_listing_returncode": None, "ctest_command": [],
        "ctest_returncode": None,
        "tests": [{"name": name, "status": "Not Run", "time_s": None,
                   "skip_or_fail_message": ""} for name in _NAMES],
        "wall_time_s": None, "data_class": "synthetic", "overall": "NOT_RUN",
    }
    out = None

    def finish(code):
        report["wall_time_s"] = time.monotonic() - start
        for error in report["preflight_errors"]:
            print(error, file=sys.stderr)
        try:
            if out is not None:
                (out / "rehearsal.json").write_text(
                    json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
                    encoding="utf-8")
                (out / "rehearsal.md").write_text(_markdown(report), encoding="utf-8")
        except OSError as exc:
            print(f"Cannot write rehearsal reports: {exc}", file=sys.stderr)
            return 2
        print(f"F-1130 rehearsal: {report['overall']}")
        return code

    try:
        out = Path(args.out).resolve()
        out.mkdir(parents=True, exist_ok=True)
        # Keep provenance even when cache or dependency checks stop the rehearsal.
        commit = _run(["git", "rev-parse", "HEAD"], cwd=_REPO)
        if commit.returncode or not commit.stdout.strip():
            raise OSError("Cannot obtain commit: " + _output(commit))
        report["commit"] = commit.stdout.strip()
        build = Path(args.build_dir).resolve()
        report["build_dir"] = str(build)
        if not build.is_dir():
            raise OSError(f"Build directory does not exist or is not a directory: {build}")
        report["backends"] = _cache(build)
    except (OSError, UnicodeError, RuntimeError) as exc:
        report["preflight_errors"].append(str(exc))
        return finish(2)

    backend_gaps = [f"{key} is missing or disabled (raw value: {value!r})"
                    for key, value in report["backends"].items() if not _cmake_bool(value)]
    report["mpiexec"] = shutil.which("mpiexec")
    gaps = backend_gaps + ([] if report["mpiexec"] else ["mpiexec not found on PATH"])
    report["preflight_errors"].extend(gaps)
    if gaps and not args.allow_missing_backend:
        return finish(2)
    if gaps:
        report["readiness_gaps"] = [{"status": "Skipped", "reason": gap} for gap in gaps]
    try:
        discovery_errors = _discover(build, report)
        if discovery_errors:
            report["preflight_errors"].extend(discovery_errors)
            return finish(2)
        junit = out / "junit.xml"
        junit.unlink(missing_ok=True)
    except OSError as exc:
        report["preflight_errors"].append(str(exc))
        return finish(2)

    command = ["ctest", "--test-dir", str(build), "-R", "^(" + "|".join(_NAMES) + ")$",
               "-j2", "--output-on-failure", "--output-junit", str(junit)]
    report["ctest_command"] = command
    try:
        process = _run(command)
        report["ctest_returncode"] = process.returncode
        report["ctest_output"] = _output(process)
    except OSError as exc:
        report["ctest_output"] = str(exc)
        return finish(1)
    if not _junit(junit, report):
        return finish(1)
    # These three registrations gate executable injection on all four flags.
    # MPI launcher absence is a global gap, not an invented per-test dependency.
    if backend_gaps:
        for entry in report["tests"]:
            if entry["name"] != "f1130_uncut_cli_records" and entry["status"] == "Passed":
                entry["status"] = "Skipped"
                entry["skip_or_fail_message"] += "\nReadiness blocked:\n" + "\n".join(backend_gaps)
    passed = (not gaps and report["ctest_returncode"] == 0
              and all(entry["status"] == "Passed" for entry in report["tests"]))
    report["overall"] = "PASS" if passed else "FAIL"
    return finish(0 if passed else 1)


if __name__ == "__main__":
    sys.exit(main())
