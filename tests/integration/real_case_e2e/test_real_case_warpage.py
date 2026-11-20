"""Synthetic fixed-face, zero-dT and independent FEM warpage acceptance."""
from test_real_case_thermal import read_json, run_synthetic, solve_runs, source_root


def inspect_warpage(record, folder):
    import h5py
    import numpy as np
    runs = list(solve_runs(record))
    fixed = [(case, out, manifest) for case, out, manifest in runs
             if case.get("constraints") == {"fixed": [{"target": "face:zmin"}]}]
    uniform = [(case, out, manifest) for case, out, manifest in runs
               if len(case.get("loads", [])) == 2
               and {load.get("target") for load in case["loads"]} == {"face:zmin", "face:zmax"}
               and all(load.get("type") == "fixed_temperature"
                       and load.get("T") == case["physics"][1]["T_ref"] for load in case["loads"])]
    fem = [(case, out, manifest) for case, out, manifest in runs
           if all(physics["solver"].startswith("fem.") for physics in case["physics"])]
    assert fixed, "W3 requires a separate fixed-bottom solve"
    assert uniform, "W4 requires a separate zero-dT solve"
    assert fem, "W5 requires the synthetic noncut FEM comparison"
    for _, out, manifest in fixed:
        with h5py.File(out / manifest["steps"][0]["file"]) as output:
            blocks = [output["VTKHDF/" + name] for name in ("hex", "cut")]
            zmin = min(block["Points"][..., 2].min() for block in blocks if block["Points"].size)
            for block in blocks:
                points, u = block["Points"][...], block["PointData/U"][...]
                if points.size:
                    assert np.all(u[points[:, 2] == zmin] == 0), "W3 tolerance is zero"
    for _, out, manifest in uniform:
        qoi = {q["name"]: q["value"] for q in manifest["summary"]}
        assert qoi["warpage_pv"] == 0, qoi
        with h5py.File(out / manifest["steps"][0]["file"]) as output:
            for block in ("hex", "cut"):
                assert np.all(output["VTKHDF/" + block + "/PointData/U"][...] == 0), "W4 tolerance is zero"
    fvm = read_json(folder / "np1_r1/manifest.json")
    fvm_pv = next(q["value"] for q in fvm["summary"] if q["name"] == "warpage_pv")
    fem_pv = next(q["value"] for q in fem[0][2]["summary"] if q["name"] == "warpage_pv")
    with h5py.File(fem[0][1] / fem[0][2]["steps"][0]["file"]) as output:
        assert np.sum(output["VTKHDF/cut/NumberOfCells"][...]) == 0, "W5 reference must be noncut"
    assert fem_pv > 0, "the synthetic thermal gradient must produce a nonzero FEM reference"
    assert abs(fvm_pv - fem_pv) / fem_pv <= .05, (fvm_pv, fem_pv)
    assert "warpage_pv_vertex_recompute" in record["displacement"]


if __name__ == "__main__":
    raise SystemExit(run_synthetic("warpage", source_root(), inspect=inspect_warpage))
