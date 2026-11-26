# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Hand-prescribed serial VTKHDF inputs for gate unit tests, without a solver."""

import json

import h5py
import numpy as np

METHOD = "FVM top-face centroids; least-squares plane removal"


def manifest(run, nx, ny, *, pv=1.0, method=METHOD):
    data = {
        "schema_version": "chipcae.results/1.0",
        "mesh": {"nx0": nx, "ny0": ny, "L": 0, "padded_cells": 0,
                 "nz": 1, "mesh_sha256": "a" * 64},
        "steps": [{"n": 0, "file": "nested/first.vtkhdf"}],
        "summary": [{"name": name, "value": value, "method": label, "step": 0}
                    for name, value, label in (("warpage_pv", pv, method),
                                               ("T_max", 300.0, "synthetic"),
                                               ("U_mag_max", 2.0, "synthetic"))],
    }
    (run / "manifest.json").write_text(json.dumps(data), encoding="utf-8")
    (run / "case.json").write_text(json.dumps({"mesh": {"nx": nx, "ny": ny}}),
                                    encoding="utf-8")


def block(group, points, cells, *, kind=12, displacement=None):
    points = np.asarray(points, dtype=float).reshape((-1, 3))
    sizes = [len(cell) for cell in cells]
    group["Points"] = points
    group["NumberOfPoints"] = np.array([len(points)], dtype=np.int64)
    group["NumberOfCells"] = np.array([len(cells)], dtype=np.int64)
    group["NumberOfConnectivityIds"] = np.array([sum(sizes)], dtype=np.int64)
    group["Connectivity"] = np.array([i for cell in cells for i in cell], dtype=np.int64)
    group["Offsets"] = np.array([0, *np.cumsum(sizes)], dtype=np.int64)
    group["Types"] = np.full(len(cells), kind, dtype=np.uint8)
    group["CellData/elem_gid"] = np.arange(len(cells), dtype=np.int64)
    group["CellData/volume"] = np.ones(len(cells))
    group["PointData/T"] = np.full(len(points), 300.0)
    group["PointData/U"] = (np.zeros_like(points) if displacement is None
                             else np.asarray(displacement, dtype=float))


def grid_run(run, xs, ys, field=lambda x, y: x*x + y*y, *, pv=1.0, method=METHOD):
    run.mkdir(parents=True)
    (run / "nested").mkdir()
    xs, ys = np.asarray(xs), np.asarray(ys)
    points = np.array([(x, y, z) for z in (0.0, 1.0) for y in ys for x in xs])
    nx, ny = len(xs) - 1, len(ys) - 1
    def node(i, j, k):
        return k * len(xs) * len(ys) + j * len(xs) + i
    cells = [[node(i, j, 0), node(i+1, j, 0), node(i+1, j+1, 0), node(i, j+1, 0),
              node(i, j, 1), node(i+1, j, 1), node(i+1, j+1, 1), node(i, j+1, 1)]
             for j in range(ny) for i in range(nx)]
    u = np.zeros_like(points)
    u[:, 2] = field(points[:, 0], points[:, 1])
    with h5py.File(run / "nested/first.vtkhdf", "w") as data:
        vtk = data.create_group("VTKHDF")
        block(vtk.create_group("hex"), points, cells, displacement=u)
        block(vtk.create_group("cut"), [], [], kind=42)
    manifest(run, nx, ny, pv=pv, method=method)
    return run


def cut_run(run, *, reverse=False):
    """Three disconnected trapezoidal prisms; centroid is (7/9,4/9)."""
    grid_run(run, [0, 1], [0, 1])
    points, cells, faces, poly = [], [], [], []
    for shift in (0, 4, 9):
        base = len(points)
        ring = [(shift, 0), (shift + 2, 0), (shift + 1, 1), (shift, 1)]
        points.extend((x, y, z) for z in (1.0, 2.0) for x, y in ring)
        cells.append(list(range(base, base + 8)))
        local = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4],
                 [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]]
        poly.append(list(range(len(faces), len(faces) + len(local))))
        faces.extend([[base + i for i in (face[::-1] if reverse else face)] for face in local])
    with h5py.File(run / "nested/first.vtkhdf", "r+") as data:
        del data["VTKHDF/cut"]
        cut = data["VTKHDF"].create_group("cut")
        block(cut, points, cells, kind=42)
        for prefix, rings in (("Face", faces), ("Polyhedron", poly)):
            name = "FaceConnectivity" if prefix == "Face" else "PolyhedronToFaces"
            cut[name] = np.array([i for ring in rings for i in ring], dtype=np.int64)
            cut[prefix + "Offsets"] = np.array([0, *np.cumsum([len(r) for r in rings])],
                                                  dtype=np.int64)
        cut["NumberOfFaces"] = np.array([len(faces)], dtype=np.int64)
        cut["NumberOfFaceConnectivityIds"] = np.array([sum(map(len, faces))], dtype=np.int64)
        cut["NumberOfPolyhedronToFaceIds"] = np.array([sum(map(len, poly))], dtype=np.int64)
    return run


def change_manifest(run, change):
    path = run / "manifest.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    change(data)
    path.write_text(json.dumps(data), encoding="utf-8")
