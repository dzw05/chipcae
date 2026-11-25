# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic acceptance tests; missing ledger fails assertions, not discovery."""

from __future__ import annotations

import importlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from tools.vv.ctest_skip_audit import read_junit

ROOT = Path(__file__).resolve().parents[2]
DATA = Path(__file__).resolve().parent / "data" / "n21_ledger"
MODULE = "tools.vv.n21_ci_ledger"
REPORT_KEYS = {
    "overall", "required", "per_job", "warnings", "skip_counts_as_pass",
    "data_class", "inputs",
}
EXPECTED_JOBS = {
    "fvm_fvm_case_e2e": "mac-highmem",
    "solve_fvm_thermal": "mac-highmem",
    "solve_fvm_thermal_cut": "mac-highmem",
    "solve_fvm_thermal_np2": "mac-highmem",
    "solve_fvm_elastic": "mac-highmem",
    "solve_fvm_bimetal": "mac-highmem",
    "solve_fvm_elastic_np2": "mac-highmem",
    "cutbim_cut_bimetal": "mac-highmem",
    "solve_column_gmg_fcbga": "mac-highmem",
    "solve_column_gmg_fcbga_scaling": "mac-highmem",
    "f1130_uncut_cli_e2e": "*",
    "chain_ipc_uncut_chain": "*",
    "odbcli_odbpp_cli_e2e": "*",
    "field_compare_ranks": "*",
    "vv_result_schema": "*",
    "realcase_real_case_unit": "*",
    "realcase_real_case_thermal": "*",
    "realcase_real_case_warpage": "*",
    "realcase_real_case_assembly": "*",
}


class N21CILedgerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.temp = Path(temporary.name)
        self.required = self.temp / "required.tsv"
        self.required.write_bytes((DATA / "required.tsv").read_bytes())
        self.out = self.temp / "reports with spaces" / "nested"

    def _module(self):
        try:
            return importlib.import_module(MODULE)
        except ModuleNotFoundError as error:
            self.fail(f"N21 ledger module must exist: {error}")

    def _requirements(self, text="required_case\t*\tSynthetic requirement\n"):
        self.required.write_text(text, encoding="utf-8")

    def _args(self, jobs=None):
        if jobs is None:
            jobs = [("mac-highmem", DATA / "passed.xml")]
        args = ["--required", str(self.required), "--out", str(self.out)]
        for label, path in jobs:
            args += ["--job", f"{label}={path}"]
        return args

    def _cli(self, args):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(ROOT)
        result = subprocess.run(
            [sys.executable, "-m", MODULE, *args], cwd=ROOT, env=env,
            capture_output=True, text=True, timeout=10,
        )
        self.assertNotIn("No module named", result.stderr,
                         f"N21 ledger CLI must exist; stderr: {result.stderr}")
        self.assertNotIn("Traceback", result.stderr,
                         f"CLI must report errors without a traceback: {result.stderr}")
        return result

    def _report(self, jobs=None, code=0):
        if jobs is None:
            jobs = [("mac-highmem", DATA / "passed.xml")]
        result = self._cli(self._args(jobs))
        self.assertEqual(result.returncode, code, result.stderr)
        json_path = self.out / "n21-ledger.json"
        md_path = self.out / "n21-ledger.md"
        self.assertTrue(json_path.is_file(), "CLI must write n21-ledger.json")
        self.assertTrue(md_path.is_file(), "CLI must write n21-ledger.md")
        report = json.loads(json_path.read_text(encoding="utf-8"))
        self.assertEqual(set(report), REPORT_KEYS)
        self.assertEqual(report["overall"], "GREEN" if code == 0 else "RED")
        self.assertIs(report["skip_counts_as_pass"], False)
        self.assertEqual(report["data_class"], "synthetic")
        self.assertEqual(report["inputs"], {
            label: str(Path(path).resolve()) for label, path in jobs
        })
        self.assertIsInstance(report["warnings"], list)
        for warning in report["warnings"]:
            self.assertIsInstance(warning, str)
        required = report["required"]
        self.assertIsInstance(required, list)
        self.assertEqual([r["name"] for r in required],
                         sorted(r["name"] for r in required))
        for entry in required:
            self.assertEqual(set(entry), {"name", "job", "note", "status", "evidence"})
            self.assertEqual([e["job"] for e in entry["evidence"]],
                             sorted(e["job"] for e in entry["evidence"]))
            for evidence in entry["evidence"]:
                self.assertEqual(set(evidence), {"job", "status", "reason"})
                self.assertIsInstance(evidence["reason"], str)
        self.assertEqual(set(report["per_job"]), {label for label, _ in jobs})
        for job in report["per_job"].values():
            self.assertEqual(set(job), {"counts", "skipped", "notrun"})
            self.assertEqual(set(job["counts"]), {"Passed", "Failed", "Skipped", "NotRun"})
            for count in job["counts"].values():
                self.assertIs(type(count), int)
                self.assertGreaterEqual(count, 0)
            for key in ("skipped", "notrun"):
                self.assertIsInstance(job[key], list)
                self.assertEqual([r["name"] for r in job[key]],
                                 sorted(r["name"] for r in job[key]))
                for entry in job[key]:
                    self.assertEqual(set(entry), {"name", "reason"})
                    self.assertIsInstance(entry["reason"], str)
        self.assertEqual(md_path.read_text(encoding="utf-8").splitlines()[0],
                         f"{report['overall']} — N21 CI ledger")
        return report

    def _one(self, report, status, evidence, *, job="*", note="Synthetic requirement"):
        self.assertEqual(report["required"], [{
            "name": "required_case", "job": job, "note": note,
            "status": status, "evidence": evidence,
        }])

    def _error(self, args, *diagnostics):
        result = self._cli(args)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertTrue(result.stderr.strip(), "Invalid input needs a useful diagnostic")
        for diagnostic in diagnostics:
            self.assertIn(diagnostic, result.stderr)
        return result

    def test_all_required_pass_exact_report_and_unicode(self):
        report = self._report()
        self.assertEqual(report["required"], [
            {"name": "another_case", "job": "*", "note": "Synthetic wildcard acceptance",
             "status": "Passed", "evidence": [
                 {"job": "mac-highmem", "status": "Passed", "reason": ""}]},
            {"name": "required_case", "job": "mac-highmem",
             "note": "Synthetic designated-job acceptance", "status": "Passed",
             "evidence": [{"job": "mac-highmem", "status": "Passed", "reason": "完成"}]},
        ])
        self.assertEqual(report["per_job"], {"mac-highmem": {
            "counts": {"Passed": 2, "Failed": 0, "Skipped": 0, "NotRun": 0},
            "skipped": [], "notrun": [],
        }})
        self.assertEqual(report["warnings"], [])
        self.assertIn("完成", (self.out / "n21-ledger.json").read_text(encoding="utf-8"))

    def test_skipped_notrun_and_disabled_required_are_red(self):
        self._requirements()
        for fixture, status, reason in (
            ("skipped.xml", "Skipped", "backend | unavailable\nreturn 77"),
            ("notrun.xml", "NotRun", "not scheduled"),
            ("disabled.xml", "NotRun", "disabled backend"),
            ("failed.xml", "Failed", "failed check"),
        ):
            with self.subTest(fixture=fixture):
                report = self._report([("box", DATA / fixture)], code=1)
                self._one(report, status, [{"job": "box", "status": status, "reason": reason}])
                counts = {"Passed": 0, "Failed": 0, "Skipped": 0, "NotRun": 0}
                counts[status] = 1
                self.assertEqual(report["per_job"]["box"]["counts"], counts)
                key = "skipped" if status == "Skipped" else "notrun"
                if status != "Failed":
                    self.assertEqual(report["per_job"]["box"][key],
                                     [{"name": "required_case", "reason": reason}])

    def test_missing_test_is_case_sensitive(self):
        self._requirements()
        report = self._report([("box", DATA / "missing.xml")], code=1)
        self._one(report, "Missing", [{"job": "box", "status": "Missing",
                                      "reason": "Test not present in job JUnit"}])

    def test_specific_job_cannot_borrow_a_pass_from_another_job(self):
        self._requirements("required_case\tmac-highmem\tDesignated\n")
        report = self._report([("other", DATA / "passed.xml"),
                               ("mac-highmem", DATA / "missing.xml")], code=1)
        self._one(report, "Missing", [{"job": "mac-highmem", "status": "Missing",
                                      "reason": "Test not present in job JUnit"}],
                  job="mac-highmem", note="Designated")

    def test_absent_designated_job_has_one_jobmissing_evidence(self):
        self._requirements("required_case\tmac-highmem\tDesignated\n")
        report = self._report([("Mac-highmem", DATA / "passed.xml")], code=1)
        self._one(report, "JobMissing", [{"job": "mac-highmem", "status": "JobMissing",
                                         "reason": "Job not supplied"}],
                  job="mac-highmem", note="Designated")

    def test_wildcard_preserves_all_jobs_and_status_precedence(self):
        self._requirements()
        for other, status, other_status, other_reason, code in (
            ("skipped.xml", "Passed", "Skipped", "backend | unavailable\nreturn 77", 0),
            ("failed.xml", "Failed", "Failed", "failed check", 1),
            ("missing.xml", "Passed", "Missing", "Test not present in job JUnit", 0),
        ):
            with self.subTest(other=other):
                report = self._report([("z-pass", DATA / "passed.xml"),
                                       ("a-other", DATA / other)], code=code)
                self._one(report, status, [
                    {"job": "a-other", "status": other_status, "reason": other_reason},
                    {"job": "z-pass", "status": "Passed", "reason": "完成"},
                ])

    def test_skipped_precedes_notrun_and_notrun_precedes_missing(self):
        self._requirements()
        for fixture, status, reason in (
            ("skipped.xml", "Skipped", "backend | unavailable\nreturn 77"),
            ("missing.xml", "Missing", "Test not present in job JUnit"),
        ):
            with self.subTest(fixture=fixture):
                report = self._report([("b", DATA / "notrun.xml"),
                                       ("a", DATA / fixture)], code=1)
                self._one(report, "Skipped" if status == "Skipped" else "NotRun", [
                    {"job": "a", "status": status, "reason": reason},
                    {"job": "b", "status": "NotRun", "reason": "not scheduled"},
                ])

    def test_duplicate_final_occurrence_controls_counts_and_prefixed_warning(self):
        self._requirements()
        for fixture, status, reason, code in (
            ("duplicate_skip_last.xml", "Skipped", "final skip", 1),
            ("duplicate_pass_last.xml", "Passed", "final pass", 0),
        ):
            with self.subTest(fixture=fixture):
                report = self._report([("duplicate", DATA / fixture)], code=code)
                self._one(report, status, [{"job": "duplicate", "status": status,
                                           "reason": reason}])
                self.assertEqual(report["per_job"]["duplicate"]["counts"], {
                    "Passed": int(status == "Passed"), "Failed": 0,
                    "Skipped": int(status == "Skipped"), "NotRun": 0,
                })
                self.assertEqual(report["warnings"], [
                    "duplicate: Duplicate test 'required_case': 2 occurrences; using final occurrence"
                ])

    def test_parser_warnings_are_prefixed_in_sorted_job_order(self):
        self._requirements()
        report = self._report([("z", DATA / "warning.xml"), ("a", DATA / "warning.xml")])
        self.assertEqual(report["warnings"], [
            "a: optional: missing or invalid skipped time None",
            "z: optional: missing or invalid skipped time None",
        ])

    def test_nonrequired_skipped_notrun_and_failed_do_not_change_green(self):
        self._requirements()
        report = self._report([("box", DATA / "informational.xml")])
        self.assertEqual(report["per_job"], {"box": {
            "counts": {"Passed": 1, "Failed": 1, "Skipped": 2, "NotRun": 2},
            "skipped": [{"name": "a_skip", "reason": "optional capability"},
                        {"name": "z_skip", "reason": "optional | backend\nunavailable"}],
            "notrun": [{"name": "a_notrun", "reason": "disabled"},
                       {"name": "z_notrun", "reason": "not scheduled"}],
        }})

    def test_markdown_table_order_and_evidence_reason_escaping(self):
        self._requirements()
        self._report([("a", DATA / "skipped.xml"), ("z", DATA / "informational.xml")])
        markdown = (self.out / "n21-ledger.md").read_text(encoding="utf-8")
        self.assertEqual(markdown.splitlines()[0], "GREEN — N21 CI ledger")
        self.assertIn("| Name | Job | Status | Evidence |", markdown)
        self.assertIn("backend \\| unavailable<br>return 77", markdown)
        self.assertIn("optional \\| backend<br>unavailable", markdown)
        self.assertNotIn("backend | unavailable", markdown)
        self.assertIn("Skipped", markdown)
        self.assertIn("Passed", markdown)
        required_position = markdown.index("| Name | Job | Status | Evidence |")
        count_header = re.search(r"\| Job \| Passed \| Failed \| Skipped \| NotRun \|", markdown)
        self.assertIsNotNone(count_header, markdown)
        self.assertLess(required_position, count_header.start())
        self.assertLess(count_header.start(), markdown.index("a_skip"))
        self.assertLess(markdown.index("a_skip"), markdown.index("z_skip"))

    def test_public_renderer_escapes_names_newlines_and_pipes(self):
        module = self._module()
        tests, _ = read_junit(DATA / "passed.xml")
        required = {"required_case": {"name": "required_case", "job": "*", "note": "Synthetic"}}
        report = module.evaluate({"box": tests}, required)
        report["required"][0]["name"] = "case|name\nline"
        report["required"][0]["evidence"][0]["reason"] = "reason|detail\nnext"
        markdown = module.render_markdown({**report, "inputs": {}})
        self.assertIn("case\\|name<br>line", markdown)
        self.assertIn("reason\\|detail<br>next", markdown)

    def test_public_reader_returns_exact_mapping_and_accepts_full_label_grammar(self):
        module = self._module()
        self.assertEqual(module.read_required_jobs(DATA / "required.tsv"), {
            "required_case": {"name": "required_case", "job": "mac-highmem",
                              "note": "Synthetic designated-job acceptance"},
            "another_case": {"name": "another_case", "job": "*",
                             "note": "Synthetic wildcard acceptance"},
        })
        self._requirements("required_case\tA0_.:/-\t\n")
        self.assertEqual(module.read_required_jobs(self.required), {
            "required_case": {"name": "required_case", "job": "A0_.:/-", "note": ""},
        })
        report = self._report([("A0_.:/-", DATA / "passed.xml")])
        self.assertEqual(report["required"][0]["status"], "Passed")

    def test_public_evaluate_uses_normalized_records_and_rejects_empty_requirements(self):
        module = self._module()
        tests, _ = read_junit(DATA / "skipped.xml")
        required = {"required_case": {"name": "required_case", "job": "*", "note": "Synthetic"}}
        report = module.evaluate({"box": tests}, required)
        self.assertEqual(set(report), REPORT_KEYS - {"inputs"})
        self.assertEqual(report["overall"], "RED")
        self.assertIs(report["skip_counts_as_pass"], False)
        self.assertEqual(report["data_class"], "synthetic")
        self._one(report, "Skipped", [{"job": "box", "status": "Skipped",
                                       "reason": "backend | unavailable\nreturn 77"}], note="Synthetic")
        with self.assertRaises(ValueError):
            module.evaluate({"box": tests}, {})

    def test_repository_list_has_exact_nineteen_name_to_job_mapping(self):
        module = self._module()
        path = ROOT / "tests" / "vv" / "n21_required.tsv"
        self.assertTrue(path.is_file(), "The N21 required-job list must be supplied")
        required = module.read_required_jobs(path)
        self.assertEqual({name: record["job"] for name, record in required.items()}, EXPECTED_JOBS)
        for name, record in required.items():
            self.assertEqual(set(record), {"name", "job", "note"})
            self.assertEqual(record["name"], name)
            self.assertTrue(record["note"].strip(), name)
            if record["job"] == "mac-highmem":
                self.assertIn("mac-highmem.yml", record["note"])
                step = ("Test FVM acceptance (mandatory)" if name.startswith(("fvm_", "solve_fvm_"))
                        else "Test highmem (a, mandatory)")
                self.assertIn(step, record["note"])

    def test_invalid_tsv_cli_and_reader_include_path_and_physical_line(self):
        for fixture, line in (("invalid_width.tsv", 3), ("empty_name.tsv", 3),
                              ("duplicate_name.tsv", 4), ("invalid_job.tsv", 3)):
            with self.subTest(fixture=fixture):
                path = DATA / fixture
                args = self._args()
                args[args.index("--required") + 1] = str(path)
                result = self._error(args, str(path))
                self.assertRegex(result.stderr, rf"\bline\s+{line}\b")
                module = self._module()
                with self.assertRaises(ValueError) as raised:
                    module.read_required_jobs(path)
                self.assertIn(str(path), str(raised.exception))
                self.assertRegex(str(raised.exception), rf"\bline\s+{line}\b")

    def test_extra_tsv_column_and_empty_or_invalid_job_are_input_errors(self):
        for row in ("required_case\t*\tnote\textra\n", "required_case\t\tnote\n",
                    "required_case\t_bad\tnote\n", "required_case\tbad=label\tnote\n"):
            with self.subTest(row=row):
                self._requirements("# header\n\n" + row)
                result = self._error(self._args(), str(self.required))
                self.assertRegex(result.stderr, r"\bline\s+3\b")

    def test_empty_and_comment_only_lists_are_input_errors(self):
        for content in ("", (DATA / "comment_only.tsv").read_text(encoding="utf-8")):
            with self.subTest(content=content):
                self._requirements(content)
                self._error(self._args(), str(self.required))
                with self.assertRaises(ValueError):
                    self._module().read_required_jobs(self.required)

    def test_duplicate_cli_job_label_is_input_error(self):
        self._error(self._args([("box", DATA / "passed.xml"),
                                ("box", DATA / "skipped.xml")]), "box")

    def test_invalid_cli_job_arguments_are_input_errors(self):
        for argument, diagnostic in (
            ("missing-equals", "missing-equals"),
            (f"bad label={DATA / 'passed.xml'}", "bad label"),
            (f"*={DATA / 'passed.xml'}", "*"),
            (f"={DATA / 'passed.xml'}", ""),
            ("box=", "box"),
            (f"_bad={DATA / 'passed.xml'}", "_bad"),
        ):
            with self.subTest(argument=argument):
                self._error([*self._args([]), "--job", argument], diagnostic)

    def test_no_jobs_is_input_error(self):
        result = self._error(self._args([]))
        self.assertRegex(result.stderr, r"(?i)\bjobs?\b")

    def test_missing_and_malformed_inputs_are_input_errors(self):
        for path in (self.temp / "absent.xml", DATA / "malformed.xml"):
            with self.subTest(path=path):
                self._error(self._args([("box", path)]), str(path))
        self.required = self.temp / "absent.tsv"
        self._error(self._args(), str(self.required))

    def test_paths_with_equals_and_relative_inputs_are_resolved(self):
        path = self.temp / "junit=synthetic.xml"
        path.write_bytes((DATA / "passed.xml").read_bytes())
        relative = Path(os.path.relpath(path, ROOT))
        self._report([("mac-highmem", relative)])

    def test_output_directory_is_created_reports_replaced_other_files_preserved(self):
        self.assertFalse(self.out.exists())
        self._report()
        sentinel = self.out / "unrelated.txt"
        sentinel.write_text("preserve me", encoding="utf-8")
        (self.out / "n21-ledger.json").write_text("stale JSON", encoding="utf-8")
        (self.out / "n21-ledger.md").write_text("stale Markdown", encoding="utf-8")
        self._requirements()
        self._report([("box", DATA / "skipped.xml")], code=1)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve me")

    def test_output_path_occupied_by_regular_file_is_error(self):
        self.out.parent.mkdir(parents=True)
        self.out.write_text("preserve file", encoding="utf-8")
        self._error(self._args(), str(self.out))
        self.assertEqual(self.out.read_text(encoding="utf-8"), "preserve file")

    def test_report_write_failure_is_error(self):
        self.out.mkdir(parents=True)
        (self.out / "n21-ledger.json").mkdir()
        self._error(self._args(), "n21-ledger.json")


if __name__ == "__main__":
    unittest.main()
