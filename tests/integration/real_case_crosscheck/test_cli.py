# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Subprocess acceptance: reports, gate mapping and atomic output protection."""

import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from synth import (AMBIENT, QOIS, ROOT, SCRIPT, change_manifest, crosscheck_module,
                   h5py, mesh_runs, np, patch_qoi, synthetic_run)
from tools.vv.field_compare import compare_runs, load_run


class CrosscheckCliTests(unittest.TestCase):
    def setUp(self):
        # No production stub is needed: absence must fail, not be skipped or
        # mistaken for a successful argparse exit-code test.
        self.cc = crosscheck_module()
        self.temp = tempfile.TemporaryDirectory(prefix="crosscheck-cli-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runs = mesh_runs(self.root)
        self.out = self.root / "report.json"

    def mesh_args(self, runs=None, *, ambient=True):
        args = ["mesh-seq", *(str(path) for path in (self.runs if runs is None else runs))]
        if ambient:
            args += ["--t-ambient", str(AMBIENT)]
        return args

    def make_pair(self, relative=0.005):
        fem, fvm = self.root / "fem", self.root / "fvm"
        base = {"T_max": AMBIENT + 2.0, "warpage_pv": 1e-4, "U_mag_max": 2e-4}
        synthetic_run(fem, n=8, qoi=base)
        synthetic_run(fvm, n=8, qoi={**base, "T_max": AMBIENT + 2.0 * (1 + relative)})
        return fem, fvm

    def pair_args(self, fem, fvm):
        return ["pair", "--fem", str(fem), "--fvm", str(fvm), "--t-ambient", str(AMBIENT)]

    def invoke(self, args, *, code=0, data_class="synthetic", out=None, extra=()):
        out = self.out if out is None else out
        cmd = [sys.executable, str(SCRIPT), *args, "--data-class", data_class,
               "-o", str(out), *extra]
        completed = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=20)
        self.assertEqual(completed.returncode, code, completed.stdout + completed.stderr)
        self.assertNotIn("Traceback", completed.stdout + completed.stderr)
        return completed

    def no_outputs(self, out=None):
        out = self.out if out is None else out
        self.assertFalse(out.exists(), str(out))
        self.assertFalse(out.with_suffix(".md").exists(), str(out.with_suffix(".md")))
        self.assertFalse(Path(str(out) + ".md").exists())

    def report(self, *, kind, verdict, data_class="synthetic", out=None, case_id=""):
        out = self.out if out is None else out
        text = out.read_text(encoding="utf-8")
        def reject_constant(value):
            raise AssertionError(f"nonfinite output JSON: {value}")
        result = json.loads(text, parse_constant=reject_constant)
        markdown = out.with_suffix(".md").read_text(encoding="utf-8")
        self.assertEqual(result["schema"], "chipcae.vv_crosscheck/0.1")
        self.assertEqual(result["kind"], kind)
        self.assertEqual(result["verdict"], verdict)
        self.assertEqual(result["data_class"], data_class)
        self.assertEqual(result["case_id"], case_id)
        self.assertIsInstance(result["notes"], list)
        self.assertEqual(len(result["runs"]), 3 if kind == "mesh_seq" else 2)
        json.dumps(result, allow_nan=False)
        for source in (*self.runs, self.root / "fem", self.root / "fvm"):
            self.assertNotIn(str(source.resolve()), text)
            self.assertNotIn(str(source.resolve()), markdown)
        self.assertNotIn(str(self.root.resolve()), text + markdown)
        for run in result["runs"]:
            self.assertEqual(Path(run["run_dir"]).name, run["run_dir"])
            self.assertIn(run["run_dir"], markdown)
            self.assertIn("mesh_sha256", run)
            self.assertIsInstance(run["N"], int)
            self.assertGreater(run["N"], 0)
            self.assertGreater(run["h"], 0)
            self.assertEqual(set(run["qoi"]), set(QOIS))
            self.assertEqual(set(run["qoi_method"]), set(QOIS))
            self.assert_float_repr(markdown, [run["h"], *run["qoi"].values()])
        return result, markdown

    def assert_float_repr(self, markdown, values):
        for value in values:
            if isinstance(value, float):
                # Numeric boundaries catch truncation to a short display value.
                pattern = r"(?<![\d.])" + re.escape(repr(value)) + r"(?![\d.eE])"
                self.assertRegex(markdown, pattern)

    def test_scrambled_mesh_sequence_sorts_metadata_and_recovers_three_orders(self):
        self.invoke(self.mesh_args([self.runs[2], self.runs[0], self.runs[1]]),
                    extra=("--case-id", "manufactured"))
        result, markdown = self.report(kind="mesh_seq", verdict="report", case_id="manufactured")
        self.assertEqual([run["run_dir"] for run in result["runs"]], [p.name for p in self.runs])
        self.assertEqual([run["N"] for run in result["runs"]], [216, 125, 64])
        hs = [run["h"] for run in result["runs"]]
        self.assertTrue(hs[0] < hs[1] < hs[2])
        for path, metadata in zip(self.runs, result["runs"]):
            loaded = load_run(path)
            self.assertEqual(metadata["mesh_sha256"], loaded.mesh_sha256)
            self.assertEqual(metadata["qoi"], loaded.qoi)
            self.assertEqual(metadata["qoi_method"], loaded.qoi_method)
            expected_h = float((np.sum(loaded.cell_volume) / len(loaded.cell_gid)) ** (1 / 3))
            self.assertEqual(float(metadata["h"]).hex(), expected_h.hex())
        self.assertEqual(set(result["mesh_seq"]), {"T_max_rise", "warpage_pv", "U_mag_max"})
        for name, gci in result["mesh_seq"].items():
            self.assertLessEqual(abs(gci["p"] - 2.0), 1e-8, name)
            self.assertEqual(gci["status"], "monotone")
            self.assert_float_repr(markdown, gci.values())
        self.assertFalse(result.get("gates"))

    def test_mesh_sequence_without_ambient_omits_temperature_rise(self):
        self.invoke(self.mesh_args(ambient=False))
        result, _ = self.report(kind="mesh_seq", verdict="report")
        self.assertEqual(set(result["mesh_seq"]), {"warpage_pv", "U_mag_max"})

    def test_unsolved_displacement_qoi_is_null_with_note_and_still_reports(self):
        for name in ("warpage_pv", "U_mag_max"):
            patch_qoi(self.runs[1], name, method="not solved: synthetic thermal-only run")
            self.invoke(self.mesh_args())
            result, _ = self.report(kind="mesh_seq", verdict="report")
            self.assertIsNone(result["mesh_seq"][name])
            notes = " ".join(result["notes"])
            self.assertIn("not solved", notes)
            self.assertIn(name, notes)
            self.assertIsNotNone(result["mesh_seq"]["T_max_rise"])

    def test_nonconvergent_mesh_sequence_has_no_unapproved_gate(self):
        for path, value in zip(self.runs, (1.0, 2.0, 3.0)):
            patch_qoi(path, "warpage_pv", value)
        # Deliberately poor displacement convergence must remain a report.
        self.invoke(self.mesh_args())
        result, _ = self.report(kind="mesh_seq", verdict="report")
        self.assertEqual(result["mesh_seq"]["warpage_pv"]["status"], "no_convergence")
        self.assertFalse(result.get("gates"))

    def test_repeated_directory_and_distinct_directory_with_duplicate_hash_exit_three(self):
        self.invoke(self.mesh_args([self.runs[0], self.runs[0], self.runs[2]]), code=3)
        self.no_outputs()
        change_manifest(self.runs[1], lambda m: m["mesh"].update(mesh_sha256="a" * 64))
        self.invoke(self.mesh_args(), code=3)
        self.no_outputs()

    def test_equal_and_tolerance_close_grid_sizes_exit_three_without_outputs(self):
        # Equal cell counts make both h computations share the identical sum
        # order; hashes remain different so this exercises the h guard.
        for ratio in (1.0, 1.0 + 5e-13):
            with self.subTest(ratio=ratio):
                synthetic_run(self.runs[0], n=64, mesh_hash="a" * 64)
                synthetic_run(self.runs[1], n=64, mesh_hash="b" * 64,
                              total_volume=1e-6 * ratio**3)
                synthetic_run(self.runs[2], n=64, mesh_hash="c" * 64, total_volume=8e-6)
                self.invoke(self.mesh_args(), code=3)
                self.no_outputs()

    def test_distinct_grid_sizes_just_outside_tolerance_are_accepted(self):
        synthetic_run(self.runs[0], n=64, mesh_hash="a" * 64)
        synthetic_run(self.runs[1], n=64, mesh_hash="b" * 64, total_volume=1e-6 * (1 + 2e-12)**3)
        synthetic_run(self.runs[2], n=64, mesh_hash="c" * 64, total_volume=8e-6)
        self.invoke(self.mesh_args())
        self.report(kind="mesh_seq", verdict="report")

    def test_missing_manifest_vtkhdf_qoi_and_volume_are_input_invalid(self):
        path = self.runs[0]
        for missing in ("manifest", "vtkhdf", "T_max", "warpage_pv", "U_mag_max", "volume"):
            with self.subTest(missing=missing):
                synthetic_run(path, n=216, mesh_hash="a" * 64)
                if missing == "manifest":
                    (path / "manifest.json").unlink()
                elif missing == "vtkhdf":
                    (path / "step.vtkhdf").unlink()
                elif missing in QOIS:
                    change_manifest(path, lambda m: m.update(summary=[
                        q for q in m["summary"] if q["name"] != missing]))
                else:
                    with h5py.File(path / "step.vtkhdf", "r+") as data:
                        del data["VTKHDF/hex/CellData/volume"]
                self.invoke(self.mesh_args(), code=14)
                self.no_outputs()

    def test_nonfinite_qois_volumes_and_point_fields_are_input_invalid(self):
        for value in (math.nan, math.inf, -math.inf):
            for target in ("qoi", "volume", "T", "U"):
                with self.subTest(value=value, target=target):
                    synthetic_run(self.runs[0], n=216, mesh_hash="a" * 64)
                    if target == "qoi":
                        patch_qoi(self.runs[0], "T_max", value)
                    else:
                        name = "CellData/volume" if target == "volume" else f"PointData/{target}"
                        with h5py.File(self.runs[0] / "step.vtkhdf", "r+") as data:
                            data[f"VTKHDF/hex/{name}"][0] = value
                    self.invoke(self.mesh_args(), code=14)
                    self.no_outputs()

    def test_nonfinite_ambient_is_input_invalid_for_both_commands(self):
        fem, fvm = self.make_pair()
        for args in (self.mesh_args(ambient=False), self.pair_args(fem, fvm)[:-2]):
            for value in ("nan", "inf", "-inf"):
                with self.subTest(command=args[0], ambient=value):
                    self.invoke([*args, f"--t-ambient={value}"], code=14)
                    self.no_outputs()

    def test_finite_inputs_whose_derived_numbers_overflow_do_not_write_reports(self):
        with h5py.File(self.runs[0] / "step.vtkhdf", "r+") as data:
            data["VTKHDF/hex/CellData/volume"][:] = np.finfo(float).max
        self.invoke(self.mesh_args(), code=14)
        self.no_outputs()
        fem, fvm = self.make_pair()
        patch_qoi(fem, "T_max", -1e308)
        patch_qoi(fvm, "T_max", 1e308)
        self.invoke(self.pair_args(fem, fvm), code=14)
        self.no_outputs()

    def test_pair_temperature_rise_is_the_only_gate_and_embeds_direct_comparison(self):
        for relative, code, verdict in ((0.005, 0, "pass"), (0.02, 1, "fail")):
            with self.subTest(relative=relative):
                fem, fvm = self.make_pair(relative)
                # A large non-rigid displacement difference must not add a gate.
                with h5py.File(fvm / "step.vtkhdf", "r+") as data:
                    data["VTKHDF/hex/PointData/U"][:] *= 1e6
                patch_qoi(fvm, "warpage_pv", 5.0)
                patch_qoi(fvm, "U_mag_max", 10.0)
                self.invoke(self.pair_args(fem, fvm), code=code)
                result, markdown = self.report(kind="pair", verdict=verdict)
                expected = compare_runs(load_run(fem), load_run(fvm), preset="fem-fvm",
                                        t_ambient=AMBIENT, remove_rigid=True)
                self.assertEqual(result["pair"], expected)
                self.assertEqual([r["run_dir"] for r in result["runs"]], ["fem", "fvm"])
                gates = result["pair"]["gates"]
                self.assertEqual(len(gates), 1)
                self.assertEqual(gates[0]["metric"], "qoi.T_max_rise.rel")
                self.assertTrue(math.isclose(gates[0]["limit"], 0.01, rel_tol=0, abs_tol=1e-16))
                self.assertEqual(gates[0]["pass"], code == 0)
                self.assertGreater(result["pair"]["fields"]["U"]["max_rel"], 100)
                self.assertIsNotNone(result["pair"]["fields"]["U_rigid_removed"])
                for metric in result["pair"]["fields"].values():
                    if metric:
                        self.assert_float_repr(markdown, (metric[k] for k in ("max_abs", "max_rel", "l2_rel")))
                for metric in result["pair"]["qoi"].values():
                    self.assert_float_repr(markdown, metric.values())
                self.assert_float_repr(markdown, [gates[0]["value"], gates[0]["limit"]])

    def test_pair_different_mesh_hashes_exit_three_without_outputs(self):
        fem, fvm = self.make_pair()
        change_manifest(fvm, lambda m: m["mesh"].update(mesh_sha256="b" * 64))
        self.invoke(self.pair_args(fem, fvm), code=3)
        self.no_outputs()

    def test_pair_same_directory_and_symlink_alias_are_usage_errors(self):
        fem, _ = self.make_pair()
        self.invoke(self.pair_args(fem, fem), code=2)
        self.no_outputs()
        alias = self.root / "fem-alias"
        alias.symlink_to(fem, target_is_directory=True)
        self.invoke(self.pair_args(fem, alias), code=2)
        self.no_outputs()

    def test_pair_invalid_input_maps_to_fourteen_without_report(self):
        fem, fvm = self.make_pair()
        for missing in ("manifest", "vtkhdf", "qoi"):
            with self.subTest(missing=missing):
                synthetic_run(fvm, n=8)
                if missing == "manifest":
                    (fvm / "manifest.json").unlink()
                elif missing == "vtkhdf":
                    (fvm / "step.vtkhdf").unlink()
                else:
                    patch_qoi(fvm, "T_max", math.inf)
                self.invoke(self.pair_args(fem, fvm), code=14)
                self.no_outputs()

    def test_real_output_in_temporary_git_tree_is_rejected_for_both_commands(self):
        tree = self.root / "git-tree"
        tree.mkdir()
        (tree / ".git").mkdir()
        fem, fvm = self.make_pair()
        for args in (self.mesh_args(), self.pair_args(fem, fvm)):
            with self.subTest(command=args[0]):
                out = tree / "denied.json"
                self.invoke(args, data_class="real", out=out, code=2)
                self.no_outputs(out)

    def test_real_markdown_symlink_into_git_tree_is_checked_before_json_write(self):
        tree = self.root / "git-tree"
        tree.mkdir()
        (tree / ".git").write_text("gitdir: synthetic-placeholder", encoding="utf-8")
        protected = tree / "protected.md"
        protected.write_text("unchanged", encoding="utf-8")
        self.out.with_suffix(".md").symlink_to(protected)
        self.invoke(self.mesh_args(), data_class="real", code=2)
        self.assertFalse(self.out.exists())
        self.assertEqual(protected.read_text(encoding="utf-8"), "unchanged")

    def test_real_outside_git_writes_both_reports_with_no_numeric_stdout(self):
        # Avoid a decimal-looking random basename, so stdout checks cannot
        # accidentally match an unrelated temporary path name.
        outside = Path(tempfile.mkdtemp(prefix="crosscheck-real-"))
        if any(ch.isdigit() for ch in str(outside)):
            letters = "".join(chr(ord("a") + int(ch)) if ch.isdigit() else ch for ch in outside.name)
            destination = outside.with_name(letters)
            outside.rename(destination)
            outside = destination
        self.addCleanup(shutil.rmtree, outside)
        self.assertNotRegex(str(outside), r"\d\.\d")
        fem, fvm = self.make_pair()
        for args, kind, verdict in ((self.mesh_args(), "mesh_seq", "report"),
                                    (self.pair_args(fem, fvm), "pair", "pass")):
            with self.subTest(command=args[0]):
                out = outside / "report.json"
                completed = self.invoke(args, data_class="real", out=out)
                self.report(kind=kind, verdict=verdict, data_class="real", out=out)
                self.assertNotRegex(completed.stdout, r"\d\.\d")
                self.assertIn(verdict, completed.stdout)
                self.assertIn(str(out), completed.stdout)
                remaining = completed.stdout.replace(str(out), "").replace(str(out.with_suffix(".md")), "")
                self.assertEqual(re.findall(r"[A-Za-z_]+", remaining), [verdict])

    def test_public_data_class_and_explicit_case_id_are_preserved(self):
        self.invoke(self.mesh_args(), data_class="public", extra=("--case-id", "synthetic-case"))
        self.report(kind="mesh_seq", verdict="report", data_class="public", case_id="synthetic-case")

    def test_absolute_path_case_id_cannot_leak_into_reports(self):
        self.invoke(self.mesh_args(), extra=("--case-id", str(self.runs[0].resolve())), code=2)
        self.no_outputs()

    def test_invalid_input_preserves_preexisting_reports(self):
        self.out.write_text("existing-json", encoding="utf-8")
        self.out.with_suffix(".md").write_text("existing-markdown", encoding="utf-8")
        (self.runs[0] / "manifest.json").unlink()
        self.invoke(self.mesh_args(), code=14)
        self.assertEqual(self.out.read_text(encoding="utf-8"), "existing-json")
        self.assertEqual(self.out.with_suffix(".md").read_text(encoding="utf-8"), "existing-markdown")

    def test_bad_enums_subcommands_and_missing_required_options_are_usage_errors(self):
        fem, fvm = self.make_pair()
        mesh = self.mesh_args()
        pair = self.pair_args(fem, fvm)
        invalid = [[], ["Mesh-seq", *mesh[1:]], ["PAIR", *pair[1:]],
                   ["mesh-seq", str(self.runs[0]), str(self.runs[1])],
                   ["mesh-seq", *(str(p) for p in self.runs), str(fem)],
                   pair[:-2], ["pair", "--fem", str(fem), "--t-ambient", "1"],
                   ["pair", "--fvm", str(fvm), "--t-ambient", "1"]]
        for args in invalid:
            with self.subTest(args=args):
                self.invoke(args, code=2)
                self.no_outputs()
        for data_class in ("REAL", "Synthetic", "unknown"):
            with self.subTest(data_class=data_class):
                self.invoke(mesh, data_class=data_class, code=2)
                self.no_outputs()
        for args in (mesh, pair):
            for tail in (("-o", str(self.out)), ("--data-class", "synthetic")):
                with self.subTest(command=args[0], supplied=tail):
                    completed = subprocess.run([sys.executable, str(SCRIPT), *args, *tail],
                                               cwd=ROOT, capture_output=True, text=True, timeout=20)
                    self.assertEqual(completed.returncode, 2, completed.stdout + completed.stderr)
                    self.assertNotIn("Traceback", completed.stderr)
                    self.no_outputs()


if __name__ == "__main__":
    unittest.main(argv=[__file__], verbosity=2)
