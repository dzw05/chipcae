"""Free bilayer: analytic accuracy, mesh convergence, and MPI agreement."""
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

exe = os.environ.get("CHIPCAE_SOLVE_EXE")
if not exe:
    print("SKIP: CHIPCAE_SOLVE_EXE is unset (numerical backend required)")
    sys.exit(77)
try:
    import h5py
    import numpy as np
except Exception as error:
    print(f"SKIP: h5py and numpy must be importable: {error}")
    sys.exit(77)

source = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(source / "tools/vv"))
sys.path.insert(0, str(source))
from tools.vv.derive_cards import timoshenko

W_ref = abs(timoshenko(100e9, .02, 5e-6, 200e9, .02, 15e-6, -100)) * 1**2 / 4
assert math.isfinite(W_ref) and W_ref > 0, W_ref
print(f"W_ref={W_ref:.17g}", flush=True)
revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=source,
                          capture_output=True, text=True, check=True).stdout.strip()


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


with tempfile.TemporaryDirectory(prefix="bimetal-") as tmp:
    print(f"workspace={tmp}", flush=True)

    def run(nx, nz, ranks, launcher=None):
        name = f"nx{nx}-ny{nx}-nz{nz}-ranks{ranks}"
        work = Path(tmp) / name
        shutil.copytree(Path(__file__).parent / "data", work)
        case = read(work / "case.json")
        case["mesh"].update(nx=nx, ny=nx, min_elems_per_layer=nz // 2)
        assert case["constraints"] == {"rigid": "3-2-1"}
        assert case["loads"] == [{"type": "uniform_temperature", "T_ref": 400, "T_target": 300}]
        write(work / "case.json", case)
        shutil.copyfile(work / f"profile-ranks{ranks}.json", work / "profile.json")
        command = [exe, "--case", str(work / "case.json"), "--profile",
                   str(work / "profile.json"), "--out", str(work / "solve")]
        if launcher:
            command = [launcher, "-n", "2", *command]
        print(f"{name} command={command!r}", flush=True)
        try:
            result = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=420)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise AssertionError(f"command={command!r}\nlaunch/timeout failure: {error}") from error
        detail = (f"command={command!r}\nexit={result.returncode}\n"
                  f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        print(detail, flush=True)
        assert result.returncode == 0, detail
        manifest_path = work / "solve/manifest.json"
        assert manifest_path.is_file(), f"missing manifest: {manifest_path}"
        manifest = read(manifest_path)
        summary = {item["name"]: item["value"] for item in manifest["summary"]}
        assert "warpage_pv" in summary, summary
        measured = summary["warpage_pv"]
        assert isinstance(measured, (int, float)) and math.isfinite(measured) and measured > 0, summary
        mesh = manifest.get("mesh")
        assert isinstance(mesh, dict), f"missing/invalid mesh summary: {mesh!r}"
        counts = tuple(mesh.get(key) for key in ("nx0", "ny0", "nz"))
        assert all(type(v) is int for v in counts) and counts == (nx, nx, nz), counts
        events = (work / "solve/events.bin").read_bytes()
        assert b"3-2-1 constrained_dofs=6 (3+2+1)" in events, "missing six-DOF event evidence"
        assert (work / "solve/out/step-0.vtkhdf").is_file(), "missing VTKHDF artifact"
        error = abs(measured - W_ref) / W_ref
        write(work / "result.json", {
            "schema": "chipcae.vv_result/0.2", "case_id": name,
            "data_class": "synthetic", "label": "实测",
            "date": datetime.now(timezone.utc).strftime("%Y%m%d"), "commit": revision,
            "qoi": {"warpage_pv": measured},
            "reference": {"value": W_ref, "type": "解析",
                          "source": "Timoshenko 1925 双材料条，tools/vv/derive_cards.py"},
            "error": error, "tolerance": .03, "verdict": "pass" if error <= .03 else "fail"})
        print(f"实测 {name}: mesh={counts} W_h={measured:.17g} e_h={error:.17g}", flush=True)
        return measured, error, counts

    runs = [run(nx, nz, 1) for nx, nz in ((4, 2), (8, 4), (16, 8))]
    launcher = shutil.which("mpiexec") or shutil.which("mpirun")
    parallel = run(8, 4, 2, launcher) if launcher else None
    if launcher is None:
        print("SKIP ranks=2: launcher not found", flush=True)
    errors = [r[1] for r in runs]
    detail = f"all rank-1 errors: e_4={errors[0]:.17g}, e_8={errors[1]:.17g}, e_16={errors[2]:.17g}"
    print(detail, flush=True)
    for coarse, fine in zip(errors, errors[1:]):
        if coarse > 0 and fine > 0:
            print(f"diagnostic p={math.log2(coarse / fine):.17g}", flush=True)
    if parallel:
        assert parallel[2] == runs[1][2], (parallel[2], runs[1][2])
        difference = abs(runs[1][0] - parallel[0]) / runs[1][0]
        print(f"rank relative difference={difference:.17g}", flush=True)
    assert errors[2] <= .03, f"3% accuracy assertion failed: {detail}"
    assert errors[1] <= errors[0] + .001 and errors[2] <= errors[1] + .001, f"monotonicity failed: {detail}"
    if parallel:
        assert difference <= 1e-8, f"rank agreement failed: {difference:.17g}"
