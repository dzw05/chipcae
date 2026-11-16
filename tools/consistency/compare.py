"""Compare CPU and GPU warpage fields.

Bitwise equality is not required.

A consistency case is eligible only when both sides report a linear relative
residual at or below 1e-12 (a direct solver may report 0) and a Newton
tolerance at or below 1e-10.

On an eligible pair:
- warpage relative difference <= 1e-6
- displacement relative difference <= 1e-6 (L2)
- max |stress difference| <= 1e-6 * max |stress|
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

WARPAGE_REL_MAX = 1e-6
DISPLACEMENT_REL_MAX = 1e-6
STRESS_REL_MAX = 1e-6
LINEAR_RESIDUAL_MAX = 1e-12
NEWTON_TOLERANCE_MAX = 1e-10


def _as_floats(values: object, field: str) -> list[float]:
    if isinstance(values, (int, float)) and not isinstance(values, bool):
        return [float(values)]
    if not isinstance(values, Sequence) or isinstance(values, (str, bytes)):
        raise ValueError(f"{field} must be a number or a list of numbers")
    return [float(item) for item in values]


def l2_norm(values: Sequence[float]) -> float:
    return math.sqrt(sum(float(value) * float(value) for value in values))


def relative_l2(left: Sequence[float], right: Sequence[float]) -> float:
    if len(left) != len(right):
        raise ValueError("vectors differ in length")
    difference = l2_norm([a - b for a, b in zip(left, right)])
    scale = max(l2_norm(left), l2_norm(right))
    if scale == 0.0:
        return 0.0 if difference == 0.0 else math.inf
    return difference / scale


def relative_scalar(left: float, right: float) -> float:
    scale = max(abs(left), abs(right))
    if scale == 0.0:
        return 0.0 if left == right else math.inf
    return abs(left - right) / scale


def stress_ratio(left: Sequence[float], right: Sequence[float]) -> tuple[float, float, float]:
    if len(left) != len(right):
        raise ValueError("stress vectors differ in length")
    peak = 0.0
    worst = 0.0
    for a, b in zip(left, right):
        peak = max(peak, abs(a), abs(b))
        worst = max(worst, abs(a - b))
    if peak == 0.0:
        ratio = 0.0 if worst == 0.0 else math.inf
    else:
        ratio = worst / peak
    return worst, peak, ratio


def _residual(record: Mapping, side: str) -> float:
    if "linear_relative_residual" not in record:
        raise ValueError(f"{side} is missing linear_relative_residual")
    return float(record["linear_relative_residual"])


def _newton_tolerance(record: Mapping, side: str) -> float:
    if "newton_tolerance" not in record:
        raise ValueError(f"{side} is missing newton_tolerance")
    return float(record["newton_tolerance"])


def compare_records(cpu: Mapping, gpu: Mapping) -> dict:
    """Return a report. `ok` is true only when every gate passes."""
    failures: list[str] = []
    cpu_residual = _residual(cpu, "cpu")
    gpu_residual = _residual(gpu, "gpu")
    cpu_newton = _newton_tolerance(cpu, "cpu")
    gpu_newton = _newton_tolerance(gpu, "gpu")
    solver_ok = True
    if cpu_residual > LINEAR_RESIDUAL_MAX or gpu_residual > LINEAR_RESIDUAL_MAX:
        solver_ok = False
        failures.append(
            f"linear relative residual must be <= {LINEAR_RESIDUAL_MAX:g} "
            f"(cpu={cpu_residual:g}, gpu={gpu_residual:g})"
        )
    if cpu_newton > NEWTON_TOLERANCE_MAX or gpu_newton > NEWTON_TOLERANCE_MAX:
        solver_ok = False
        failures.append(
            f"Newton tolerance must be <= {NEWTON_TOLERANCE_MAX:g} "
            f"(cpu={cpu_newton:g}, gpu={gpu_newton:g})"
        )

    warpage = relative_scalar(float(cpu["warpage"]), float(gpu["warpage"]))
    displacement = relative_l2(
        _as_floats(cpu["displacement"], "displacement"),
        _as_floats(gpu["displacement"], "displacement"),
    )
    stress_abs, stress_peak, stress = stress_ratio(
        _as_floats(cpu["stress"], "stress"),
        _as_floats(gpu["stress"], "stress"),
    )
    field_failures = []
    if not solver_ok:
        field_failures.append("field comparison skipped until the solver tolerances pass")
    else:
        if warpage > WARPAGE_REL_MAX:
            field_failures.append(f"warpage relative difference {warpage:.3e} > {WARPAGE_REL_MAX:.3e}")
        if displacement > DISPLACEMENT_REL_MAX:
            field_failures.append(
                f"displacement relative difference {displacement:.3e} > {DISPLACEMENT_REL_MAX:.3e}"
            )
        if stress > STRESS_REL_MAX:
            field_failures.append(
                f"stress difference {stress_abs:.3e} > {STRESS_REL_MAX:.3e} * max|stress| ({stress_peak:.3e})"
            )
    failures.extend(field_failures)
    bitwise_equal = (
        float(cpu["warpage"]) == float(gpu["warpage"])
        and list(_as_floats(cpu["displacement"], "displacement"))
        == list(_as_floats(gpu["displacement"], "displacement"))
        and list(_as_floats(cpu["stress"], "stress")) == list(_as_floats(gpu["stress"], "stress"))
    )
    return {
        "schema_version": 1,
        "ok": not failures,
        "blocks": bool(failures),
        "solver_ok": solver_ok,
        "bitwise_equal": bitwise_equal,
        "bitwise_required": False,
        "limits": {
            "warpage_rel_max": WARPAGE_REL_MAX,
            "displacement_rel_max": DISPLACEMENT_REL_MAX,
            "stress_rel_max": STRESS_REL_MAX,
            "linear_residual_max": LINEAR_RESIDUAL_MAX,
            "newton_tolerance_max": NEWTON_TOLERANCE_MAX,
        },
        "metrics": {
            "warpage_rel": warpage,
            "displacement_rel": displacement,
            "stress_abs": stress_abs,
            "stress_peak": stress_peak,
            "stress_rel": stress,
            "cpu_linear_relative_residual": cpu_residual,
            "gpu_linear_relative_residual": gpu_residual,
            "cpu_newton_tolerance": cpu_newton,
            "gpu_newton_tolerance": gpu_newton,
        },
        "failures": failures,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compare CPU and GPU warpage results.")
    parser.add_argument("--cpu", type=Path, required=True)
    parser.add_argument("--gpu", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        report = compare_records(
            json.loads(args.cpu.read_text(encoding="utf-8")),
            json.loads(args.gpu.read_text(encoding="utf-8")),
        )
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        print(f"consistency input error: {exc}", file=sys.stderr)
        return 2
    text = json.dumps(report, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
