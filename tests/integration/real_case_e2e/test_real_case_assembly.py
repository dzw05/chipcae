"""Generated lid and axis-aligned ring, with assembly material membership checks."""
import copy
import os
from pathlib import Path
import shutil
import subprocess

from test_real_case_thermal import (driver, read_json, run_synthetic, solve_runs,
                                    source_root, write_json)


def assembly_params(root, source):
    generator = os.environ.get("SP4_FIXTURE_GENERATOR")
    importer = os.environ.get("CHIPCAE_IMPORT_EXE")
    if not generator or not Path(generator).is_file() or not importer:
        print("SKIP: assembly fixture generator/import executable missing (NEEDS-ATTENTION)")
        return None
    subprocess.run([generator, str(root), "normal"], check=True, timeout=60)
    shutil.copyfile(source / "tests/integration/ecad/data/board_2layer.xml", root / "board.xml")
    write_json(root / "map.json", {"FR4": "fr4_eq", "copper": "copper"})
    library = read_json(source / "schema/examples/fcbga_demo.materials.json")
    for material in library["materials"]:
        for section in ("elastic", "thermal_expansion"):
            table = material[section]["table"]
            table["temperatures"] = table["temperatures"][:1]
            table["columns"] = {key: values[:1] for key, values in table["columns"].items()}
            table.update(interpolation="constant", extrapolation="clamp")
    for name in ("fr4_eq", "copper"):
        library["materials"].append(dict(copy.deepcopy(library["materials"][0]), id=name))
    write_json(root / "materials.json", library)
    subprocess.run([importer, str(root / "board.xml"), "-o", str(root / "board.stack.json"),
                    "--polyset", str(root / "board.polyset"), "--material-map", str(root / "map.json")],
                   check=True, timeout=60)
    top = read_json(root / "board.stack.json")["layers"][-1]["name"]
    document = {"schema": "chipcae.assembly/0.1",
                "board": {"path": "board.xml", "format": "ipc2581", "material_map": "map.json"},
                "materials": "materials.json", "parts": [
                    {"id": part, "step": part + ".step", "material": "copper",
                     "placement": {"attach": "layer:" + top + "@top"}}
                    for part in ("lid", "stiffener")]}
    write_json(root / "assembly.json", document)
    return {"case_id": "generated-assembly", "data_class": "synthetic",
            "input": str(root / "assembly.json"), "material_map": None, "materials": None,
            "mesh": {"nx": 4, "ny": 4, "levels": 0, "max_ratio": 100, "min_elems_per_layer": 1}}


def inspect_assembly(record, folder):
    import h5py
    import numpy as np
    stack = read_json(folder / "inputs/stack.json")
    regions = [region for layer in stack["layers"] for region in layer.get("regions", [])]
    parts = [region for region in regions if region.get("body") in ("lid", "stiffener")]
    assert {"lid", "stiffener"} <= {part["body"] for part in parts}, parts
    assert all("box" in part and "polygon" not in part for part in parts), parts
    manifest = read_json(folder / "np1_r1/manifest.json")
    with h5py.File(folder / "np1_r1" / manifest["steps"][0]["file"]) as output:
        names = output["VTKHDF/hex/FieldData/material_names"].asstr()[...].tolist()
        ids = output["VTKHDF/hex/CellData/material_id"][...]
        assert {"copper", "fr4_eq"} <= {names[int(i) - 1] for i in ids}, (names, ids)
        assert np.sum(output["VTKHDF/cut/NumberOfCells"][...]) == 0
    assert isinstance(record["assembly"]["welded_interfaces"], int)
    if driver.readiness("warpage", source_root())[0]:
        assert record["checks"]["W4"] is True
        uniform = [(case, out, m) for case, out, m in solve_runs(record)
                   if len(case.get("loads", [])) == 2
                   and {load.get("target") for load in case["loads"]} == {"face:zmin", "face:zmax"}
                   and all(load.get("type") == "fixed_temperature"
                           and load.get("T") == case["physics"][1]["T_ref"] for load in case["loads"])]
        assert uniform, "assembly W4 requires a uniform T_ref solve"
        for _, out, m in uniform:
            assert next(q["value"] for q in m["summary"] if q["name"] == "warpage_pv") == 0
            with h5py.File(out / m["steps"][0]["file"]) as output:
                for block in ("hex", "cut"):
                    assert np.all(output["VTKHDF/" + block + "/PointData/U"][...] == 0)


if __name__ == "__main__":
    raise SystemExit(run_synthetic("assembly", source_root(), assembly_params, inspect_assembly))
