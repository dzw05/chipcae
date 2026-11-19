# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Field comparison acceptance: real HDF5 inputs, API metrics and CLI boundaries."""
from dataclasses import fields, is_dataclass, replace
import json
import math
from pathlib import Path
import re
import shutil
import tempfile
import unittest

from fixtures import (AMBIENT, FIELD_KEYS, GATE_KEYS, PRESETS, SCRIPT, cli,
                      comparison_module, h5py, np, patch_qoi, synthetic_run, write_json)


class SyntheticFixtureTests(unittest.TestCase):
    def test_generated_input_is_readable_by_existing_summary(self):
        # Establish independently that RED is not caused by malformed test input.
        from tools.vv.result_summary import summarize
        with tempfile.TemporaryDirectory(prefix="field-fixture-") as tmp:
            run = Path(tmp) / "run"
            synthetic_run(run)
            result = summarize(run)
            self.assertEqual(result["blocks"]["hex"]["cells"], 3)
            self.assertEqual(result["blocks"]["cut"]["cells"], 1)
            self.assertGreater(result["temperature"]["max"], AMBIENT)

    def test_permuted_and_cell_temperature_fixtures_remain_valid(self):
        from tools.vv.result_summary import summarize
        with tempfile.TemporaryDirectory(prefix="field-fixture-") as tmp:
            for permute in (False, True):
                run = Path(tmp) / str(permute)
                synthetic_run(run, t_cell=True, permute=permute)
                result = summarize(run)
                self.assertEqual(result["blocks"]["hex"]["cells"], 3)
                self.assertEqual(result["blocks"]["cut"]["cells"], 1)
                self.assertEqual(result["temperature"]["max"], 347.)


class FieldCompareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="field-compare-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ref = self.root / "reference-run-synthetic-a"
        self.other = self.root / "candidate-run-synthetic-b"
        self.ref_step = synthetic_run(self.ref)
        self.other_step = synthetic_run(self.other)
        self.out = self.root / "comparison.json"
        # Python needs no production stub to compile. Turn absence into an
        # assertion failure, retaining the full future behavioral tests below.
        self.fc = comparison_module()

    def compare(self, *, preset="same-mesh-solver", ambient=AMBIENT, rigid=False):
        return self.fc.compare_runs(self.fc.load_run(self.ref), self.fc.load_run(self.other),
                                    preset=preset, t_ambient=ambient, remove_rigid=rigid)

    def run_cli(self, code=0, *, preset="same-mesh-solver", ambient=AMBIENT, extra=(), out=None):
        result = cli(self.ref, self.other, out or self.out, preset=preset,
                     ambient=ambient, extra=extra)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        return result

    def failed_gates(self, result, expected):
        self.assertEqual(result["verdict"], "fail" if expected else "pass", result)
        self.assertEqual({g["metric"] for g in result["gates"] if not g["pass"]}, set(expected))
        self.assertEqual(len(result["gates"]), 5)
        self.assertEqual({g["metric"] for g in result["gates"]}, GATE_KEYS)
        for gate in result["gates"]:
            self.assertEqual(gate["limit"], 1e-8)
            self.assertRegex(gate["source"], r"[^\s:]+\.(?:py|md):\d+")

    def set_dataset(self, path, value, *, step=None):
        with h5py.File(step or self.other_step, "r+") as data:
            del data[path]
            data[path] = value

    def remove_field(self, name, *, step=None):
        with h5py.File(step or self.other_step, "r+") as data:
            for block in ("hex", "cut"):
                path = f"VTKHDF/{block}/{name}"
                if path in data:
                    del data[path]

    def manifest(self, change):
        path = self.other / "manifest.json"
        value = json.loads(path.read_text())
        change(value)
        # Invalid numeric input is deliberately legal here, unlike output JSON.
        path.write_text(json.dumps(value), encoding="utf-8")

    def invalid(self, message=None, *, load_invalid=True):
        if load_invalid:
            with self.subTest(interface="load_run"):
                if message:
                    with self.assertRaisesRegex(self.fc.InputInvalid, message):
                        self.fc.load_run(self.other)
                else:
                    with self.assertRaises(self.fc.InputInvalid):
                        self.fc.load_run(self.other)
        with self.subTest(interface="API"):
            if message:
                with self.assertRaisesRegex(self.fc.InputInvalid, message):
                    self.compare()
            else:
                with self.assertRaises(self.fc.InputInvalid):
                    self.compare()
        with self.subTest(interface="CLI"):
            completed = self.run_cli(14)
            if message:
                self.assertRegex(completed.stderr, message)
            self.assertFalse(self.out.exists())
            self.assertFalse(Path(str(self.out) + ".md").exists())

    def test_public_runfields_contract_and_canonical_data(self):
        run = self.fc.load_run(self.ref)
        self.assertTrue(is_dataclass(run))
        self.assertEqual(type(run), self.fc.RunFields)
        self.assertEqual([f.name for f in fields(run)], [
            "mesh_sha256", "qoi", "qoi_method", "points", "T", "U", "cell_gid",
            "cell_volume", "cell_T", "cell_T_source", "dup_point_spread"])
        self.assertEqual(run.points.shape, (24, 3))
        self.assertEqual(run.T.shape, (24,))
        self.assertEqual(run.U.shape, (24, 3))
        expected_points = sorted((x, y, z) for x in (0, 1, 3, 6, 7, 11)
                                 for y in (0, 1) for z in (0, 1))
        np.testing.assert_array_equal(run.points, expected_points)
        np.testing.assert_array_equal(run.T, AMBIENT + 4*run.points[:, 0]
                                      + 2*run.points[:, 1] + run.points[:, 2])
        np.testing.assert_array_equal(run.U, np.column_stack(
            (.125*run.points[:, 0]**2, .25*run.points[:, 1]*run.points[:, 2],
             .5*run.points[:, 0]*run.points[:, 2])))
        self.assertEqual(run.cell_gid.dtype, np.dtype("int64"))
        np.testing.assert_array_equal(run.cell_gid, [7, 11, 23, 2**62 + 7])
        np.testing.assert_array_equal(run.cell_volume, [1., 2., 3., 4.])
        self.assertEqual(run.qoi_method, {key: "synthetic" for key in run.qoi})
        self.assertNotIn("T_max_rise", run.qoi)
        self.assertTrue(issubclass(self.fc.UsageError, ValueError))
        self.assertTrue(issubclass(self.fc.InputInvalid, ValueError))
        self.assertIsInstance(self.fc.GATES, dict)

    def test_self_comparison_all_gated_presets_and_metadata_boundary(self):
        ref = self.fc.load_run(self.ref)
        for preset in PRESETS:
            with self.subTest(preset=preset):
                result = self.fc.compare_runs(ref, ref, preset=preset, t_ambient=AMBIENT)
                self.assertEqual(result["schema"], "chipcae.vv_field_compare/0.1")
                self.assertEqual(result["verdict"], "pass", result)
                self.assertTrue(result["same_mesh"])
                self.assertEqual(set(result["fields"]), FIELD_KEYS)
                for metric in result["fields"].values():
                    if metric is not None:
                        self.assertEqual(metric["max_abs"], 0)
                        self.assertEqual(set(metric), {"max_abs", "max_rel", "l2_rel", "argmax", "n"})
                self.assertIsNone(result["fields"]["U_rigid_removed"])
                self.assertNotIn("case_id", result)
                self.assertNotIn("data_class", result)
                self.assertNotIn(self.ref.name, json.dumps(result))
                self.assertEqual(result["ref"]["cell_T_source"], "vertex_mean")
                self.assertTrue(all(g["pass"] for g in result["gates"]))
                json.dumps(result, allow_nan=False)

    def test_zero_reference_qoi_is_undefined_and_fails_even_self_comparison(self):
        patch_qoi(self.ref, "U_mag_max", 0)
        patch_qoi(self.other, "U_mag_max", 0)
        for preset in ("same-mesh-solver", "ranks"):
            with self.subTest(preset=preset):
                result = self.compare(preset=preset)
                self.assertIsNone(result["qoi"]["U_mag_max"]["rel"])
                self.failed_gates(result, {"qoi.U_mag_max.rel"})
                self.run_cli(1, preset=preset)

    def test_temperature_perturbation_straddles_gate_in_isolation(self):
        for relative in (2e-8, 5e-9):
            with self.subTest(relative=relative):
                with h5py.File(self.other_step, "r+") as data:
                    p = data["VTKHDF/hex/Points"][:]
                    target = np.all(p == (0, 0, 0), axis=1)
                    values = AMBIENT + 4*p[:, 0] + 2*p[:, 1] + p[:, 2]
                    values[target] += 47*relative
                    data["VTKHDF/hex/PointData/T"][:] = values
                result = self.compare()
                self.assertTrue(math.isclose(result["fields"]["T"]["max_rel"], relative,
                                             rel_tol=2e-7, abs_tol=0))
                ref = self.fc.load_run(self.ref)
                expected_l2 = abs((AMBIENT + 47*relative)-AMBIENT) / np.linalg.norm(ref.T-AMBIENT)
                self.assertTrue(math.isclose(result["fields"]["T"]["l2_rel"], expected_l2,
                                             rel_tol=1e-14, abs_tol=0))
                self.failed_gates(result, {"fields.T.max_rel"} if relative > 1e-8 else set())
                self.run_cli(1 if relative > 1e-8 else 0)

    def test_vector_displacement_error_uses_euclidean_norm(self):
        reference = self.fc.load_run(self.ref)
        scale = np.max(np.linalg.norm(reference.U, axis=1))
        delta = scale * 2e-8
        with h5py.File(self.other_step, "r+") as data:
            points = data["VTKHDF/hex/Points"][:]
            index = np.flatnonzero(np.all(points == (0, 0, 0), axis=1))[0]
            values = data["VTKHDF/hex/PointData/U"][:]
            values[index] += [delta*.6, delta*.8, 0]
            data["VTKHDF/hex/PointData/U"][:] = values
        result = self.compare()
        self.assertTrue(math.isclose(result["fields"]["U"]["max_abs"], delta, rel_tol=1e-14))
        self.assertTrue(math.isclose(result["fields"]["U"]["max_rel"], 2e-8, rel_tol=1e-14))
        self.failed_gates(result, {"fields.U.max_rel"})
        other = self.fc.load_run(self.other)
        magnitudes = np.abs(np.linalg.norm(other.U, axis=1) - np.linalg.norm(reference.U, axis=1))
        self.assertTrue(math.isclose(result["fields"]["U_mag"]["max_abs"],
                                    np.max(magnitudes), rel_tol=1e-14))
        expected = np.linalg.norm(other.U-reference.U) / np.linalg.norm(reference.U)
        self.assertTrue(math.isclose(result["fields"]["U"]["l2_rel"], expected, rel_tol=1e-14))
        self.assertTrue(math.isclose(result["fields"]["U_mag"]["l2_rel"], expected, rel_tol=1e-14))
        self.assertTrue(math.isclose(result["fields"]["U_mag"]["max_rel"], 2e-8, rel_tol=1e-14))
        self.run_cli(1)

    def test_each_manifest_qoi_gate_is_independent(self):
        for key, value in (("warpage_pv", .25), ("T_max", 347.), ("U_mag_max", 17.)):
            for relative in (2e-8, 5e-9):
                with self.subTest(qoi=key, relative=relative):
                    patch_qoi(self.other, key, value*(1+relative))
                    result = self.compare()
                    self.failed_gates(result, {f"qoi.{key}.rel"} if relative > 1e-8 else set())
                    self.assertEqual(result["fields"]["T"]["max_abs"], 0)
                    self.assertEqual(result["fields"]["U"]["max_abs"], 0)
                    self.run_cli(1 if relative > 1e-8 else 0)
                    patch_qoi(self.other, key, value)

    def test_cell_temperature_vertex_means_are_exact_and_cell_values_take_precedence(self):
        run = self.fc.load_run(self.ref)
        self.assertEqual(run.cell_T_source, "vertex_mean")
        # Independent eight-vertex arithmetic means, with binary-exact inputs.
        np.testing.assert_array_equal(run.cell_T, [303.5, 309.5, 319.5, 337.5])
        shutil.rmtree(self.other)
        synthetic_run(self.other, t_cell=True)
        run = self.fc.load_run(self.other)
        self.assertEqual(run.cell_T_source, "T_cell")
        np.testing.assert_array_equal(run.cell_T, [313.5, 319.5, 329.5, 347.5])
        self.assertEqual(self.compare()["other"]["cell_T_source"], "T_cell")

    def test_weighted_cell_l2_uses_reference_volumes_and_temperature_rise(self):
        for run in (self.ref, self.other):
            shutil.rmtree(run)
            synthetic_run(run, t_cell=True)
        with h5py.File(self.other_step, "r+") as data:
            data["VTKHDF/hex/CellData/T_cell"][:] += [1., -2., 3.]
            data["VTKHDF/cut/CellData/T_cell"][:] += [-4.]
            # Other volumes must not change the reference-weighted metric.
            data["VTKHDF/hex/CellData/volume"][:] = [8., 9., 10.]
        result = self.compare()
        weights = np.array([1., 2., 3., 4.])
        rise = np.array([13.5, 19.5, 29.5, 47.5])
        diff = np.array([1., -2., 3., -4.])
        expected = np.sqrt(np.sum(weights*diff**2)) / np.sqrt(np.sum(weights*rise**2))
        actual = result["fields"]["cell_T"]["l2_rel"]
        self.assertLessEqual(abs(actual/expected-1), 1e-14)
        self.assertTrue(math.isclose(result["fields"]["cell_T"]["max_rel"], 4/47.5, rel_tol=1e-14))
        self.assertEqual(result["fields"]["cell_T"]["argmax"], {"block": "cut", "elem_gid": 7})
        self.assertEqual(result["fields"]["cell_T"]["n"], 4)

    def test_missing_volumes_use_equal_weights_with_note(self):
        self.remove_field("CellData/volume", step=self.ref_step)
        self.remove_field("CellData/volume")
        with h5py.File(self.other_step, "r+") as data:
            data["VTKHDF/cut/PointData/T"][:] += 2
        result = self.compare()
        expected = 2 / np.linalg.norm(np.array([3.5, 9.5, 19.5, 37.5]))
        self.assertTrue(math.isclose(result["fields"]["cell_T"]["l2_rel"], expected, rel_tol=1e-14))
        self.assertRegex(str(result["notes"]).lower(), r"equal|uniform|等权")

    def test_duplicate_point_spreads_and_averaging_precede_alignment(self):
        run = self.fc.load_run(self.ref)
        self.assertEqual(run.dup_point_spread, {"T": 0, "U": 0})
        # Keep the discrepancy representable to the brief's 1e-12 relative
        # tolerance; subtraction at 312 K would itself lose that precision.
        for step in (self.ref_step, self.other_step):
            with h5py.File(step, "r+") as data:
                p = data["VTKHDF/hex/Points"][:]
                t = data["VTKHDF/hex/PointData/T"][:]
                t[np.all(p == (3, 0, 0), axis=1)] = 1
                data["VTKHDF/hex/PointData/T"][:] = t
        with h5py.File(self.other_step, "r+") as data:
            # x=3 belongs to both ranks. Change only the rank 1 copy at y=z=0.
            data["VTKHDF/hex/PointData/T"][12] += 1e-3
            u = data["VTKHDF/hex/PointData/U"][12]
            data["VTKHDF/hex/PointData/U"][12] = u + [1e-3, 0, 0]
        other = self.fc.load_run(self.other)
        for key in ("T", "U"):
            self.assertTrue(math.isclose(other.dup_point_spread[key], 1e-3, rel_tol=1e-12))
        index = np.flatnonzero(np.all(other.points == (3, 0, 0), axis=1))[0]
        self.assertTrue(math.isclose(other.T[index], 1.0005, rel_tol=0, abs_tol=1e-15))
        self.assertTrue(math.isclose(other.U[index, 0], 1.1255, rel_tol=0, abs_tol=1e-15))
        # Cell means must be computed from local vertices before duplicate averaging.
        np.testing.assert_allclose(other.cell_T, [303.5, 270.625, 280.625125, 337.5],
                                   rtol=0, atol=1e-13)
        self.assertEqual(self.compare()["fields"]["T"]["n"], 24)

    def test_permuted_ranks_points_connectivity_and_cells_preserve_results_and_argmax(self):
        def perturb(step):
            with h5py.File(step, "r+") as data:
                for name in ("hex", "cut"):
                    group = data["VTKHDF/" + name]
                    p = group["Points"][:]
                    t = group["PointData/T"][:]
                    t[np.all(p == (0, 0, 0), axis=1) | np.all(p == (1, 0, 0), axis=1)] += .5
                    group["PointData/T"][:] = t
                    gid = group["CellData/elem_gid"][:]
                    cell_t = group["CellData/T_cell"][:]
                    cell_t[gid == 7] += 2
                    group["CellData/T_cell"][:] = cell_t
        for run in (self.ref, self.other):
            shutil.rmtree(run)
            synthetic_run(run, t_cell=True)
        perturb(self.other_step)
        expected = self.compare()
        self.assertEqual(expected["fields"]["T"]["argmax"], {"xyz": [0., 0., 0.]})
        self.assertEqual(expected["fields"]["cell_T"]["argmax"], {"block": "hex", "elem_gid": 7})
        for run in (self.ref, self.other):
            shutil.rmtree(run)
            synthetic_run(run, t_cell=True, permute=True)
        perturb(self.other_step)
        self.assertEqual(self.compare(), expected)
        # Also align two different storage orders, rather than matching permutations.
        shutil.rmtree(self.ref)
        synthetic_run(self.ref, t_cell=True)
        self.assertEqual(self.compare(), expected)

    def test_mesh_mismatch_bypasses_alignment_and_metrics_but_retains_qoi(self):
        self.manifest(lambda m: m["mesh"].update(mesh_sha256="b"*64))
        with h5py.File(self.other_step, "r+") as data:
            data["VTKHDF/hex/Points"][:] += 100
        self.run_cli(3)
        result = json.loads(self.out.read_text())
        self.assertFalse(result["same_mesh"])
        self.assertEqual(result["verdict"], "mesh_mismatch")
        self.assertEqual(result["fields"], dict.fromkeys(FIELD_KEYS))
        self.assertEqual(result["qoi"]["T_max"]["ref"], 347.)
        self.assertEqual(self.compare()["verdict"], "mesh_mismatch")

    def test_illegal_raw_ids_are_rejected_before_encoding(self):
        for ids in ([-1, 11, 23], [2**62, 11, 23], [7.5, 11., 23.],
                    [float("nan"), 11., 23.], [float("inf"), 11., 23.]):
            with self.subTest(ids=ids):
                self.set_dataset("VTKHDF/hex/CellData/elem_gid", np.array(ids))
                self.invalid()

    def test_unmatched_legal_cell_ids_are_an_alignment_error(self):
        self.set_dataset("VTKHDF/hex/CellData/elem_gid", np.array([7, 11, 24], dtype="int64"))
        self.invalid(load_invalid=False)

    def test_same_rank_and_identical_cross_rank_ghost_ids_are_ambiguous(self):
        for ids in ([7, 7, 23], [7, 11, 7]):
            with self.subTest(ids=ids):
                self.set_dataset("VTKHDF/hex/CellData/elem_gid", np.array(ids, dtype="int64"))
                # Make the entire duplicated rank's fields identical too.
                with h5py.File(self.other_step, "r+") as data:
                    data["VTKHDF/hex/PointData/T"][:] = 320
                    data["VTKHDF/hex/PointData/U"][:] = 1
                self.invalid("ambiguous elem_gid")

    def test_id_boundaries_encode_exactly_and_same_ids_in_different_blocks_align(self):
        for step in (self.ref_step, self.other_step):
            self.set_dataset("VTKHDF/hex/CellData/elem_gid",
                             np.array([0, 2**62-1, 23], dtype="int64"), step=step)
            self.set_dataset("VTKHDF/cut/CellData/elem_gid",
                             np.array([2**62-1], dtype="int64"), step=step)
        run = self.fc.load_run(self.ref)
        np.testing.assert_array_equal(run.cell_gid, [0, 23, 2**62-1, 2**63-1])
        self.assertEqual(run.cell_gid.dtype, np.dtype("int64"))
        self.failed_gates(self.compare(), set())
        self.run_cli()

    def test_identical_cut_ghost_across_ranks_is_rejected(self):
        with h5py.File(self.other_step, "r+") as data:
            cut = data["VTKHDF/cut"]
            for key in ("Points", "PointData/T", "PointData/U", "Types", "CellData/elem_gid",
                        "CellData/volume", "CellData/layer_id", "CellData/material_id", "CellData/cell_kind"):
                values = cut[key][:]
                del cut[key]
                cut[key] = np.concatenate((values, values))
            for key, values in (("NumberOfPoints", [8, 8]), ("NumberOfCells", [1, 1]),
                                ("NumberOfConnectivityIds", [8, 8]), ("Offsets", [0, 8, 0, 8]),
                                ("Connectivity", list(range(8))*2)):
                del cut[key]
                cut[key] = np.array(values, dtype="int64")
        self.invalid("ambiguous elem_gid")

    def test_nonfinite_fields_and_coordinates_are_rejected(self):
        paths = ("Points", "PointData/T", "PointData/U", "CellData/volume")
        for path in paths:
            for value in (float("nan"), float("inf"), -float("inf")):
                with self.subTest(path=path, value=value):
                    shutil.copyfile(self.ref_step, self.other_step)
                    with h5py.File(self.other_step, "r+") as data:
                        a = data["VTKHDF/hex/" + path][:]
                        a.flat[0] = value
                        data["VTKHDF/hex/" + path][:] = a
                    self.invalid()
        shutil.copyfile(self.ref_step, self.other_step)
        with h5py.File(self.other_step, "r+") as data:
            data["VTKHDF/hex/CellData/T_cell"] = [float("nan"), 300., 300.]
        self.invalid("nonfinite field")

    def test_missing_manifest_vtkhdf_ids_and_required_qois(self):
        for file in (self.other / "manifest.json", self.other_step):
            with self.subTest(file=file.name):
                content = file.read_bytes()
                file.unlink()
                self.invalid()
                file.write_bytes(content)
        self.remove_field("CellData/elem_gid")
        self.invalid("elem_gid absent; add elem_gid to output.fields")
        shutil.copyfile(self.ref_step, self.other_step)
        original = (self.other / "manifest.json").read_text()
        for name in ("warpage_pv", "T_max", "U_mag_max"):
            with self.subTest(qoi=name):
                self.manifest(lambda m: m.update(summary=[v for v in m["summary"] if v["name"] != name]))
                self.invalid()
                (self.other / "manifest.json").write_text(original)

    def test_manifest_schema_hash_steps_and_nonfinite_qois_are_validated(self):
        original = (self.other / "manifest.json").read_text()
        changes = [lambda m: m.update(schema_version="unknown"),
                   lambda m: m["mesh"].pop("mesh_sha256"),
                   lambda m: m["mesh"].update(mesh_sha256=""),
                   lambda m: m.update(steps=[]),
                   lambda m: m.update(steps=[{}]),
                   lambda m: m["summary"][0].update(value=float("inf")),
                   lambda m: m["summary"][1].update(value=float("nan"))]
        for index, change in enumerate(changes):
            with self.subTest(variant=index):
                self.manifest(change)
                self.invalid()
                (self.other / "manifest.json").write_text(original)

    def test_first_step_is_used(self):
        self.manifest(lambda m: m["steps"].append({"n": 1, "file": "absent-second.vtkhdf"}))
        self.failed_gates(self.compare(), set())

    def test_malformed_field_shapes_and_rank_local_topology_are_rejected(self):
        replacements = [
            ("PointData/T", np.zeros((20, 1))), ("PointData/U", np.zeros((20, 2))),
            ("Points", np.zeros((20, 2))), ("CellData/elem_gid", np.array([1, 2])),
            ("CellData/T_cell", np.array([1., 2.])),
            ("NumberOfPoints", np.array([12, 9], dtype="int64")),
            ("NumberOfCells", np.array([3], dtype="int64")),
            ("NumberOfConnectivityIds", np.array([16, 7], dtype="int64")),
            ("Offsets", np.array([0, 8, 16, 16, 24], dtype="int64")),
            ("Connectivity", np.array([99]*24, dtype="int64")),
        ]
        for path, values in replacements:
            with self.subTest(path=path):
                shutil.copyfile(self.ref_step, self.other_step)
                # T_cell need not exist in the baseline fixture.
                with h5py.File(self.other_step, "r+") as data:
                    key = "VTKHDF/hex/" + path
                    if key in data:
                        del data[key]
                    data[key] = values
                self.invalid()

    def test_unmatched_points_report_counts_and_coordinates(self):
        with h5py.File(self.other_step, "r+") as data:
            data["VTKHDF/hex/Points"][0] = [.125, .25, .375]
        with self.assertRaises(self.fc.InputInvalid) as caught:
            self.compare()
        diagnostic = str(caught.exception)
        self.assertRegex(diagnostic.lower(), r"unmatched|unpaired|未配对")
        self.assertIn("0.125", diagnostic)
        self.assertIn("0.25", diagnostic)
        self.assertIn("0.375", diagnostic)
        self.run_cli(14)

    def test_one_sided_temperature_is_invalid_both_missing_temperature_fails_gate(self):
        self.remove_field("PointData/T")
        self.invalid(load_invalid=False)
        self.remove_field("PointData/T", step=self.ref_step)
        result = self.compare()
        self.assertIsNone(result["fields"]["T"])
        self.assertIsNone(result["fields"]["cell_T"])
        self.failed_gates(result, {"fields.T.max_rel"})
        self.assertTrue(result["notes"])
        self.run_cli(1)

    def test_empty_hex_is_invalid_empty_cut_is_valid(self):
        shutil.rmtree(self.other)
        synthetic_run(self.other, empty_hex=True)
        self.invalid()
        for run in (self.ref, self.other):
            shutil.rmtree(run)
            synthetic_run(run, empty_cut=True)
        self.failed_gates(self.compare(), set())
        self.assertEqual(self.compare()["fields"]["cell_T"]["n"], 3)
        self.run_cli()

    def test_missing_cut_temperature_sentinel_is_excluded_and_not_serialized(self):
        # All cut coordinates also exist in hex, so merged point T is complete.
        # This tests the specified cell sentinel without inventing semantics for
        # point coordinates that have no available temperature in any block.
        from fixtures import cube
        for step in (self.ref_step, self.other_step):
            with h5py.File(step, "r+") as data:
                p = cube(0, 1)
                data["VTKHDF/cut/Points"][:] = p
                data["VTKHDF/cut/PointData/U"][:] = np.column_stack(
                    (.125*p[:, 0]**2, .25*p[:, 1]*p[:, 2], .5*p[:, 0]*p[:, 2]))
                data["VTKHDF/cut/CellData/volume"][:] = 1
                del data["VTKHDF/cut/PointData/T"]
        run = self.fc.load_run(self.ref)
        self.assertTrue(np.isnan(run.cell_T[-1]))
        self.assertTrue(np.all(np.isfinite(run.T)))
        result = self.compare()
        self.failed_gates(result, set())
        self.assertEqual(result["fields"]["cell_T"]["n"], 3)
        note = str(result["notes"]).lower()
        self.assertIn("1", note)
        self.assertRegex(note, r"cut|单元")
        json.dumps(result, allow_nan=False)
        self.run_cli()
        json.loads(self.out.read_text(), parse_constant=lambda v: self.fail(f"nonfinite JSON: {v}"))

    def test_fem_fvm_only_gates_temperature_rise(self):
        with h5py.File(self.other_step, "r+") as data:
            for block in ("hex", "cut"):
                data[f"VTKHDF/{block}/PointData/T"][:] += .5
                data[f"VTKHDF/{block}/PointData/U"][:] += .5
        patch_qoi(self.other, "warpage_pv", 20)
        patch_qoi(self.other, "U_mag_max", 100)
        for relative, verdict in ((.009, "pass"), (.011, "fail")):
            with self.subTest(relative=relative):
                patch_qoi(self.other, "T_max", AMBIENT + 47*(1+relative))
                result = self.compare(preset="fem-fvm")
                self.assertEqual(result["verdict"], verdict)
                self.assertEqual(len(result["gates"]), 1)
                gate = result["gates"][0]
                self.assertEqual(gate["metric"], "qoi.T_max_rise.rel")
                self.assertEqual(gate["limit"], 1e-2)
                self.assertTrue(math.isclose(gate["value"], relative, rel_tol=1e-12))
                self.assertRegex(gate["source"], r"fvm-solver-io\.md:55")
                self.assertEqual(gate["pass"], verdict == "pass")
                self.assertEqual(result["qoi"]["T_max_rise"]["ref"], 47.)
                self.assertEqual(result["fields"]["T"]["max_abs"], .5)
                self.run_cli(0 if verdict == "pass" else 1, preset="fem-fvm")

    def test_gated_presets_require_finite_ambient_api_and_cli(self):
        for preset in PRESETS:
            for ambient in (None, float("nan"), float("inf")):
                with self.subTest(preset=preset, ambient=ambient):
                    with self.assertRaises(self.fc.UsageError):
                        self.compare(preset=preset, ambient=ambient)
                    self.run_cli(2, preset=preset, ambient=ambient)
                    self.assertFalse(self.out.exists())

    def test_report_without_ambient_keeps_absolute_temperature_metrics_only(self):
        result = self.compare(preset="report", ambient=None)
        self.assertEqual(result["verdict"], "report")
        self.assertEqual(result["gates"], [])
        for key in ("T", "cell_T"):
            self.assertEqual(result["fields"][key]["max_abs"], 0)
            self.assertIsNone(result["fields"][key]["max_rel"])
            self.assertIsNone(result["fields"][key]["l2_rel"])
        self.run_cli(preset="report", ambient=None)

    def test_zero_temperature_and_displacement_scales_remain_null(self):
        r = self.fc.load_run(self.ref)
        r = replace(r, T=np.full_like(r.T, AMBIENT), U=np.zeros_like(r.U),
                    cell_T=np.full_like(r.cell_T, AMBIENT))
        result = self.fc.compare_runs(r, r, preset="same-mesh-solver", t_ambient=AMBIENT)
        for key in ("T", "cell_T", "U", "U_mag"):
            self.assertEqual(result["fields"][key]["max_abs"], 0)
            self.assertIsNone(result["fields"][key]["max_rel"])
            self.assertIsNone(result["fields"][key]["l2_rel"])
        self.failed_gates(result, {"fields.T.max_rel", "fields.U.max_rel"})
        json.dumps(result, allow_nan=False)

    def test_zero_reference_temperature_rise_is_undefined_and_fails_fem_fvm(self):
        patch_qoi(self.ref, "T_max", AMBIENT)
        patch_qoi(self.other, "T_max", AMBIENT)
        result = self.compare(preset="fem-fvm")
        self.assertIsNone(result["qoi"]["T_max_rise"]["rel"])
        self.assertEqual(len(result["gates"]), 1)
        self.assertIsNone(result["gates"][0]["value"])
        self.assertFalse(result["gates"][0]["pass"])
        self.assertEqual(result["verdict"], "fail")
        self.run_cli(1, preset="fem-fvm")

    def test_independent_centered_rigid_removal_preserves_raw_displacement_metrics(self):
        translation = np.array([1e-6, -2e-6, 3e-6])
        rotation = np.array([1e-5, 0, 2e-5])
        with h5py.File(self.other_step, "r+") as data:
            for block in ("hex", "cut"):
                p = data[f"VTKHDF/{block}/Points"][:]
                u = data[f"VTKHDF/{block}/PointData/U"][:]
                data[f"VTKHDF/{block}/PointData/U"][:] = u + translation + np.cross(rotation, p)
        result = self.compare(rigid=True)
        reference = self.fc.load_run(self.ref)
        scale = np.max(np.linalg.norm(reference.U, axis=1))
        self.assertLessEqual(result["fields"]["U_rigid_removed"]["max_abs"], 1e-15*scale)
        self.assertGreater(result["fields"]["U"]["max_rel"], 0)
        self.run_cli(1, extra=("--remove-rigid",))
        self.assertIsNotNone(json.loads(self.out.read_text())["fields"]["U_rigid_removed"])

    def test_rigid_removal_is_stable_far_from_coordinate_origin(self):
        ref = self.fc.load_run(self.ref)
        motion = np.array([1e-6, -2e-6, 3e-6]) + np.cross([1e-5, 0, 2e-5], ref.points)
        shifted = replace(ref, points=ref.points + [1e6, -2e6, 3e6])
        other = replace(shifted, U=ref.U + motion)
        result = self.fc.compare_runs(shifted, other, preset="report", t_ambient=AMBIENT,
                                      remove_rigid=True)
        scale = np.max(np.linalg.norm(ref.U, axis=1))
        self.assertLessEqual(result["fields"]["U_rigid_removed"]["max_abs"], 1e-15*scale)
        self.assertGreater(result["fields"]["U"]["max_abs"], 0)

    def test_cli_lowercase_enums_metadata_overwrite_and_finite_json(self):
        for flag, value in (("--preset", "RANKS"), ("--data-class", "Synthetic")):
            with self.subTest(flag=flag):
                self.run_cli(2, extra=(flag, value))
        keep = self.root / "keep.txt"
        keep.write_text("unrelated")
        self.out.write_text("old JSON")
        Path(str(self.out) + ".md").write_text("old markdown")
        self.run_cli(extra=("--case-id", "synthetic-case"))
        result = json.loads(self.out.read_text(), parse_constant=lambda v: self.fail(f"nonfinite JSON: {v}"))
        self.assertEqual(result["data_class"], "synthetic")
        self.assertEqual(result["case_id"], "synthetic-case")
        self.assertIn(self.ref.name, result["ref"].values())
        self.assertIn(self.other.name, result["other"].values())
        self.assertEqual(keep.read_text(), "unrelated")
        self.assertIn("T", Path(str(self.out) + ".md").read_text())
        self.assertNotEqual(Path(str(self.out) + ".md").read_text(), "old markdown")
        json.dumps(result, allow_nan=False)
        self.run_cli(extra=("--data-class", "public"))
        self.assertEqual(json.loads(self.out.read_text())["data_class"], "public")
        self.assertEqual(json.loads(self.out.read_text())["case_id"], "")
        # No fixed decimal truncation of native Python double values.
        precise = .12345678901234566
        patch_qoi(self.other, "warpage_pv", precise)
        self.run_cli(preset="report")
        self.assertEqual(json.loads(self.out.read_text())["qoi"]["warpage_pv"]["other"], precise)

    def test_real_output_inside_git_and_symlink_targets_is_rejected_atomically(self):
        git_tree = self.root / "git-tree"
        git_tree.mkdir()
        (git_tree / ".git").write_text("gitdir: elsewhere\n")
        nested = git_tree / "nested"
        nested.mkdir()
        link = self.root / "linked-tree"
        link.symlink_to(nested, target_is_directory=True)
        for out in (nested / "compare.json", link / "compare.json"):
            with self.subTest(out=out):
                with self.assertRaises(self.fc.UsageError):
                    self.fc.ensure_output_allowed(out, "real")
                self.run_cli(2, out=out, extra=("--data-class", "real"))
                self.assertFalse(out.exists())
                self.assertFalse(Path(str(out) + ".md").exists())
        # Validate the markdown destination too before either file is written.
        md_target = nested / "markdown-target"
        Path(str(self.out) + ".md").symlink_to(md_target)
        self.run_cli(2, extra=("--data-class", "real"))
        self.assertFalse(self.out.exists())
        self.assertFalse(md_target.exists())

    def test_real_outside_git_writes_both_files_without_numeric_stdout(self):
        self.fc.ensure_output_allowed(self.out, "real")
        self.fc.ensure_output_allowed(SCRIPT.parent / "synthetic.json", "synthetic")
        result = self.run_cli(extra=("--data-class", "real"))
        self.assertIsNone(re.search(r"\d\.\d", result.stdout))
        self.assertIn("pass", result.stdout)
        self.assertTrue(self.out.exists())
        self.assertTrue(Path(str(self.out) + ".md").is_file())
        self.assertEqual(json.loads(self.out.read_text())["data_class"], "real")


if __name__ == "__main__":
    unittest.main(argv=[__file__], verbosity=2)
