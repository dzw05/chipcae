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
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIR = ROOT / "tools/vv/calculix_trilayer"
REF = DIR / "REFERENCE.json"
CONTRACT = DIR / "reference.schema.json"
PROTOTYPE = 4.269532769e-3


def validate_reference(data, contract):
    """Bind the selected value with standard JSON Schema const/contains.

    JSON Schema cannot reference another instance field directly, so normalize
    the contract with the artifact's top-level value before validating it.
    """
    import jsonschema

    normalized = copy.deepcopy(contract)
    normalized["properties"]["meshes"]["allOf"].append({
        "contains": {
            "properties": {
                "name": {"const": data.get("selected_mesh")},
                "W_bfp_m": {"const": data.get("W_bfp_m")},
            },
            "required": ["name", "W_bfp_m"],
        },
        "minContains": 1,
        "maxContains": 1,
    })
    jsonschema.Draft202012Validator(normalized).validate(data)


class TrilayerCalculixRefTest(unittest.TestCase):
    def test_contract_rejects_invalid_artifacts(self):
        import jsonschema

        data = json.loads(REF.read_text(encoding="utf-8"))
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        jsonschema.Draft202012Validator.check_schema(contract)
        validate_reference(data, contract)
        reversed_meshes = copy.deepcopy(data)
        reversed_meshes["meshes"].reverse()
        validate_reference(reversed_meshes, contract)
        # Extra provenance is optional; the brief requires top-level environment strings.
        portable = copy.deepcopy(data)
        portable.pop("provenance", None)
        validate_reference(portable, contract)
        variants = []
        for field in ("host", "calculix_version"):
            metadata = dict(portable, provenance={field: "another environment"})
            validate_reference(metadata, contract)
            for value in (123, ""):
                validate_reference(dict(portable, provenance={field: value}), contract)
        validate_reference(dict(portable, provenance={"future_metadata": 123}), contract)
        for field in ("label", "W_bfp_m", "host", "calculix_version"):
            invalid = copy.deepcopy(data)
            del invalid[field]
            variants.append(invalid)
            invalid = copy.deepcopy(data)
            invalid[field] = 123 if field != "W_bfp_m" else "invalid"
            variants.append(invalid)
        for field in ("host", "calculix_version"):
            variants.append(dict(data, **{field: ""}))
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
        variants.append(invalid)
        for index, invalid in enumerate(variants):
            with self.subTest(variant=index):
                with self.assertRaises(jsonschema.ValidationError):
                    validate_reference(invalid, contract)

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
        validate_reference(data, contract)
        self.assertEqual(data["schema_version"], "1")
        self.assertEqual(data["kind"], "REFERENCE")
        self.assertEqual(data["element_type"], "C3D20")
        self.assertEqual(data["selected_mesh"], "fine")
        self.assertIsInstance(data["label"], str)
        self.assertIn("参考值（原型计算值）", data["label"])
        for field in ("host", "calculix_version"):
            self.assertIsInstance(data[field], str)
            self.assertTrue(data[field].strip())
        self.assertAlmostEqual(data["prototype_W_bfp_m"], PROTOTYPE, places=12)
        names = [m["name"] for m in data["meshes"]]
        self.assertCountEqual(names, ["coarse", "fine"])
        for mesh in data["meshes"]:
            self.assertEqual(mesh["n_elem_z_per_layer"], [2, 2, 2])
            self.assertEqual(len(mesh["n_elem_xyz"]), 3)
            self.assertEqual(mesh["n_elem_xyz"][2], sum(mesh["n_elem_z_per_layer"]))
            self.assertIsInstance(mesh["W_bfp_m"], float)
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
            f"ccx={data['calculix_version']}"
        )
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "v-ccx-delta.md"
            log.write_text(line + "\n", encoding="utf-8")
            self.assertEqual(log.read_text(encoding="utf-8"), line + "\n")
        print(line)


if __name__ == "__main__":
    unittest.main()
