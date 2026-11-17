# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Lint-safe checks for the WP-2 CalculiX REFERENCE artifact.

Does not import numpy and does not invoke ccx. The file records a reference
value (not a measurement). No pass/fail is raised on the prototype delta;
that criterion is left to the V&V specialist.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIR = ROOT / "tools/vv/calculix_trilayer"
REF = DIR / "REFERENCE.json"
CONTRACT = DIR / "reference.schema.json"
PROTOTYPE = 4.269532769e-3


class TrilayerCalculixRefTest(unittest.TestCase):
    def test_reference_schema_and_report_delta(self):
        with REF.open(encoding="utf-8") as handle:
            data = json.load(handle)
        with CONTRACT.open(encoding="utf-8") as handle:
            contract = json.load(handle)
        self.assertEqual(contract.get("$schema"), "https://json-schema.org/draft/2020-12/schema")
        self.assertIn("required", contract)
        self.assertIn("properties", contract)
        for key in contract["required"]:
            self.assertIn(key, data, f"missing required field {key}")
        self.assertNotIn("host", data)
        self.assertNotIn("label", data)
        import jsonschema
        jsonschema.Draft202012Validator(contract).validate(data)
        self.assertEqual(data["schema_version"], "1")
        self.assertEqual(data["kind"], "REFERENCE")
        self.assertEqual(data["element_type"], "C3D20")
        self.assertEqual(data["selected_mesh"], "fine")
        self.assertIsInstance(data.get("calculix_version"), str)
        self.assertTrue(data["calculix_version"].strip())
        self.assertNotIn("host", data)
        self.assertIsInstance(data.get("provenance"), dict)
        self.assertIsInstance(data["W_bfp_m"], float)
        self.assertGreater(data["W_bfp_m"], 0.0)
        self.assertAlmostEqual(data["prototype_W_bfp_m"], PROTOTYPE, places=12)
        names = [m["name"] for m in data["meshes"]]
        self.assertEqual(names, ["coarse", "fine"])
        for mesh in data["meshes"]:
            self.assertEqual(mesh["n_elem_z_per_layer"], [2, 2, 2])
            self.assertEqual(len(mesh["n_elem_xyz"]), 3)
            self.assertIsInstance(mesh["W_bfp_m"], float)
        selected = next(m for m in data["meshes"] if m["name"] == "fine")
        self.assertAlmostEqual(data["W_bfp_m"], selected["W_bfp_m"], places=15)
        w = data["W_bfp_m"]
        abs_delta = abs(w - PROTOTYPE)
        rel_delta = abs_delta / PROTOTYPE
        self.assertAlmostEqual(data["abs_delta_m"], abs_delta, places=15)
        self.assertAlmostEqual(data["rel_delta"], rel_delta, places=12)
        print(
            f"W_bfp={w} prototype=4.269532769e-3 abs_delta={abs_delta} "
            f"rel_delta={rel_delta} mesh=fine element=C3D20 "
            f"ccx={data['calculix_version']}"
        )


if __name__ == "__main__":
    unittest.main()
