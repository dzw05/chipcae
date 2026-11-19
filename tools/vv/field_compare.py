# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Compare first-step fields on an identical mesh (library and command line)."""

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import sys

import h5py
import numpy as np


class UsageError(ValueError):
    """An invalid comparison option or output destination."""


class InputInvalid(ValueError):
    """An invalid run or an ambiguous field alignment."""


@dataclass
class RunFields:
    mesh_sha256: str
    qoi: dict[str, float]
    qoi_method: dict[str, str]
    points: np.ndarray
    T: np.ndarray | None
    U: np.ndarray | None
    cell_gid: np.ndarray
    cell_volume: np.ndarray | None
    cell_T: np.ndarray | None
    cell_T_source: str
    dup_point_spread: dict[str, float]


_QOI = ("warpage_pv", "T_max", "U_mag_max")
_FIELDS = ("T", "cell_T", "U", "U_mag", "U_rigid_removed")
_EQUIVALENCE = ("fields.T.max_rel", "fields.U.max_rel", "qoi.warpage_pv.rel",
                "qoi.T_max.rel", "qoi.U_mag_max.rel")
_BLOCK_STRIDE = 2**62
GATES: dict[str, dict] = {
    "same-mesh-solver": {
        metric: {"limit": 1e-8, "source": "tests/solve/test_column_gmg.py:150-155; issue #205"}
        for metric in _EQUIVALENCE
    },
    "ranks": {
        metric: {"limit": 1e-8,
                 "source": "tests/integration/f1130_uncut_cli/test_uncut_cli_e2e.py:26; "
                           "briefs/fvm-case-e2e.md:62"}
        for metric in _EQUIVALENCE
    },
    "fem-fvm": {"qoi.T_max_rise.rel": {
        "limit": 1e-2, "source": "handoff/rulings/fvm-solver-io.md:55"}},
    "report": {},
}


def _require(condition, message):
    if not condition:
        raise InputInvalid(message)


def _array(group, path, shape=None, *, integer=False):
    _require(path in group and isinstance(group[path], h5py.Dataset),
             f"{group.name}/{path}: dataset absent")
    value = group[path][...]
    _require(shape is None or value.shape == shape,
             f"{group.name}/{path}: expected shape {shape}, got {value.shape}")
    _require(value.dtype.kind in ("iu" if integer else "iuf"),
             f"{group.name}/{path}: expected {'integer' if integer else 'numeric'} array")
    _require(np.all(np.isfinite(value)), f"nonfinite field: {group.name}/{path}")
    return value


def _counts(block, path):
    values = _array(block, path, integer=True)
    _require(values.ndim == 1 and len(values) > 0 and np.all(values >= 0),
             f"{block.name}/{path}: expected nonnegative partition counts")
    return [int(v) for v in values]


def _block(block, index):
    """Read rank-local topology before any point merging or cell sorting."""
    pc = _counts(block, "NumberOfPoints")
    cc = _counts(block, "NumberOfCells")
    nc = _counts(block, "NumberOfConnectivityIds")
    _require(len(pc) == len(cc) == len(nc), f"{block.name}: partition count mismatch")
    p, c, n = sum(pc), sum(cc), sum(nc)
    _require(index != 0 or c > 0, "hex block has no cells")
    points = _array(block, "Points", (p, 3)).astype(float)
    conn = _array(block, "Connectivity", (n,), integer=True)
    offsets = _array(block, "Offsets", (c + len(cc),), integer=True)
    types = _array(block, "Types", (c,), integer=True)
    _require(np.all(types == (12 if index == 0 else 42)), f"{block.name}: invalid cell types")
    t = _array(block, "PointData/T", (p,)).astype(float) if "PointData/T" in block else None
    u = _array(block, "PointData/U", (p, 3)).astype(float) if "PointData/U" in block else None
    _require(c == 0 or "CellData/elem_gid" in block,
             "elem_gid absent; add elem_gid to output.fields")
    raw = (_array(block, "CellData/elem_gid", (c,)) if "CellData/elem_gid" in block
           else np.empty(0, dtype=np.int64))
    # Check the original numeric values, including floats, before any conversion.
    _require(np.all(raw >= 0) and np.all(raw < _BLOCK_STRIDE)
             and (raw.dtype.kind != "f" or np.all(raw == np.floor(raw))),
             f"{block.name}: invalid elem_gid")
    ids = [int(value) for value in raw]
    _require(len(set(ids)) == len(ids), f"ambiguous elem_gid: {block.name}")
    gids = np.array([index * _BLOCK_STRIDE + gid for gid in ids], dtype=np.int64)
    volume = (_array(block, "CellData/volume", (c,)).astype(float)
              if "CellData/volume" in block else None)
    if volume is not None:
        _require(np.all(volume > 0), f"{block.name}: nonpositive cell volume")
    cell_t = (_array(block, "CellData/T_cell", (c,)).astype(float)
              if "CellData/T_cell" in block else None)
    source = "T_cell" if cell_t is not None and c else "none"
    means = []
    point_cursor = cell_cursor = conn_cursor = offset_cursor = 0
    for points_count, cells_count, connections_count in zip(pc, cc, nc):
        local_offsets = offsets[offset_cursor:offset_cursor + cells_count + 1]
        local_conn = conn[conn_cursor:conn_cursor + connections_count]
        _require(local_offsets[0] == 0 and local_offsets[-1] == connections_count
                 and np.all(local_offsets[1:] > local_offsets[:-1]),
                 f"{block.name}: invalid rank-local offsets")
        _require(np.all(local_conn >= 0) and np.all(local_conn < points_count),
                 f"{block.name}: invalid rank-local connectivity")
        for cell in range(cells_count):
            start, stop = int(local_offsets[cell]), int(local_offsets[cell + 1])
            vertices = local_conn[start:stop]
            _require(index != 0 or len(vertices) == 8, "hex cell must have eight vertices")
            _require(len(np.unique(vertices)) == len(vertices),
                     f"{block.name}: repeated vertex in cell connectivity")
            if t is not None and cell_t is None:
                means.append(float(np.mean(t[point_cursor + vertices])))
        point_cursor += points_count
        cell_cursor += cells_count
        conn_cursor += connections_count
        offset_cursor += cells_count + 1
    _require(cell_cursor == c, f"{block.name}: invalid cell count")
    if cell_t is None and t is not None and c:
        cell_t, source = np.array(means), "vertex_mean"
    return points, t, u, gids, volume, cell_t, source


def _point_keys(points):
    diagonal = float(np.linalg.norm(np.ptp(points, axis=0)))
    _require(math.isfinite(diagonal) and diagonal > 0, "point bounding box has zero/invalid diagonal")
    tol = 1e-12 * diagonal
    # Python integers avoid int64 overflow for meshes far from the origin.
    scaled = points / tol
    _require(np.all(np.isfinite(scaled)), "nonfinite field: quantized coordinates")
    return [tuple(int(round(v)) for v in row) for row in scaled]


def _merge_points(blocks):
    points = np.concatenate([b[0] for b in blocks])
    groups = {}
    for row, key in enumerate(_point_keys(points)):
        groups.setdefault(key, []).append(row)
    ordered = [groups[key] for key in sorted(groups)]
    # Sort values inside each group so sums are independent of rank storage order.
    merged_points = np.array([np.mean(np.sort(points[rows], axis=0), axis=0) for rows in ordered])
    merged, spread = [], {}
    for name, column, width in (("T", 1, ()), ("U", 2, (3,))):
        if not any(b[column] is not None and len(b[0]) for b in blocks):
            merged.append(None)
            continue
        values = np.concatenate([b[column] if b[column] is not None
                                 else np.zeros((len(b[0]), *width)) for b in blocks])
        present = np.concatenate([np.full(len(b[0]), b[column] is not None) for b in blocks])
        output, maximum = [], 0.
        for rows in ordered:
            local = values[rows][present[rows]]
            _require(len(local) > 0,
                     f"partial point field {name}: no value at {points[rows[0]].tolist()}")
            output.append(np.mean(np.sort(local, axis=0), axis=0))
            if name == "T":
                difference = float(np.ptp(local))
            else:
                # Spread is the largest Euclidean distance between duplicate vectors.
                difference = max(float(np.max(np.linalg.norm(local - v, axis=1))) for v in local)
            maximum = max(maximum, difference)
        merged.append(np.array(output))
        spread[name] = maximum
    return merged_points, *merged, spread


def _load_run(run_dir):
    manifest = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
    _require(isinstance(manifest, dict)
             and manifest.get("schema_version") == "chipcae.results/1.0", "invalid manifest schema")
    mesh = manifest.get("mesh")
    _require(isinstance(mesh, dict) and isinstance(mesh.get("mesh_sha256"), str)
             and bool(mesh["mesh_sha256"]), "mesh_sha256 absent or invalid")
    steps = manifest.get("steps")
    _require(isinstance(steps, list) and len(steps) > 0 and isinstance(steps[0], dict)
             and isinstance(steps[0].get("file"), str) and bool(steps[0]["file"]),
             "steps[0].file absent or invalid")
    entries = manifest.get("summary")
    _require(isinstance(entries, list), "manifest summary absent or invalid")
    qoi, methods = {}, {}
    for entry in entries:
        _require(isinstance(entry, dict), "invalid summary entry")
        if "step" in entry and entry["step"] != steps[0].get("n"):
            continue
        name = entry.get("name")
        if name not in _QOI:
            continue
        value = entry.get("value")
        _require(not isinstance(value, bool) and isinstance(value, (int, float))
                 and math.isfinite(value), f"invalid/nonfinite QoI: {name}")
        _require(name not in qoi, f"duplicate QoI: {name}")
        method = entry.get("method", "")
        _require(isinstance(method, str), f"invalid QoI method: {name}")
        qoi[name], methods[name] = float(value), method
    _require(all(name in qoi for name in _QOI), "required QoI absent")
    with h5py.File(run_dir / steps[0]["file"], "r") as data:
        blocks = [_block(data["VTKHDF/" + name], index)
                  for index, name in enumerate(("hex", "cut"))]
    points, t, u, spread = _merge_points(blocks)
    gids = np.concatenate([b[3] for b in blocks])
    order = np.argsort(gids)
    volumes = (np.concatenate([b[4] if b[4] is not None else np.empty(0) for b in blocks])[order]
               if all(b[4] is not None or len(b[3]) == 0 for b in blocks) else None)
    sources = {b[6] for b in blocks if b[6] != "none"}
    # The frozen source enum cannot represent mixed per-block provenance.
    _require(len(sources) <= 1, "mixed cell_T sources require a defined cell_T_source label")
    source = next(iter(sources), "none")
    cell_t = None
    if sources:
        _require(blocks[0][5] is not None, "hex cell temperature absent")
        cell_t = np.concatenate([b[5] if b[5] is not None
                                 else np.full(len(b[3]), np.nan) for b in blocks])[order]
    return RunFields(mesh["mesh_sha256"], qoi, methods, points, t, u, gids[order],
                     volumes, cell_t, source, spread)


def load_run(run_dir: Path) -> RunFields:
    """Read and canonically order an owned-cell, first-step result."""
    try:
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            return _load_run(Path(run_dir))
    except InputInvalid:
        raise
    except FloatingPointError as exc:
        raise InputInvalid(f"nonfinite computed field: {exc}") from exc
    except (OSError, ValueError, TypeError, KeyError, IndexError, AttributeError,
            RuntimeError, EOFError, OverflowError) as exc:
        raise InputInvalid(str(exc)) from exc


def _relative(numerator, denominator):
    _require(math.isfinite(numerator) and math.isfinite(denominator),
             "nonfinite computed relative metric")
    if denominator == 0:
        return None
    value = float(numerator / denominator)
    _require(math.isfinite(value), "nonfinite computed relative metric")
    return value


def _metric(ref, other, locations, *, ambient=None, temperature=False, weights=None):
    vector = ref.ndim == 2
    delta = np.linalg.norm(other - ref, axis=1) if vector else np.abs(other - ref)
    maximum = int(np.argmax(delta))
    max_abs = float(delta[maximum])
    _require(np.all(np.isfinite(delta)), "nonfinite computed field difference")
    max_rel = l2_rel = None
    if not temperature or ambient is not None:
        scaled = ref - ambient if temperature else ref
        norm = np.linalg.norm(scaled, axis=1) if vector else np.abs(scaled)
        max_rel = _relative(max_abs, float(np.max(norm)))
        weights = np.ones(len(ref)) if weights is None else weights
        l2_rel = _relative(float(np.sqrt(np.sum(weights * delta**2))),
                           float(np.sqrt(np.sum(weights * norm**2))))
    return dict(max_abs=max_abs, max_rel=max_rel, l2_rel=l2_rel,
                argmax=locations(maximum), n=int(len(ref)))


def _remove_rigid(points, displacement):
    centered = points - points.mean(axis=0)
    x, y, z = centered.T
    design = np.zeros((len(points), 3, 6))
    design[:, :, :3] = np.eye(3)
    design[:, 0, 4], design[:, 0, 5] = z, -y
    design[:, 1, 3], design[:, 1, 5] = -z, x
    design[:, 2, 3], design[:, 2, 4] = y, -x
    matrix = design.reshape(-1, 6)
    # Scale columns to avoid mixing very small mesh lengths with unit translations.
    scale = np.linalg.norm(matrix, axis=0)
    scale[scale == 0] = 1
    matrix = matrix / scale
    coefficients = np.linalg.lstsq(matrix, displacement.reshape(-1), rcond=None)[0]
    return displacement - (matrix @ coefficients).reshape(-1, 3)


def _aligned_other(ref, other):
    ref_keys, other_keys = _point_keys(ref.points), _point_keys(other.points)
    rmap = dict(zip(ref_keys, range(len(ref_keys))))
    amap = dict(zip(other_keys, range(len(other_keys))))
    missing_r, missing_a = sorted(rmap.keys() - amap.keys()), sorted(amap.keys() - rmap.keys())
    if missing_r or missing_a:
        raise InputInvalid(
            f"unmatched points: ref={len(missing_r)}, other={len(missing_a)}; "
            f"ref first five={ref.points[[rmap[k] for k in missing_r[:5]]].tolist()}; "
            f"other first five={other.points[[amap[k] for k in missing_a[:5]]].tolist()}")
    _require(np.array_equal(ref.cell_gid, other.cell_gid),
             "unmatched cell IDs: ref/other cell_gid differ")
    return np.array([amap[key] for key in ref_keys], dtype=int)


def compare_runs(ref: RunFields, other: RunFields, *, preset: str,
                 t_ambient: float | None = None, remove_rigid: bool = False) -> dict:
    """Return comparison content; callers supply identity and data classification."""
    if preset not in GATES:
        raise UsageError("unknown preset")
    if ((preset != "report" and t_ambient is None)
            or (t_ambient is not None and not math.isfinite(t_ambient))):
        raise UsageError("finite t_ambient required for gated presets")
    notes = []
    def metadata(run):
        return dict(mesh_sha256=run.mesh_sha256, qoi=dict(run.qoi),
                    qoi_method=dict(run.qoi_method), cell_T_source=run.cell_T_source,
                    dup_point_spread=dict(run.dup_point_spread))
    result = dict(schema="chipcae.vv_field_compare/0.1", preset=preset,
                  ref=metadata(ref), other=metadata(other),
                  same_mesh=ref.mesh_sha256 == other.mesh_sha256,
                  fields=dict.fromkeys(_FIELDS), qoi={}, gates=[], verdict="report", notes=notes)
    qois = [(key, ref.qoi[key], other.qoi[key]) for key in _QOI]
    if t_ambient is not None:
        qois.append(("T_max_rise", ref.qoi["T_max"] - t_ambient, other.qoi["T_max"] - t_ambient))
    for key, r, a in qois:
        difference = abs(a - r)
        _require(math.isfinite(difference), "nonfinite computed QoI difference")
        result["qoi"][key] = dict(ref=r, other=a, abs=difference, rel=_relative(difference, abs(r)))
    if not result["same_mesh"]:
        result["verdict"] = "mesh_mismatch"
        return result
    order = _aligned_other(ref, other)
    point_location = lambda i: {"xyz": ref.points[i].tolist()}
    for key in ("T", "U"):
        r, a = getattr(ref, key), getattr(other, key)
        _require((r is None) == (a is None), f"one-sided {key} field")
        if r is None:
            notes.append(f"{key} unavailable on both runs; applicable gates fail")
            continue
        a = a[order]
        result["fields"][key] = _metric(r, a, point_location, ambient=t_ambient, temperature=key == "T")
        if key == "U":
            result["fields"]["U_mag"] = _metric(np.linalg.norm(r, axis=1), np.linalg.norm(a, axis=1),
                                                   point_location)
            if remove_rigid:
                result["fields"]["U_rigid_removed"] = _metric(
                    _remove_rigid(ref.points, r), _remove_rigid(other.points[order], a), point_location)
    _require((ref.cell_T is None) == (other.cell_T is None), "one-sided cell_T field")
    if ref.cell_T is not None:
        valid = np.isfinite(ref.cell_T) & np.isfinite(other.cell_T)
        excluded = int(np.count_nonzero(~valid))
        if excluded:
            notes.append(f"excluded {excluded} cut cells without temperature")
        gids = ref.cell_gid[valid]
        def cell_location(i):
            block, gid = divmod(int(gids[i]), _BLOCK_STRIDE)
            return {"block": ("hex", "cut")[block], "elem_gid": gid}
        weights = ref.cell_volume[valid] if ref.cell_volume is not None else None
        if weights is None:
            notes.append("cell_T L2 uses equal weights: complete reference volumes unavailable")
        if len(gids):
            result["fields"]["cell_T"] = _metric(ref.cell_T[valid], other.cell_T[valid], cell_location,
                                                     ambient=t_ambient, temperature=True, weights=weights)
    for path, gate in GATES[preset].items():
        value = result
        for key in path.split("."):
            value = value.get(key) if isinstance(value, dict) else None
        result["gates"].append(dict(metric=path, value=value, **gate,
                                    **{"pass": value is not None and value <= gate["limit"]}))
    if preset != "report":
        result["verdict"] = "pass" if all(g["pass"] for g in result["gates"]) else "fail"
    return result


def ensure_output_allowed(out: Path, data_class: str) -> None:
    """Keep explicitly classified real results outside all git trees, resolving links."""
    if data_class == "real":
        resolved = Path(out).resolve()
        if any((parent / ".git").exists() for parent in (resolved, *resolved.parents)):
            raise UsageError("real output destination is inside a git tree")


def _markdown(result):
    lines = ["# Field comparison", "", f"Verdict: {result['verdict']}", "",
             f"Case: {result['case_id']}", f"Data class: {result['data_class']}",
             f"Preset: {result['preset']}", "",
             "| Field | max_abs | max_rel | l2_rel | argmax | n |", "|---|---|---|---|---|---|"]
    for key, metric in result["fields"].items():
        values = [metric[k] for k in ("max_abs", "max_rel", "l2_rel", "argmax", "n")] if metric else [None]*5
        lines.append("| " + " | ".join([key, *(str(v) for v in values)]) + " |")
    lines += ["", "| QoI | ref | other | abs | rel |", "|---|---|---|---|---|"]
    for key, metric in result["qoi"].items():
        lines.append("| " + " | ".join([key, *(str(metric[k]) for k in ("ref", "other", "abs", "rel"))]) + " |")
    lines += ["", "| Gate | value | limit | pass | source |", "|---|---|---|---|---|"]
    for gate in result["gates"]:
        lines.append("| " + " | ".join(str(gate[k]) for k in ("metric", "value", "limit", "pass", "source")) + " |")
    lines += ["", *(str(note) for note in result["notes"]), ""]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ref_run_dir", type=Path)
    parser.add_argument("other_run_dir", type=Path)
    parser.add_argument("--preset", choices=tuple(GATES), required=True)
    parser.add_argument("--t-ambient", type=float)
    parser.add_argument("--remove-rigid", action="store_true")
    parser.add_argument("--data-class", choices=("synthetic", "public", "real"), default="synthetic")
    parser.add_argument("--case-id", default="")
    parser.add_argument("-o", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        markdown = Path(str(args.o) + ".md")
        for destination in (args.o, markdown):
            ensure_output_allowed(destination, args.data_class)
        result = compare_runs(load_run(args.ref_run_dir), load_run(args.other_run_dir),
                              preset=args.preset, t_ambient=args.t_ambient, remove_rigid=args.remove_rigid)
        result.update(case_id=args.case_id, data_class=args.data_class)
        result["ref"]["run_dir"] = args.ref_run_dir.name
        result["other"]["run_dir"] = args.other_run_dir.name
        content = json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
        table = _markdown(result)
        args.o.write_text(content, encoding="utf-8")
        markdown.write_text(table, encoding="utf-8")
    except UsageError as exc:
        print(f"field_compare: USAGE: {exc}", file=sys.stderr)
        return 2
    except (InputInvalid, OSError, ValueError) as exc:
        print(f"field_compare: INPUT_INVALID: {exc}", file=sys.stderr)
        return 14
    print(f"{result['verdict']}: {args.o}")
    return {"pass": 0, "report": 0, "fail": 1, "mesh_mismatch": 3}[result["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
