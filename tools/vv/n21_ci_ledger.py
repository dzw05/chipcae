# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Evaluate N21 required tests across CI jobs using normalized CTest JUnit.

Only Passed satisfies a requirement. JUnit parsing, duplicate handling and
status normalization are inherited from ctest_skip_audit.read_junit.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

from tools.vv.ctest_skip_audit import _cell, read_junit


_JOB_LABEL = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:/-]*")
_STATUSES = ("Passed", "Failed", "Skipped", "NotRun")
_PRIORITY = ("Failed", "Passed", "Skipped", "NotRun", "JobMissing", "Missing")


def read_required_jobs(path):
    """Read exact test-name keys, rejecting duplicate names and invalid rows."""
    required = {}
    for number, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        diagnostic = f"Required TSV {path}: line {number}: "
        if len(fields) != 3:
            raise ValueError(diagnostic + "expected exactly three tab-separated fields")
        name, job, note = fields
        if not name or name in required:
            raise ValueError(diagnostic + f"empty or duplicate test name {name!r}")
        if job != "*" and not _JOB_LABEL.fullmatch(job):
            raise ValueError(diagnostic + f"invalid job label {job!r}")
        required[name] = {"name": name, "job": job, "note": note}
    if not required:
        raise ValueError(f"Required TSV {path}: no required tests")
    return required


def evaluate(jobs, required):
    """Summarize normalized jobs and evaluate requirements in exact name order.

    Specific jobs never borrow evidence from another label. Wildcards retain
    every supplied job, including jobs missing the test. Before sorting, work is
    linear in normalized tests plus the number of candidate requirement/job pairs.
    """
    if not required:
        raise ValueError("At least one required test is needed")
    labels = sorted(jobs)
    per_job = {}
    for label in labels:
        counts = dict.fromkeys(_STATUSES, 0)
        skipped, notrun = [], []
        for name, case in jobs[label].items():
            status = case["status"]
            counts[status] += 1
            if status in {"Skipped", "NotRun"}:
                entry = {"name": name, "reason": case["reason"]}
                (skipped if status == "Skipped" else notrun).append(entry)
        per_job[label] = {
            "counts": counts,
            "skipped": sorted(skipped, key=lambda entry: entry["name"]),
            "notrun": sorted(notrun, key=lambda entry: entry["name"]),
        }

    results = []
    for name, requirement in sorted(required.items()):
        candidates = labels if requirement["job"] == "*" else [requirement["job"]]
        evidence = []
        for label in candidates:
            if label not in jobs:
                status, reason = "JobMissing", "Job not supplied"
            elif name not in jobs[label]:
                status, reason = "Missing", "Test not present in job JUnit"
            else:
                case = jobs[label][name]
                status, reason = case["status"], case["reason"]
            evidence.append({"job": label, "status": status, "reason": reason})
        statuses = {entry["status"] for entry in evidence}
        status = next((value for value in _PRIORITY if value in statuses), "Missing")
        results.append({"name": name, "job": requirement["job"], "note": requirement["note"],
                        "status": status, "evidence": evidence})
    return {
        "overall": "GREEN" if all(row["status"] == "Passed" for row in results) else "RED",
        "required": results,
        "per_job": per_job,
        "warnings": [],
        "skip_counts_as_pass": False,
        "data_class": "synthetic",
    }


def render_markdown(report):
    """Render all candidate evidence, then job counts and optional skip details."""
    lines = [f"{report['overall']} — N21 CI ledger", ""]

    def table(title, headers, rows):
        lines.extend([f"## {title}", "", "| " + " | ".join(headers) + " |",
                      "| " + " | ".join("---" for _ in headers) + " |"])
        lines.extend("| " + " | ".join(_cell(value) for value in row) + " |" for row in rows)
        lines.append("")

    table("Required tests", ("Name", "Job", "Status", "Evidence"),
          ((row["name"], row["job"], row["status"],
            "\n".join(f"{entry['job']}: {entry['status']} — {entry['reason']}"
                      for entry in row["evidence"])) for row in report["required"]))
    table("Job counts", ("Job", *_STATUSES),
          ((label, *(job["counts"][status] for status in _STATUSES))
           for label, job in sorted(report["per_job"].items())))
    required_names = {row["name"] for row in report["required"]}
    for key, title in (("skipped", "Nonrequired SKIP"), ("notrun", "Nonrequired NotRun")):
        table(title, ("Job", "Name", "Reason"),
              ((label, entry["name"], entry["reason"])
               for label, job in sorted(report["per_job"].items()) for entry in job[key]
               if entry["name"] not in required_names))
    table("Warnings", ("Warning",), ((warning,) for warning in report["warnings"]))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job", action="append", default=[], metavar="LABEL=PATH")
    parser.add_argument("--required", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        paths = {}
        for argument in args.job:
            label, separator, path = argument.partition("=")
            if not separator:
                raise ValueError(f"Invalid job {argument!r}: expected LABEL=PATH")
            if not _JOB_LABEL.fullmatch(label):
                raise ValueError(f"Invalid job label {label!r}")
            if not path:
                raise ValueError(f"Job {label!r}: empty path")
            if label in paths:
                raise ValueError(f"Duplicate job label {label!r}")
            paths[label] = Path(path).resolve()
        if not paths:
            raise ValueError("At least one --job LABEL=PATH is required")
        required = read_required_jobs(args.required)
        jobs, warnings, inputs = {}, [], {}
        for label, path in sorted(paths.items()):
            jobs[label], reader_warnings = read_junit(path)
            warnings.extend(f"{label}: {warning}" for warning in reader_warnings)
            inputs[label] = str(path)
        report = evaluate(jobs, required)
        report["warnings"] = warnings
        report["inputs"] = inputs
        encoded = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        markdown = render_markdown(report)
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "n21-ledger.json").write_text(encoded, encoding="utf-8")
        (args.out / "n21-ledger.md").write_text(markdown, encoding="utf-8")
    except (ValueError, OSError) as error:
        print(f"n21-ledger: {error}", file=sys.stderr)
        return 2
    return 0 if report["overall"] == "GREEN" else 1


if __name__ == "__main__":
    sys.exit(main())
