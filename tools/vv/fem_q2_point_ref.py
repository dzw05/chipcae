# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Evaluate axis-aligned tensor Q2 FEM fields at the caller's exact sample points.

Only the sidecar accuracy-q2-rank*.json format is consumed. No solver or
vertex-field fallback is used. Geometry and fields are stored in gid order;
the spatial index is rebuilt for each evaluation because Q2Mesh arrays remain
mutable even though its dataclass is frozen.
"""

import argparse
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import json
import math
from numbers import Integral, Real
import os
from pathlib import Path
import re
import sys
from time import perf_counter

import numpy as np

from tools.vv.warpage_qoi import best_fit_plane


_REL_TOL = 1e-12
_POINT_BATCH = 4096
_CELL_BATCH = 1024


class Q2RefError(ValueError):
    """Invalid Q2 reference input or unsupported geometry."""


@dataclass(frozen=True)
class Q2Mesh:
    gid: np.ndarray
    lo: np.ndarray
    hi: np.ndarray
    U: np.ndarray
    T: np.ndarray


def _array(value, name, prefix="Q2REF_BAD_ARG"):
    try:
        if np.iscomplexobj(value):
            raise ValueError("complex values are unsupported")
        return np.asarray(value, dtype=np.float64)
    except (TypeError, ValueError, OverflowError) as exc:
        raise Q2RefError(f"{prefix}: {name} must be a real numeric array") from exc


def _node_array(value, name):
    try:
        return _array(value, name, "Q2REF_BAD_JSON")
    except Q2RefError as exc:
        # NumPy rejects ragged shapes before converting their numeric values.
        # Check the leaves to distinguish bad values from malformed node arrays.
        pending = [value]
        while pending:
            item = pending.pop()
            if isinstance(item, (list, tuple)) or (isinstance(item, np.ndarray) and item.ndim > 0):
                pending.extend(item)
            else:
                _array(item, name, "Q2REF_BAD_JSON")
        raise Q2RefError(f"Q2REF_NODE_COUNT: {name} must be a rectangular numeric array") from exc


def _scalars(**values):
    result = {}
    for name, value in values.items():
        if not isinstance(value, Real) or isinstance(value, (bool, np.bool_)):
            raise Q2RefError(f"Q2REF_BAD_ARG: {name} must be a real scalar")
        try:
            result[name] = float(value)
        except (ValueError, OverflowError) as exc:
            raise Q2RefError(f"Q2REF_BAD_ARG: {name} cannot be represented as float64") from exc
    return result


def _finite(**values):
    for name, value in values.items():
        if not np.all(np.isfinite(value)):
            raise Q2RefError(f"Q2REF_NONFINITE: {name}")


def _spans(lo, hi):
    with np.errstate(over="ignore", invalid="ignore"):
        span = hi - lo
    _finite(span=span)
    if np.any(span <= _REL_TOL * np.maximum(np.max(span, axis=-1, keepdims=True), 1e-300)):
        raise Q2RefError("Q2REF_NOT_AXIS_ALIGNED: degenerate element")
    return span


def _gid(value):
    # Never pass an integer through float64: valid gids may exceed 2**53.
    if isinstance(value, (bool, np.bool_)) or not isinstance(value, Real):
        raise Q2RefError("Q2REF_BAD_JSON: gid must be an int64 integer")
    if not isinstance(value, Integral) and (not math.isfinite(value) or value != math.trunc(value)):
        raise Q2RefError("Q2REF_BAD_JSON: gid must be an int64 integer")
    integer = int(value)
    if not -(2**63) <= integer < 2**63:
        raise Q2RefError("Q2REF_BAD_JSON: gid is outside int64 range")
    return integer


def _coordinate_indices(values, span):
    order = np.argsort(values, kind="stable")
    labels = np.empty(27, dtype=np.intp)
    levels = []
    for node in order:
        value = values[node]
        if not levels or value - levels[-1] > _REL_TOL * span:
            levels.append(value)
        labels[node] = len(levels) - 1
    if len(levels) != 3 or abs(levels[1] - (levels[0] + 0.5 * span)) > _REL_TOL * span:
        raise Q2RefError("Q2REF_NOT_AXIS_ALIGNED: expected three equally spaced coordinate levels")
    return labels


def q2_mesh_from_elements(elements: Sequence[Mapping]) -> Q2Mesh:
    """Validate exported nodes and store their coordinate-indexed tensor values."""
    if not isinstance(elements, Sequence) or isinstance(elements, (str, bytes)):
        raise Q2RefError("Q2REF_BAD_JSON: elements must be a sequence of mappings")
    if not elements:
        raise Q2RefError("Q2REF_EMPTY: elements is empty")
    rows = []
    seen = set()
    for element in elements:
        if not isinstance(element, Mapping) or not all(key in element for key in ("gid", "points", "U", "T")):
            raise Q2RefError("Q2REF_BAD_JSON: element requires gid, points, U and T")
        gid = _gid(element["gid"])
        if gid in seen:
            raise Q2RefError(f"Q2REF_DUP_GID: {gid}")
        seen.add(gid)
        rows.append((gid, element))
    rows.sort(key=lambda row: row[0])
    count = len(rows)
    gids = np.array([gid for gid, _ in rows], dtype=np.int64)
    lower = np.empty((count, 3), dtype=np.float64)
    upper = np.empty_like(lower)
    displacement = np.empty((count, 3, 3, 3, 3), dtype=np.float64)
    temperature = np.empty((count, 3, 3, 3), dtype=np.float64)
    for row, (gid, element) in enumerate(rows):
        arrays = {name: _node_array(element[name], name) for name in ("points", "U", "T")}
        for name, shape in (("points", (27, 3)), ("U", (27, 3)), ("T", (27,))):
            if arrays[name].shape != shape:
                raise Q2RefError(f"Q2REF_NODE_COUNT: gid {gid} {name} must have shape {shape}")
        _finite(**arrays)
        points = arrays["points"]
        lo, hi = np.min(points, axis=0), np.max(points, axis=0)
        span = _spans(lo, hi)
        indices = np.column_stack([_coordinate_indices(points[:, axis], span[axis]) for axis in range(3)])
        linear = indices[:, 0] * 9 + indices[:, 1] * 3 + indices[:, 2]
        if len(np.unique(linear)) != 27:
            raise Q2RefError(f"Q2REF_DUP_NODE: gid {gid} has repeated tensor nodes")
        lower[row], upper[row] = lo, hi
        i, j, k = indices.T
        displacement[row, i, j, k] = arrays["U"]
        temperature[row, i, j, k] = arrays["T"]
    return Q2Mesh(gids, lower, upper, displacement, temperature)


def load_fem_q2(run_dir: str | os.PathLike) -> Q2Mesh:
    """Read every rank sidecar in numeric rank order, then sort all cells by gid."""
    try:
        paths = list(Path(run_dir).glob("accuracy-q2-rank*.json"))
    except (TypeError, ValueError, OSError) as exc:
        raise Q2RefError(f"Q2REF_BAD_ARG: cannot discover rank files in {run_dir}: {exc}") from exc
    if not paths:
        raise Q2RefError(f"Q2REF_NO_FILES: {run_dir}")
    def rank(path):
        match = re.fullmatch(r"accuracy-q2-rank(\d+)\.json", path.name)
        if match is None:
            raise Q2RefError(f"Q2REF_BAD_JSON: invalid rank filename {path}")
        return int(match[1]), path.name
    paths.sort(key=rank)
    elements = []
    for path in paths:
        try:
            with path.open(encoding="utf-8") as stream:
                document = json.load(stream)
        except (OSError, ValueError, UnicodeError) as exc:
            raise Q2RefError(f"Q2REF_BAD_JSON: {path}: {exc}") from exc
        if not isinstance(document, Mapping):
            raise Q2RefError(f"Q2REF_BAD_JSON: {path}: expected a JSON object")
        if "elements" not in document or document["elements"] == []:
            raise Q2RefError(f"Q2REF_EMPTY: {path}: missing or empty elements")
        if not isinstance(document["elements"], list):
            raise Q2RefError(f"Q2REF_BAD_JSON: {path}: elements must be a list")
        elements.extend(document["elements"])
    return q2_mesh_from_elements(elements)


def _validate_mesh(q2):
    if not isinstance(q2, Q2Mesh) or not isinstance(q2.gid, np.ndarray) or q2.gid.ndim != 1:
        raise Q2RefError("Q2REF_BAD_ARG: expected Q2Mesh with a one-dimensional gid array")
    count = len(q2.gid)
    if count == 0:
        raise Q2RefError("Q2REF_EMPTY: mesh is empty")
    for name, shape, dtype in (("gid", (count,), np.int64), ("lo", (count, 3), np.float64),
                               ("hi", (count, 3), np.float64), ("U", (count, 3, 3, 3, 3), np.float64),
                               ("T", (count, 3, 3, 3), np.float64)):
        value = getattr(q2, name)
        if not isinstance(value, np.ndarray) or value.shape != shape or value.dtype != dtype:
            raise Q2RefError(f"Q2REF_BAD_ARG: Q2Mesh.{name} must have shape {shape} and dtype {np.dtype(dtype)}")
    _finite(lo=q2.lo, hi=q2.hi, U=q2.U, T=q2.T)
    if len(np.unique(q2.gid)) != count:
        raise Q2RefError("Q2REF_DUP_GID: Q2Mesh.gid")
    return _spans(q2.lo, q2.hi)


@dataclass
class _BoxNode:
    lo: np.ndarray
    hi: np.ndarray
    cells: np.ndarray | None = None
    left: "_BoxNode | None" = None
    right: "_BoxNode | None" = None


def _box_tree(q2, span):
    # Every ancestor contains the tolerance-expanded boxes of its descendants.
    lo, hi = q2.lo - _REL_TOL * span, q2.hi + _REL_TOL * span
    centers = q2.lo + 0.5 * span
    def build(rows):
        node = _BoxNode(np.min(lo[rows], axis=0), np.max(hi[rows], axis=0))
        if len(rows) <= 8:
            node.cells = np.sort(rows)
            return node
        axis = int(np.argmax(np.ptp(centers[rows], axis=0)))
        rows = rows[np.argsort(centers[rows, axis], kind="stable")]
        middle = len(rows) // 2
        node.left, node.right = build(rows[:middle]), build(rows[middle:])
        return node
    return build(np.arange(len(q2.gid))), lo, hi


def _locate(tree, lo, hi, points):
    selected = np.full(len(points), -1, dtype=np.intp)
    stack = [(tree, np.arange(len(points)))]
    while stack:
        node, candidates = stack.pop()
        candidates = candidates[selected[candidates] < 0]
        xyz = points[candidates]
        candidates = candidates[np.all((xyz >= node.lo) & (xyz <= node.hi), axis=1)]
        if not len(candidates):
            continue
        if node.cells is None:
            # Left-first traversal and sorted leaf rows make boundary ties deterministic.
            stack.append((node.right, candidates))
            stack.append((node.left, candidates))
        else:
            for cell in node.cells:
                remaining = candidates[selected[candidates] < 0]
                xyz = points[remaining]
                hit = np.all((xyz >= lo[cell]) & (xyz <= hi[cell]), axis=1)
                selected[remaining[hit]] = cell
    outside = np.flatnonzero(selected < 0)
    if len(outside):
        raise Q2RefError(f"Q2REF_POINT_OUTSIDE: {points[outside[0]].tolist()}")
    return selected


def fem_q2_point_values(q2: Q2Mesh, xyz: np.ndarray, field: str = "U") -> np.ndarray:
    """Locate points with an AABB tree and evaluate the tensor Lagrange basis."""
    points = _array(xyz, "xyz")
    if points.ndim != 2 or points.shape[1] != 3:
        raise Q2RefError("Q2REF_BAD_ARG: xyz must have shape (n,3)")
    if not isinstance(field, str) or field not in ("U", "T"):
        raise Q2RefError("Q2REF_BAD_ARG: field must be U or T")
    _finite(xyz=points)
    span = _validate_mesh(q2)
    tree, lo, hi = _box_tree(q2, span)
    result = np.empty((len(points), 3) if field == "U" else (len(points),), dtype=np.float64)
    for start in range(0, len(points), _POINT_BATCH):
        stop = start + _POINT_BATCH
        batch = points[start:stop]
        cells = _locate(tree, lo, hi, batch)
        xi = 2 * ((batch - q2.lo[cells]) / span[cells]) - 1
        basis = np.stack((xi * (xi - 1) / 2, 1 - xi**2, xi * (xi + 1) / 2), axis=-1)
        if field == "U":
            result[start:stop] = np.einsum("ni,nj,nk,nijkc->nc", basis[:, 0], basis[:, 1],
                                          basis[:, 2], q2.U[cells], optimize=True)
        else:
            result[start:stop] = np.einsum("ni,nj,nk,nijk->n", basis[:, 0], basis[:, 1],
                                          basis[:, 2], q2.T[cells], optimize=True)
    _finite(result=result)
    return result


def node_selfcheck(q2: Q2Mesh) -> float:
    """Check both fields at every tensor node, including shared cell boundaries."""
    span = _validate_mesh(q2)
    weights = np.indices((3, 3, 3)).reshape(3, -1).T / 2
    scales = {field: max(float(np.max(np.abs(getattr(q2, field)))), np.finfo(np.float64).tiny)
              for field in ("U", "T")}
    error = 0.0
    for start in range(0, len(q2.gid), _CELL_BATCH):
        stop = start + _CELL_BATCH
        nodes = q2.lo[start:stop, None, :] + weights[None, :, :] * span[start:stop, None, :]
        nodes = np.where(weights[None, :, :] == 1, q2.hi[start:stop, None, :], nodes)
        points = nodes.reshape(-1, 3)
        for field in ("U", "T"):
            stored = getattr(q2, field)[start:stop]
            actual = fem_q2_point_values(q2, points, field).reshape(stored.shape)
            error = max(error, float(np.max(np.abs(actual - stored))) / scales[field])
    return error


def uniform_top_centroids(x0: float, y0: float, x1: float, y1: float, nx: int, ny: int) -> np.ndarray:
    """Uniform, uncut strip centroids in x-fastest order (padded_cells=0)."""
    values = _scalars(x0=x0, y0=y0, x1=x1, y1=y1, nx=nx, ny=ny)
    _finite(**values)
    if any(not isinstance(n, Integral) or isinstance(n, (bool, np.bool_)) or n <= 0 for n in (nx, ny)):
        raise Q2RefError("Q2REF_BAD_ARG: nx and ny must be positive integers")
    if x1 <= x0 or y1 <= y0:
        raise Q2RefError("Q2REF_BAD_ARG: bbox must have positive area")
    # Preserve the specified scalar formula's multiply/divide/add order exactly.
    x = x0 + (np.arange(nx, dtype=np.float64) + 0.5) * (x1 - x0) / nx
    y = y0 + (np.arange(ny, dtype=np.float64) + 0.5) * (y1 - y0) / ny
    result = np.column_stack((np.tile(x, ny), np.repeat(y, nx)))
    _finite(centroids=result)
    return result


def _xy_array(xy):
    points = _array(xy, "xy")
    if points.ndim != 2 or points.shape[1] != 2:
        raise Q2RefError("Q2REF_BAD_ARG: xy must have shape (n,2)")
    return points


def plane_removed_pv(xy: np.ndarray, w: np.ndarray) -> float:
    """Peak-to-valley residual of the existing shared least-squares plane fit."""
    points, values = _xy_array(xy), _array(w, "w")
    if values.shape != (len(points),):
        raise Q2RefError("Q2REF_BAD_ARG: w must have shape (n,)")
    _finite(xy=points, w=values)
    if not len(points):
        raise Q2RefError("Q2REF_BAD_ARG: xy must not be empty")
    try:
        _, residual = best_fit_plane(points[:, 0], points[:, 1], values)
    except (ValueError, np.linalg.LinAlgError) as exc:
        raise Q2RefError(f"Q2REF_BAD_ARG: plane fit failed: {exc}") from exc
    _finite(residual=residual)
    pv = float(np.max(residual) - np.min(residual))
    _finite(pv=pv)
    return pv


def same_point_pv(q2: Q2Mesh, xy: np.ndarray, z_top: float) -> dict:
    """Check the entire mesh, then evaluate top-surface uz and remove its plane.

    eval_seconds includes node selfcheck, point evaluation and plane removal.
    It is a measurement only and never participates in a pass decision.
    """
    points = _xy_array(xy)
    values = _scalars(z_top=z_top)
    _finite(xy=points, **values)
    if not len(points):
        raise Q2RefError("Q2REF_BAD_ARG: xy must not be empty")
    start = perf_counter()
    check = node_selfcheck(q2)
    if not math.isfinite(check) or check > _REL_TOL:
        raise Q2RefError(f"Q2REF_SELFCHECK: relative node error {check!r} exceeds {_REL_TOL}")
    xyz = np.column_stack((points, np.full(len(points), values["z_top"])))
    displacement = fem_q2_point_values(q2, xyz)
    pv = plane_removed_pv(points, displacement[:, 2])
    return {"pv": pv, "n_points": len(points), "n_elements": len(q2.gid),
            "selfcheck_rel": float(check), "eval_seconds": perf_counter() - start}


def richardson(f_coarse: float, f_medium: float, f_fine: float, ratio: float = 2.0, fs: float = 1.25) -> dict:
    """Report observed convergence and GCI; only monotone data has an extrapolate."""
    values = _scalars(f_coarse=f_coarse, f_medium=f_medium, f_fine=f_fine, ratio=ratio, fs=fs)
    _finite(**values)
    f_coarse, f_medium, f_fine, ratio, fs = values.values()
    if ratio <= 1 or fs <= 0:
        raise Q2RefError("Q2REF_BAD_ARG: ratio must exceed 1 and fs must be positive")
    result = {"R": None, "p": None, "f_ext": None, "gci_fine": None, "status": "INDETERMINATE"}
    if f_coarse == f_medium:
        return result
    if f_medium == f_fine:
        result["R"] = 0.0
        return result
    coarse_delta, fine_delta = f_coarse - f_medium, f_medium - f_fine
    _finite(coarse_delta=coarse_delta, fine_delta=fine_delta)
    R = fine_delta / coarse_delta
    _finite(R=R)
    result["R"] = R
    if R < 0:
        result["status"] = "OSCILLATORY"
    elif R >= 1:
        result["status"] = "DIVERGENT"
    elif R == 0:
        raise Q2RefError("Q2REF_BAD_ARG: convergence ratio underflows float64")
    else:
        # (1-R)/R equals ratio**p-1, with no power overflow for large p.
        correction = (f_fine - f_medium) * R / (1 - R)
        p = -math.log(R) / math.log(ratio)
        f_ext = f_fine + correction
        _finite(p=p, f_ext=f_ext)
        result.update(p=p, f_ext=f_ext, status="MONOTONE")
        # Relative GCI is undefined at zero; the order and extrapolate remain valid.
        if f_fine != 0:
            gci = fs * abs(correction / f_fine)
            _finite(gci_fine=gci)
            result["gci_fine"] = gci
    return result


def dual_reference(fvm_pv: float, exact_point_pv: float, richardson_pv: float, tol: float) -> dict:
    """Compare against both references using the required explicit tolerance."""
    values = _scalars(fvm_pv=fvm_pv, exact_point_pv=exact_point_pv, richardson_pv=richardson_pv, tol=tol)
    _finite(**values)
    fvm_pv, exact_point_pv, richardson_pv, tol = values.values()
    if tol <= 0 or exact_point_pv == 0 or richardson_pv == 0:
        raise Q2RefError("Q2REF_BAD_ARG: tol must be positive and reference PVs must be nonzero")
    e_exact, e_richardson = fvm_pv / exact_point_pv - 1, fvm_pv / richardson_pv - 1
    _finite(e_exact=e_exact, e_richardson=e_richardson)
    pass_exact, pass_richardson = abs(e_exact) <= tol, abs(e_richardson) <= tol
    status = "PASS" if pass_exact and pass_richardson else "FAIL"
    if pass_exact != pass_richardson:
        status = "REFERENCE-DISAGREE"
    return {"e_exact": e_exact, "e_richardson": e_richardson, "pass_exact": pass_exact,
            "pass_richardson": pass_richardson, "status": status}


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise Q2RefError(f"Q2REF_BAD_ARG: {message}")


def main(argv=None):
    parser = _ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True, parser_class=_ArgumentParser)
    point = commands.add_parser("point-pv", help="evaluate FEM Q2 at uniform top centroids")
    point.add_argument("--fem-run", type=Path, required=True)
    point.add_argument("--bbox", type=float, nargs=4, required=True, metavar=("X0", "Y0", "X1", "Y1"))
    point.add_argument("--nx", type=int, required=True)
    point.add_argument("--ny", type=int, required=True)
    point.add_argument("--z-top", type=float, required=True)
    point.add_argument("--data-class", choices=("synthetic", "public", "real"), required=True)
    point.add_argument("--json", type=Path)
    dual = commands.add_parser("dual", help="compare FVM PV against two references")
    for name in ("fvm-pv", "exact-pv", "richardson-pv", "tol"):
        dual.add_argument(f"--{name}", type=float, required=True)
    dual.add_argument("--json", type=Path)
    try:
        args = parser.parse_args(argv)
        if args.command == "point-pv":
            # All numeric CLI types/shapes have been checked by argparse. Check
            # finiteness across both groups before either group's range checks.
            _finite(bbox=args.bbox, z_top=args.z_top)
            xy = uniform_top_centroids(*args.bbox, args.nx, args.ny)
            result = same_point_pv(load_fem_q2(args.fem_run), xy, args.z_top)
            exit_code = 0
        else:
            result = dual_reference(args.fvm_pv, args.exact_pv, args.richardson_pv, args.tol)
            exit_code = {"PASS": 0, "FAIL": 1, "REFERENCE-DISAGREE": 3}[result["status"]]
        record = json.dumps(result, allow_nan=False)
        if args.json is not None:
            try:
                # Keep the reused policy's existing h5py dependency off the core
                # numerical import path. No HDF5 file is read by this tool.
                from tools.vv.field_compare import ensure_output_allowed
                ensure_output_allowed(args.json, args.data_class if args.command == "point-pv" else "synthetic")
                args.json.write_text(record + "\n", encoding="utf-8")
            except (OSError, ValueError, ImportError) as exc:
                raise Q2RefError(f"Q2REF_BAD_ARG: cannot write {args.json}: {exc}") from exc
        print(record)
        return exit_code
    except Q2RefError as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
