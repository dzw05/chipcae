# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Lint-safe checks for the WP-2 CalculiX REFERENCE artifact.

Does not import numpy and does not invoke ccx. The file records a reference
value (not a measurement). Only a broad sanity bound is applied to the prototype delta;
that criterion is left to the V&V specialist.
"""

from __future__ import annotations

import copy
import json
import math
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DIR = ROOT / "tools/vv/calculix_trilayer"
REF = DIR / "REFERENCE.json"


class TrilayerCalculixRefTest(unittest.TestCase):
    def validate_artifact(self, data):
        required = {"kind", "label", "W_bfp_m", "element_type", "geometry",
                    "meshes", "selected_mesh", "prototype_W_bfp_m",
                    "abs_delta_m", "rel_delta", "calculix_version",
                    "host"}
        self.assertIsInstance(data, dict)
        self.assertTrue(required <= data.keys(), f"missing required fields: {required - data.keys()}")
        for field in ("calculix_version", "host"):
            self.assertIsInstance(data[field], str)
            self.assertTrue(data[field].strip())
        for field in ("W_bfp_m", "prototype_W_bfp_m", "abs_delta_m", "rel_delta"):
            self.assertIn(type(data[field]), (int, float))
            self.assertTrue(math.isfinite(data[field]))
        self.assertEqual(data["kind"], "REFERENCE")
        self.assertEqual(data["element_type"], "C3D20")
        self.assertEqual(data["selected_mesh"], "fine")
        self.assertIsInstance(data["label"], str)
        self.assertEqual(
            data["label"],
            "参考值（原型计算值）对照用的独立 CalculiX 结果，不是实测，不是解析解",
        )
        geometry = data["geometry"]
        self.assertIsInstance(geometry, dict)
        self.assertTrue({"L_m", "thicknesses_m", "E_Pa", "nu",
                                         "alpha_1_per_K", "dT_K"} <= geometry.keys())
        for field in ("L_m", "thicknesses_m", "E_Pa", "nu", "alpha_1_per_K", "dT_K"):
            value = geometry[field]
            for number in value if isinstance(value, list) else [value]:
                self.assertIn(type(number), (int, float), field)
                self.assertTrue(math.isfinite(number), field)
        self.assertEqual(geometry["L_m"], 1.0)
        self.assertEqual(geometry["thicknesses_m"], [0.05, 0.01, 0.04])
        self.assertEqual(geometry["E_Pa"], [2.5e10, 3e9, 1.3e11])
        self.assertEqual(geometry["nu"], [0.2, 0.35, 0.28])
        self.assertEqual(geometry["alpha_1_per_K"], [15e-6, 50e-6, 2.6e-6])
        self.assertEqual(geometry["dT_K"], -100.0)
        self.assertIsInstance(data["meshes"], list)
        self.assertEqual(len(data["meshes"]), 2)
        names = [m["name"] for m in data["meshes"]]
        self.assertCountEqual(names, ["coarse", "fine"])
        for mesh in data["meshes"]:
            self.assertTrue({"name", "n_elem_xyz", "n_elem_z_per_layer", "W_bfp_m"} <= mesh.keys())
            for field in ("n_elem_xyz", "n_elem_z_per_layer"):
                for number in mesh[field]:
                    self.assertEqual(type(number), int)
                    self.assertGreater(number, 0)
            self.assertEqual(len(mesh["n_elem_z_per_layer"]), 3)
            self.assertTrue(all(n >= 2 for n in mesh["n_elem_z_per_layer"]))
            self.assertEqual(len(mesh["n_elem_xyz"]), 3)
            self.assertEqual(mesh["n_elem_xyz"][2], sum(mesh["n_elem_z_per_layer"]))
            self.assertIn(type(mesh["W_bfp_m"]), (int, float))
            self.assertTrue(math.isfinite(mesh["W_bfp_m"]))
            self.assertGreater(mesh["W_bfp_m"], 0.0)
        meshes = {m["name"]: m for m in data["meshes"]}
        coarse, fine = meshes["coarse"], meshes["fine"]
        for axis in (0, 1):
            self.assertEqual(fine["n_elem_xyz"][axis], 2 * coarse["n_elem_xyz"][axis])
        self.assertEqual(fine["n_elem_z_per_layer"], coarse["n_elem_z_per_layer"])
        selected = meshes[data["selected_mesh"]]
        w = data["W_bfp_m"]
        self.assertEqual(w, selected["W_bfp_m"])
        self.assertIn(type(w), (int, float))
        self.assertGreater(w, 0.0)
        prototype = data["prototype_W_bfp_m"]
        self.assertGreater(w * prototype, 0.0)
        abs_delta = abs(w - prototype)
        rel_delta = abs_delta / abs(prototype)
        self.assertLess(rel_delta, 0.5)
        for field, expected in (("abs_delta_m", abs_delta), ("rel_delta", rel_delta)):
            self.assertTrue(math.isclose(data[field], expected, rel_tol=1e-12, abs_tol=1e-18))
        return w, abs_delta, rel_delta

    def test_rejects_invalid_artifacts(self):
        self.assertTrue(REF.is_file(), f"missing required artifact: {REF}")
        data = json.loads(REF.read_text(encoding="utf-8"))
        reversed_meshes = copy.deepcopy(data)
        reversed_meshes["meshes"].reverse()
        self.validate_artifact(reversed_meshes)
        extended = copy.deepcopy(data)
        extended.pop("SPDX-License-Identifier", None)
        extended["geometry"]["description"] = "model metadata"
        for mesh in extended["meshes"]:
            mesh["description"] = "mesh metadata"
            mesh["n_elem_z_per_layer"] = [3, 2, 2]
            mesh["n_elem_xyz"][2] = 7
        extended["prototype_W_bfp_m"] *= 1.001
        extended["abs_delta_m"] = abs(extended["W_bfp_m"] - extended["prototype_W_bfp_m"])
        extended["rel_delta"] = extended["abs_delta_m"] / extended["prototype_W_bfp_m"]
        self.validate_artifact(extended)
        for field in data.keys() - {"SPDX-License-Identifier"}:
            invalid = copy.deepcopy(data)
            del invalid[field]
            with self.subTest(missing=field), self.assertRaises(AssertionError):
                self.validate_artifact(invalid)
        variants = []
        for field, values in {
            "label": (123, "参考值（原型计算值）", "不是实测，不是解析解"),
            "W_bfp_m": ("invalid", True, float("nan")),
            "selected_mesh": ("", "unknown", "coarse"),
        }.items():
            variants.extend(dict(data, **{field: value}) for value in values)
        for value in (123, "", "   ", None, True):
            for field in ("calculix_version", "host"):
                variants.append(dict(data, **{field: value}))
        for names in (("coarse", "coarse"), ("fine", "fine"),
                      ("coarse", "unknown"), ("coarse", "fine", "fine")):
            invalid = copy.deepcopy(data)
            invalid["meshes"] = [dict(data["meshes"][0], name=name) for name in names]
            variants.append(invalid)
        for field in ("unknown_field", "provenance"):
            self.validate_artifact(dict(data, **{field: "additional metadata"}))
        for index, invalid in enumerate(variants):
            with self.subTest(variant=index), self.assertRaises(AssertionError):
                self.validate_artifact(invalid)
        invalid = copy.deepcopy(data)
        invalid["W_bfp_m"] *= 1.01
        with self.assertRaises(AssertionError):
            self.validate_artifact(invalid)

    def test_reference_schema_and_report_delta(self):
        self.assertTrue(REF.is_file(), f"missing required artifact: {REF}")
        data = json.loads(REF.read_text(encoding="utf-8"))
        w, abs_delta, rel_delta = self.validate_artifact(data)
        line = (
            f"W_bfp={w} prototype={data['prototype_W_bfp_m']} abs_delta={abs_delta} "
            f"rel_delta={rel_delta} mesh=fine element=C3D20 "
            f"ccx={data['calculix_version']}"
        )
        log = Path("/workspace/tasks/logs/v-ccx-delta.md")
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(line + "\n", encoding="utf-8")
        print(line)


if __name__ == "__main__":
    unittest.main()
