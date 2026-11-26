# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Solver acceptance for synthetic strip scheme A; no golden solver outputs."""

from copy import deepcopy
import importlib
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

try:
    import h5py
    import numpy as np
except ModuleNotFoundError as exc:
    if exc.name not in ("numpy", "h5py"):
        raise
    print(f"missing optional dependency: {exc.name}")
    raise SystemExit(77)

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests/integration/fvm_case_e2e"))
from fvm_case_util import bimaterial_strip, curvature, fixture, read_json, write_json
from tools.vv.field_compare import load_run
from tests.vv.fvm_warpage_gate_a.support import (
    available_memory_bytes, measured_solver, memory_preflight,
)


class StripSchemeATests(unittest.TestCase):
    def test_strip_scheme_a(self):
        assert (ROOT / "tools/vv/fvm_warpage_gate.py").is_file(), (
            "missing feature: tools/vv/fvm_warpage_gate.py; cannot judge scheme A")
        gate = importlib.import_module("tools.vv.fvm_warpage_gate")
        executable = os.environ.get("CHIPCAE_SOLVE_EXE") or shutil.which("chipcae-solve")
        self.assertTrue(executable and Path(executable).is_file(), "missing solver backend")
        memory_preflight()
        gate.require_convergence_quantity("warpage_pv")
        gate.require_convergence_quantity("deformation_max")
        with tempfile.TemporaryDirectory(prefix="scheme-a-strip-") as temp:
            root = Path(temp)
            template = root / "template"
            base_case = bimaterial_strip(template)
            base_stack = read_json(template / "stack.json")
            base_materials = read_json(template / "materials.json")

            def solve(family, nx, ny, kz):
                memory_preflight()
                work = root / f"{family}-{nx}-{ny}-kz{kz}"
                fixture(work)
                stack = deepcopy(base_stack)
                for layer in stack["layers"]:
                    layer["n_elem_min"] = kz
                case = deepcopy(base_case)
                case["mesh"].update(nx=nx, ny=ny, min_elems_per_layer=kz)
                if family == "fem":
                    for physics in case["physics"]:
                        physics["solver"] = physics["solver"].replace("fvm.", "fem.")
                    case["solver"]["linear"].update(preconditioner="direct", rtol=1e-10)
                self.assertEqual(case["loads"], base_case["loads"])
                write_json(work / "stack.json", stack)
                write_json(work / "materials.json", deepcopy(base_materials))
                self.assertEqual(read_json(work / "materials.json"), base_materials)
                write_json(work / "case.json", case)
                out = work / "result"
                out.mkdir()
                command = [executable, "--case", work / "case.json", "--profile",
                           work / "profile1.json", "--out", out]
                measurement = measured_solver(command, work, reference=family == "fem")
                fields = load_run(out)
                pv = fields.qoi["warpage_pv"]
                self.assertTrue(math.isfinite(pv) and pv > 0, "invalid/nonpositive PV")
                print(f"{family} nx={nx} ny={ny} kz={kz} PV={pv:.17g}", flush=True)
                return out, measurement

            references = {}

            def reference(kz):
                if kz in references:
                    return references[kz]
                coarse, _ = solve("fem", 12, 4, kz)
                nx, ny = 24, 8
                while True:
                    fine, _ = solve("fem", nx, ny, kz)
                    p_coarse = load_run(coarse).qoi["warpage_pv"]
                    p_fine = load_run(fine).qoi["warpage_pv"]
                    relative = abs(p_fine - p_coarse) / p_fine
                    print(f"FEM reference kz={kz} nx={nx} ny={ny} adjacent_rel={relative:.17g}",
                          flush=True)
                    if relative <= 1e-3:
                        references[kz] = fine
                        return fine
                    coarse, nx, ny = fine, 2 * nx, 2 * ny

            def blocks(run):
                step = read_json(run / "manifest.json")["steps"][0]["file"]
                with h5py.File(run / step, "r") as data:
                    return {name: {key: data[f"VTKHDF/{name}/{key}"][:]
                                   for key in ("Points", "PointData/U")}
                            for name in ("hex", "cut")}

            records = []
            grids = [(48, 16), (96, 32)]
            highmem = sys.platform == "darwin" and os.environ.get("CHIPCAE_VV_HIGHMEM") == "1"
            if highmem:
                grids.append((192, 64))
            else:
                print("192×64 未运行（box 不跑，待 Mac runner）；kz16 未运行", flush=True)
            for nx, ny in grids:
                for kz in (4, 8):
                    fem = reference(kz)
                    fvm, measurement = solve("fvm", nx, ny, kz)
                    error = gate.same_point_error(fvm, fem)
                    def_fem, def_fvm = gate.deformation_max(fem), gate.deformation_max(fvm)
                    self.assertTrue(math.isfinite(def_fem) and def_fem > 0)
                    deformation_rel = def_fvm / def_fem - 1
                    self.assertTrue(math.isfinite(deformation_rel))
                    self.assertGreater(curvature(blocks(fvm)) * curvature(blocks(fem)), 0,
                                       "FVM/FEM warpage direction differs")
                    record = dict(nx=nx, ny=ny, kz=kz,
                                  same_point_rel=error["same_point_rel"],
                                  interp_indicator=error["interp_indicator"],
                                  deformation_rel=deformation_rel)
                    records.append(record)
                    fields = {**error, "deformation_rel": deformation_rel, **measurement}
                    print(f"scheme A nx={nx} ny={ny} kz={kz} " + " ".join(
                        f"{key}={value:.17g}" for key, value in fields.items()
                        if isinstance(value, (int, float))), flush=True)
            if highmem:
                fem = reference(16)
                fvm, _ = solve("fvm", 96, 32, 16)
                error = gate.same_point_error(fvm, fem)
                print("kz16 diagnostic " + " ".join(f"{k}={v:.17g}" for k, v in error.items()),
                      flush=True)
                records.append(dict(nx=96, ny=32, kz=16, same_point_rel=error["same_point_rel"],
                                    interp_indicator=error["interp_indicator"],
                                    deformation_rel=gate.deformation_max(fvm) / gate.deformation_max(fem) - 1))
            for nx, ny in grids:
                e4, e8 = [r["same_point_rel"] for r in records
                          if r["nx"] == nx and r["kz"] in (4, 8)]
                p = math.log2(abs(e4 / e8)) if e4 and e8 else math.nan
                print(f"nx={nx} ny={ny} p_kz={p:.17g}", flush=True)
            # Retain G1's original final-pair in-plane order as well as new kz order.
            for kz in (4, 8):
                pair = [next(r for r in records if r["nx"] == nx and r["kz"] == kz)
                        for nx, _ in grids[-2:]]
                e_coarse, e_fine = [abs(r["same_point_rel"]) for r in pair]
                self.assertGreater(e_fine, 0, "retained G1 in-plane order undefined")
                order = math.log2(e_coarse / e_fine) if e_coarse else math.nan
                print(f"retained G1 kz={kz} p_xy={order:.17g}", flush=True)
                self.assertGreaterEqual(order, 1, "retained G1 in-plane order <1")
            result = gate.scheme_a_gate(records)
            for check in result["checks"]:
                print(" ".join(f"{key}={value:.17g}" if isinstance(value, (int, float))
                               and not isinstance(value, bool) else f"{key}={value}"
                               for key, value in check.items()), flush=True)
            self.assertTrue(result["pass"], f"scheme A gate failed: {result}")


if __name__ == "__main__":
    print(f"preflight observation MemAvailable_bytes={available_memory_bytes()}", flush=True)
    unittest.main(argv=[__file__], verbosity=2)
