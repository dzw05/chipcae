# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Independent, synthetic first-step inputs; never run a solver or read case data."""

import importlib
import json
from pathlib import Path
import sys

try:
    import h5py
    import numpy as np
except ModuleNotFoundError as exc:
    if exc.name not in ("numpy", "h5py"):
        raise
    print(f"SKIP: optional test dependency missing: {exc.name}")
    raise SystemExit(77)

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
SCRIPT = ROOT / "tools" / "vv" / "real_case_crosscheck.py"
AMBIENT = 1.0
QOIS = ("T_max", "warpage_pv", "U_mag_max")


def crosscheck_module():
    """Missing feature is a failed assertion, never a dependency skip."""
    assert SCRIPT.is_file(), "missing feature: tools/vv/real_case_crosscheck.py"
    return importlib.import_module("tools.vv.real_case_crosscheck")


def write_manifest(run, manifest):
    # Deliberately permit nonfinite values when constructing invalid inputs.
    (run / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")


def change_manifest(run, change):
    manifest = json.loads((run / "manifest.json").read_text(encoding="utf-8"))
    change(manifest)
    write_manifest(run, manifest)


def patch_qoi(run, name, value=None, *, method=None):
    def change(manifest):
        entry = next(item for item in manifest["summary"] if item["name"] == name)
        if value is not None:
            entry["value"] = value
        if method is not None:
            entry["method"] = method
    change_manifest(run, change)


def synthetic_run(run, *, n=64, mesh_hash="a" * 64, total_volume=1e-6,
                  hex_counts=None, cut_counts=None, volumes=None, qoi=None):
    """Write rank-local topology, including legal empty cut counts/offsets.

    Return the prescribed float64 volume order, independently of the loader.
    Each cell has its own eight vertices. Nonempty cut cells use VTK type 42;
    this fixture supplies the minimal topology accepted by field_compare.
    """
    run.mkdir(parents=True, exist_ok=True)
    hex_counts = tuple(hex_counts if hex_counts is not None else (n,))
    cut_counts = tuple(cut_counts if cut_counts is not None else (0,) * len(hex_counts))
    assert len(hex_counts) == len(cut_counts)
    n = sum(hex_counts) + sum(cut_counts)
    volumes = np.asarray(volumes if volumes is not None else np.full(n, total_volume / n),
                         dtype=np.float64)
    assert volumes.shape == (n,)
    h = float((np.sum(volumes) / n) ** (1 / 3))
    qoi = dict(qoi if qoi is not None else {
        "T_max": AMBIENT + 2.0 + 0.5 * h**2,
        "warpage_pv": 1e-4 * (1 + 0.5 * h**2),
        "U_mag_max": 2e-4 * (1 + 0.5 * h**2),
    })
    corners = np.array([(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
                        (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)], dtype=float)
    with h5py.File(run / "step.vtkhdf", "w") as data:
        vtk = data.create_group("VTKHDF")
        vtk.attrs["Version"] = np.array([2, 0], dtype=np.int64)
        vtk.attrs["Type"] = np.bytes_("UnstructuredGrid")
        cursor = 0
        for name, counts, cell_type in (("hex", hex_counts, 12), ("cut", cut_counts, 42)):
            block = vtk.create_group(name)
            size = sum(counts)
            cell_volumes = volumes[cursor:cursor + size]
            points = np.array([
                (corners + (3 * (cursor + i), 0, 0)) * float(v ** (1 / 3))
                for i, v in enumerate(cell_volumes)
            ], dtype=float).reshape((-1, 3))
            block["Points"] = points
            block["NumberOfPoints"] = np.array(counts, dtype=np.int64) * 8
            block["NumberOfCells"] = np.array(counts, dtype=np.int64)
            block["NumberOfConnectivityIds"] = np.array(counts, dtype=np.int64) * 8
            block["Connectivity"] = np.concatenate([
                np.arange(count * 8, dtype=np.int64) for count in counts])
            block["Offsets"] = np.concatenate([
                np.arange(count + 1, dtype=np.int64) * 8 for count in counts])
            block["Types"] = np.full(size, cell_type, dtype=np.uint8)
            block["CellData/elem_gid"] = np.arange(size, dtype=np.int64)
            block["CellData/volume"] = cell_volumes
            scaled = points / max(float(np.max(points)) if len(points) else 0, 1e-3)
            block["PointData/T"] = AMBIENT + (qoi["T_max"] - AMBIENT) * (
                0.5 + 0.5 * scaled[:, 0])
            u = np.column_stack((scaled[:, 0]**2, scaled[:, 1] * scaled[:, 0],
                                 scaled[:, 2]**2))
            norm = float(np.max(np.linalg.norm(u, axis=1))) if len(u) else 1.0
            block["PointData/U"] = u * qoi["U_mag_max"] / max(norm, 1e-30)
            cursor += size
    write_manifest(run, {
        "schema_version": "chipcae.results/1.0",
        "mesh": {"nx0": n, "ny0": 1, "nz": 1, "L": [0.01, 0.01, 0.01],
                 "padded_cells": 0, "mesh_sha256": mesh_hash},
        "steps": [{"n": 0, "file": "step.vtkhdf"}],
        "summary": [{"name": key, "value": qoi[key], "method": "synthetic", "step": 0}
                    for key in QOIS],
    })
    return volumes.copy()


def mesh_runs(root):
    runs = [root / name for name in ("fine", "middle", "coarse")]
    for run, n, letter in zip(runs, (216, 125, 64), ("a", "b", "c")):
        synthetic_run(run, n=n, mesh_hash=letter * 64)
    return runs
