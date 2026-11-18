"""DEMO-GATE coupled cut solve and repeat, target-case, zero-source controls."""
import copy
from datetime import datetime
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from zoneinfo import ZoneInfo

exe = os.environ.get("CHIPCAE_SOLVE_EXE")
if not exe:
    print("SKIP: CHIPCAE_SOLVE_EXE is unset (numerical backend required)")
    sys.exit(77)
try:
    import h5py
    import numpy as np
except Exception as error:
    # Binary dependency failures (for example, an ABI mismatch) can raise
    # ValueError or OSError rather than ImportError during import.
    print(f"SKIP: h5py and numpy must be importable: {error}")
    sys.exit(77)

source = Path(sys.argv[1]).resolve()


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def fields(out):
    with h5py.File(out / "out/step-0.vtkhdf", "r") as file:
        return {name: {key: file["VTKHDF/" + name + "/" + key][:]
                       for key in ("Types", "Points", "PointData/T", "PointData/U",
                                   "CellData/cell_kind", "CellData/layer_id",
                                   "CellData/material_id", "CellData/elem_gid")}
                for name in ("hex", "cut")}


def warpage(manifest):
    summary = {item["name"]: item["value"] for item in manifest["summary"]}
    value = summary["warpage_pv"]
    assert math.isfinite(value) and value > 0, value
    return value


with tempfile.TemporaryDirectory(prefix="demogate-") as tmp:
    work = Path(tmp) / "demo_gate"
    shutil.copytree(source / "examples/demo_gate", work)
    original = read(work / "case.json")

    def run(name, case, rejected=False):
        (work / "case.json").write_text(json.dumps(case), encoding="utf-8")
        command = [exe, "--case", "case.json", "--profile", "profile.json", "--out", name]
        start = time.monotonic()
        result = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=420)
        elapsed = time.monotonic() - start
        detail = f"command={command!r}\nexit={result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        if rejected:
            print("case-sensitive target observation:", detail, flush=True)
            assert result.returncode == 14, detail
            assert "chipcae-solve: invalid target: layer:Die_layer" in result.stderr, detail
            return
        assert result.returncode == 0, detail
        out = work / name
        return read(out / "manifest.json"), fields(out), elapsed

    manifest, blocks, elapsed = run("out", original)
    polygon = np.asarray(read(work / "stack.json")["outline"]["polygon"])
    assert len(blocks["cut"]["Types"]) > 0
    assert np.all(blocks["cut"]["Types"] == 42)
    assert len(blocks["hex"]["Types"]) > 0
    assert np.all(blocks["hex"]["CellData/cell_kind"] == 0)
    for name, block in blocks.items():
        n_cells = len(block["Types"])
        for field in ("cell_kind", "layer_id", "material_id", "elem_gid"):
            values = block["CellData/" + field]
            assert values.shape == (n_cells,) and values.size > 0, (name, field, values.shape)
            assert np.issubdtype(values.dtype, np.integer), (name, field, values.dtype)
        assert np.all(block["CellData/cell_kind"] == (0 if name == "hex" else 1)), name
        points = block["Points"]
        assert np.all(np.isfinite(points)), name
        assert block["PointData/T"].shape == (len(points),)
        assert block["PointData/U"].shape == (len(points), 3)
        # CCW edges: cross(edge, point - start) / |edge| is positive inside.
        # Apply the same signed-distance tolerance to every edge, including all diagonals.
        for a, b in zip(polygon, np.roll(polygon, -1, axis=0)):
            edge = b - a
            delta = points[:, :2] - a
            distance = (edge[0] * delta[:, 1] - edge[1] * delta[:, 0]) / np.linalg.norm(edge)
            assert np.all(distance >= -1e-12), (name, a, b, distance.min())
    temperatures = np.concatenate([b["PointData/T"] for b in blocks.values()])
    assert np.all(np.isfinite(temperatures))
    assert temperatures.min() >= 298.15 - 1e-8, temperatures.min()
    assert 298.15 + 1e-6 < temperatures.max() < 400, temperatures.max()
    pv = warpage(manifest)
    displacement = np.concatenate([b["PointData/U"] for b in blocks.values()])
    assert np.all(np.isfinite(displacement))
    # VTKHDF vertices omit the solver's Q2 nodes and cut-face quadrature points.
    # A vertex plane fit therefore cannot cross-check the solver's warpage QoI.
    mesh_hash = manifest["mesh"]["mesh_sha256"]
    assert isinstance(mesh_hash, str) and len(mesh_hash) == 64
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=source,
                              capture_output=True, text=True, check=True).stdout.strip()
    implementation = subprocess.run(
        ["git", "log", "-1", "--format=%H", "--", "examples/demo_gate"], cwd=source,
        capture_output=True, text=True, check=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=normal"],
                           cwd=source, capture_output=True, text=True, check=True).stdout.strip()
    print("实测", datetime.now(ZoneInfo("Asia/Shanghai")).isoformat(),
          "machine", platform.node(), "commit", revision,
          "example_implementation_commit", implementation, "working_tree_dirty", bool(dirty),
          "counts", {n: {"cells": len(b["Types"]), "points": len(b["Points"])}
                     for n, b in blocks.items()},
          "T_max", float(temperatures.max()), "warpage_pv", pv,
          "mesh_sha256", mesh_hash, "main_wall_seconds", elapsed, flush=True)

    repeated, _, _ = run("repeat", original)
    assert abs(warpage(repeated) - pv) / pv <= 1e-12
    assert repeated["mesh"]["mesh_sha256"] == mesh_hash
    wrong_case = copy.deepcopy(original)
    heat = next(load for load in wrong_case["loads"] if load["type"] == "heat_source")
    heat["target"] = "layer:Die_layer"
    run("wrong-case", wrong_case, rejected=True)
    zero = copy.deepcopy(original)
    zero["loads"] = [load for load in zero["loads"] if load["type"] != "heat_source"]
    _, zero_blocks, _ = run("zero", zero)
    zero_t = np.concatenate([b["PointData/T"] for b in zero_blocks.values()])
    assert np.all(np.isfinite(zero_t))
    assert np.max(np.abs(zero_t - 298.15)) <= 1e-8
    zero_u = np.concatenate([b["PointData/U"] for b in zero_blocks.values()])
    assert np.all(np.isfinite(zero_u))
    assert np.max(np.linalg.norm(zero_u, axis=1)) <= 1e-12
