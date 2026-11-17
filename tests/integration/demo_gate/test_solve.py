"""DEMO-GATE coupled cut solve and repeat, target-case, zero-source controls."""
import copy
from datetime import datetime
import importlib.util
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
except ImportError:
    print("SKIP: h5py and numpy are required")
    sys.exit(77)

source = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location("warpage_qoi", source / "tools/vv/warpage_qoi.py")
qoi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qoi)


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def fields(out):
    with h5py.File(out / "out/step-0.vtkhdf", "r") as file:
        return {name: {key: file["VTKHDF/" + name + "/" + key][:]
                       for key in ("Types", "Points", "PointData/T", "PointData/U",
                                   "CellData/cell_kind")}
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
            assert result.returncode != 0, detail
            return
        assert result.returncode == 0, detail
        out = work / name
        return read(out / "manifest.json"), fields(out), elapsed

    manifest, blocks, elapsed = run("out", original)
    polygon = np.asarray(read(work / "stack.json")["outline"]["polygon"])
    assert len(blocks["cut"]["Types"]) > 0
    assert np.all(blocks["cut"]["Types"] == 42)
    assert np.all(blocks["hex"]["CellData/cell_kind"] == 0)
    for name, block in blocks.items():
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
    points = np.concatenate([b["Points"] for b in blocks.values()])
    displacement = np.concatenate([b["PointData/U"] for b in blocks.values()])
    assert np.all(np.isfinite(displacement))
    zmax = points[:, 2].max()
    height = zmax - points[:, 2].min()
    assert height > 0
    top = points[:, 2] >= zmax - 1e-12 * height
    vertices, indices = np.unique(points[top], axis=0, return_index=True)
    assert len(vertices) >= 3
    _, residual = qoi.best_fit_plane(vertices[:, 0], vertices[:, 1], displacement[top][indices, 2])
    wbfp = float(np.ptp(residual))
    relative = abs(wbfp - pv) / pv
    mesh_hash = manifest["mesh"]["mesh_sha256"]
    assert isinstance(mesh_hash, str) and len(mesh_hash) == 64
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=source,
                              capture_output=True, text=True, check=True).stdout.strip()
    print("实测", datetime.now(ZoneInfo("Asia/Shanghai")).isoformat(),
          "machine", platform.node(), "commit", revision,
          "counts", {n: {"cells": len(b["Types"]), "points": len(b["Points"])}
                     for n, b in blocks.items()},
          "T_max", float(temperatures.max()), "warpage_pv", pv,
          "W_bfp", wbfp, "relative_difference", relative,
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
