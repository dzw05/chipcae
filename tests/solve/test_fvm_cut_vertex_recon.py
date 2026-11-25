#!/usr/bin/env python3
"""DEMO-GATE cut-point temperature must not overshoot the FVM solution (fvm_pipeline.cpp Reconstruction::at).

Cut points on convective/adiabatic boundary faces are extrapolated from the
cell centre with a Green-Gauss gradient; in a heat-generating cell with a
near-adiabatic top this overshoots by ~q h^2/(4k). Hex vertices already use
boundary-face temperatures (solvers/fvm/src/thermal.cpp T_vertex).
Gate: max cut PointData/T <= max hex PointData/T + 1e-3 * rise, and the
max over both blocks within DEMO_GATE_RISE_RTOL of the FEM node max.
Red on f83c52c: cut max 320.491 K vs hex 318.904 K; rise error vs FEM 7.68 %.
Usage: test_fvm_cut_vertex_recon.py <chipcae-solve> <repo-root> [--with-fem]
"""
import json
import math
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import h5py
    import numpy as np  # noqa: F401
except ImportError as error:
    print(f"未执行: {error}")
    sys.exit(77)

DEMO_GATE_RISE_RTOL = 0.01


def solve(exe, repo, tmp, fvm):
    d = Path(tmp) / ("fvm" if fvm else "fem")
    shutil.copytree(Path(repo) / "examples/demo_gate", d)
    case = json.loads((d / "case.json").read_text())
    if fvm:
        for p in case["physics"]:
            p["solver"] = p["solver"].replace("fem.", "fvm.")
        case["solver"]["linear"] = {"preconditioner": "gamg", "max_iter": 2000}
    (d / "case.json").write_text(json.dumps(case, indent=1))
    start = time.perf_counter()
    proc = subprocess.run([exe, "--case", "case.json", "--profile", "profile.json", "--out", "out"], cwd=d,
                          capture_output=True, text=True)
    print(f"实测 {'FVM' if fvm else 'FEM'} wall={time.perf_counter() - start:.3f} s")
    assert proc.returncode == 0, proc.stderr[-2000:]
    m = json.loads((d / "out/manifest.json").read_text())
    with h5py.File(d / "out" / m["steps"][0]["file"]) as f:
        return {b: float(f["VTKHDF"][b]["PointData/T"][()].max()) for b in ("hex", "cut")}, case


def main(exe, repo, with_fem):
    with tempfile.TemporaryDirectory(prefix="chipcae-cut-vertex-") as tmp:
        fvm, case = solve(exe, repo, tmp, True)
        t_ref = next(l["T"] for l in case["loads"] if l["type"] == "fixed_temperature")
        assert math.isfinite(t_ref) and all(math.isfinite(t) for t in fvm.values()), fvm
        rise = fvm["hex"] - t_ref
        assert rise > 0, f"FVM comparison rise must be positive: {rise}"
        print(f"实测 FVM vertex max hex={fvm['hex']:.6f} cut={fvm['cut']:.6f} T_ref={t_ref}")
        if with_fem:
            fem, _ = solve(exe, repo, tmp, False)
            assert all(math.isfinite(t) for t in fem.values()), fem
            vf, vv = max(fem.values()), max(fvm.values())
            assert vf - t_ref > 0, f"FEM comparison rise must be positive: {vf - t_ref}"
            err = abs(vv - vf) / (vf - t_ref)
            print(f"实测 FEM vertex max={vf:.6f}; rise rel err={err:.5f}")
        assert fvm["cut"] <= fvm["hex"] + 1e-3 * rise, (
            f"cut boundary temperature overshoot: cut={fvm['cut']:.6f}, "
            f"hex={fvm['hex']:.6f}, allowed={1e-3 * rise:.6f} K"
        )
        if with_fem:
            assert err <= DEMO_GATE_RISE_RTOL, f"FVM/FEM peak rise relative error: {err:.5f} > 0.01"
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], "--with-fem" in sys.argv))
