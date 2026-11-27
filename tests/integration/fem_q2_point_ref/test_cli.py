# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Real subprocess CLI checks, including parser errors before argparse usage text."""
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from q2_synth import (DUAL_KEYS, PV_KEYS, SOURCE_DIR, analytic_pv, bending, centroids,
                      strip_elements, write_rank)
from tools.vv import fem_q2_point_ref as ref


class CLITests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="q2ref-cli-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.run = self.root / "run"
        data = strip_elements(field=bending)
        write_rank(self.run, 10, data[::2])
        write_rank(self.run, 9, data[1::2])
        self.point_args = ["point-pv", "--fem-run", str(self.run), "--bbox", "0", "0", "1", "0.2",
                           "--nx", "12", "--ny", "4", "--z-top", "0.1", "--data-class", "synthetic"]
        self.dual_args = ["dual", "--fvm-pv", "1", "--exact-pv", "1", "--richardson-pv", "1",
                          "--tol", "0.02"]

    def invoke(self, args, code, prefix=None):
        result = subprocess.run([sys.executable, "-m", ref.__name__, *map(str, args)],
                                cwd=SOURCE_DIR, capture_output=True, text=True, timeout=25)
        self.assertNotEqual(result.returncode, 77, result.stdout + result.stderr)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        if prefix:
            self.assertTrue(result.stderr.splitlines(), result)
            self.assertTrue(result.stderr.splitlines()[0].startswith(prefix), result.stderr)
        return result

    def replace_value(self, args, option, value):
        result = list(args)
        result[result.index(option)+1] = str(value)
        return result

    def omit(self, args, option, count=1):
        result = list(args)
        index = result.index(option)
        del result[index:index+count+1]
        return result

    def test_point_pv_json_from_two_rank_exports(self):
        for nx, ny in ((12, 4), (48, 16)):
            with self.subTest(nx=nx, ny=ny):
                args = self.replace_value(self.point_args, "--nx", nx)
                args = self.replace_value(args, "--ny", ny)
                out = self.root / f"point-{nx}.json"
                result = self.invoke([*args, "--json", out], 0)
                record = json.loads(result.stdout)
                self.assertEqual(set(record), PV_KEYS)
                self.assertEqual(record, json.loads(out.read_text(encoding="utf-8")))
                expected = analytic_pv(centroids(0, 0, 1, 0.2, nx, ny))
                self.assertLessEqual(abs(record["pv"]-expected), 1e-12*expected)
                self.assertEqual(record["n_points"], nx*ny)
                self.assertEqual(record["n_elements"], 48)
                self.assertTrue(math.isfinite(record["eval_seconds"]))
                self.assertGreaterEqual(record["eval_seconds"], 0)
                self.assertTrue(math.isfinite(record["selfcheck_rel"]))
                self.assertGreaterEqual(record["selfcheck_rel"], 0)
                self.assertLessEqual(record["selfcheck_rel"], 1e-12)
        args = self.omit(self.point_args, "--data-class") + ["--data-class", "public"]
        self.assertEqual(set(json.loads(self.invoke(args, 0).stdout)), PV_KEYS)

    def test_empty_and_bad_input_exit_two_with_input_prefix(self):
        empty = self.root / "empty"
        empty.mkdir()
        args = self.replace_value(self.point_args, "--fem-run", empty)
        self.invoke(args, 2, "Q2REF_NO_FILES")
        (empty / "accuracy-q2-rank0.json").write_text("{broken", encoding="utf-8")
        self.invoke(args, 2, "Q2REF_BAD_JSON")
        self.invoke(self.replace_value(self.point_args, "--z-top", "0.2"), 2, "Q2REF_POINT_OUTSIDE")

    def test_real_output_rejected_inside_any_git_tree(self):
        # A synthetic git-tree marker keeps this independent of checkout writability.
        tree = self.root / "git-tree"
        tree.mkdir()
        (tree / ".git").mkdir()
        out = tree / "result.json"
        args = self.replace_value(self.point_args, "--data-class", "real")
        self.invoke([*args, "--json", out], 2, "Q2REF_BAD_ARG")
        self.assertFalse(out.exists())
        checkout_out = SOURCE_DIR / f".q2ref-forbidden-{self.root.name}.json"
        self.addCleanup(lambda: checkout_out.unlink(missing_ok=True))
        self.invoke([*args, "--json", checkout_out], 2, "Q2REF_BAD_ARG")
        self.assertFalse(checkout_out.exists())
        # Real classification without a file output is allowed.
        self.assertEqual(set(json.loads(self.invoke(args, 0).stdout)), PV_KEYS)

    def test_output_write_failures_use_bad_arg(self):
        for args in (self.point_args, self.dual_args):
            for out in (self.root / "absent-parent" / "out.json", self.root):
                with self.subTest(command=args[0], out=str(out)):
                    self.invoke([*args, "--json", out], 2, "Q2REF_BAD_ARG")

    def test_dual_states_exit_codes_and_full_precision_json(self):
        cases = [("1.01", "0.99", 0, "PASS", True, True),
                 ("2", "4", 1, "FAIL", False, False),
                 ("1.01", "2", 3, "REFERENCE-DISAGREE", True, False),
                 ("2", "1.01", 3, "REFERENCE-DISAGREE", False, True)]
        for index, (exact, rich, code, status, pass_exact, pass_rich) in enumerate(cases):
            with self.subTest(status=status, pass_exact=pass_exact):
                args = self.replace_value(self.dual_args, "--exact-pv", exact)
                args = self.replace_value(args, "--richardson-pv", rich)
                out = self.root / f"dual-{index}.json"
                result = self.invoke([*args, "--json", out], code)
                record = json.loads(result.stdout)
                self.assertEqual(set(record), DUAL_KEYS)
                self.assertEqual(record, json.loads(out.read_text(encoding="utf-8")))
                self.assertEqual(record["status"], status)
                self.assertIs(record["pass_exact"], pass_exact)
                self.assertIs(record["pass_richardson"], pass_rich)
                self.assertEqual(float(record["e_exact"]).hex(), (1/float(exact)-1).hex())
                self.assertEqual(float(record["e_richardson"]).hex(), (1/float(rich)-1).hex())

    def test_parser_errors_at_top_level_and_both_subcommands(self):
        cases = [[], ["unknown"], ["POINT-PV"], ["Dual"], ["--unknown"],
                 ["--unknown", *self.dual_args], ["point-pv"], ["dual"],
                 [*self.point_args, "--unknown"], [*self.dual_args, "--unknown"],
                 self.replace_value(self.point_args, "--nx", "1.5"),
                 self.replace_value(self.point_args, "--ny", "bad"),
                 self.replace_value(self.point_args, "--z-top", "bad"),
                 self.replace_value(self.point_args, "--bbox", "bad"),
                 self.replace_value(self.point_args, "--data-class", "REAL"),
                 self.replace_value(self.point_args, "--data-class", "private"),
                 [*self.point_args, "--json"], [*self.dual_args, "--json"]]
        for option, count in (("--fem-run", 1), ("--bbox", 4), ("--nx", 1),
                              ("--ny", 1), ("--z-top", 1), ("--data-class", 1)):
            cases.append(self.omit(self.point_args, option, count))
        # Incomplete bbox is a parser arity error, rather than a numerical range error.
        short_bbox = self.point_args.copy()
        del short_bbox[short_bbox.index("--bbox")+4]
        cases.append(short_bbox)
        for option in ("--fvm-pv", "--exact-pv", "--richardson-pv", "--tol"):
            cases.append(self.omit(self.dual_args, option))
            cases.append(self.replace_value(self.dual_args, option, "bad"))
        for args in cases:
            with self.subTest(args=args):
                self.invoke(args, 2, "Q2REF_BAD_ARG")

    def test_numeric_range_and_nonfinite_errors(self):
        for option, value in (("--nx", "0"), ("--ny", "-1")):
            self.invoke(self.replace_value(self.point_args, option, value), 2, "Q2REF_BAD_ARG")
        for values in ((0, 0, 0, 0.2), (0, 0.2, 1, 0)):
            args = self.point_args.copy()
            index = args.index("--bbox")
            args[index+1:index+5] = list(map(str, values))
            self.invoke(args, 2, "Q2REF_BAD_ARG")
        for option, value in (("--tol", "0"), ("--tol", "-0.1"),
                              ("--exact-pv", "0"), ("--richardson-pv", "0")):
            self.invoke(self.replace_value(self.dual_args, option, value), 2, "Q2REF_BAD_ARG")
        for option in ("--fvm-pv", "--exact-pv", "--richardson-pv", "--tol"):
            for value in ("nan", "inf"):
                self.invoke(self.replace_value(self.dual_args, option, value), 2, "Q2REF_NONFINITE")
        for option in ("--bbox", "--z-top"):
            for value in ("nan", "inf"):
                self.invoke(self.replace_value(self.point_args, option, value), 2, "Q2REF_NONFINITE")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
