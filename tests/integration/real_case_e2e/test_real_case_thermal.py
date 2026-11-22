"""Synthetic thermal acceptance and shared E2E helpers."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(SOURCE))
from tools.vv import real_case_e2e as driver


def source_root():
    return Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else SOURCE


def ready(kind, source):
    state = driver.readiness(kind, source)
    assert isinstance(state, tuple) and len(state) == 2, "readiness contract is not implemented"
    if not state[0]:
        print(f"SKIP: {kind} upstream not merged: {state[1]} (NEEDS-ATTENTION)")
    return state[0]


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def thermal_params(root, source):
    shutil.copyfile(source / "tests/integration/ecad/data/board_4layer.xml", root / "board.xml")
    shutil.copyfile(source / "tests/integration/f1130_uncut_cli/material_map.json", root / "map.json")
    return {"case_id": "generated-board", "data_class": "synthetic", "input": str(root / "board.xml"),
            "material_map": str(root / "map.json"), "materials": None,
            "mesh": {"nx": 4, "ny": 4, "levels": 0, "max_ratio": 100, "min_elems_per_layer": 1}}


def solve_runs(record):
    for command in record["commands"]:
        argv = command["argv"]
        if "--case" not in argv or "--out" not in argv:
            continue
        case = read_json(Path(argv[argv.index("--case") + 1]))
        out = Path(argv[argv.index("--out") + 1])
        yield case, out, read_json(out / "manifest.json")


def run_synthetic(kind, source, make_params=thermal_params, inspect=None):
    # Keep this before fixtures, imports, generation or solving.
    if not ready(kind, source):
        return 77
    # Each artifact lives under the temporary root and is removed on exit.
    with tempfile.TemporaryDirectory(prefix="chipcae-realcase-e2e-") as tmp:
        root = Path(tmp)
        params = make_params(root, source)
        if params is None:
            return 77
        mpi = os.environ.get("CHIPCAE_MPIEXEC") or shutil.which("mpiexec") or shutil.which("mpirun")
        nps = [1, 2] if mpi else [1]
        record = driver.run_item(kind, params, root / "outputs", nps, 2, source)
        assert isinstance(record, dict), "run_item must return its finalized record"
        rc = record["exit_code"]
        if rc:
            print(json.dumps({"verdict": record["verdict"], "needs_attention": record["needs_attention"],
                              "notes": record["notes"]}, ensure_ascii=False))
            return rc
        assert record["verdict"] == "pass", record
        paths = list((root / "outputs").rglob("run_record.json"))
        assert len(paths) == 1, paths
        folder = paths[0].parent
        provisional_w5 = False
        if kind == "warpage":
            manifest = read_json(folder / "np1_r1/manifest.json")
            method = next(q for q in manifest["summary"] if q["name"] == "warpage_pv").get("method")
            provisional_w5 = method in (driver.WARPAGE_METHOD, "fvm_cell_centers_lsq")
        expected = {f"C{i}" for i in range(1, 11)}
        if kind == "warpage":
            expected |= {f"W{i}" for i in range(1, 6)}
        if kind == "assembly":
            expected |= {"A1", "A2", "A3"}
            w_state = driver.readiness("warpage", source)
            if w_state[0]:
                expected |= {"W1", "W2", "W4"}
            assert not {"W3", "W5"} & set(record["checks"]), record["checks"]
        assert set(record["checks"]) == expected, record["checks"]
        for key in expected:
            if key == "C8" and not mpi:
                assert record["checks"][key] is None
            elif key == "W5" and provisional_w5:
                assert record["checks"][key] == "PROVISIONAL", record["checks"]
            else:
                assert record["checks"][key] is True, (key, record["checks"], record["notes"])
        assert record["digests"]["np1_r1"] == record["digests"]["np1_r2"]
        assert ("np2_r1" in record["digests"]) == bool(mpi)
        assert read_json(paths[0]) == record
        assert (folder / "run_record.md").is_file()
        from tools.vv.check_result_data_class import check_file
        assert check_file(folder / "result.json") == []
        if inspect:
            inspect(record, folder)
    return rc


if __name__ == "__main__":
    raise SystemExit(run_synthetic("thermal", source_root()))
