#!/usr/bin/env python3
"""Check t2m/1.0 fixtures. Standard library only.

Reimplements map_thermal_to_mechanic validation and arithmetic from
coupling/src/thermal_mechanic_map.cpp:24-106 (same order, messages, and codes).
Simple fixture values are exact in IEEE-754 double; no extra-precision rounding
is applied.

  python3 check.py
  python3 check.py --dir DIR
Exit 0 = pass, 1 = mismatch, 2 = usage / IO error.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
INPUT_INVALID = 14
INPUT_REFERENCE_MISSING = 12
NONE = 0
CODE_NAME = {0: "NONE", 12: "INPUT_REFERENCE_MISSING", 14: "INPUT_INVALID"}


class CheckError(Exception):
    def __init__(self, message: str):
        super().__init__(message)
        self.message = message


def load_json(path: Path):
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CheckError(f"cannot read {path}: {exc}") from exc
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise CheckError(f"{path}: not valid JSON ({exc})") from exc


def expand_number(value):
    if isinstance(value, dict) and set(value.keys()) == {"__nonfinite__"}:
        token = value["__nonfinite__"]
        if token == "nan":
            return float("nan")
        if token == "inf":
            return float("inf")
        if token == "-inf":
            return float("-inf")
        raise CheckError(f"unknown nonfinite token: {token}")
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CheckError("expected a number")
    return float(value)


def expand_list(values):
    return [expand_number(v) for v in values]


def status(code: int, message: str, dT=None, wT=0.0, wdT=0.0) -> dict:
    return {
        "code": code,
        "code_name": CODE_NAME[code],
        "message": message,
        "dT": list(dT or []),
        "volume_weighted_T": wT,
        "volume_weighted_dT": wdT,
    }


def map_thermal_to_mechanic(inp: dict) -> dict:
    volume = expand_list(inp.get("volume") or [])
    if not volume:
        return status(INPUT_INVALID, "invalid volume")
    sum_volume = 0.0
    for value in volume:
        if not math.isfinite(value) or value <= 0.0:
            return status(INPUT_INVALID, "invalid volume")
        sum_volume += value
        if not math.isfinite(sum_volume):
            return status(INPUT_INVALID, "invalid volume")
    if sum_volume <= 0.0:
        return status(INPUT_INVALID, "invalid volume")

    t_ref = expand_number(inp.get("T_ref", 0.0))
    if not math.isfinite(t_ref):
        return status(INPUT_INVALID, "invalid T_ref")

    unit = inp.get("temperature_unit", "K")
    if unit is None:
        unit = "K"
    if not isinstance(unit, str) or (unit != "" and unit != "K"):
        return status(INPUT_INVALID, "expected unit K")

    has_uniform = "T_uniform" in inp and inp["T_uniform"] is not None
    t_cell_raw = inp.get("T_cell") or []
    if has_uniform:
        if t_cell_raw:
            return status(INPUT_INVALID, "inconsistent temperature sources")
        t_uniform = expand_number(inp["T_uniform"])
        if not math.isfinite(t_uniform):
            return status(INPUT_INVALID, "invalid T_uniform")
        temperatures = [t_uniform] * len(volume)
    else:
        if not t_cell_raw:
            return status(INPUT_REFERENCE_MISSING, "missing T_cell")
        temperatures = expand_list(t_cell_raw)
        if len(temperatures) != len(volume):
            return status(INPUT_INVALID, "length mismatch")
        for value in temperatures:
            if not math.isfinite(value):
                return status(INPUT_INVALID, "nonfinite T_cell")

    dT = []
    sum_TV = 0.0
    sum_dTV = 0.0
    for temp, vol in zip(temperatures, volume):
        delta = temp - t_ref
        if not math.isfinite(delta):
            return status(INPUT_INVALID, "invalid temperature arithmetic")
        tv = temp * vol
        dtv = delta * vol
        if not math.isfinite(tv) or not math.isfinite(dtv):
            return status(INPUT_INVALID, "invalid temperature arithmetic")
        dT.append(delta)
        sum_TV += tv
        sum_dTV += dtv
        if not math.isfinite(sum_TV) or not math.isfinite(sum_dTV):
            return status(INPUT_INVALID, "invalid temperature arithmetic")
    weighted_t = sum_TV / sum_volume
    weighted_dt = sum_dTV / sum_volume
    if not math.isfinite(weighted_t) or not math.isfinite(weighted_dt):
        return status(INPUT_INVALID, "invalid temperature arithmetic")
    delta = weighted_t - t_ref
    if not math.isfinite(delta):
        return status(INPUT_INVALID, "invalid temperature arithmetic")
    residual = weighted_dt - delta
    if not math.isfinite(residual):
        return status(INPUT_INVALID, "invalid temperature arithmetic")
    tolerance = 1e-14 * max(abs(delta), 1.0)
    if not math.isfinite(tolerance) or abs(residual) > tolerance:
        return status(INPUT_INVALID, "invalid temperature arithmetic")
    return status(NONE, "", dT, weighted_t, weighted_dt)


def same_number(actual, expected, path: str, failures: list, rel: str) -> None:
    if isinstance(expected, float) and math.isnan(expected):
        if not (isinstance(actual, float) and math.isnan(actual)):
            failures.append(f"{rel}: {path} expected nan, got {actual!r}")
        return
    if actual != expected:
        failures.append(f"{rel}: {path} {actual!r} != {expected!r}")


def compare(actual: dict, expected: dict, rel: str, failures: list) -> None:
    for key in ("code", "code_name", "message"):
        if actual.get(key) != expected.get(key):
            failures.append(
                f"{rel}: {key} {actual.get(key)!r} != {expected.get(key)!r}"
            )
    exp_dt = expected.get("dT")
    act_dt = actual.get("dT")
    if not isinstance(act_dt, list) or len(act_dt) != len(exp_dt):
        failures.append(f"{rel}: dT length {act_dt!r} != {exp_dt!r}")
    else:
        for i, (a, e) in enumerate(zip(act_dt, exp_dt)):
            same_number(a, e, f"dT[{i}]", failures, rel)
    same_number(
        actual.get("volume_weighted_T"),
        expected.get("volume_weighted_T"),
        "volume_weighted_T",
        failures,
        rel,
    )
    same_number(
        actual.get("volume_weighted_dT"),
        expected.get("volume_weighted_dT"),
        "volume_weighted_dT",
        failures,
        rel,
    )


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", type=Path, default=HERE)
    args = parser.parse_args(argv)
    root = args.dir.resolve()
    catalog_path = root / "expected.json"
    if not catalog_path.exists():
        sys.stderr.write("expected.json missing; run gen.py first\n")
        return 1
    try:
        catalog = load_json(catalog_path)
    except CheckError as exc:
        sys.stderr.write(exc.message + "\n")
        return 2

    failures: list[str] = []
    cases = list(catalog.get("positives", [])) + list(catalog.get("negatives", []))
    if not cases:
        failures.append("expected.json has no cases")
    for rel in cases:
        case_dir = root / rel
        try:
            inp = load_json(case_dir / "input.json")
            expected = load_json(case_dir / "expected.json")
            actual = map_thermal_to_mechanic(inp)
            compare(actual, expected, rel, failures)
        except CheckError as exc:
            failures.append(f"{rel}: {exc.message}")
    if failures:
        for line in failures:
            print(line)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
