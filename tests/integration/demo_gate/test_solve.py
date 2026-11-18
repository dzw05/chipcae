"""DEMO-GATE coupled cut solve and repeat, target-case, zero-source controls."""
import copy
from datetime import datetime, timedelta, timezone
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
    # Writer contract: io/README.md and brief §必须复用: PDC hex (12), cut (42).
    with h5py.File(out / "out/step-0.vtkhdf", "r") as file:
        for name in ("hex", "cut"):
            assert "VTKHDF/" + name in file, f"missing required VTKHDF {name} block"
        return {name: {key: file["VTKHDF/" + name + "/" + key][:]
                       for key in ("Types", "Points", "Connectivity", "Offsets", "PointData/T", "PointData/U",
                                   "CellData/cell_kind", "CellData/layer_id",
                                   "CellData/material_id", "CellData/elem_gid")}
                for name in ("hex", "cut")}


def warpage(manifest):
    summary = {item["name"]: item["value"] for item in manifest["summary"]}
    value = summary["warpage_pv"]
    assert math.isfinite(value) and 1e-8 < value < 1e-4, value
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
    assert np.all(blocks["hex"]["Types"] == 12)
    assert np.all(blocks["hex"]["CellData/cell_kind"] == 0)
    for name, block in blocks.items():
        n_cells = len(block["Types"])
        for field in ("cell_kind", "layer_id", "material_id", "elem_gid"):
            values = block["CellData/" + field]
            assert values.shape == (n_cells,) and values.size > 0, (name, field, values.shape)
            assert np.issubdtype(values.dtype, np.integer), (name, field, values.dtype)
        assert np.all(block["CellData/cell_kind"] == (0 if name == "hex" else 1)), name
        points = block["Points"]
        # Single-rank offsets start at zero. Classifications are one-based in
        # stack traversal order (tensor_grid.cpp); test the spatial assignment.
        conn, offsets = block["Connectivity"], block["Offsets"]
        centers = np.array([points[conn[a:b]].mean(axis=0)
                            for a, b in zip(offsets[:-1], offsets[1:])])
        layer = np.where(centers[:, 2] < .001, 1,
                         np.where(centers[:, 2] < .0011, 2, 3))
        die = np.all((centers[:, :2] > .005) & (centers[:, :2] < .015), axis=1)
        material = np.where(layer != 3, layer, np.where(die, 4, 3))
        assert np.array_equal(block["CellData/layer_id"], layer), name
        assert np.array_equal(block["CellData/material_id"], material), name
        assert np.all(np.isfinite(points)), name
        assert block["PointData/T"].shape == (len(points),)
        assert block["PointData/U"].shape == (len(points), 3)
        # CCW edges: cross(edge, point - start) / |edge| is positive inside.
        # Coordinates are metres; 1e-12 m is 5e-11 of the 20 mm span.
        # Apply the same signed-distance tolerance to every edge, including all diagonals.
        for a, b in zip(polygon, np.roll(polygon, -1, axis=0)):
            edge = b - a
            delta = points[:, :2] - a
            distance = (edge[0] * delta[:, 1] - edge[1] * delta[:, 0]) / np.linalg.norm(edge)
            assert np.all(distance >= -1e-12), (name, a, b, distance.min())
    for field, expected in (("layer_id", {1, 2, 3}), ("material_id", {1, 2, 3, 4})):
        assert set(np.concatenate([b["CellData/" + field] for b in blocks.values()])) == expected
    gids = np.concatenate([b["CellData/elem_gid"] for b in blocks.values()])
    # Frozen mesh regression: max_ratio and footprint alignment refine the
    # requested 4 x 4 grid to 288 columns x 6 z cells; cuts replace parents.
    assert len(gids) == 1728 and np.array_equal(np.sort(gids), np.arange(1728))
    temperatures = np.concatenate([b["PointData/T"] for b in blocks.values()])
    assert np.all(np.isfinite(temperatures))
    assert temperatures.min() >= 298.15 - 1e-8, temperatures.min()
    # P*t/(k*A) = 8.23 K for the substrate alone. Allow spreading and
    # distributed die-layer generation, while excluding 10x/0.001x power.
    substrate_r = .001 / (3.1 * 3.92e-4)
    rise = temperatures.max() - 298.15
    assert .1 * 10 * substrate_r < rise < 5 * 10 * substrate_r, rise
    assert temperatures.max() < 340, temperatures.max()
    points = np.concatenate([b["Points"] for b in blocks.values()])
    # Remove per-cell duplicate vertices before averaging each horizontal plane.
    _, unique = np.unique(points, axis=0, return_index=True)
    z, t = points[unique, 2], temperatures[unique]
    means = np.array([t[np.isclose(z, level, rtol=0, atol=1e-14)].mean()
                      for level in np.unique(z)])
    # Bottom is cold; heating is above it. Mean T increases with z, not decreases.
    assert np.all(np.diff(means) >= -1e-8), means
    pv = warpage(manifest)
    displacement = np.concatenate([b["PointData/U"] for b in blocks.values()])
    assert np.all(np.isfinite(displacement))
    assert 1e-9 < np.linalg.norm(displacement, axis=1).max() < 1e-4
    # Thermal mismatch scale alpha*dT*L^2/H is micrometres for this stack;
    # the broad 0.01--100 um warpage band catches unit/CTE scale regressions.
    # PV is nonnegative by definition; signed surface shape is checked below.
    sys.path.insert(0, str(source / "tools/vv"))
    from warpage_qoi import best_fit_plane
    top = points[:, 2] >= points[:, 2].max() - 1e-12 * .0016
    xy, indices = np.unique(points[top, :2], axis=0, return_index=True)
    uz = displacement[top, 2][indices]
    _, residual = best_fit_plane(xy[:, 0], xy[:, 1], uz)
    signed_shape = np.mean(residual[np.linalg.norm(xy - .01, axis=1) < .004])
    assert 1e-8 < signed_shape < 1e-4, signed_shape
    print("signed_center_residual", signed_shape, "W_bfp", np.ptp(residual), flush=True)
    # VTKHDF vertices omit the solver's Q2 nodes and cut-face quadrature points.
    # A vertex plane fit therefore cannot cross-check the solver's warpage QoI.
    mesh_hash = manifest["mesh"]["mesh_sha256"]
    assert isinstance(mesh_hash, str) and len(mesh_hash) == 64
    def git_diagnostic(*args):
        try:
            result = subprocess.run(["git", *args], cwd=source,
                                    capture_output=True, text=True, check=True)
            return result.stdout.strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            return "unknown"

    revision = git_diagnostic("rev-parse", "HEAD")
    implementation = git_diagnostic("log", "-1", "--format=%H", "--", "examples/demo_gate")
    dirty = git_diagnostic("status", "--porcelain", "--untracked-files=normal")
    print("实测", datetime.now(timezone(timedelta(hours=8))).isoformat(),
          "machine", platform.node(), "commit", revision,
          "example_implementation_commit", implementation, "working_tree_dirty", ("unknown" if dirty == "unknown" else bool(dirty)),
          "counts", {n: {"cells": len(b["Types"]), "points": len(b["Points"])}
                     for n, b in blocks.items()},
          "T_max", float(temperatures.max()), "warpage_pv", pv,
          "mesh_sha256", mesh_hash, "main_wall_seconds", elapsed, flush=True)

    repeated, repeated_blocks, _ = run("repeat", original)
    assert abs(warpage(repeated) - pv) / pv <= 1e-12
    assert repeated["mesh"]["mesh_sha256"] == mesh_hash
    for name, block in blocks.items():
        for field in ("Points", "PointData/T", "PointData/U"):
            np.testing.assert_allclose(repeated_blocks[name][field], block[field],
                                       rtol=1e-12, atol=0)
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
