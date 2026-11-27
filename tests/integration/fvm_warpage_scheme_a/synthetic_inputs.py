# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic serial VTKHDF prisms; no solver output or customer data."""
import copy
import json
from pathlib import Path

import h5py
import numpy as np

METHOD = "FVM top-face centroids; least-squares plane removal"
FVM_KEYS = ((48, 16, 4), (48, 16, 8), (96, 32, 16))


def edit_json(path, change):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    change(data)
    Path(path).write_text(json.dumps(data), encoding="utf-8")


def replace(block, name, values):
    del block[name]
    block[name] = np.asarray(values)


def _block(root, name, points, cells, faces=None, poly_faces=None):
    block = root.create_group(name)
    block["Points"] = np.asarray(points, dtype=np.float64).reshape(-1, 3)
    block["NumberOfPoints"] = np.array([len(points)], dtype=np.int64)
    block["NumberOfCells"] = np.array([len(cells)], dtype=np.int64)
    block["NumberOfConnectivityIds"] = np.array([sum(map(len, cells))], dtype=np.int64)
    block["Connectivity"] = np.array([p for cell in cells for p in cell], dtype=np.int64)
    block["Offsets"] = np.cumsum([0] + list(map(len, cells)), dtype=np.int64)
    block["Types"] = np.full(len(cells), 12 if name == "hex" else 42, dtype=np.uint8)
    if name == "cut":
        faces, poly_faces = faces or [], poly_faces or []
        block["NumberOfFaces"] = np.array([len(faces)], dtype=np.int64)
        block["NumberOfFaceConnectivityIds"] = np.array([sum(map(len, faces))], dtype=np.int64)
        block["NumberOfPolyhedronToFaceIds"] = np.array(
            [sum(map(len, poly_faces))], dtype=np.int64)
        block["FaceConnectivity"] = np.array([p for f in faces for p in f], dtype=np.int64)
        block["FaceOffsets"] = np.cumsum([0] + list(map(len, faces)), dtype=np.int64)
        block["PolyhedronToFaces"] = np.array(
            [f for fs in poly_faces for f in fs], dtype=np.int64)
        block["PolyhedronOffsets"] = np.cumsum([0] + list(map(len, poly_faces)), dtype=np.int64)


def write_manifest(run, mesh, pv=0.01, method=METHOD):
    run = Path(run)
    run.mkdir(parents=True, exist_ok=True)
    (run / "manifest.json").write_text(json.dumps({
        "schema_version": "chipcae.results/1.0",
        "mesh": dict(mesh, mesh_sha256="0" * 64),
        "steps": [{"n": 7, "file": "out/first.vtkhdf"}],
        "summary": [{"name": "warpage_pv", "value": pv, "unit": "m",
                     "method": method, "step": 7}],
    }), encoding="utf-8")
    return run


def hex_run(run, nx0=8, ny0=4, level=0, hx=1 / 64, hy=1 / 32):
    """Cell order is y-major, x-minor; dyadic spacings make means exact."""
    nx, ny = nx0 * 2**level, ny0 * 2**level
    run = write_manifest(run, dict(nx0=nx0, ny0=ny0, L=level, padded_cells=0))
    (run / "case.json").write_text(json.dumps({"mesh": {"nx": nx, "ny": ny}}),
                                   encoding="utf-8")
    points, cells = [], []
    for j in range(ny):
        for i in range(nx):
            start = len(points)
            ring = [(i * hx, j * hy), ((i + 1) * hx, j * hy),
                    ((i + 1) * hx, (j + 1) * hy), (i * hx, (j + 1) * hy)]
            points.extend([(x, y, z) for z in (0.0, 1.0) for x, y in ring])
            cells.append(list(range(start, start + 8)))
    step = run / "out/first.vtkhdf"
    step.parent.mkdir()
    with h5py.File(step, "w") as data:
        root = data.create_group("VTKHDF")
        _block(root, "hex", points, cells)
        _block(root, "cut", [], [])
    expected = np.array([((i + 0.5) * hx, (j + 0.5) * hy)
                         for j in range(ny) for i in range(nx)], dtype=np.float64)
    return run, step, expected


def cut_run(run, polygons, reverse=False):
    """Extrude disjoint xy polygons into closed, independent unit-height prisms."""
    run, step, _ = hex_run(run, 3, 1)
    points, cells, faces, refs = [], [], [], []
    for polygon in polygons:
        n, base, first = len(polygon), len(points), len(faces)
        points.extend([(x, y, z) for z in (0.0, 1.0) for x, y in polygon])
        cells.append(list(range(base, base + 2 * n)))
        top = list(range(base + n, base + 2 * n))
        faces.extend([list(reversed(range(base, base + n))),
                      list(reversed(top)) if reverse else top])
        faces.extend([[base + i, base + (i + 1) % n,
                       base + n + (i + 1) % n, base + n + i] for i in range(n)])
        refs.append(list(range(first, len(faces))))
    with h5py.File(step, "r+") as data:
        root = data["VTKHDF"]
        del root["hex"], root["cut"]
        _block(root, "hex", [], [])
        _block(root, "cut", points, cells, faces, refs)
    return run, step


def gate_records():
    return [dict(nx=nx, ny=nx // 3, kz=kz, same_point_rel=e,
                 deformation_rel=0.01, fvm_curvature=-0.01, fem_curvature=-0.02)
            for nx, kz, e in ((48, 4, .024), (48, 8, .006),
                              (96, 4, .012), (96, 8, .003))]


def tiers(keys=FVM_KEYS):
    return [dict(nx=nx, ny=ny, kz=kz, nodal_pv=1.0,
                 same_point_pv={key: 1.0 for key in keys})
            for nx, ny, kz in ((12, 4, 2), (12, 4, 4), (24, 8, 2),
                               (48, 16, 2), (96, 32, 2))]


def record(records, nx, kz):
    return next(r for r in records if r["nx"] == nx and r["kz"] == kz)


def clone(value):
    return copy.deepcopy(value)
