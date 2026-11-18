"""Synthetic cut bilayer: signed curvature, 1D heat, and MPI acceptance.

Catch inaccurate cut integration/interpolation or rank-dependent results using
analytic references. Keep all mandated gates, including coarse fit sufficiency,
and collect numerical failures so every completed solve gets a validated record.
"""
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
sys.path.insert(0, str(source))
from tools.vv.derive_cards import timoshenko
from tools.vv.check_result_data_class import check_file

kappa_ref = timoshenko(100e9, .02, 5e-6, 200e9, .02, 15e-6, -100)
assert math.isfinite(kappa_ref) and abs(kappa_ref) > 0, kappa_ref
print(f"kappa_ref={kappa_ref:.17g}", flush=True)
revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=source,
                          capture_output=True, text=True, check=True).stdout.strip()

POLYGONS = {
    "A": [[0, 0], [.9, 0], [.9, .9], [0, .9]],
    "B": [[.1, 0], [.9, 0], [1, .1], [1, .9], [.9, 1], [.1, 1], [0, .9], [0, .1]],
    "C": [[0, 0], [.9, 0], [.9, .9], [0, .9]],
}


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
                    encoding="utf-8")


def make_inputs(geometry, nx, min_per_layer, ranks, thermal):
    def table(columns):
        return {"temperatures": [300], "columns": {k: [v] for k, v in columns.items()},
                "interpolation": "constant", "extrapolation": "clamp"}

    materials = []
    for name, E, alpha, k in (("base", 100e9, 5e-6, 1), ("top", 200e9, 15e-6, 10)):
        materials.append({
            "id": name, "schema_version": "1", "unit_system": "SI", "model": "thermo_elastic",
            "density": 2000, "conductivity": k,
            "elastic": {"symmetry": "isotropic", "table": table({"E": E, "nu": 0})},
            "thermal_expansion": {
                "symmetry": "isotropic", "cte_type": "tangent", "stress_free_temperature": 400,
                "table": table({"alpha": alpha})},
            "provenance": {"source_type": "illustrative", "confidence": "illustrative"}})
    stack = {
        "id": "synthetic-cut-bimetal", "unit": "m", "symmetry": "none", "package_type": "FCBGA",
        "outline": {"box": [0, 0, .9, .9] if geometry == "C" else [0, 0, 1, 1],
                    "polygon": POLYGONS[geometry]},
        "layers": [{"name": name, "kind": "Explicit", "material": name, "thickness": .02,
                    "n_elem_min": 1, "grading": 1} for name in ("base", "top")]}
    case = {
        "schema": "chipcae.case/1.0", "stack": {"ref": "stack.json"},
        "materials": {"ref": "materials.json"},
        "mesh": {"type": "aligned_cartesian_cut", "nx": nx, "ny": nx, "levels": 1,
                 "min_elems_per_layer": min_per_layer, "max_ratio": 10,
                 "cut": {"stabilization": "ghost_penalty", "quad_degree": 6, "gamma_g": .1}},
        "physics": [{"id": "mech", "solver": "fem.solid"}],
        "loads": [{"type": "uniform_temperature", "T_ref": 400, "T_target": 300}],
        "constraints": {"rigid": "3-2-1"},
        "solver": {"linear": {"preconditioner": "direct"}},
        "output": {"format": "vtkhdf", "warpage": {
            "standard": "JESD22-B112", "remove_rigid": "best_fit_plane"}}}
    if thermal:
        case["physics"] = [
            {"id": "thermal", "solver": "fem.conduction", "mode": "steady"},
            {"id": "mech", "solver": "fem.solid", "temp_from": "thermal", "T_ref": 298.15}]
        case["loads"] = [
            {"type": "fixed_temperature", "target": "face:zmin", "T": 298.15},
            {"type": "convection", "target": "face:zmax", "h": 1000, "T_inf": 348.15}]
    return {"case.json": case, "stack.json": stack,
            "materials.json": {"schema": "chipcae.material_library/1", "materials": materials},
            "profile.json": {"schema": "chipcae.run_profile/1.0", "backend": "cpu",
                             "gpus": 0, "ranks": ranks}}


def read_fields(path):
    blocks = {}
    with h5py.File(path, "r") as fields:
        for name in ("hex", "cut"):
            if name == "cut" and "VTKHDF/cut" not in fields:
                continue  # Old format: zero cut cells, rejected by the cut-case gate.
            block = fields[f"VTKHDF/{name}"]
            points, u, t = (block[key][:] for key in ("Points", "PointData/U", "PointData/T"))
            counts = block["NumberOfCells"][:]
            assert points.ndim == 2 and points.shape[1] == 3, (name, points.shape)
            assert u.shape == points.shape and t.shape == (len(points),), (name, u.shape, t.shape)
            assert counts.ndim == 1 and np.issubdtype(counts.dtype, np.integer), (name, counts)
            assert np.all(counts >= 0), (name, counts)
            for key, values in (("Points", points), ("U", u), ("T", t)):
                assert np.all(np.isfinite(values)), f"non-finite {name}/{key}"
            # Datasets concatenate every output partition; do not select rank zero.
            blocks[name] = (points, u, t, int(np.sum(counts)))
    assert sum(b[3] for b in blocks.values()) > 0, "no volume cells"
    return blocks


def fit_curvature(blocks, polygon):
    points = np.concatenate([b[0] for b in blocks.values()])
    u = np.concatenate([b[1] for b in blocks.values()])
    top = np.abs(points[:, 2] - .04) <= 1e-9
    xy, w = points[top, :2], u[top, 2]
    distance = np.full(len(xy), np.inf)
    vertices = np.asarray(polygon, dtype=float)
    for start, end in zip(vertices, np.roll(vertices, -1, axis=0)):
        edge = end - start
        projection = np.clip((xy - start) @ edge / (edge @ edge), 0, 1)
        distance = np.minimum(distance, np.linalg.norm(xy - start - projection[:, None] * edge,
                                                     axis=1))
    xy, w = xy[distance >= .1], w[distance >= .1]
    xy, unique = np.unique(xy, axis=0, return_index=True)  # Lexicographic, deterministic.
    w = w[unique]
    x, y = xy.T
    matrix = np.column_stack((np.ones(len(x)), x, y, x*x, y*y, x*y))
    coefficients, _, rank, _ = np.linalg.lstsq(matrix, w, rcond=None)
    count = len(xy)
    detail = f"fit count={count} rank={rank} (requires count>=20, rank=6)"
    print(detail, flush=True)
    assert np.all(np.isfinite(coefficients)), f"non-finite coefficients: {detail}"
    # Count/rank acceptance is collected by run_case, allowing remaining solves
    # and numerical comparisons to run. An undersampled fit never passes.
    return {"count": count, "rank": int(rank), "coefficients": coefficients.tolist(),
            "kappa_h": float(coefficients[3] + coefficients[4])}


def temperature_error(blocks):
    heat_flux = 50 / (.02 / 1 + .02 / 10 + 1 / 1000)
    worst = None
    for name, (points, _, t, _) in blocks.items():
        if not len(points):
            continue
        z = points[:, 2]
        expected = 298.15 + heat_flux * (np.clip(z, 0, .02) + np.clip(z - .02, 0, .02) / 10)
        errors = np.abs(t - expected)
        index = int(np.argmax(errors))
        candidate = {"value": float(errors[index]), "block": name,
                     "coordinate": points[index].tolist(), "measured": float(t[index]),
                     "reference": float(expected[index])}
        if worst is None or candidate["value"] > worst["value"]:
            worst = candidate
    assert worst is not None, "no temperature points"
    print(f"thermal maximum error: {worst}", flush=True)
    return worst


def run_case(root, geometry, nx, min_per_layer, ranks=1, thermal=False, launcher=None):
    case_id = f"cut_bimetal_{'thermal' if thermal else geometry}_nx{nx}_ranks{ranks}"
    work = root / case_id
    work.mkdir()
    inputs = make_inputs(geometry, nx, min_per_layer, ranks, thermal)
    for name, value in inputs.items():
        write(work / name, value)
    result = {"case_id": case_id, "work": work, "thermal": thermal, "nx": nx,
              "failures": [], "summary": {}, "fit": None, "cut_cells": None,
              "hex_cells": None, "kappa_h": None, "error": None, "temperature": None}

    def expect(condition, message):
        if not condition:
            result["failures"].append(message)

    command = [exe, "--case", str(work / "case.json"), "--profile", str(work / "profile.json"),
               "--out", str(work / "solve")]
    if launcher:
        command = [launcher, "-n", str(ranks), *command]
    print(f"{case_id} command={command!r}", flush=True)
    try:
        completed = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=420)
    except (OSError, subprocess.TimeoutExpired) as error:
        detail = (f"command={command!r}\nlaunch/timeout failure: {error}\n"
                  f"stdout:\n{getattr(error, 'stdout', None)}\n"
                  f"stderr:\n{getattr(error, 'stderr', None)}")
        print(detail, flush=True)
        result["failures"].append(detail)
        return result
    detail = (f"command={command!r}\nexit={completed.returncode}\n"
              f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}")
    print(detail, flush=True)
    if completed.returncode != 0:
        result["failures"].append(detail)
        return result
    try:
        manifest = read(work / "solve/manifest.json")
        summary = {item["name"]: item["value"] for item in manifest["summary"]}
        for key in ("warpage_pv", "U_mag_max", "T_max"):
            assert type(summary[key]) in (float, int) and math.isfinite(summary[key]), (key, summary)
        result["summary"] = {key: summary[key] for key in ("warpage_pv", "U_mag_max", "T_max")}
        events = (work / "solve/events.bin").read_bytes()
        expect(b"3-2-1 constrained_dofs=6 (3+2+1)" in events, "missing six-DOF event evidence")
        blocks = read_fields(work / "solve/out/step-0.vtkhdf")
        result["hex_cells"] = blocks["hex"][3]
        result["cut_cells"] = blocks["cut"][3] if "cut" in blocks else 0
        expect(result["cut_cells"] == 0 if geometry == "C" else result["cut_cells"] > 0,
               f"cut-cell assertion failed: geometry={geometry} cut_cells={result['cut_cells']}")
        if thermal:
            result["temperature"] = temperature_error(blocks)
            result["error"] = result["temperature"]["value"]
            expect(result["error"] <= 1e-6, f"1e-6 K assertion failed: {result['temperature']}")
        else:
            fit = result["fit"] = fit_curvature(blocks, POLYGONS[geometry])
            expect(fit["count"] >= 20 and fit["rank"] == 6,
                   f"fit assertion failed: count={fit['count']} rank={fit['rank']} (need >=20 and 6)")
            result["kappa_h"] = fit["kappa_h"]
            result["error"] = abs(fit["kappa_h"] - kappa_ref) / abs(kappa_ref)
            if nx == 16:
                expect(result["error"] <= .03,
                       f"3% accuracy assertion failed: e_kappa={result['error']:.17g}")
                if geometry in ("A", "B"):
                    d, e, f = fit["coefficients"][3:]
                    result["isotropy"] = {"d_minus_e": abs(d - e) / abs(kappa_ref),
                                          "f": abs(f) / abs(kappa_ref)}
                    expect(result["isotropy"]["d_minus_e"] <= .02 and result["isotropy"]["f"] <= .02,
                           f"2% isotropy assertion failed: {result['isotropy']}")
    except (AssertionError, KeyError, ValueError, OSError, np.linalg.LinAlgError) as error:
        result["failures"].append(f"artifact assertion failed: {error}")
    print(f"实测 {case_id}: kappa_h={result['kappa_h']} error={result['error']} "
          f"summary={result['summary']} cut_cells={result['cut_cells']} "
          f"hex_cells={result['hex_cells']} fit={result['fit']}", flush=True)
    return result


def relative_difference(reference, measured):
    assert reference is not None and measured is not None, "missing comparison quantity"
    assert math.isfinite(reference) and math.isfinite(measured) and abs(reference) > 0, (
        "comparison requires finite values and nonzero denominator", reference, measured)
    return abs(measured - reference) / abs(reference)


def write_result(run):
    thermal = run["thermal"]
    qoi = {"temperature_max_error" if thermal else "kappa_h":
           run["error"] if thermal else run["kappa_h"],
           **run["summary"], "cut_cells": run["cut_cells"], "hex_cells": run["hex_cells"],
           "fit": run["fit"], "temperature": run["temperature"]}
    for key in ("isotropy", "cut_control_difference", "mpi_differences"):
        if key in run:
            qoi[key] = run[key]
    path = run["work"] / "result.json"
    write(path, {
        "schema": "chipcae.vv_result/0.2", "case_id": run["case_id"],
        "data_class": "synthetic", "label": "实测",
        "date": datetime.now(timezone.utc).strftime("%Y%m%d"), "commit": revision,
        "qoi": qoi, "reference": {"value": 0 if thermal else kappa_ref, "type": "解析",
            "source": "1D 串联热阻" if thermal else "Timoshenko 1925 双材料条，tools/vv/derive_cards.py"},
        "error": {"value": run["error"], "metric": "max_absolute_K" if thermal else "relative_curvature"},
        "tolerance": {"value": 1e-6 if thermal else .03},
        "verdict": "fail" if run["failures"] else "pass", "notes": run["failures"]})
    violations = check_file(path)
    assert violations == [], (path, violations)
    print(f"validated result.json: {run['case_id']} verdict={'fail' if run['failures'] else 'pass'}",
          flush=True)


with tempfile.TemporaryDirectory(prefix="cut-bimetal-") as tmp:
    print(f"workspace={tmp}", flush=True)
    root = Path(tmp)
    runs = {}
    for geometry in ("A", "B"):
        for nx, minimum in ((4, 1), (8, 2), (16, 4)):
            runs[geometry, nx, 1] = run_case(root, geometry, nx, minimum)
    runs["C", 16, 1] = run_case(root, "C", 16, 4)
    launcher = shutil.which("mpiexec") or shutil.which("mpirun")
    if launcher is None:
        print("SKIP ranks=2: launcher not found", flush=True)
    else:
        runs["A", 8, 2] = run_case(root, "A", 8, 2, ranks=2, launcher=launcher)
    runs["thermal", 8, 1] = run_case(root, "B", 8, 2, thermal=True)

    for geometry in ("A", "B"):
        errors = [runs[geometry, nx, 1]["error"] for nx in (4, 8, 16)]
        print(f"all {geometry} e_kappa={errors}", flush=True)
        for coarse, fine in zip(errors, errors[1:]):
            if coarse is not None and fine is not None and coarse > 0 and fine > 0:
                print(f"{geometry} diagnostic log2(e_coarse/e_fine)={math.log2(coarse / fine):.17g}",
                      flush=True)
    finest = runs["A", 16, 1]
    try:
        difference = relative_difference(runs["C", 16, 1]["kappa_h"], finest["kappa_h"])
        finest["cut_control_difference"] = difference
        print(f"cut/control relative difference={difference:.17g}", flush=True)
        assert difference <= .01, f"1% cut/control assertion failed: {difference:.17g}"
    except AssertionError as error:
        finest["failures"].append(str(error))
    if launcher:
        parallel = runs["A", 8, 2]
        parallel["mpi_differences"] = {}
        serial = runs["A", 8, 1]
        for key in ("kappa_h", "warpage_pv", "U_mag_max"):
            try:
                first = serial["kappa_h"] if key == "kappa_h" else serial["summary"].get(key)
                second = parallel["kappa_h"] if key == "kappa_h" else parallel["summary"].get(key)
                difference = relative_difference(first, second)
                parallel["mpi_differences"][key] = difference
                print(f"MPI {key} relative difference={difference:.17g}", flush=True)
                assert difference <= 1e-8, f"1e-8 MPI assertion failed: {key}={difference:.17g}"
            except AssertionError as error:
                parallel["failures"].append(f"MPI {key}: {error}")
    failures = []
    for run in runs.values():
        write_result(run)  # Including failed numerical comparisons, before aggregate failure.
        failures.extend(f"{run['case_id']}: {message}" for message in run["failures"])
    assert not failures, "acceptance assertions failed:\n" + "\n".join(failures)
