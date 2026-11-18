#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""DEMO-GATE artifact acceptance: standard library only, never imports the generator.

The additional evidence objects make (f), temperature transfer, and the reported
heat balance auditable. Each level has n_elements, n_nodes, elements_by_type,
max_aspect_ratio, resources {label, peak_rss_KiB, wall_seconds}, heat_source
{die_volume_m3, q_W_per_m3, integrated_power_W, relative_error},
temperature_transfer {n_nodes, max_abs_delta_K}, and thermal_balance
{bottom_outward_W, top_outward_W, relative_error}. symmetry_check contains the
full-model QoIs and the same evidence; element_cross contains the L3 C3D20 QoIs.
Both checks record relative_difference keyed by the three acceptance QoI names.
decks is a list of {file, sha256}; material properties use SI units as named below.
These are test-first artifact contracts, not computed reference values.
"""

from __future__ import annotations

import copy
import hashlib
import json
import math
import re
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
ARTIFACT_DIR = ROOT / "tools/vv/calculix_demo_gate"
QOIS = ("W_bfp_m", "T_max_K", "T_top_min_K")
T_REF = 298.15


class DemoGateReferenceTest(unittest.TestCase):
    def number(self, value, name, minimum=None):
        self.assertIn(type(value), (int, float), name)
        self.assertTrue(math.isfinite(value), name)
        if minimum is not None:
            self.assertGreaterEqual(value, minimum, name)
        return value

    def close(self, actual, expected, name):
        self.number(actual, name)
        self.assertTrue(math.isclose(actual, expected, rel_tol=1e-12, abs_tol=0.0),
                        f"{name}: stored {actual!r}, recomputed {expected!r}")

    def fields(self, obj, names, name):
        names = set(names)
        self.assertIsInstance(obj, dict, name)
        self.assertTrue(names <= obj.keys(), f"{name}: missing {names - obj.keys()}")

    def text(self, value, name):
        self.assertIsInstance(value, str, name)
        self.assertTrue(value.strip(), name)

    def integer(self, value, name):
        self.assertIs(type(value), int, name)
        self.assertGreater(value, 0, name)

    def finite_tree(self, obj):
        if isinstance(obj, dict):
            for value in obj.values():
                self.finite_tree(value)
        elif isinstance(obj, list):
            for value in obj:
                self.finite_tree(value)
        elif type(obj) in (int, float):
            self.assertTrue(math.isfinite(obj), "nonfinite JSON number")

    def qois(self, obj, name):
        self.fields(obj, QOIS, name)
        for key in QOIS:
            self.assertGreater(self.number(obj[key], f"{name}.{key}"),
                               0.0 if key == "W_bfp_m" else T_REF)

    @staticmethod
    def rise(value, key):
        return value if key == "W_bfp_m" else value - T_REF

    def evidence(self, obj, fraction):
        self.fields(obj, ("heat_source", "temperature_transfer", "thermal_balance", "resources"),
                    "solver evidence")
        heat = obj["heat_source"]
        self.fields(heat, ("die_volume_m3", "q_W_per_m3", "integrated_power_W", "relative_error"),
                    "heat_source")
        # Quarter area = 98 mm²; full area = 392 mm², including the chamfer.
        self.close(heat["die_volume_m3"], 392e-6 * 0.0005 * fraction, "die volume")
        self.close(heat["q_W_per_m3"], 10.0 / (392e-6 * 0.0005), "power density")
        power = self.number(heat["integrated_power_W"], "integrated power", 0.0)
        error = abs(power - 10.0 * fraction) / (10.0 * fraction)
        self.close(heat["relative_error"], error, "heat source relative error")
        transfer = obj["temperature_transfer"]
        self.fields(transfer, ("n_nodes", "max_abs_delta_K"), "temperature_transfer")
        self.integer(transfer["n_nodes"], "temperature node count")
        self.assertEqual(transfer["n_nodes"], obj["n_nodes"])
        self.assertLessEqual(self.number(transfer["max_abs_delta_K"], "temperature transfer", 0),
                             1e-3)
        balance = obj["thermal_balance"]
        self.fields(balance, ("bottom_outward_W", "top_outward_W", "relative_error"),
                    "thermal_balance")
        bottom = self.number(balance["bottom_outward_W"], "bottom outward heat flow")
        top = self.number(balance["top_outward_W"], "top outward heat flow")
        self.close(balance["relative_error"],
                   (bottom + top - 10.0 * fraction) / (10.0 * fraction), "signed heat balance")
        resources = obj["resources"]
        self.fields(resources, ("label", "peak_rss_KiB", "wall_seconds"), "resources")
        self.assertEqual(resources["label"], "实测")
        self.assertGreater(self.number(resources["wall_seconds"], "wall time"), 0)
        self.assertGreater(self.number(resources["peak_rss_KiB"], "peak RSS"), 0)
        self.assertLessEqual(resources["peak_rss_KiB"], 8 * 1024 * 1024)
        return error <= 1e-10

    def validate_artifact(self, data):
        required = ("SPDX-License-Identifier", "schema_version", "kind", "case", "reference_type",
                    "label", "calculix_version", "ccx_package", "host", "model_scope", "element_type",
                    "inputs_sha256", "materials_at_T_ref", "levels", "symmetry_check", "element_cross",
                    "observed_order", "selected", "U_ref", "reference", "status")
        self.fields(data, required, "REFERENCE")
        self.finite_tree(data)
        serialized = json.dumps(data, ensure_ascii=False)
        for forbidden in ("/" + "workspace", "/" + "home", "demo-1214"):
            self.assertNotIn(forbidden, serialized)
        for key, value in {
            "SPDX-License-Identifier": "Apache-2.0", "schema_version": "chipcae.vv.reference/0.1",
            "kind": "REFERENCE", "case": "DEMO-GATE", "reference_type": "独立代码",
            "model_scope": "quarter",
        }.items():
            self.assertEqual(data[key], value, key)
        for key in ("label", "calculix_version", "ccx_package", "host", "element_type"):
            self.text(data[key], key)
        for phrase in ("CalculiX", "独立参考值", "不是实测", "不是解析解"):
            self.assertIn(phrase, data["label"])
        self.assertRegex(data["calculix_version"], r"^This is Version 2\.23$")
        self.assertTrue(data["ccx_package"] == "unknown" or
                        ("calculix" in data["ccx_package"].lower() and
                         re.search(r"\b2\.23\b", data["ccx_package"])))
        self.assertCountEqual(re.findall(r"C3D\d+R?", data["element_type"]), ["C3D20R", "C3D15"])
        hashes = data["inputs_sha256"]
        self.fields(hashes, ("case", "stack", "materials", "profile"), "inputs_sha256")
        for name in ("case", "stack", "materials", "profile"):
            self.assertIsInstance(hashes[name], str)
            self.assertRegex(hashes[name], r"^[0-9a-f]{64}$")
            expected = hashlib.sha256((ROOT / "examples/demo_gate" / f"{name}.json").read_bytes())
            self.assertEqual(hashes[name], expected.hexdigest(), f"stale input: {name}")

        # Independently interpolate the current input tables, not generator helpers.
        material_input = json.loads((ROOT / "examples/demo_gate/materials.json").read_text())
        materials = data["materials_at_T_ref"]
        self.fields(materials, (m["id"] for m in material_input["materials"]), "materials_at_T_ref")
        for material in material_input["materials"]:
            values = materials[material["id"]]
            self.fields(values, ("E_Pa", "nu", "alpha_1_per_K", "conductivity_W_per_mK"), material["id"])
            for section, column, field in (("elastic", "E", "E_Pa"), ("elastic", "nu", "nu"),
                                           ("thermal_expansion", "alpha", "alpha_1_per_K")):
                table = material[section]["table"]
                low, high = table["temperatures"]
                a, b = table["columns"][column]
                self.close(values[field], a + (b - a) * (T_REF - low) / (high - low),
                           f"{material['id']}.{field}")
            self.close(values["conductivity_W_per_mK"], material["conductivity"], "conductivity")

        self.assertIsInstance(data["levels"], list)
        self.assertEqual(len(data["levels"]), 3)
        for level in data["levels"]:
            self.fields(level, ("name", "h_m", "n_elem_z_per_layer", "n_elements", "n_nodes",
                                "elements_by_type", "max_aspect_ratio", "decks", "W_bfp_nodes_m"), "level")
            self.text(level["name"], "level name")
        self.assertCountEqual([level["name"] for level in data["levels"]], ["L1", "L2", "L3"])
        levels = {level["name"]: level for level in data["levels"]}
        heat_ok = True
        for name, h, nz, count, wedge in (("L1", 0.001, 2, 594, 12),
                                          ("L2", 0.0005, 4, 4728, 48),
                                          ("L3", 0.00025, 8, 37728, 192)):
            level = levels[name]
            self.close(level["h_m"], h, f"{name}.h_m")
            self.assertIsInstance(level["n_elem_z_per_layer"], list)
            for n in level["n_elem_z_per_layer"]:
                self.integer(n, "elements per layer")
            self.assertEqual(level["n_elem_z_per_layer"], [nz] * 3)
            self.integer(level["n_elements"], "n_elements")
            self.assertEqual(level["n_elements"], count)
            self.integer(level["n_nodes"], "n_nodes")
            self.fields(level["elements_by_type"], ("C3D20R", "C3D15"), "element counts")
            self.assertEqual(set(level["elements_by_type"]), {"C3D20R", "C3D15"})
            for value in level["elements_by_type"].values():
                self.integer(value, "element count")
            self.assertEqual(level["elements_by_type"]["C3D15"], wedge)
            self.assertEqual(level["elements_by_type"]["C3D20R"], count - wedge)
            self.number(level["max_aspect_ratio"], "longest/shortest edge ratio", 1.0)
            self.qois(level, name)
            self.assertGreater(self.number(level["W_bfp_nodes_m"], "nodal warpage"), 0.0)
            self.assertIsInstance(level["decks"], list)
            files = []
            for deck in level["decks"]:
                self.fields(deck, ("file", "sha256"), "deck")
                self.text(deck["file"], "deck file")
                self.assertIsInstance(deck["sha256"], str)
                self.assertRegex(deck["sha256"], r"^[0-9a-f]{64}$")
                files.append(deck["file"])
            self.assertTrue(sorted(files) in (
                [f"{name}_quarter.inp"],
                [f"{name}_quarter_mech.inp", f"{name}_quarter_thermal.inp"],
            ), f"invalid deck list for {name}: {files}")
            if name == "L1":
                for deck in level["decks"]:
                    path = ARTIFACT_DIR / deck["file"]
                    self.assertTrue(path.is_file(), f"missing archived L1 deck: {deck['file']}")
                    self.assertEqual(deck["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            heat_ok = self.evidence(level, 0.25) and heat_ok

        # Recompute (a)-(c) on rises, adding T_ref back only to selected temperatures.
        convergence_ok = order_ok = uncertainty_ok = True
        for field in ("observed_order", "selected", "U_ref", "reference"):
            self.fields(data[field], QOIS, field)
        for key in QOIS:
            q1, q2, q3 = [self.rise(levels[name][key], key) for name in ("L1", "L2", "L3")]
            d12, d23 = q1 - q2, q2 - q3
            monotonic = d12 * d23 > 0
            convergence_ok = monotonic and abs(d23) / abs(q3) <= 0.002 and convergence_ok
            order_ok = monotonic and order_ok
            if not monotonic:
                # An unqualified artifact can be written to work/, never accepted into the repo.
                self.fail(f"{key}: cannot compute observed order from nonmonotonic/zero differences")
            p = math.log(d12 / d23) / math.log(2.0)
            selected = "richardson" if 1.5 <= p <= 4.5 else "finest"
            reference = q3 + (q3 - q2) / (2.0 ** p - 1.0) if selected == "richardson" else q3
            self.close(data["observed_order"][key], p, f"observed_order.{key}")
            self.assertEqual(data["selected"][key], selected)
            self.close(data["reference"][key], reference if key == "W_bfp_m" else reference + T_REF,
                       f"reference.{key}")
            uncertainty = abs(q3 - q2)
            self.close(data["U_ref"][key], uncertainty, f"U_ref.{key}")
            uncertainty_ok = uncertainty <= 0.002 * abs(reference) and uncertainty_ok

        symmetry = data["symmetry_check"]
        cross = data["element_cross"]
        for obj, name in ((symmetry, "symmetry_check"), (cross, "element_cross")):
            self.qois(obj, name)
            self.fields(obj, ("relative_difference", "n_elements", "n_nodes", "element_type"), name)
            self.fields(obj["relative_difference"], QOIS, f"{name}.relative_difference")
            self.integer(obj["n_elements"], f"{name}.n_elements")
            self.integer(obj["n_nodes"], f"{name}.n_nodes")
            self.text(obj["element_type"], f"{name}.element_type")
        self.assertEqual(symmetry["n_elements"], 2376)
        self.assertEqual(cross["n_elements"], 37728)
        self.assertEqual(symmetry["element_type"], data["element_type"])
        self.assertCountEqual(re.findall(r"C3D\d+R?", cross["element_type"]), ["C3D20", "C3D15"])
        heat_ok = self.evidence(symmetry, 1.0) and heat_ok
        heat_ok = self.evidence(cross, 0.25) and heat_ok
        symmetry_ok = cross_ok = True
        for key in QOIS:
            for obj, main, limit, name in ((symmetry, levels["L1"], 1e-6, "symmetry_check"),
                                           (cross, levels["L3"], 0.001, "element_cross")):
                relative = abs(obj[key] - main[key]) / abs(self.rise(main[key], key))
                self.close(obj["relative_difference"][key], relative, f"{name}.{key}")
                if name == "symmetry_check":
                    symmetry_ok = relative <= limit and symmetry_ok
                else:
                    cross_ok = relative <= limit and cross_ok
        status = ("参考不合格" if not all((convergence_ok, order_ok, uncertainty_ok, symmetry_ok, heat_ok))
                  else "需讨论" if not cross_ok else "合格")
        self.assertEqual(data["status"], status, "status disagrees with independently recomputed (a)-(f)")
        self.assertEqual(data["status"], "合格", "only a qualified reference may be archived")

    def load_artifact(self):
        path = ARTIFACT_DIR / "REFERENCE.json"
        if not path.exists():
            self.skipTest("qualified DEMO-GATE REFERENCE.json has not been published")
        self.assertTrue(path.is_file(), "missing required DEMO-GATE artifact: tools/vv/calculix_demo_gate/REFERENCE.json")
        return json.loads(path.read_text(encoding="utf-8"))

    def test_reference_schema_and_independent_criteria(self):
        self.validate_artifact(self.load_artifact())

    def test_required_negative_mutations_are_rejected(self):
        data = self.load_artifact()
        self.validate_artifact(data)
        for mutation in ("L3 warpage +1%", "stack hash", "host path", "reference type"):
            with self.subTest(mutation=mutation):
                bad = copy.deepcopy(data)
                if mutation == "L3 warpage +1%":
                    next(level for level in bad["levels"] if level["name"] == "L3")["W_bfp_m"] *= 1.01
                elif mutation == "stack hash":
                    bad["inputs_sha256"]["stack"] = "0" * 64
                elif mutation == "host path":
                    bad["host"] = "/" + "workspace/x"
                else:
                    bad["reference_type"] = "解析"
                with self.assertRaises(AssertionError):
                    self.validate_artifact(bad)

    def test_missing_fields_bad_types_and_nonfinite_values_are_rejected(self):
        data = self.load_artifact()
        self.validate_artifact(data)
        objects = [()] + [("levels", i) for i in range(3)] + [
            ("levels", 0, name) for name in
            ("resources", "heat_source", "temperature_transfer", "thermal_balance")]
        for path in objects:
            target = data
            for part in path:
                target = target[part]
            for key in target:
                # Additive metadata is allowed; only deletion of known required fields is tested.
                if key not in {"SPDX-License-Identifier", "schema_version", "kind", "case", "reference_type",
                               "label", "calculix_version", "ccx_package", "host", "model_scope", "element_type",
                               "inputs_sha256", "materials_at_T_ref", "levels", "symmetry_check", "element_cross",
                               "observed_order", "selected", "U_ref", "reference", "status", "name", "h_m",
                               "n_elem_z_per_layer", "n_elements", "n_nodes", "elements_by_type", "max_aspect_ratio",
                               "decks", "W_bfp_nodes_m", "heat_source", "temperature_transfer", "thermal_balance",
                               "resources", "peak_rss_KiB", "wall_seconds", "die_volume_m3", "q_W_per_m3",
                               "integrated_power_W", "relative_error", "max_abs_delta_K", "bottom_outward_W",
                               "top_outward_W", *QOIS}:
                    continue
                with self.subTest(path=path, missing=key):
                    bad = copy.deepcopy(data)
                    nested = bad
                    for part in path:
                        nested = nested[part]
                    del nested[key]
                    with self.assertRaises(AssertionError):
                        self.validate_artifact(bad)
        for value in (True, "invalid", None, float("nan"), float("inf"), -float("inf")):
            with self.subTest(value=value):
                bad = copy.deepcopy(data)
                bad["levels"][0]["T_max_K"] = value
                with self.assertRaises(AssertionError):
                    self.validate_artifact(bad)
        bad = copy.deepcopy(data)
        bad["levels"][2]["name"] = "L2"
        with self.assertRaises(AssertionError):
            self.validate_artifact(bad)


if __name__ == "__main__":
    # chipcae_auto_tests passes the source directory as argv[1].
    if len(sys.argv) == 2 and Path(sys.argv[1]).is_dir():
        sys.argv.pop()
    unittest.main(verbosity=2)
