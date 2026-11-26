# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Same-point scheme A acceptance helpers, independent of solver internals.

PV always uses the supplied discrete samples. No midpoint is inserted and no
analytic extremum is extrapolated. ``fem_top_interpolate`` returns the internal
pair ``(values, relative_stencil_indicator)``. Its indicator compares the primary
quadratic stencil with a one-node shift in each available coordinate direction.
"""

from functools import wraps
import json
import math
from numbers import Integral, Real
from pathlib import Path

import h5py
import numpy as np

from tools.vv.field_compare import InputInvalid, _remove_rigid, load_run


FVM_METHOD = "FVM top-face centroids; least-squares plane removal"
_TOP_TOL = 1e-14


def _require(condition, message):
    if not condition:
        raise InputInvalid(message)


def _input_errors(function):
    @wraps(function)
    def checked(*args, **kwargs):
        try:
            with np.errstate(over="raise", invalid="raise", divide="raise"):
                return function(*args, **kwargs)
        except InputInvalid:
            raise
        except (OSError, ValueError, TypeError, KeyError, IndexError, AttributeError,
                RuntimeError, OverflowError, FloatingPointError, np.linalg.LinAlgError) as exc:
            raise InputInvalid(f"{function.__name__}: {exc}") from exc
    return checked


def _finite(value, name):
    _require(isinstance(value, Real) and not isinstance(value, (bool, np.bool_)),
             f"{name}: finite number required")
    value = float(value)
    _require(math.isfinite(value), f"{name}: finite number required")
    return value


def _positive_int(value, name):
    _require(isinstance(value, Integral) and not isinstance(value, (bool, np.bool_))
             and value > 0, f"{name}: positive integer required")
    return int(value)


def _array(group, name, *, shape=None, integer=False):
    _require(name in group and isinstance(group[name], h5py.Dataset),
             f"{group.name}/{name}: dataset absent")
    value = group[name][...]
    _require(value.dtype.kind in ("iu" if integer else "iuf"),
             f"{group.name}/{name}: invalid numeric type")
    _require(shape is None or value.shape == shape, f"{group.name}/{name}: invalid shape")
    _require(np.all(np.isfinite(value)), f"{group.name}/{name}: nonfinite data")
    return value


def _count(group, name):
    value = _array(group, name, shape=(1,), integer=True)
    _require(value[0] >= 0, f"{group.name}/{name}: nonnegative single-partition count required")
    return int(value[0])


def _rings(group, offsets_name, conn_name, count, size, upper):
    offsets = _array(group, offsets_name, shape=(count + 1,), integer=True)
    conn = _array(group, conn_name, shape=(size,), integer=True)
    _require(offsets[0] == 0 and offsets[-1] == size
             and np.all(offsets[1:] > offsets[:-1]), f"{group.name}: invalid {offsets_name}")
    _require(np.all(conn >= 0) and np.all(conn < upper),
             f"{group.name}: invalid {conn_name}")
    rings = [conn[int(a):int(b)] for a, b in zip(offsets[:-1], offsets[1:])]
    _require(all(len(np.unique(ring)) == len(ring) for ring in rings),
             f"{group.name}: repeated connectivity id")
    return rings


def _geometry_block(group, *, cut):
    npoints = _count(group, "NumberOfPoints")
    ncells = _count(group, "NumberOfCells")
    nconn = _count(group, "NumberOfConnectivityIds")
    points = _array(group, "Points", shape=(npoints, 3)).astype(float)
    cells = _rings(group, "Offsets", "Connectivity", ncells, nconn, npoints)
    types = _array(group, "Types", shape=(ncells,), integer=True)
    _require(np.all(types == (42 if cut else 12)), f"{group.name}: invalid cell types")
    _require(all(len(cell) >= 4 if cut else len(cell) == 8 for cell in cells),
             f"{group.name}: invalid cell vertex count")
    faces = []
    if cut and (ncells or "FaceOffsets" in group):
        nfaces = _count(group, "NumberOfFaces")
        nfconn = _count(group, "NumberOfFaceConnectivityIds")
        faces = _rings(group, "FaceOffsets", "FaceConnectivity", nfaces, nfconn, npoints)
        _require(all(len(face) >= 3 for face in faces), "cut face needs at least three vertices")
        npoly = _count(group, "NumberOfPolyhedronToFaceIds")
        poly = _rings(group, "PolyhedronOffsets", "PolyhedronToFaces", ncells, npoly, nfaces)
        used = set()
        for cell, face_ids in zip(cells, poly):
            _require(len(face_ids) >= 4, "cut polyhedron needs at least four faces")
            vertices = {int(v) for i in face_ids for v in faces[int(i)]}
            _require(vertices == set(map(int, cell)), "cut cell/face connectivity mismatch")
            used.update(map(int, face_ids))
        _require(used == set(range(nfaces)), "unreferenced cut face")
    return points, cells, faces


def _polygon_centroid(xy):
    # Translate before cross products to avoid cancellation far from the origin.
    origin = xy[0]
    local = xy - origin
    following = np.roll(local, -1, axis=0)
    cross = local[:, 0] * following[:, 1] - following[:, 0] * local[:, 1]
    area2 = float(np.sum(cross))
    _require(math.isfinite(area2) and area2 != 0, "zero-area top face")
    result = origin + np.sum((local + following) * cross[:, None], axis=0) / (3 * area2)
    _require(np.all(np.isfinite(result)), "nonfinite top face centroid")
    return result


@_input_errors
def top_face_centroids(run_dir, requested_nxny):
    """Read ordered serial top faces and verify the uncut finest grid request.

    ``nx0,ny0`` are coarse dimensions and ``L`` is the finest level index.
    The caller supplies the independently written case's finest dimensions.
    """
    _require(isinstance(requested_nxny, tuple) and len(requested_nxny) == 2,
             "requested_nxny must be an (nx,ny) tuple")
    requested = tuple(_positive_int(value, name)
                      for value, name in zip(requested_nxny, ("requested nx", "requested ny")))
    run_dir = Path(run_dir)
    manifest = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
    _require(manifest["schema_version"] == "chipcae.results/1.0", "invalid manifest schema")
    step = manifest["steps"][0]["file"]
    _require(isinstance(step, str) and bool(step), "invalid steps[0].file")
    with h5py.File(run_dir / step, "r") as data:
        blocks = [_geometry_block(data["VTKHDF/" + name], cut=name == "cut")
                  for name in ("hex", "cut")]
    tops = [float(points[:, 2].max()) for points, _, _ in blocks if len(points)]
    _require(bool(tops), "no geometry points")
    z_top = max(tops)
    result = []
    for index, (points, cells, faces) in enumerate(blocks):
        for ring in (cells if index == 0 else faces):
            vertices = points[ring]
            top = np.abs(vertices[:, 2] - z_top) < _TOP_TOL
            if index == 0 and np.count_nonzero(top) == 4:
                result.append(np.mean(vertices[top, :2], axis=0))
            elif index == 1 and np.all(top):
                result.append(_polygon_centroid(vertices[:, :2]))
    _require(len(result) >= 3, "at least three top face samples required")
    if not blocks[1][1]:
        mesh = manifest["mesh"]
        nx0, ny0 = (_positive_int(mesh[name], name) for name in ("nx0", "ny0"))
        level = mesh["L"]
        _require(isinstance(level, Integral) and not isinstance(level, (bool, np.bool_))
                 and level >= 0, "L: nonnegative integer required")
        # A larger L cannot fit either requested dimension; reject before a
        # potentially enormous integer left shift on malformed metadata.
        _require(level < min(requested).bit_length(), "L exceeds requested finest dimensions")
        finest = nx0 << int(level), ny0 << int(level)
        _require(finest == requested, "finest mesh dimensions differ from case request")
        padded = mesh["padded_cells"]
        _require(isinstance(padded, Integral) and not isinstance(padded, (bool, np.bool_))
                 and padded == 0, "uncut strip requires integer padded_cells == 0")
        _require(len(result) == finest[0] * finest[1],
                 "uncut top face sample count differs from (nx0*2^L)*(ny0*2^L)")
    result = np.asarray(result, dtype=float)
    _require(np.all(np.isfinite(result)), "nonfinite top face samples")
    return result


def _samples(xy):
    xy = np.asarray(xy)
    _require(xy.dtype.kind in "iuf" and xy.ndim == 2 and xy.shape[1] == 2
             and len(xy) >= 3 and np.all(np.isfinite(xy)), "invalid xy samples")
    return xy.astype(float)


@_input_errors
def plane_pv(xy, w):
    """Least-squares plane removal followed by sampled residual max minus min."""
    xy = _samples(xy)
    w = np.asarray(w)
    _require(w.dtype.kind in "iuf" and w.shape == (len(xy),) and np.all(np.isfinite(w)),
             "invalid sample values")
    centered = xy - xy[0]
    centered -= centered.mean(axis=0)
    scale = np.max(np.abs(centered), axis=0)
    _require(np.all(scale > 0), "plane samples do not span xy")
    matrix = np.column_stack((np.ones(len(xy)), centered / scale))
    values = w.astype(float) - float(w[0])
    values -= values.mean()
    coefficients, _, rank, _ = np.linalg.lstsq(matrix, values, rcond=None)
    _require(rank == 3, "plane samples do not span xy")
    residual = values - matrix @ coefficients
    pv = float(np.ptp(residual))
    _require(math.isfinite(pv), "nonfinite plane PV")
    return pv


def _stencil(nodes, targets):
    # Center on the containing interval; clip to a one-sided boundary stencil.
    return np.clip(np.searchsorted(nodes, targets, side="right") - 2, 0, len(nodes) - 3)


def _weights(nodes, starts, targets):
    local = nodes[starts[:, None] + np.arange(3)]
    weights = np.ones_like(local)
    for i in range(3):
        for j in range(3):
            if i != j:
                weights[:, i] *= (targets - local[:, j]) / (local[:, i] - local[:, j])
    return weights


def _interpolate(xs, ys, lattice, xy, ix, iy):
    wx, wy = _weights(xs, ix, xy[:, 0]), _weights(ys, iy, xy[:, 1])
    values = np.zeros(len(xy))
    for i in range(3):
        for j in range(3):
            values += wx[:, i] * wy[:, j] * lattice[ix + i, iy + j]
    return values


@_input_errors
def fem_top_interpolate(run_dir, xy):
    """Return quadratic top U_z samples and a relative shifted-stencil indicator.

    Boundary stencils are one-sided. Shifted stencils may extend past the target
    within the lattice; targets outside the complete top domain are rejected.
    """
    try:
        xy = _samples(xy)
        run = load_run(run_dir)
        _require(run.U is not None, "displacement absent")
        top = np.abs(run.points[:, 2] - np.max(run.points[:, 2])) < _TOP_TOL
        points, w = run.points[top, :2], run.U[top, 2]
        xs, xi = np.unique(points[:, 0], return_inverse=True)
        ys, yi = np.unique(points[:, 1], return_inverse=True)
        _require(len(xs) >= 3 and len(ys) >= 3 and max(len(xs), len(ys)) >= 4,
                 "two quadratic stencils unavailable")
        _require(len(points) == len(xs) * len(ys)
                 and len(np.unique(xi * len(ys) + yi)) == len(points),
                 "incomplete rectangular top lattice")
        _require(np.all(xy >= [xs[0], ys[0]]) and np.all(xy <= [xs[-1], ys[-1]]),
                 "extrapolation outside top domain")
        lattice = np.empty((len(xs), len(ys)))
        lattice[xi, yi] = w
        ix, iy = _stencil(xs, xy[:, 0]), _stencil(ys, xy[:, 1])
        values = _interpolate(xs, ys, lattice, xy, ix, iy)
        difference = 0.0
        for axis, nodes, starts in ((0, xs, ix), (1, ys, iy)):
            if len(nodes) < 4:
                continue
            alternate = starts + np.where(starts < len(nodes) - 3, 1, -1)
            other = _interpolate(xs, ys, lattice, xy,
                                 alternate if axis == 0 else ix,
                                 alternate if axis == 1 else iy)
            difference = max(difference, float(np.max(np.abs(values - other))))
        pv = plane_pv(xy, values)
        _require(pv > 0, "nonpositive reference PV")
        indicator = difference / pv
        _require(math.isfinite(indicator) and np.all(np.isfinite(values)),
                 "nonfinite reference interpolation")
        return values, indicator
    except InputInvalid as exc:
        raise InputInvalid(f"参考插值不足：{exc}") from exc


@_input_errors
def same_point_error(fvm_run_dir, fem_run_dir):
    """Use manifest face PV and signed relative errors against the FEM reference.

    Read the written ``case.json`` in the result directory, or beside it in
    the CLI input directory. Never infer the request from result metadata.
    """
    fvm, fem = load_run(fvm_run_dir), load_run(fem_run_dir)
    _require(fvm.qoi_method["warpage_pv"] == FVM_METHOD, "incorrect FVM warpage method")
    fvm_pv, nodal_pv = fvm.qoi["warpage_pv"], fem.qoi["warpage_pv"]
    _require(fvm_pv > 0 and nodal_pv > 0, "positive finite manifest PV required")
    case_path = Path(fvm_run_dir) / "case.json"
    if not case_path.is_file():
        case_path = Path(fvm_run_dir).parent / "case.json"
    case = json.loads(case_path.read_text(encoding="utf-8"))
    requested = tuple(_positive_int(case["mesh"][name], f"case mesh.{name}")
                      for name in ("nx", "ny"))
    xy = top_face_centroids(fvm_run_dir, requested)
    values, indicator = fem_top_interpolate(fem_run_dir, xy)
    same_pv = plane_pv(xy, values)
    _require(same_pv > 0, "positive finite same-point PV required")
    same_rel, nodal_rel = (fvm_pv - same_pv) / same_pv, fvm_pv / nodal_pv - 1
    _require(math.isfinite(same_rel) and math.isfinite(nodal_rel), "nonfinite relative PV error")
    return dict(fvm_pv=fvm_pv, fem_same_point_pv=same_pv, same_point_rel=same_rel,
                fem_nodal_pv=nodal_pv, nodal_rel=nodal_rel,
                interp_indicator=indicator, n_samples=len(xy))


@_input_errors
def deformation_max(run_dir):
    """Maximum displacement norm after the existing six-mode rigid removal."""
    run = load_run(run_dir)
    _require(run.U is not None and np.all(np.isfinite(run.U)), "finite displacement required")
    result = float(np.max(np.linalg.norm(_remove_rigid(run.points, run.U), axis=1)))
    _require(math.isfinite(result), "nonfinite deformation maximum")
    return result


def require_convergence_quantity(name):
    """Guard mechanical convergence analysis against rigid-dependent raw U."""
    if name not in ("warpage_pv", "deformation_max"):
        raise ValueError(f"unsupported mechanical convergence quantity: {name}")


@_input_errors
def scheme_a_gate(records):
    """Judge required kz4/kz8 pairs; kz16 is finite diagnostic data only.

    Records are uniquely keyed by (nx,ny,kz), with duplicates rejected. Ordinary
    grids are mandatory; additional nx>=96 grids use the same limits. Missing
    pairs fail explicitly. Checks sort by (nx,ny,kz,criterion).
    """
    _require(isinstance(records, list), "records must be a list")
    indexed = {}
    grids = {(48, 16), (96, 32)}
    for record in records:
        _require(isinstance(record, dict), "invalid gate record")
        nx, ny, kz = (_positive_int(record[name], name) for name in ("nx", "ny", "kz"))
        _require(((nx, ny) == (48, 16) or nx >= 96) and kz in (4, 8, 16),
                 "unsupported scheme A grid or thickness")
        key = nx, ny, kz
        _require(key not in indexed, "duplicate (nx,ny,kz) record")
        error, indicator, deformation = (_finite(record[name], name) for name in
                                        ("same_point_rel", "interp_indicator", "deformation_rel"))
        _require(indicator >= 0, "negative interpolation indicator")
        direction = None
        if kz != 16:
            try:
                fvm, fem = (_finite(record.get(name), name)
                            for name in ("fvm_curvature", "fem_curvature"))
                if fvm != 0 and fem != 0:
                    direction = 1 if (fvm > 0) == (fem > 0) else -1
            except InputInvalid:
                pass  # Unresolved direction is an explicit failed gate check.
        indexed[key] = error, indicator, deformation, direction
        if kz != 16:
            grids.add((nx, ny))
    checks = []

    def check(nx, ny, kz, name, value, limit, passed, reason):
        checks.append(dict(nx=nx, ny=ny, kz=kz, name=name, value=value, limit=limit,
                           **{"pass": bool(passed)}, reason="" if passed else reason))

    for nx, ny in sorted(grids):
        for kz in (4, 8):
            key = nx, ny, kz
            check(nx, ny, kz, "required_run", kz if key in indexed else None, kz,
                  key in indexed, "缺 kz≥8 运行" if kz == 8 else "缺 kz4 运行")
            if key not in indexed:
                continue
            error, indicator, deformation, direction = indexed[key]
            tau = (.03 if nx == 48 else .015) if kz == 4 else (.010 if nx == 48 else .005)
            check(nx, ny, kz, "same_point_abs", abs(error), tau, abs(error) <= tau,
                  "同点 PV 误差超门")
            check(nx, ny, kz, "deformation_abs", abs(deformation), .05, abs(deformation) <= .05,
                  "去刚体变形误差超门")
            limit = min(tau / 10, 1e-3)
            check(nx, ny, kz, "interp_indicator", indicator, limit, indicator <= limit,
                  "参考插值不足")
            check(nx, ny, kz, "curvature_direction", direction, 1, direction == 1,
                  "翘曲方向相反或不可判")
        e4 = indexed.get((nx, ny, 4))
        e8 = indexed.get((nx, ny, 8))
        p = None
        if e4 is not None and e8 is not None:
            coarse, fine = e4[0], e8[0]
            if (abs(coarse) > 1e-3 and 0 < abs(fine) < abs(coarse)
                    and (coarse > 0) == (fine > 0)):
                ratio = abs(coarse) / abs(fine)
                if math.isfinite(ratio):
                    p = math.log2(ratio)
        check(nx, ny, 8, "p_kz", p, 1.5, p is not None and p >= 1.5,
              "阶不可判" if p is None else "厚度收敛阶 <1.5")
    checks.sort(key=lambda c: (c["nx"], c["ny"], c["kz"], c["name"]))
    return {"pass": all(c["pass"] for c in checks), "checks": checks}


@_input_errors
def scheme_a_preconditions(nx, kz_min, same_point_rel, nodal_rel, legacy_gate):
    """Add thickness and same-point checks using only the caller's legacy gate."""
    _positive_int(nx, "nx")
    kz_min = _positive_int(kz_min, "kz_min")
    legacy_gate = _finite(legacy_gate, "legacy_gate")
    _require(legacy_gate > 0, "legacy_gate must be positive")
    same_point_rel = _finite(same_point_rel, "same_point_rel")
    checks = [dict(name="kz_min>=8", value=kz_min, limit=8, **{"pass": kz_min >= 8}),
              dict(name="same_point_abs<=legacy_gate", value=abs(same_point_rel),
                   limit=legacy_gate, **{"pass": abs(same_point_rel) <= legacy_gate})]
    if nodal_rel is not None:
        nodal_rel = _finite(nodal_rel, "nodal_rel")
        checks.append(dict(name="nodal_abs<=legacy_gate", value=abs(nodal_rel), limit=legacy_gate,
                           **{"pass": abs(nodal_rel) <= legacy_gate}))
    passed = all(c["pass"] for c in checks)
    return {"pass": passed, "checks": checks,
            "reason": "; ".join(c["name"] for c in checks if not c["pass"])}
