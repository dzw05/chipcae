# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Summarize CTest JUnit and audit required tests without executing CTest.

Readers raise ValueError or OSError on invalid input. Test names are exact keys;
JUnit duplicates use the last occurrence, and report lists are sorted by name.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
from xml.etree import ElementTree as ET


def _enabled(raw):
    value = raw.strip().upper()
    return value not in {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"} and not value.endswith("-NOTFOUND")


def read_cache(path):
    """Read BOOL values and the source directory from one CMakeCache snapshot."""
    switches = {}
    source = None
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([^:#/][^:]*):BOOL=(.*)", line)
        if match:
            name, raw = match.groups()
            switches[name] = {"raw": raw, "enabled": _enabled(raw)}
        elif line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            source = line.partition("=")[2]
    return switches, source


def _reason(case, skipped):
    if skipped is not None:
        message = skipped.get("message")
        if message:
            return message[:500]
        if skipped.text:
            return skipped.text[:500]
    return (case.findtext("system-out") or "")[:500]


def read_junit(path):
    """Return normalized test records plus warnings, reading XML once."""
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as error:
        raise ValueError(f"JUnit {path}: {error}") from error
    if root.tag not in {"testsuite", "testsuites"}:
        raise ValueError(f"JUnit {path}: expected testsuite or testsuites root")

    tests, occurrences, warnings = {}, {}, []

    # Validate parentage before normalization so malformed nesting cannot hide a
    # testcase. Iteration below then preserves document order across all suites.
    for parent in root.iter():
        for case in parent:
            if ((case.tag == "testcase" and parent.tag != "testsuite")
                    or (case.tag == "testsuite" and parent.tag not in {"testsuite", "testsuites"})
                    or case.tag == "testsuites"):
                raise ValueError(f"JUnit {path}: invalid XML structure for {case.tag}")

    for case in root.iter("testcase"):
        name = case.get("name")
        if not name:
            raise ValueError(f"JUnit {path}: testcase missing name")
        raw_status = case.get("status", "").lower()
        if raw_status not in {"", "run", "fail", "notrun", "disabled"}:
            raise ValueError(f"JUnit {path}: {name}: unknown status {raw_status!r}")
        skipped = case.find("skipped")
        if raw_status == "disabled":
            status = "NotRun"
        elif skipped is not None:
            status = "Skipped"
        elif raw_status == "notrun":
            status = "NotRun"
        elif case.find("failure") is not None or raw_status == "fail":
            status = "Failed"
        elif raw_status == "run":
            status = "Passed"
        else:
            raise ValueError(f"JUnit {path}: {name}: missing status")
        record = {"name": name, "status": status, "reason": _reason(case, skipped)}
        if status == "Skipped":
            try:
                elapsed = float(case.get("time", ""))
                if not math.isfinite(elapsed) or elapsed < 0:
                    raise ValueError("not a finite nonnegative duration")
            except ValueError:
                elapsed = None
                warnings.append(f"{name}: missing or invalid skipped time {case.get('time')!r}")
            record["elapsed_seconds"] = elapsed
        tests[name] = record
        occurrences[name] = occurrences.get(name, 0) + 1
    for name, count in sorted(occurrences.items()):
        if count > 1:
            warnings.append(f"Duplicate test {name!r}: {count} occurrences; using final occurrence")
    return tests, warnings


def read_required(path):
    """Read three-column TSV; duplicate requirement names are input errors."""
    required = {}
    for number, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3:
            raise ValueError(f"Required TSV {path}: line {number}: expected exactly three tab-separated fields")
        name, requires, note = fields
        if not name or name in required:
            raise ValueError(f"Required TSV {path}: line {number}: empty or duplicate test name {name!r}")
        if requires != "-" and not all(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", switch)
                                       for switch in requires.split(",")):
            raise ValueError(f"Required TSV {path}: line {number}: invalid requires {requires!r}")
        required[name] = {"name": name, "requires": requires, "note": note}
    return required


def _commit(source, warnings):
    if not source:
        warnings.append("Cannot discover commit: CMAKE_HOME_DIRECTORY:INTERNAL missing from cache")
        return None
    try:
        result = subprocess.run(["git", "-C", source, "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True, timeout=10)
        commit = result.stdout.strip()
        if not re.fullmatch(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", commit):
            raise ValueError("git returned an invalid HEAD")
        return commit
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        detail = error.stderr.strip() if isinstance(error, subprocess.CalledProcessError) else str(error)
        warnings.append(f"Cannot discover source commit with git -C {source!r}: {detail}")
        return None


def evaluate(tests, switches, required, *, junit, source=None, warnings=()):
    """Evaluate requirements against normalized tests and the cache snapshot."""
    warnings = list(warnings)
    counts = dict.fromkeys(("Passed", "Failed", "Skipped", "NotRun"), 0)
    skipped, notrun, violations, inactive, missing = [], [], [], [], []
    for name, case in sorted(tests.items()):
        status = case["status"]
        counts[status] += 1
        if status in {"Skipped", "NotRun"}:
            entry = {"name": name, "reason": case["reason"]}
            if status == "Skipped":
                entry["elapsed_seconds"] = case["elapsed_seconds"]
            (skipped if status == "Skipped" else notrun).append(entry)
    for name, requirement in sorted(required.items()):
        gates = [] if requirement["requires"] == "-" else requirement["requires"].split(",")
        off = []
        for switch in gates:
            if switch not in switches:
                warnings.append(f"{name}: required switch {switch} missing from cache; treating as OFF")
            if not switches.get(switch, {}).get("enabled", False):
                off.append(switch)
        if name not in tests:
            missing.append(name)
        if off:
            inactive.append({**requirement, "off_switches": off})
        else:
            status = tests[name]["status"] if name in tests else "Missing"
            if status in {"Skipped", "NotRun", "Missing"}:
                violations.append({**requirement, "status": status})
    return {
        "commit": _commit(source, warnings), "junit": str(Path(junit).resolve()),
        "switches": {name: value for name, value in sorted(switches.items())
                     if name.startswith("CHIPCAE_ENABLE_")},
        "mpiexec": shutil.which("mpiexec"), "counts": counts,
        "skipped": skipped, "notrun": notrun, "required_violations": violations,
        "required_inactive": inactive, "missing_required": missing,
        "warnings": warnings, "data_class": "synthetic",
        "overall": "VIOLATION" if violations else "OK",
    }


def _cell(value):
    return str(value).replace("&", "&amp;").replace("<", "&lt;").replace(
        ">", "&gt;").replace("\\", "\\\\").replace("|", "\\|").replace(
        "\r\n", "<br>").replace("\r", "<br>").replace("\n", "<br>")


def render_markdown(report):
    """Render every report field, escaping table cells only for display."""
    lines = [f"{report['overall']} — CTest skip audit", ""]

    def table(title, headers, rows):
        lines.extend([f"## {title}", "", "| " + " | ".join(headers) + " |",
                      "| " + " | ".join("---" for _ in headers) + " |"])
        lines.extend("| " + " | ".join(_cell(value) for value in row) + " |" for row in rows)
        lines.append("")

    table("Required violations", ("Name", "Requires", "Status", "Note"),
          ((r["name"], r["requires"], r["status"], r["note"]) for r in report["required_violations"]))
    table("Skipped", ("Name", "Reason", "Elapsed seconds"),
          ((r["name"], r["reason"], r["elapsed_seconds"]) for r in report["skipped"]))
    table("NotRun", ("Name", "Reason"), ((r["name"], r["reason"]) for r in report["notrun"]))
    table("Counts", ("Status", "Count"), report["counts"].items())
    table("Provenance", ("Field", "Value"), ((key, report[key]) for key in
          ("commit", "junit", "mpiexec", "data_class")))
    lines.extend(["Commit is the current HEAD of the cache-selected source checkout; "
                  "it may differ from the original build revision.", ""])
    table("Warnings", ("Warning",), ((warning,) for warning in report["warnings"]))
    table("Required inactive", ("Name", "Requires", "OFF/missing switches", "Note"),
          ((r["name"], r["requires"], ",".join(r["off_switches"]), r["note"])
           for r in report["required_inactive"]))
    table("Missing required", ("Name",), ((name,) for name in report["missing_required"]))
    table("Switch snapshot", ("Switch", "Raw", "Enabled"),
          ((name, value["raw"], value["enabled"]) for name, value in report["switches"].items()))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--junit", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--required", type=Path)
    args = parser.parse_args(argv)
    try:
        tests, warnings = read_junit(args.junit)
        switches, source = read_cache(args.build_dir / "CMakeCache.txt")
        required = read_required(args.required) if args.required is not None else {}
        report = evaluate(tests, switches, required, junit=args.junit, source=source, warnings=warnings)
        markdown = render_markdown(report)
        encoded = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "skip-audit.json").write_text(encoded, encoding="utf-8")
        (args.out / "skip-audit.md").write_text(markdown, encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"skip-audit: {error}", file=sys.stderr)
        return 2
    return 1 if report["required_violations"] else 0


if __name__ == "__main__":
    sys.exit(main())
