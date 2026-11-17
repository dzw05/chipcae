# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Lint-safe checks for the WP-2 CalculiX REFERENCE artifact.

Does not import numpy and does not invoke ccx. The file records a reference
value (not a measurement). Only a broad sanity bound is applied to the prototype delta;
that criterion is left to the V&V specialist.
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import math
import unittest
from pathlib import Path
from unittest import mock


from tools.vv.calculix_trilayer.contract import CONTRACT, validate_reference


ROOT = Path(__file__).resolve().parents[2]
DIR = ROOT / "tools/vv/calculix_trilayer"
REF = DIR / "REFERENCE.json"
PROTOTYPE = 4.269532769e-3


class TrilayerCalculixRefTest(unittest.TestCase):
    def test_contract_rejects_invalid_artifacts(self):
        import jsonschema

        self.assertTrue(REF.is_file(), f"missing required artifact: {REF}")
        data = json.loads(REF.read_text(encoding="utf-8"))
        contract = copy.deepcopy(CONTRACT)
        jsonschema.Draft202012Validator.check_schema(contract)
        validate_reference(data)
        reversed_meshes = copy.deepcopy(data)
        reversed_meshes["meshes"].reverse()
        validate_reference(reversed_meshes, contract)
        variants = []
        for field in ("artifact_version", "provenance"):
            variants.append(dict(data, **{field: 1 if field == "artifact_version" else {}}))
        for label in ("参考值（原型计算值）", "不是实测，不是解析解"):
            variants.append(dict(data, label=label))
        for field in ("label", "W_bfp_m"):
            invalid = copy.deepcopy(data)
            del invalid[field]
            variants.append(invalid)
            invalid = copy.deepcopy(data)
            invalid[field] = 123 if field != "W_bfp_m" else "invalid"
            variants.append(invalid)
        for field in ("host", "calculix_version"):
            for value in (123, "", None, True):
                variants.append(dict(data, **{field: value}))
            invalid = copy.deepcopy(data)
            invalid.pop(field, None)
            validate_reference(invalid, contract)
            moved = copy.deepcopy(invalid)
            moved["provenance"] = {field: "optional metadata"}
            variants.append(moved)
        portable = copy.deepcopy(data)
        for field in ("host", "calculix_version"):
            portable.pop(field, None)
        validate_reference(portable, contract)
        for name in ("", "unknown", "coarse"):
            invalid = copy.deepcopy(data)
            invalid["selected_mesh"] = name
            variants.append(invalid)
        for names in (("coarse", "coarse"), ("fine", "fine"),
                      ("coarse", "unknown"), ("coarse", "fine", "fine")):
            invalid = copy.deepcopy(data)
            invalid["meshes"] = [dict(data["meshes"][0], name=name) for name in names]
            variants.append(invalid)
        invalid = copy.deepcopy(data)
        selected = next(m for m in invalid["meshes"]
                        if m["name"] == invalid["selected_mesh"])
        selected["W_bfp_m"] *= 1.01
        # Structural checks alone cannot compare instance fields.
        jsonschema.Draft202012Validator(contract).validate(invalid)
        # Full test validation must still reject the mismatched value.
        variants.append(invalid)
        for index, invalid in enumerate(variants):
            with self.subTest(variant=index):
                with self.assertRaises(jsonschema.ValidationError):
                    validate_reference(invalid, contract)

    def test_report_without_optional_provenance(self):
        data = json.loads(REF.read_text(encoding="utf-8"))
        for field in ("host", "calculix_version"):
            data.pop(field, None)
        output = io.StringIO()
        with mock.patch.object(Path, "open", mock.mock_open(read_data=json.dumps(data))):
            with contextlib.redirect_stdout(output):
                self.test_reference_schema_and_report_delta()
        lines = output.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].endswith("mesh=fine element=C3D20 ccx=unknown"))

    def test_reference_schema_and_report_delta(self):
        self.assertTrue(REF.is_file(), f"missing required artifact: {REF}")
        with REF.open(encoding="utf-8") as handle:
            data = json.load(handle)
        contract = copy.deepcopy(CONTRACT)
        self.assertEqual(contract.get("$schema"), "https://json-schema.org/draft/2020-12/schema")
        self.assertIn("required", contract)
        self.assertIn("properties", contract)
        for key in contract["required"]:
            self.assertIn(key, data, f"missing required field {key}")
        validate_reference(data, contract)
        self.assertEqual(data["kind"], "REFERENCE")
        self.assertEqual(data["element_type"], "C3D20")
        self.assertEqual(data["selected_mesh"], "fine")
        self.assertIsInstance(data["label"], str)
        self.assertEqual(
            data["label"],
            "参考值（原型计算值）对照用的独立 CalculiX 结果，不是实测，不是解析解",
        )
        for field in ("host", "calculix_version"):
            if field in data:
                self.assertIsInstance(data[field], str)
                self.assertTrue(data[field].strip())
        geometry = data["geometry"]
        self.assertEqual(geometry["L_m"], 1.0)
        self.assertEqual(geometry["thicknesses_m"], [0.05, 0.01, 0.04])
        self.assertEqual(geometry["E_Pa"], [2.5e10, 3e9, 1.3e11])
        self.assertEqual(geometry["nu"], [0.2, 0.35, 0.28])
        self.assertEqual(geometry["alpha_1_per_K"], [15e-6, 50e-6, 2.6e-6])
        self.assertEqual(geometry["dT_K"], -100.0)
        self.assertAlmostEqual(data["prototype_W_bfp_m"], PROTOTYPE, places=12)
        names = [m["name"] for m in data["meshes"]]
        self.assertCountEqual(names, ["coarse", "fine"])
        for mesh in data["meshes"]:
            self.assertEqual(mesh["n_elem_z_per_layer"], [2, 2, 2])
            self.assertEqual(len(mesh["n_elem_xyz"]), 3)
            self.assertEqual(mesh["n_elem_xyz"][2], sum(mesh["n_elem_z_per_layer"]))
            self.assertIsInstance(mesh["W_bfp_m"], float)
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
        self.assertIsInstance(w, float)
        self.assertGreater(w, 0.0)
        abs_delta = abs(w - PROTOTYPE)
        rel_delta = abs_delta / PROTOTYPE
        self.assertLess(rel_delta, 0.5)
        for field, expected in (("abs_delta_m", abs_delta), ("rel_delta", rel_delta)):
            self.assertTrue(math.isclose(data[field], expected, rel_tol=1e-12, abs_tol=1e-18))
        line = (
            f"W_bfp={w} prototype=4.269532769e-3 abs_delta={abs_delta} "
            f"rel_delta={rel_delta} mesh=fine element=C3D20 "
            f"ccx={data.get('calculix_version', 'unknown')}"
        )
        print(line)


if __name__ == "__main__":
    unittest.main()
