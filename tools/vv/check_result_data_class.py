# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Validate V&V result content and enforce the D03 repository policy."""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

ALLOWED_DIRS: tuple[str, ...] = ("cases/benchmarks/", "tests/vv/data/")
MAX_BYTES: int = 2 * 1024 * 1024
SCHEMA_PATH = Path(__file__).resolve().parent / "schema" / "vv-result-0.2.schema.json"

_SCHEMA_IDS = ("chipcae.vv_result/0.2", "chipcae.vv_result/0.3")
_OBJECT_FIELDS = ("timing", "memory", "machine", "build", "run", "iterations", "caps", "cap_checks")


def _reject_constant(value: str) -> None:
    raise ValueError(f"Nonstandard JSON constant: {value}")


def _matches(pattern: str, value: object) -> bool:
    return isinstance(value, str) and re.fullmatch(pattern, value) is not None


def _number_or_null(value: object) -> bool:
    # JSON booleans are distinct from numbers even though bool subclasses int.
    return value is None or type(value) in (int, float)


def _check_field(
    obj: dict,
    key: str,
    is_valid: Callable[[object], bool],
    reasons: list[str],
    *,
    parent: str = "",
    required: bool = True,
) -> None:
    field = f"{parent}.{key}" if parent else key
    if key not in obj:
        if required:
            reasons.append(f"missing {field}")
    elif not is_valid(obj[key]):
        reasons.append(f"invalid {field}")


def check_file(path: Path) -> list[str]:
    """Return ordered content violations for a UTF-8 JSON result."""
    try:
        raw = path.read_bytes()
    except (OSError, ValueError):
        return ["cannot read file"]
    try:
        obj = json.loads(raw.decode("utf-8"), parse_constant=_reject_constant)
    except (ValueError, RecursionError):
        return ["not valid JSON"]
    if not isinstance(obj, dict):
        return ["top-level not an object"]

    reasons: list[str] = []
    _check_field(obj, "data_class", lambda value: value in ("synthetic", "public", "real"), reasons)
    if obj.get("data_class") == "real":
        reasons.append("real result must stay local (D03)")

    verdicts = ("pass", "fail", "需讨论", "skip")
    reference_types = ("解析", "独立代码", "原型", "回归")
    if obj.get("schema") == "chipcae.vv_result/0.3":
        verdicts += ("needs_attention",)
        reference_types += ("无解析解（物理合理性检查）",)

    _check_field(obj, "schema", lambda value: value in _SCHEMA_IDS, reasons)
    _check_field(obj, "case_id", lambda value: _matches(r"[A-Za-z0-9][A-Za-z0-9_.-]*", value), reasons)
    _check_field(obj, "label", lambda value: value in ("实测", "参考值", "计划值"), reasons)
    _check_field(obj, "date", lambda value: _matches(r"[0-9]{8}", value), reasons)
    _check_field(obj, "commit", lambda value: _matches(r"[0-9a-f]{7,40}", value), reasons)
    _check_field(obj, "qoi", lambda value: isinstance(value, dict), reasons)
    _check_field(obj, "verdict", lambda value: value in verdicts, reasons)

    for field in ("reference", "error", "tolerance"):
        value = obj.get(field)
        if value is None:
            continue
        if not isinstance(value, dict):
            reasons.append(f"invalid {field}")
            continue
        _check_field(value, "value", _number_or_null, reasons, parent=field)
        if field == "reference":
            _check_field(
                value, "type", lambda item: item in reference_types,
                reasons, parent=field,
            )
            _check_field(
                value, "source", lambda item: isinstance(item, str) and item != "",
                reasons, parent=field,
            )
        else:
            _check_field(
                value, "metric", lambda item: isinstance(item, str),
                reasons, parent=field, required=False,
            )

    for field in _OBJECT_FIELDS:
        _check_field(
            obj, field, lambda value: value is None or isinstance(value, dict),
            reasons, required=False,
        )
    _check_field(
        obj, "notes", lambda value: isinstance(value, list) and all(
            isinstance(item, str) for item in value
        ),
        reasons, required=False,
    )
    _check_field(obj, "src_dirty", lambda value: isinstance(value, bool), reasons, required=False)
    return reasons


def _git(repo: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=repo, stderr=subprocess.PIPE)


def _repository_root(repo: Path | None) -> Path:
    cwd = Path.cwd() if repo is None else repo.resolve()
    root = Path(os.fsdecode(_git(cwd, "rev-parse", "--show-toplevel").removesuffix(b"\n"))).resolve()
    if repo is not None and cwd != root:
        raise ValueError(f"--repo must name the Git worktree root: {repo}")
    return root


def _tracked_results(repo: Path) -> list[Path]:
    paths = [Path(os.fsdecode(name)) for name in _git(repo, "ls-files", "-z").split(b"\0") if name]
    return [path for path in paths if path.name == "result.json"]


def _check_candidate(repo: Path, path: Path) -> tuple[str, list[str]]:
    try:
        relative = path.relative_to(repo).as_posix()
    except ValueError:
        return path.as_posix(), ["outside allowed dirs"]
    if not relative.startswith(ALLOWED_DIRS):
        return relative, ["outside allowed dirs"]
    try:
        info = path.stat()
    except OSError:
        return relative, ["cannot read file"]
    if info.st_size > MAX_BYTES:
        return relative, ["exceeds 2 MiB"]
    if not stat.S_ISREG(info.st_mode):
        return relative, ["cannot read file"]
    return relative, check_file(path)


def _report(path: str, reasons: list[str]) -> None:
    display = path.replace("\r", r"\r").replace("\n", r"\n")
    for reason in reasons:
        print(f"{display}: {reason}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, help="Git worktree root (default: infer from Git)")
    parser.add_argument("paths", nargs="*", type=Path, help="files to check, relative to the root or absolute")
    args = parser.parse_args()
    try:
        repo = _repository_root(args.repo)
        paths = args.paths or _tracked_results(repo)
    except subprocess.CalledProcessError as error:
        parser.error(f"Git discovery failed: {os.fsdecode(error.stderr).strip()}")
    except (OSError, RuntimeError, ValueError) as error:
        parser.error(str(error))

    if not paths:
        print("0 result.json checked")
        return 0

    seen = set()
    status = 0
    for candidate in paths:
        try:
            path = (repo / candidate).resolve()
        except (OSError, RuntimeError):
            _report(candidate.as_posix(), ["cannot read file"])
            status = 1
            continue
        if path in seen:
            continue
        seen.add(path)
        display, reasons = _check_candidate(repo, path)
        if reasons:
            _report(display, reasons)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
