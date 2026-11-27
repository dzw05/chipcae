# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""INT-a scheme A decisions on supplied data; no solver or FEM evaluator."""

import json
import math
import numbers
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import h5py
import numpy as np

from .warpage_qoi import best_fit_plane
from .field_compare import _remove_rigid


class InputInvalid(ValueError):
    """Missing, malformed, ambiguous, or nonfinite input data."""


_FVM_METHOD = "FVM top-face centroids; least-squares plane removal"
_REFERENCE_XY = ((12, 4), (24, 8), (48, 16), (96, 32))


def _require(condition, reason):
    if not condition:
        raise InputInvalid(reason)


def _integer(value, name, minimum=1):
    _require(isinstance(value, numbers.Integral) and not isinstance(value, (bool, np.bool_)),
             f"{name} must be an integer")
    _require(value >= minimum, f"{name} must be >= {minimum}")
    return int(value)


def _number(value, name, positive=False):
    _require(isinstance(value, numbers.Real) and not isinstance(value, (bool, np.bool_)),
             f"{name} must be a real number")
    try:
        result = float(value)
    except (OverflowError, ValueError) as exc:
        raise InputInvalid(f"invalid {name}") from exc
    _require(math.isfinite(result), f"{name} must be finite")
    if positive:
        _require(result > 0, f"{name} must be positive")
    return result


def _array(values, name):
    try:
        raw = np.asarray(values)
        _require(raw.dtype.kind in "iuf", f"{name} must be numeric")
        result = np.asarray(raw, dtype=np.float64)
    except (TypeError, ValueError, OverflowError) as exc:
        raise InputInvalid(f"invalid {name}: {exc}") from exc
    _require(np.isfinite(result).all(), f"{name} must be finite")
    return result


def plane_pv(xy, w) -> float:
    """PV after the shared least-squares plane removal, in binary64."""
    xy, w = _array(xy, "xy"), _array(w, "w")
    _require(xy.ndim == 2 and xy.shape[1] == 2 and len(xy) >= 3,
             "xy must have shape (n,2), n>=3")
    _require(w.shape == (len(xy),), "w must have shape (n,)")
    try:
        _, residual = best_fit_plane(xy[:, 0], xy[:, 1], w)
        return _number(np.ptp(residual), "plane PV")
    except np.linalg.LinAlgError as exc:
        raise InputInvalid(f"plane removal failed: {exc}") from exc


def _json(path):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError, UnicodeError) as exc:
        raise InputInvalid(f"cannot read {path}: {exc}") from exc
    _require(isinstance(data, dict), f"{path} must contain an object")
    return data


def _first_step(manifest):
    steps = manifest.get("steps")
    _require(isinstance(steps, list) and steps and isinstance(steps[0], dict),
             "manifest requires steps[0]")
    return steps[0]


def _int_array(block, name):
    _require(name in block, f"missing {block.name}/{name}")
    values = np.asarray(block[name])
    _require(values.ndim == 1 and values.dtype.kind in "iu",
             f"{block.name}/{name} must be a one-dimensional integer array")
    _require((values >= 0).all(), f"negative {block.name}/{name}")
    return values


def _count(block, name):
    values = _int_array(block, name)
    _require(len(values) == 1, f"{block.name}/{name} requires exactly one partition")
    return int(values[0])


def _offsets(block, name, size, count):
    offsets = _int_array(block, name)
    _require(len(offsets) == count + 1 and offsets[0] == 0 and offsets[-1] == size,
             f"inconsistent {block.name}/{name}")
    _require((offsets[1:] > offsets[:-1]).all(), f"empty/reversed {block.name}/{name}")
    return offsets


def _block(block, cut):
    npoints = _count(block, "NumberOfPoints")
    ncells = _count(block, "NumberOfCells")
    nconn = _count(block, "NumberOfConnectivityIds")
    _require("Points" in block, f"missing {block.name}/Points")
    points = _array(block["Points"], "Points")
    _require(points.shape == (npoints, 3), "Points count/shape mismatch")
    conn = _int_array(block, "Connectivity")
    _require(len(conn) == nconn and (conn < npoints).all(), "invalid cell connectivity")
    offsets = _offsets(block, "Offsets", nconn, ncells)
    _require((npoints == 0) == (ncells == 0), "inconsistent empty block")
    if not cut:
        _require((np.diff(offsets) == 8).all(), "hex cells require eight vertices")
        return points, conn, offsets, None, None
    faces = _int_array(block, "FaceConnectivity")
    _require((faces < npoints).all(), "face point out of bounds")
    nfaces = _count(block, "NumberOfFaces")
    _require(_count(block, "NumberOfFaceConnectivityIds") == len(faces),
             "face connectivity count mismatch")
    face_offsets = _offsets(block, "FaceOffsets", len(faces), nfaces)
    _require((np.diff(face_offsets) >= 3).all(), "faces require at least three vertices")
    refs = _int_array(block, "PolyhedronToFaces")
    _require(_count(block, "NumberOfPolyhedronToFaceIds") == len(refs),
             "polyhedron face count mismatch")
    _require((refs < nfaces).all(), "polyhedron face reference out of bounds")
    poly_offsets = _offsets(block, "PolyhedronOffsets", len(refs), ncells)
    _require(set(map(int, refs)) == set(range(nfaces)), "unreferenced cut face")
    # Check each referenced face belongs to its cell, not merely to the block.
    for i in range(ncells):
        vertices = set(map(int, conn[offsets[i]:offsets[i + 1]]))
        cell_refs = refs[poly_offsets[i]:poly_offsets[i + 1]]
        _require(len(set(map(int, cell_refs))) == len(cell_refs), "duplicate cell face")
        for face in cell_refs:
            ids = faces[face_offsets[face]:face_offsets[face + 1]]
            _require(set(map(int, ids)) <= vertices, "face not in polyhedron connectivity")
    return points, conn, offsets, faces, face_offsets


def _polygon_centroid(xy):
    # Translation reduces cancellation without changing the signed moments.
    local = xy - xy[0]
    nxt = np.roll(local, -1, axis=0)
    cross = local[:, 0] * nxt[:, 1] - nxt[:, 0] * local[:, 1]
    area2 = float(np.sum(cross))
    _require(math.isfinite(area2) and area2 != 0, "zero/nonfinite top polygon area")
    centroid = xy[0] + np.sum((local + nxt) * cross[:, None], axis=0) / (3 * area2)
    _require(np.isfinite(centroid).all(), "nonfinite top polygon centroid")
    return centroid


def top_face_centroids(run_dir, requested_nxny) -> np.ndarray:
    """Reconstruct global top samples, hex-cell order then cut-face order."""
    _require(isinstance(requested_nxny, (tuple, list, np.ndarray))
             and len(requested_nxny) == 2, "requested_nxny requires two integers")
    requested = tuple(_integer(v, "requested dimension") for v in requested_nxny)
    manifest = _json(Path(run_dir) / "manifest.json")
    mesh = manifest.get("mesh")
    _require(isinstance(mesh, dict), "missing manifest mesh")
    nx0, ny0 = (_integer(mesh.get(k), k) for k in ("nx0", "ny0"))
    level = _integer(mesh.get("L"), "L", 0)
    _require(_integer(mesh.get("padded_cells"), "padded_cells", 0) == 0,
             "padded_cells must be zero")
    # Compare via bit lengths before shifting, so malformed huge L cannot allocate.
    _require(level <= max(requested).bit_length(), "refined dimensions mismatch")
    nx, ny = nx0 << level, ny0 << level
    _require((nx, ny) == requested, "refined dimensions mismatch")
    filename = _first_step(manifest).get("file")
    _require(isinstance(filename, str) and filename, "missing first-step file")
    try:
        with h5py.File(Path(run_dir) / filename, "r") as data:
            blocks = [_block(data["VTKHDF"][name], name == "cut") for name in ("hex", "cut")]
    except (OSError, KeyError, TypeError, ValueError, OverflowError) as exc:
        raise InputInvalid(f"invalid VTKHDF: {exc}") from exc
    nonempty = [b[0] for b in blocks if len(b[0])]
    _require(nonempty, "empty mesh")
    z_top = max(float(p[:, 2].max()) for p in nonempty)
    samples = []
    points, conn, offsets, _, _ = blocks[0]
    top = np.abs(points[:, 2] - z_top) < 1e-14
    for begin, end in zip(offsets[:-1], offsets[1:]):
        ids = conn[begin:end]
        selected = ids[top[ids]]
        if len(selected) == 4:
            samples.append(points[selected, :2].mean(axis=0))
    points, _, _, faces, offsets = blocks[1]
    top = np.abs(points[:, 2] - z_top) < 1e-14
    for begin, end in zip(offsets[:-1], offsets[1:]):
        ids = faces[begin:end]
        if top[ids].all():
            samples.append(_polygon_centroid(points[ids, :2]))
    _require(len(samples) >= 3, "fewer than three top samples")
    if not len(blocks[1][0]):
        _require(len(samples) == nx * ny, "uncut strip top sample count mismatch")
    return np.asarray(samples, dtype=np.float64)


def _manifest_pv(run_dir, fvm=False):
    manifest = _json(Path(run_dir) / "manifest.json")
    step = _first_step(manifest).get("n")
    _require(step is not None, "missing first step number")
    summary = manifest.get("summary")
    _require(isinstance(summary, list), "missing manifest summary")
    entries = [e for e in summary if isinstance(e, dict)
               and e.get("name") == "warpage_pv" and e.get("step") == step]
    _require(len(entries) == 1, "missing or duplicate first-step warpage_pv")
    if fvm:
        _require(entries[0].get("method") == _FVM_METHOD, "incorrect FVM warpage_pv method")
    return _number(entries[0].get("value"), "manifest warpage_pv", positive=True)


def same_point_record(fvm_run_dir, fem_run_dir, fem_same_point_pv) -> dict:
    """Compare manifest FVM PV with one caller-supplied same-point evaluation."""
    mesh = _json(Path(fvm_run_dir) / "case.json").get("mesh")
    _require(isinstance(mesh, dict), "missing case mesh")
    requested = tuple(_integer(mesh.get(k), f"case mesh.{k}") for k in ("nx", "ny"))
    xy = top_face_centroids(fvm_run_dir, requested)
    fvm = _manifest_pv(fvm_run_dir, fvm=True)
    nodal = _manifest_pv(fem_run_dir)
    _require(callable(fem_same_point_pv), "same-point callback is required")
    try:
        same = fem_same_point_pv(fem_run_dir, xy)
    except Exception as exc:
        raise InputInvalid(f"same-point callback failed: {exc}") from exc
    same = _number(same, "same-point PV", positive=True)
    return dict(fvm_pv=fvm, fem_same_point_pv=same,
                same_point_rel=_number((fvm - same) / same, "same-point error"),
                fem_nodal_pv=nodal, nodal_rel=_number(fvm / nodal - 1, "nodal error"),
                n_samples=len(xy))


def require_convergence_quantity(name):
    """Reject raw displacement magnitudes and unspecified convergence quantities."""
    if name not in ("warpage_pv", "deformation_max"):
        raise ValueError(f"unsupported convergence quantity: {name}")


def _check(name, value, limit, passes, reason=""):
    return dict(name=name, value=value, limit=limit, **{"pass": bool(passes)},
                reason="" if passes else reason)


def _result(checks, **extra):
    return dict(extra, checks=checks, **{"pass": all(c["pass"] for c in checks)})


def _invalid_result(exc, **extra):
    return _result([_check("input", None, None, False, str(exc))], **extra)


def _key(record):
    _require(isinstance(record, dict), "record must be a dictionary")
    return tuple(_integer(record.get(k), k) for k in ("nx", "ny", "kz"))


def fem_reference_self_convergence(tiers) -> dict:
    """Select each FVM key's first converged adjacent kz2 FEM reference."""
    require_convergence_quantity("warpage_pv")
    try:
        _require(isinstance(tiers, (list, tuple)) and tiers, "empty/invalid FEM tiers")
        indexed = {}
        keys = set()
        for tier in tiers:
            key = _key(tier)
            _require(key not in indexed, f"duplicate FEM tier {key}")
            nodal = _number(tier.get("nodal_pv"), "nodal_pv", positive=True)
            same = tier.get("same_point_pv")
            _require(isinstance(same, dict) and same, "missing same_point_pv mapping")
            values = {}
            for fvm, pv in same.items():
                _require(isinstance(fvm, tuple) and len(fvm) == 3, "invalid FVM key")
                fvm = tuple(_integer(v, "FVM dimension") for v in fvm)
                values[fvm] = _number(pv, "same_point_pv", positive=True)
            keys.update(values)
            indexed[key] = (nodal, values)
        required = [(12, 4, 2), (12, 4, 4), *((nx, ny, 2) for nx, ny in _REFERENCE_XY[1:])]
        for key in required:
            _require(key in indexed, f"missing FEM tier {key}")
            _require(set(indexed[key][1]) == keys, f"missing FVM keys at FEM tier {key}")
    except InputInvalid as exc:
        return _invalid_result(exc, reference={})
    checks, reference = [], {}
    for fvm in sorted(keys):
        pv2, pv4 = indexed[(12, 4, 2)][1][fvm], indexed[(12, 4, 4)][1][fvm]
        thickness = abs(pv4 - pv2) / pv4
        checks.append(_check(f"thickness {fvm}", thickness, 1e-3, thickness <= 1e-3,
                             f"FEM kz2 厚度向未收敛: {thickness:.17g} > {1e-3:.17g}"))
        selected = None
        for coarse, fine in zip(_REFERENCE_XY[:-1], _REFERENCE_XY[1:]):
            cnode, csame = indexed[(*coarse, 2)]
            fnode, fsame = indexed[(*fine, 2)]
            same = abs(fsame[fvm] - csame[fvm]) / fsame[fvm]
            nodal = abs(fnode - cnode) / fnode
            if same <= 1e-3 and nodal <= 1e-3:
                selected = (*fine, 2)
                break
        if selected is not None:
            reference[fvm] = selected
        checks.append(_check(f"reference {fvm}", dict(same=same, nodal=nodal), 1e-3,
                             selected is not None,
                             f"参考未收敛: same={same:.17g}, nodal={nodal:.17g}"))
    return _result(checks, reference=reference)


def deformation_max(points, displacement) -> float:
    """Maximum displacement norm after shared infinitesimal rigid removal."""
    points, displacement = _array(points, "points"), _array(displacement, "displacement")
    _require(points.ndim == 2 and points.shape[1] == 3 and len(points) > 0,
             "points must have shape (n,3), n>0")
    _require(displacement.shape == points.shape, "displacement shape must match points")
    try:
        residual = _remove_rigid(points, displacement)
        return _number(np.linalg.norm(residual, axis=1).max(), "deformation_max")
    except np.linalg.LinAlgError as exc:
        raise InputInvalid(f"rigid removal failed: {exc}") from exc


def _order(name, coarse, fine, limit, thickness=False):
    if (coarse == 0 or fine == 0 or (coarse > 0) != (fine > 0)
            or abs(fine) >= abs(coarse) or (thickness and abs(coarse) <= 1e-3)):
        return _check(name, None, limit, False,
                      f"阶不可判: coarse={coarse:.17g}, fine={fine:.17g}")
    order = math.log2(abs(coarse) / abs(fine))
    return _check(name, order, limit, order >= limit,
                  f"{name}: {order:.17g} < {limit:.17g}")


def scheme_a_gate(records) -> dict:
    """Retain legacy kz4/G5 checks and require the added scheme A conditions."""
    require_convergence_quantity("warpage_pv")
    try:
        _require(isinstance(records, (list, tuple)) and records, "empty/invalid records")
        indexed = {}
        for record in records:
            key = _key(record)
            if key[2] == 16:
                continue
            _require(key[2] in (4, 8), f"unsupported gated kz: {key}")
            _require(key not in indexed, f"duplicate record {key}")
            _require(key[:2] == (48, 16) or (key[0] >= 96 and key[1] * 3 == key[0]),
                     f"unsupported strip dimensions {key}")
            indexed[key] = {name: _number(record.get(name), name) for name in
                            ("same_point_rel", "deformation_rel", "fvm_curvature", "fem_curvature")}
    except InputInvalid as exc:
        return _invalid_result(exc)
    checks = []
    groups = sorted({(48, 16), (96, 32)} | {key[:2] for key in indexed})
    for nx, ny in groups:
        for kz in (4, 8):
            key = (nx, ny, kz)
            if key not in indexed:
                checks.append(_check(f"record {key}", None, None, False,
                                     "缺 kz≥8 运行" if kz == 8 else "缺 kz4 运行（保留原门）"))
                continue
            record = indexed[key]
            error = abs(record["same_point_rel"])
            limit = (.03 if nx == 48 else .015) if kz == 4 else (.010 if nx == 48 else .005)
            checks.append(_check(f"same_point_abs {key}", error, limit, error <= limit,
                                 f"same_point_abs {key}: {error:.17g} > {limit:.17g}"))
            deformation = abs(record["deformation_rel"])
            checks.append(_check(f"deformation_abs {key}", deformation, .05, deformation <= .05,
                                 f"deformation_abs {key}: {deformation:.17g} > {0.05:.17g}"))
            fvm, fem = record["fvm_curvature"], record["fem_curvature"]
            checks.append(_check(f"curvature {key}", (fvm, fem), "nonzero, same sign",
                                 fvm != 0 and fem != 0 and (fvm > 0) == (fem > 0),
                                 f"curvature direction: FVM={fvm:.17g}, FEM={fem:.17g}"))
        if (nx, ny, 4) in indexed and (nx, ny, 8) in indexed:
            checks.append(_order(f"p_kz {(nx, ny)}", indexed[(nx, ny, 4)]["same_point_rel"],
                                 indexed[(nx, ny, 8)]["same_point_rel"], 1.5, thickness=True))
    for kz in (4, 8):
        if (48, 16, kz) in indexed and (96, 32, kz) in indexed:
            checks.append(_order(f"p_xy kz{kz}", indexed[(48, 16, kz)]["same_point_rel"],
                                 indexed[(96, 32, kz)]["same_point_rel"], 1.0))
    return _result(checks)


def scheme_a_preconditions(nx, kz_min, same_point_rel, nodal_rel, legacy_gate) -> dict:
    """Caller-supplied legacy threshold plus the mandatory kz>=8 prerequisite."""
    require_convergence_quantity("warpage_pv")
    try:
        _integer(nx, "nx")
        kz = _integer(kz_min, "kz_min")
        limit = _number(legacy_gate, "legacy_gate", positive=True)
        same = abs(_number(same_point_rel, "same_point_rel"))
        nodal = None if nodal_rel is None else abs(_number(nodal_rel, "nodal_rel"))
    except InputInvalid as exc:
        return _invalid_result(exc, reason=str(exc))
    checks = [_check("kz_min", kz, 8, kz >= 8, f"缺 kz≥8 运行: kz={kz}"),
              _check("same_point_abs", same, limit, same <= limit,
                     f"same_point_abs: {same:.17g} > {limit:.17g}")]
    if nodal is not None:
        checks.append(_check("nodal_abs", nodal, limit, nodal <= limit,
                             f"nodal_abs: {nodal:.17g} > {limit:.17g}"))
    return _result(checks, reason="; ".join(c["reason"] for c in checks if not c["pass"]))


def rss_mib(ru_maxrss, platform) -> float:
    """Convert the platform-specific wait4 peak RSS to MiB."""
    value = _number(ru_maxrss, "ru_maxrss")
    _require(value >= 0, "negative ru_maxrss")
    _require(platform in ("linux", "darwin"), f"unsupported platform: {platform}")
    return value / (1024 if platform == "linux" else 1024**2)


def run_measured(argv, env, cwd) -> dict:
    """Measure exactly this child's wait4 usage, inheriting all output streams."""
    _require(sys.platform in ("linux", "darwin"), f"unsupported platform: {sys.platform}")
    start = time.monotonic()
    try:
        child = subprocess.Popen(argv, env=env, cwd=cwd)
        _, status, usage = os.wait4(child.pid, 0)
        child.returncode = os.waitstatus_to_exitcode(status)
    except (OSError, ValueError, TypeError) as exc:
        raise InputInvalid(f"child measurement failed: {exc}") from exc
    return dict(returncode=child.returncode, wall_s=time.monotonic() - start,
                peak_rss_mib=rss_mib(usage.ru_maxrss, sys.platform))


def _nonnegative_decimal(text, name):
    _require(re.fullmatch(r"[0-9]+", text) is not None, f"invalid {name}: {text}")
    return int(text)


def mem_available_bytes() -> int:
    """Read Linux MemAvailable or the specified Darwin page-category sum."""
    try:
        if sys.platform == "linux":
            text = Path("/proc/meminfo").read_text(encoding="utf-8")
            matches = re.findall(r"^MemAvailable:\s*(\S+)\s+kB\s*$", text, re.MULTILINE)
            _require(len(matches) == 1, "missing/duplicate MemAvailable")
            return _nonnegative_decimal(matches[0], "MemAvailable") * 1024
        if sys.platform == "darwin":
            text = subprocess.check_output(["vm_stat"], text=True)
            size_text = subprocess.check_output(["sysctl", "-n", "hw.pagesize"], text=True)
            size = _nonnegative_decimal(size_text.strip(), "page size")
            _require(size > 0, "page size must be positive")
            pages = 0
            for category in ("free", "inactive", "speculative"):
                matches = re.findall(rf"^Pages {category}:\s*(\S+)\.\s*$", text, re.MULTILINE)
                _require(len(matches) == 1, f"missing/duplicate Pages {category}")
                pages += _nonnegative_decimal(matches[0], category)
            return pages * size
        raise InputInvalid(f"unsupported platform: {sys.platform}")
    except (OSError, ValueError, UnicodeError, subprocess.SubprocessError) as exc:
        raise InputInvalid(f"available memory read failed: {exc}") from exc
