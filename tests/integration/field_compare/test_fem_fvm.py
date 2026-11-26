# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""FEM/FVM has only the temperature-rise gate; displacement is diagnostic."""
from dataclasses import replace
import json
from pathlib import Path
import sys
import tempfile

from fixtures import AMBIENT, comparison_module, solve, solver_environment


def main():
    exe, source = solver_environment(sys.argv[1])
    pipeline = source / "apps/chipcae-solve/src/fvm_pipeline.cpp"
    if not pipeline.exists():
        print("SKIP: fvm upstream not merged (NEEDS-ATTENTION)")
        return 77
    header = source / "solvers/fvm/elastic/include/chipcae/solvers/fvm/elastic.hpp"
    elastic = header.exists() and "not solved: fvm thermal-only stage" not in pipeline.read_text()
    with tempfile.TemporaryDirectory(prefix="field-fem-fvm-") as tmp:
        root = Path(tmp)
        ref_dir, _ = solve(root, "fem", exe, source)
        other_dir, _ = solve(root, "fvm", exe, source, fvm=True,
                             preconditioner="jacobi", elastic=elastic)
        fc = comparison_module()
        ref, other = fc.load_run(ref_dir), fc.load_run(other_dir)
        assert ref.mesh_sha256 == other.mesh_sha256, "fixture meshes differ"
        assert ref.qoi["T_max"] > AMBIENT, ref.qoi
        if not elastic:
            ref, other = replace(ref, U=None), replace(other, U=None)
        result = fc.compare_runs(ref, other, preset="fem-fvm", t_ambient=AMBIENT, remove_rigid=True)
        if not elastic:
            for key in ("U", "U_mag", "U_rigid_removed"):
                result["fields"][key] = None
            result["notes"].append("fvm elastic not wired")
        result.update(case_id="synthetic-fem-fvm", data_class="synthetic")
        result["ref"]["run_dir"] = ref_dir.name
        result["other"]["run_dir"] = other_dir.name
        print("实测 " + json.dumps(result, ensure_ascii=False, allow_nan=False), flush=True)
        assert result["other"]["cell_T_source"] in ("T_cell", "vertex_mean"), result
        assert len(result["gates"]) == 1, result
        gate = result["gates"][0]
        assert gate["metric"] == "qoi.T_max_rise.rel" and gate["limit"] == 1e-2, gate
        assert gate["pass"] and result["verdict"] == "pass", ("NEEDS-ATTENTION", result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
