# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Report three-grid discretization uncertainty or a same-mesh solver comparison."""

import argparse
from collections.abc import Sequence
import json
import math
from pathlib import Path
import sys

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.vv.field_compare import (InputInvalid, RunFields, UsageError, compare_runs,
                                    ensure_output_allowed, load_run)


class _MeshMismatch(ValueError):
    """Runs cannot form the requested mesh sequence or same-mesh pair."""


def grid_size(run: RunFields) -> tuple[int, float]:
    """Use all owned hex/cut volumes in the loader's canonical cell order."""
    n = len(run.cell_gid)
    if run.cell_volume is None or n == 0:
        raise InputInvalid("cell volumes and at least one cell required")
    try:
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            h = float((np.sum(np.asarray(run.cell_volume, dtype=np.float64)) / n) ** (1 / 3))
    except (FloatingPointError, OverflowError, ValueError) as exc:
        raise InputInvalid("invalid computed grid size") from exc
    if not math.isfinite(h) or h <= 0:
        raise InputInvalid("invalid computed grid size")
    return n, h


def celik_gci(h: Sequence[float], phi: Sequence[float], *, fs: float = 1.25,
              tol: float = 1e-12, max_iter: int = 200) -> dict:
    """Celik et al. (2008) three-grid fixed-point iteration, finest grid first.

    Exact zero differences are flat. Numerical iteration failures are reported
    with null derived quantities; invalid arguments raise InputInvalid.
    """
    try:
        if len(h) != 3 or len(phi) != 3:
            raise InputInvalid("h and phi must each contain three values")
        h1, h2, h3 = map(float, h)
        phi1, phi2, phi3 = map(float, phi)
        fs, tol = float(fs), float(tol)
        valid = (all(math.isfinite(v) for v in (h1, h2, h3, phi1, phi2, phi3, fs, tol))
                 and 0 < h1 < h2 < h3 and fs > 0 and tol > 0
                 and isinstance(max_iter, int) and not isinstance(max_iter, bool) and max_iter > 0)
    except (TypeError, ValueError, OverflowError) as exc:
        raise InputInvalid("invalid GCI arguments") from exc
    if not valid:
        raise InputInvalid("finite values, increasing positive h and positive iteration controls required")

    r21, r32 = h2 / h1, h3 / h2
    eps21, eps32 = phi2 - phi1, phi3 - phi2
    result = dict(r21=r21, r32=r32, eps21=eps21, eps32=eps32, s=None,
                  p=None, iterations=0, phi_ext21=None, e_a21=None, e_ext21=None,
                  gci_fine21=None, status="no_convergence")
    # Even finite arguments can overflow while forming ratios/differences.
    for key in ("r21", "r32", "eps21", "eps32"):
        if not math.isfinite(result[key]):
            result[key] = None
    if eps21 == 0 or eps32 == 0:
        result["status"] = "flat"
        return result
    if any(result[key] is None for key in ("r21", "r32", "eps21", "eps32")):
        return result

    try:
        ratio = eps32 / eps21
        s = (ratio > 0) - (ratio < 0)
        result["s"] = s
        log_ratio, log_r21 = math.log(abs(ratio)), math.log(r21)
        p = abs(log_ratio) / log_r21
        for _ in range(max_iter):
            q = math.log((r21**p - s) / (r32**p - s))
            updated = abs(log_ratio + q) / log_r21
            if not math.isfinite(updated):
                return result
            result["iterations"] += 1
            converged = abs(updated - p) <= tol
            p = updated
            if converged:
                break
        else:
            return result

        power = r21**p
        denominator = power - 1
        if denominator <= 0 or not math.isfinite(denominator):
            return result
        phi_ext21 = (power * phi1 - phi2) / denominator
        e_a21 = abs((phi1 - phi2) / phi1) if phi1 != 0 else None
        e_ext21 = abs((phi_ext21 - phi1) / phi_ext21) if phi_ext21 != 0 else None
        gci_fine21 = fs * e_a21 / denominator if e_a21 is not None else None
        derived = dict(p=p, phi_ext21=phi_ext21, e_a21=e_a21,
                       e_ext21=e_ext21, gci_fine21=gci_fine21)
        if any(v is not None and not math.isfinite(v) for v in derived.values()):
            return result
    except (OverflowError, ValueError, ZeroDivisionError):
        return result
    result.update(derived, status="oscillatory" if s < 0 else "monotone")
    return result


def _ambient(t_ambient):
    if t_ambient is not None and not math.isfinite(t_ambient):
        raise InputInvalid("t_ambient must be finite")


def _metadata(path, run):
    n, h = grid_size(run)
    return dict(run_dir=Path(path).resolve().name, mesh_sha256=run.mesh_sha256,
                N=n, h=h, qoi=dict(run.qoi), qoi_method=dict(run.qoi_method))


def _record(kind, runs, notes):
    return dict(schema="chipcae.vv_crosscheck/0.1", case_id="", data_class="synthetic",
                kind=kind, runs=runs, verdict="report", notes=notes)


def _mesh_sequence(loaded, *, t_ambient):
    from tools.vv.fvm_warpage_gate import deformation_max, require_convergence_quantity

    ordered = sorted(((_metadata(path, run), path, run) for path, run in loaded),
                     key=lambda entry: entry[0]["h"])
    runs = [metadata for metadata, _, _ in ordered]
    if len({run["mesh_sha256"] for run in runs}) != 3:
        raise _MeshMismatch("mesh sequence requires three distinct mesh hashes")
    hs = [run["h"] for run in runs]
    if any(b - a <= 1e-12 * b for a, b in zip(hs, hs[1:])):
        raise _MeshMismatch("mesh sequence requires distinct increasing grid sizes")
    notes, estimates = [], {}
    if t_ambient is not None:
        temperatures = [run["qoi"]["T_max"] - t_ambient for run in runs]
        if not all(math.isfinite(v) for v in temperatures):
            raise InputInvalid("nonfinite computed temperature rise")
        estimates["T_max_rise"] = celik_gci(hs, temperatures)
    for name in ("warpage_pv", "deformation_max", "U_mag_max"):
        source = "U_mag_max" if name == "deformation_max" else name
        if any(run["qoi_method"][source].startswith("not solved") for run in runs):
            estimates[name] = None
            notes.append(f"{name}: not solved in at least one run; GCI unavailable")
        elif name == "U_mag_max":
            estimates[name] = dict(values=[run["qoi"][name] for run in runs],
                                   convergence_use=False,
                                   explanation="U_mag_max 含刚体位移，只打印，不作收敛判据")
        else:
            require_convergence_quantity(name)
            values = ([deformation_max(path) for _, path, _ in ordered]
                      if name == "deformation_max" else [run["qoi"][name] for run in runs])
            estimates[name] = celik_gci(hs, values)
    result = _record("mesh_seq", runs, notes)
    result["mesh_seq"] = estimates
    return result


def mesh_sequence(run_dirs: Sequence[Path], *, t_ambient: float | None) -> dict:
    """Load exactly three distinct meshes and report available QoI estimates."""
    if len(run_dirs) != 3:
        raise UsageError("mesh sequence requires exactly three runs")
    _ambient(t_ambient)
    return _mesh_sequence([(path, load_run(path)) for path in run_dirs], t_ambient=t_ambient)


def _solver_pair(loaded, *, t_ambient):
    runs = [_metadata(path, run) for path, run in loaded]
    try:
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            pair = compare_runs(loaded[0][1], loaded[1][1], preset="fem-fvm",
                                t_ambient=t_ambient, remove_rigid=True)
    except (FloatingPointError, OverflowError, np.linalg.LinAlgError) as exc:
        raise InputInvalid("invalid computed pair metric") from exc
    if pair["verdict"] == "mesh_mismatch":
        raise _MeshMismatch("solver pair requires identical mesh hashes")
    result = _record("pair", runs, list(pair["notes"]))
    result.update(pair=pair, verdict=pair["verdict"])
    return result


def solver_pair(fem_run: Path, fvm_run: Path, *, t_ambient: float) -> dict:
    """Embed the unchanged fem-fvm comparison, including its sole 1% gate."""
    if Path(fem_run).resolve() == Path(fvm_run).resolve():
        raise UsageError("fem and fvm must be different run directories")
    _ambient(t_ambient)
    if t_ambient is None:
        raise UsageError("solver pair requires t_ambient")
    return _solver_pair([(path, load_run(path)) for path in (fem_run, fvm_run)],
                        t_ambient=t_ambient)


def _markdown(result):
    # Flatten every leaf so nested comparison metrics, locations and gates retain
    # exactly the same float representations as the serialized JSON record.
    def rows(value, path=""):
        if isinstance(value, dict):
            for key, child in value.items():
                yield from rows(child, f"{path}.{key}" if path else key)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                yield from rows(child, f"{path}[{index}]")
        else:
            display = repr(value) if isinstance(value, float) else str(value)
            display = display.replace("|", "\\|").replace("\r", " ").replace("\n", " ")
            yield f"| {path} | {display} |"

    return "\n".join(["# Run crosscheck", "", "| Quantity | Value |", "|---|---|",
                      *rows(result), ""])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    mesh = subparsers.add_parser("mesh-seq", help="report three-grid QoI uncertainty")
    mesh.add_argument("run_dirs", type=Path, nargs=3)
    pair = subparsers.add_parser("pair", help="compare FEM and FVM on the same mesh")
    pair.add_argument("--fem", type=Path, required=True)
    pair.add_argument("--fvm", type=Path, required=True)
    for command in (mesh, pair):
        command.add_argument("--t-ambient", type=float, required=command is pair)
        command.add_argument("--data-class", choices=("synthetic", "public", "real"), required=True)
        command.add_argument("--case-id", default="")
        command.add_argument("-o", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        # Identifiers are path-free labels, keeping local paths out of records.
        if "/" in args.case_id or "\\" in args.case_id:
            raise UsageError("case-id must be a path-free identifier")
        markdown = args.o.with_suffix(".md")
        if args.o.resolve() == markdown.resolve():
            raise UsageError("JSON and Markdown destinations must differ")
        for destination in (args.o, markdown):
            ensure_output_allowed(destination, args.data_class)
        if args.command == "mesh-seq":
            result = mesh_sequence(args.run_dirs, t_ambient=args.t_ambient)
        else:
            result = solver_pair(args.fem, args.fvm, t_ambient=args.t_ambient)
        result.update(case_id=args.case_id, data_class=args.data_class)
        content = json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
        table = _markdown(result)
        args.o.write_text(content, encoding="utf-8")
        markdown.write_text(table, encoding="utf-8")
    except UsageError as exc:
        print(f"real_case_crosscheck: USAGE: {exc}", file=sys.stderr)
        return 2
    except _MeshMismatch as exc:
        print(f"real_case_crosscheck: MESH_MISMATCH: {exc}", file=sys.stderr)
        return 3
    except (InputInvalid, OSError, ValueError) as exc:
        print(f"real_case_crosscheck: INPUT_INVALID: {exc}", file=sys.stderr)
        return 14
    print(f"{result['verdict']}: {args.o} {markdown}")
    return {"report": 0, "pass": 0, "fail": 1}[result["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
