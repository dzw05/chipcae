# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Self-generated fields and small solver cases; no stored solver results."""
import importlib
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import zlib

try:
    import h5py
    import numpy as np
except ImportError as error:
    print(f"SKIP: numpy/h5py unavailable: {error}")
    raise SystemExit(77)

SOURCE = Path(os.environ.get("PROJECT_SOURCE_DIR", Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(SOURCE))
SCRIPT = SOURCE / "tools/vv/field_compare.py"
AMBIENT = 300.0
PRESETS = ("same-mesh-solver", "ranks", "fem-fvm")
FIELD_KEYS = {"T", "cell_T", "U", "U_mag", "U_rigid_removed"}
GATE_KEYS = {"fields.T.max_rel", "fields.U.max_rel", "qoi.warpage_pv.rel",
             "qoi.T_max.rel", "qoi.U_mag_max.rel"}


def comparison_module():
    try:
        return importlib.import_module("tools.vv.field_compare")
    except ModuleNotFoundError as error:
        if error.name != "tools.vv.field_compare":
            raise
        raise AssertionError(
            "Missing acceptance feature: tools.vv.field_compare must provide "
            "load_run(), compare_runs(), and the field comparison CLI"
        ) from None


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def cube(left, right):
    return np.array([(left, 0, 0), (right, 0, 0), (right, 1, 0), (left, 1, 0),
                     (left, 0, 1), (right, 0, 1), (right, 1, 1), (left, 1, 1)],
                    dtype="float64")


def synthetic_run(run, *, t_cell=False, empty_cut=False, empty_hex=False, permute=False):
    """Unequal partition allocations expose independent point/cell/offset cursors.

    Hex volumes are 1,2,3 and cut volume is 4. Rank boundary x=3 has
    four duplicate vertices. All rank offsets and connectivity restart at zero.
    """
    run.mkdir(parents=True, exist_ok=True)
    step = run / "out/step-0.vtkhdf"
    step.parent.mkdir()
    p0 = np.unique(np.concatenate((cube(0, 1), cube(1, 3))), axis=0)
    conn0 = [np.array([np.flatnonzero(np.all(p0 == v, axis=1))[0] for v in cube(a, b)])
             for a, b in ((0, 1), (1, 3))]
    blocks = {
        "hex": [(p0, conn0, [7, 11], [1., 2.]),
                (cube(3, 6), [np.arange(8)], [23], [3.])],
        "cut": [(cube(7, 11), [np.arange(8)], [7], [4.]),
                (np.empty((0, 3)), [], [], [])],
    }
    for name, empty in (("hex", empty_hex), ("cut", empty_cut)):
        if empty:
            blocks[name] = [(np.empty((0, 3)), [], [], []) for _ in range(2)]
    with h5py.File(step, "w") as data:
        root = data.create_group("VTKHDF")
        root.attrs.update(Version=[2, 8], Type=np.bytes_("PartitionedDataSetCollection"))
        for index, (name, ranks) in enumerate(blocks.items()):
            block = root.create_group(name)
            block.attrs.update(Version=[2, 8], Type=np.bytes_("UnstructuredGrid"), Index=index)
            if permute:
                ranks = ranks[::-1]
            arrays = {key: [] for key in ("Points", "Connectivity", "Offsets", "CellData/elem_gid",
                                          "CellData/volume", "CellData/T_cell")}
            pc, cc, nc = [], [], []
            for points, cells, gids, volumes in ranks:
                if permute:
                    order = np.arange(len(points))[::-1]
                    inverse = np.argsort(order)
                    points = points[order]
                    cells = [inverse[c] for c in cells[::-1]]
                    gids, volumes = gids[::-1], volumes[::-1]
                temps = AMBIENT + 4*points[:, 0] + 2*points[:, 1] + points[:, 2]
                arrays["Points"].append(points)
                arrays["Connectivity"].extend(np.concatenate(cells).tolist() if cells else [])
                arrays["Offsets"].extend(np.cumsum([0] + [len(c) for c in cells]).tolist())
                arrays["CellData/elem_gid"].extend(gids)
                arrays["CellData/volume"].extend(volumes)
                arrays["CellData/T_cell"].extend(float(np.mean(temps[c])) + 10 for c in cells)
                pc.append(len(points))
                cc.append(len(cells))
                nc.append(sum(len(c) for c in cells))
            block["NumberOfPoints"] = np.array(pc, dtype="int64")
            block["NumberOfCells"] = np.array(cc, dtype="int64")
            block["NumberOfConnectivityIds"] = np.array(nc, dtype="int64")
            points = np.concatenate(arrays.pop("Points"))
            block["Points"] = points
            for key, value in arrays.items():
                if key.endswith("T_cell") and not t_cell:
                    continue
                dtype = "float64" if key.endswith(("volume", "T_cell")) else "int64"
                block[key] = np.array(value, dtype=dtype)
            block["Types"] = np.full(sum(cc), 12 if name == "hex" else 42, dtype="uint8")
            for key in ("layer_id", "material_id", "cell_kind"):
                block["CellData/" + key] = np.zeros(sum(cc), dtype="int32")
            block["PointData/T"] = AMBIENT + 4*points[:, 0] + 2*points[:, 1] + points[:, 2]
            block["PointData/U"] = np.column_stack((.125*points[:, 0]**2,
                                                     .25*points[:, 1]*points[:, 2],
                                                     .5*points[:, 0]*points[:, 2]))
        for name in ("eb_surface", "particles"):
            group = root.create_group(name)
            group.attrs["Version"] = [2, 8]
        root["eb_surface/Points"] = np.empty((0, 3), dtype="float64")
        root["eb_surface/NumberOfPoints"] = np.array([0, 0], dtype="int64")
        root["eb_surface/Polygons/NumberOfCells"] = np.array([0, 0], dtype="int64")
        assembly = root.create_group("Assembly")
        for category in ("volume", "boundary", "lagrangian"):
            assembly.create_group(category)
        for key, target in (("volume/hex", "hex"), ("volume/cut", "cut"),
                            ("boundary/eb_surface", "eb_surface"),
                            ("lagrangian/particles", "particles")):
            assembly[key] = h5py.SoftLink("/VTKHDF/" + target)
    manifest = {
        "schema_version": "chipcae.results/1.0",
        "mesh": {"nx0": 3, "ny0": 1, "L": 1, "padded_cells": 3, "nz": 1,
                 "mesh_sha256": "a"*64},
        "steps": [{"n": 0, "file": "out/step-0.vtkhdf"}],
        "summary": [{"name": name, "value": value, "method": "synthetic",
                     "unit": "K" if name == "T_max" else "m"}
                    for name, value in (("T_max", 347.), ("U_mag_max", 17.),
                                        ("warpage_pv", .25))],
    }
    write_json(run / "manifest.json", manifest)
    return step


def patch_qoi(run, key, value):
    path = run / "manifest.json"
    manifest = json.loads(path.read_text())
    next(item for item in manifest["summary"] if item["name"] == key)["value"] = value
    write_json(path, manifest)


def cli(ref, other, out, *, preset="same-mesh-solver", ambient=AMBIENT, extra=()):
    command = [sys.executable, str(SCRIPT), str(ref), str(other), "--preset", preset,
               "-o", str(out)]
    if ambient is not None:
        command += ["--t-ambient", str(ambient)]
    return subprocess.run([*command, *extra], capture_output=True, text=True, timeout=60)


def events(path):
    content = path.read_bytes()
    result = []
    while content:
        assert content[:8] == b"CAELOG1\x00", "invalid event magic"
        length, crc = struct.unpack_from("<II", content, 8)
        body = content[16:16 + length]
        assert len(body) == length and zlib.crc32(body) == crc, "invalid event frame"
        seq, wall, kind = struct.unpack_from("<QqB", body)
        result.append((kind, body[17:]))
        content = content[16 + length:]
    return result


def solver_environment(source):
    exe = os.environ.get("CHIPCAE_SOLVE_EXE")
    if not exe:
        print("SKIP: CHIPCAE_SOLVE_EXE is unset")
        raise SystemExit(77)
    return exe, Path(source).resolve()


def mpi_launcher():
    configured = os.environ.get("CHIPCAE_MPIEXEC")
    launcher = shlex.split(configured) if configured else [shutil.which("mpiexec")]
    if not launcher or not launcher[0]:
        print("SKIP: MPI launcher not found")
        raise SystemExit(77)
    return launcher


def solve(root, name, exe, source, *, ranks=1, launcher=None, levels=1,
          preconditioner="direct", bbox=False, fvm=False, elastic=True):
    work = root / name
    shutil.copytree(Path(__file__).parent / "data", work)
    case = {
        "schema": "chipcae.case/1.0",
        "stack": {"ref": "stack_bbox_outline.json" if bbox else "stack.json"},
        "materials": {"ref": "materials.json"},
        "mesh": {"type": "aligned_cartesian_cut", "nx": 8, "ny": 8,
                 "levels": levels, "max_ratio": 10},
        "physics": [{"id": "thermal", "solver": "fvm.conduction" if fvm else "fem.conduction",
                     "mode": "steady"}],
        "loads": [{"type": "fixed_temperature", "target": "face:zmin", "T": AMBIENT},
                  {"type": "heat_source", "target": "layer:heater", "power": 10},
                  {"type": "convection", "target": "face:zmax", "h": 10, "T_inf": AMBIENT}],
        "constraints": {"rigid": "3-2-1"},
        "solver": {"linear": {"preconditioner": preconditioner, "rtol": 1e-10}},
        "output": {"format": "vtkhdf",
                   "warpage": {"standard": "JESD22-B112", "remove_rigid": "best_fit_plane"},
                   "fields": ["T", "U", "layer_id", "material_id", "cell_kind", "elem_gid", "volume"]},
    }
    if elastic:
        case["physics"].append({"id": "mech", "solver": "fvm.solid" if fvm else "fem.solid",
                                "temp_from": "thermal", "T_ref": AMBIENT,
                                "nonlinear": {"geometric": False}})
    write_json(work / "case.json", case)
    write_json(work / "profile.json", {"schema": "chipcae.run_profile/1.0", "ranks": ranks,
                                        "backend": "cpu", "gpus": 0})
    out = work / "solve"
    command = [exe, "--case", str(work / "case.json"), "--profile", str(work / "profile.json"),
               "--out", str(out)]
    if launcher:
        command = [*launcher, "-n", str(ranks), *command]
    try:
        completed = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired as error:
        stderr = error.stderr or b""
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        raise AssertionError(f"{name}: status=timeout (600s)\nstderr last 20 lines:\n" +
                             "\n".join(stderr.splitlines()[-20:])) from error
    assert completed.returncode == 0, (
        f"{name}: status={completed.returncode}\nstderr last 20 lines:\n" +
        "\n".join(completed.stderr.splitlines()[-20:]))
    from tools.vv.result_summary import summarize
    summarize(out)  # Exactly once per successful solve, before field loading.
    logs = [payload for kind, payload in events(out / "events.bin") if kind == 20]
    if elastic:
        assert any(b"3-2-1 constrained_dofs=6 (3+2+1)" in log for log in logs), logs
    return out, logs


def owned_ids(run):
    manifest = json.loads((run / "manifest.json").read_text())
    with h5py.File(run / manifest["steps"][0]["file"]) as data:
        for block in ("hex", "cut"):
            group = data["VTKHDF/" + block]
            ids = group["CellData/elem_gid"][:]
            assert ids.dtype.kind in "iu" and np.all(ids >= 0) and np.all(ids < 2**62), ids
            assert len(np.unique(ids)) == len(ids), f"ambiguous elem_gid: {block}: {ids}"
            assert len(ids) == int(np.sum(group["NumberOfCells"][:])), block


def measured_comparison(ref_dir, other_dir, preset):
    fc = comparison_module()
    ref, other = fc.load_run(ref_dir), fc.load_run(other_dir)
    assert ref.mesh_sha256 == other.mesh_sha256, "fixture meshes differ"
    assert ref.qoi["T_max"] > AMBIENT and ref.qoi["warpage_pv"] > 0, ref.qoi
    result = fc.compare_runs(ref, other, preset=preset, t_ambient=AMBIENT, remove_rigid=True)
    result.update(case_id=other_dir.parent.name, data_class="synthetic")
    result["ref"]["run_dir"] = ref_dir.name
    result["other"]["run_dir"] = other_dir.name
    print("实测 " + json.dumps(result, ensure_ascii=False, allow_nan=False), flush=True)
    assert result["verdict"] == "pass", result
    assert {g["metric"] for g in result["gates"]} == GATE_KEYS, result
    assert len(result["gates"]) == 5 and all(g["pass"] for g in result["gates"]), result
    return result
