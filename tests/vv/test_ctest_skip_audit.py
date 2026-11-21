# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Acceptance tests for the CTest skip auditor (no solver/backend required).

Exercise the public CLI, rather than prescribing parser implementation details.
Missing modules fail assertions inside each test, so discovery still works and a
Python startup exit of 1 cannot masquerade as the required audit violation.
"""

from __future__ import annotations

import contextlib
import importlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
DATA = Path(__file__).resolve().parent / "data" / "skip_audit"
MODULE = "tools.vv.ctest_skip_audit"
MPI = "CHIPCAE_ENABLE_MPI"
HDF5 = "CHIPCAE_ENABLE_HDF5"
REPORT_KEYS = {
    "commit", "junit", "switches", "mpiexec", "counts", "skipped", "notrun",
    "required_violations", "required_inactive", "missing_required", "warnings",
    "data_class", "overall",
}


class CTestSkipAuditTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        cls.source = Path(temporary.name) / "source with spaces"
        cls.source.mkdir()
        # A real source HEAD proves discovery uses the cache, not the auditor's cwd.
        for args in (
            ("init", "--quiet"),
            ("-c", "user.name=Synthetic Test", "-c", "user.email=test@example.invalid",
             "-c", "commit.gpgsign=false", "commit", "--allow-empty", "--quiet",
             "-m", "synthetic source"),
        ):
            subprocess.run(["git", "-C", str(cls.source), *args], check=True,
                           capture_output=True, text=True)
        cls.source_commit = subprocess.check_output(
            ["git", "-C", str(cls.source), "rev-parse", "HEAD"], text=True
        ).strip()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.temp = Path(temporary.name)
        self.build = self.temp / "build"
        self.build.mkdir()
        self.out = self.temp / "reports" / "nested"
        self.junit = self.temp / "junit.xml"
        self.junit.write_bytes((DATA / "passed.xml").read_bytes())
        self.required = self.temp / "required.tsv"
        self.required.write_bytes((DATA / "required.tsv").read_bytes())
        self._cache()

    def _cache(self, text=None, *, source=None):
        if text is None:
            text = (DATA / "cache.txt").read_text(encoding="utf-8")
        source = self.source if source is None else source
        (self.build / "CMakeCache.txt").write_text(
            text + f"\nCMAKE_HOME_DIRECTORY:INTERNAL={source}\n", encoding="utf-8"
        )

    def _xml(self, *cases, root="testsuite"):
        document = ET.Element(root)
        suite = ET.SubElement(document, "testsuite") if root == "testsuites" else document
        for case in cases:
            suite.append(ET.fromstring(case))
        ET.ElementTree(document).write(self.junit, encoding="utf-8", xml_declaration=True)

    def _requirements(self, text):
        self.required.write_text(text, encoding="utf-8")

    def _args(self, *, required=True):
        args = ["--junit", str(self.junit), "--build-dir", str(self.build),
                "--out", str(self.out)]
        if required:
            args += ["--required", str(self.required)]
        return args

    def _cli(self, *, required=True, args=None):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(ROOT)
        result = subprocess.run(
            [sys.executable, "-m", MODULE,
             *(self._args(required=required) if args is None else args)],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=10,
        )
        self.assertNotIn(
            "No module named", result.stderr,
            f"Missing skip-audit feature: {MODULE}; CLI did not start:\n{result.stderr}",
        )
        self.assertNotIn("Traceback (most recent call last)", result.stderr, result.stderr)
        return result

    def _report(self, *, code=0, required=True):
        result = self._cli(required=required)
        self.assertEqual(result.returncode, code, result.stderr)
        return self._read_report(code)

    def _read_report(self, code):
        path = self.out / "skip-audit.json"
        self.assertTrue(path.is_file(), "Auditor must write skip-audit.json")
        report = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(set(report), REPORT_KEYS)
        self.assertEqual(set(report["counts"]), {"Passed", "Failed", "Skipped", "NotRun"})
        for value in report["counts"].values():
            self.assertIs(type(value), int)
            self.assertGreaterEqual(value, 0)
        for key in ("skipped", "notrun", "required_violations", "required_inactive",
                    "missing_required", "warnings"):
            self.assertIsInstance(report[key], list, key)
        for warning in report["warnings"]:
            self.assertIsInstance(warning, str)
        self.assertIsInstance(report["junit"], str)
        self.assertEqual(Path(report["junit"]).resolve(), self.junit.resolve())
        self.assertIsInstance(report["switches"], dict)
        self.assertTrue(report["commit"] is None or isinstance(report["commit"], str))
        self.assertTrue(report["mpiexec"] is None or isinstance(report["mpiexec"], str))
        self.assertEqual(report["data_class"], "synthetic")
        self.assertEqual(report["overall"], "VIOLATION" if code == 1 else "OK")
        for key in ("skipped", "notrun", "required_violations", "required_inactive"):
            names = [entry["name"] for entry in report[key]]
            self.assertEqual(names, sorted(names), key)
        for entry in report["skipped"]:
            self.assertIsInstance(entry["reason"], str)
            self.assertIn("elapsed_seconds", entry)
            self.assertTrue(entry["elapsed_seconds"] is None or
                            type(entry["elapsed_seconds"]) in (int, float))
        for entry in report["notrun"]:
            self.assertIsInstance(entry["reason"], str)
        markdown = self.out / "skip-audit.md"
        self.assertTrue(markdown.is_file(), "Auditor must write skip-audit.md")
        self.assertIn(report["overall"], markdown.read_text(encoding="utf-8").splitlines()[0])
        return report

    def _violation(self, report, name="required_case", status="Skipped"):
        self.assertEqual(len(report["required_violations"]), 1)
        entry = report["required_violations"][0]
        self.assertEqual(entry["name"], name)
        self.assertEqual(entry["status"], status)
        self.assertEqual(entry["requires"], f"{MPI},{HDF5}")

    def _assert_invalid(self, *, diagnostic=None, args=None):
        result = self._cli(args=args)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertTrue(result.stderr.strip(), "Invalid input needs a stderr diagnostic")
        if diagnostic:
            self.assertRegex(result.stderr, diagnostic)
        self.assertFalse(self.out.exists(), "Invalid inputs must not create reports")

    def test_all_passed_exit_zero_and_exact_counts(self):
        report = self._report()
        self.assertEqual(report["counts"], {"Passed": 2, "Failed": 0, "Skipped": 0, "NotRun": 0})
        self.assertEqual(report["required_violations"], [])
        self.assertEqual(report["missing_required"], [])
        self.assertEqual(report["required_inactive"], [])

    def test_active_required_skipped_is_violation_even_with_notrun_attribute(self):
        self.junit.write_bytes((DATA / "mixed.xml").read_bytes())
        report = self._report(code=1)
        self._violation(report)
        self.assertEqual(report["counts"], {"Passed": 1, "Failed": 1, "Skipped": 1, "NotRun": 2})
        self.assertEqual(report["skipped"][0]["reason"], "synthetic MPI unavailable")
        self.assertAlmostEqual(report["skipped"][0]["elapsed_seconds"], 0.125)

    def test_required_notrun_and_disabled_are_violations(self):
        for status, child in (("notrun", ""), ("disabled", '<skipped message="Disabled"/>'),
                              ("Disabled", '<skipped message="Disabled"/>')):
            with self.subTest(status=status):
                self._xml(f'<testcase name="required_case" status="{status}">{child}</testcase>')
                report = self._report(code=1)
                self._violation(report, status="NotRun")
                self.assertEqual(report["counts"], {"Passed": 0, "Failed": 0, "Skipped": 0, "NotRun": 1})
                self.assertEqual([entry["name"] for entry in report["notrun"]], ["required_case"])

    def test_one_off_switch_makes_requirement_inactive(self):
        self._cache(f"{MPI}:BOOL=ON\n{HDF5}:BOOL=OFF\n")
        self.junit.write_bytes((DATA / "mixed.xml").read_bytes())
        report = self._report()
        self.assertEqual(report["required_violations"], [])
        self.assertEqual([entry["name"] for entry in report["required_inactive"]], ["required_case"])
        self.assertIn(HDF5, json.dumps(report["required_inactive"]))

    def test_missing_active_required_is_violation(self):
        self._xml('<testcase name="other_case" status="run"/>')
        report = self._report(code=1)
        self._violation(report, status="Missing")
        self.assertIn("required_case", json.dumps(report["missing_required"]))
        self.assertEqual(report["counts"], {"Passed": 1, "Failed": 0, "Skipped": 0, "NotRun": 0})

    def test_missing_inactive_required_is_still_listed_without_violation(self):
        self._cache(f"{MPI}:BOOL=OFF\n{HDF5}:BOOL=ON\n")
        self._xml('<testcase name="other_case" status="run"/>')
        report = self._report()
        self.assertIn("required_case", json.dumps(report["missing_required"]))
        self.assertEqual([entry["name"] for entry in report["required_inactive"]], ["required_case"])
        self.assertEqual(report["required_violations"], [])

    def test_missing_cache_switch_is_off_with_named_warning(self):
        self._cache(f"{MPI}:BOOL=ON\n")
        self._xml('<testcase name="required_case" status="run"><skipped/></testcase>')
        report = self._report()
        self.assertEqual(report["required_violations"], [])
        self.assertEqual([entry["name"] for entry in report["required_inactive"]], ["required_case"])
        self.assertTrue(any(HDF5 in warning for warning in report["warnings"]))

    def test_no_required_file_summarizes_failures_and_skips_without_failing(self):
        self.junit.write_bytes((DATA / "mixed.xml").read_bytes())
        report = self._report(required=False)
        self.assertEqual(report["counts"], {"Passed": 1, "Failed": 1, "Skipped": 1, "NotRun": 2})
        for key in ("required_violations", "required_inactive", "missing_required"):
            self.assertEqual(report[key], [])

    def test_failed_required_does_not_become_skip_violation(self):
        for attributes, child in (('status="fail"', ""),
                                  ('status="run"', '<failure message="synthetic"/>')):
            with self.subTest(attributes=attributes, child=child):
                self._xml(f'<testcase name="required_case" {attributes}>{child}</testcase>')
                report = self._report()
                self.assertEqual(report["counts"], {"Passed": 0, "Failed": 1, "Skipped": 0, "NotRun": 0})
                self.assertEqual(report["required_violations"], [])

    def test_duplicate_names_use_final_occurrence_and_warn_count(self):
        passed = '<testcase name="required_case" status="run"/>'
        skipped = '<testcase name="required_case" status="notrun" time="0.1"><skipped/></testcase>'
        for cases, code, counts in (
            ((passed, skipped), 1, {"Passed": 0, "Failed": 0, "Skipped": 1, "NotRun": 0}),
            ((skipped, passed), 0, {"Passed": 1, "Failed": 0, "Skipped": 0, "NotRun": 0}),
        ):
            with self.subTest(code=code):
                # Occurrences span suites, so a per-suite dictionary is insufficient.
                self.junit.write_text(
                    f'<testsuites><testsuite>{cases[0]}</testsuite>'
                    f'<testsuite>{cases[1]}</testsuite></testsuites>', encoding="utf-8"
                )
                report = self._report(code=code)
                self.assertEqual(report["counts"], counts)
                self.assertTrue(any("required_case" in warning and re.search(r"\b2\b", warning)
                                    for warning in report["warnings"]), report["warnings"])
                if code:
                    self._violation(report)
                else:
                    self.assertEqual(report["required_violations"], [])
                    self.assertEqual(report["skipped"], [])

    def test_both_junit_roots_are_supported(self):
        for root in ("testsuite", "testsuites"):
            with self.subTest(root=root):
                self._xml('<testcase name="required_case" status="run"/>', root=root)
                self.assertEqual(self._report()["counts"]["Passed"], 1)

    def test_missing_junit_is_input_error(self):
        self.junit.unlink()
        self._assert_invalid(diagnostic=r"(?i)junit")

    def test_empty_junit_is_input_error(self):
        self.junit.write_text("", encoding="utf-8")
        self._assert_invalid()

    def test_malformed_junit_is_input_error(self):
        self.junit.write_text("<testsuite><testcase", encoding="utf-8")
        self._assert_invalid()

    def test_unknown_status_or_invalid_xml_structure_is_input_error(self):
        for xml in ('<unrelated/>', '<testsuite><testcase status="run"/></testsuite>',
                    '<testsuite><testcase name="a" status="mystery"/></testsuite>'):
            with self.subTest(xml=xml):
                self.junit.write_text(xml, encoding="utf-8")
                self._assert_invalid()

    def test_missing_cache_is_input_error(self):
        (self.build / "CMakeCache.txt").unlink()
        self._assert_invalid(diagnostic=r"CMakeCache\.txt")

    def test_unreadable_required_file_is_input_error(self):
        self.required.unlink()
        self._assert_invalid(diagnostic=r"(?i)required")

    def test_malformed_tsv_reports_exact_physical_line_number(self):
        for malformed in ("only_one_column", "a\t-", "a\t-\tnote\textra"):
            with self.subTest(malformed=malformed):
                self._requirements("# comment\n\nvalid_case\t-\tnote\n" + malformed + "\n")
                self._assert_invalid(diagnostic=r"(?i)(?:line\s+4\b|:4\b)")

    def test_invalid_cli_arguments_are_input_error(self):
        self._assert_invalid(args=[])
        self._assert_invalid(args=[*self._args(), "--unknown-option"])

    def test_cmake_truth_spellings_control_requirements_and_snapshot(self):
        for raw, enabled in (("ON", True), ("on", True), ("TRUE", True),
                             ("true", True), ("1", True), ("YES", True),
                             ("Y", True), ("OFF", False), ("FALSE", False),
                             ("0", False), ("NO", False), ("N", False),
                             ("IGNORE", False), ("NOTFOUND", False), ("", False)):
            with self.subTest(raw=raw):
                self._cache(f"{MPI}:BOOL={raw}\n{HDF5}:BOOL=ON\n")
                self._xml('<testcase name="required_case" status="run" time="0"><skipped/></testcase>')
                report = self._report(code=1 if enabled else 0)
                self.assertEqual(report["switches"][MPI], {"raw": raw, "enabled": enabled})
                self.assertEqual(len(report["required_violations"]), int(enabled))
                self.assertEqual(len(report["required_inactive"]), int(not enabled))

    def test_snapshot_contains_all_and_only_chipcae_enable_bool_entries(self):
        report = self._report()
        self.assertEqual(report["switches"], {
            MPI: {"raw": "ON", "enabled": True},
            "CHIPCAE_ENABLE_MFEM": {"raw": "TRUE", "enabled": True},
            HDF5: {"raw": "1", "enabled": True},
            "CHIPCAE_ENABLE_ECAD": {"raw": "OFF", "enabled": False},
            "CHIPCAE_ENABLE_OCCT": {"raw": "FALSE", "enabled": False},
        })

    def test_comments_blank_lines_and_unconditional_requirement(self):
        self._requirements("# header\n\n# another comment\nrequired_case\t-\tSynthetic unconditional\n\n")
        self._cache("")
        self._xml('<testcase name="required_case" status="run" time="0"><skipped/></testcase>')
        report = self._report(code=1)
        self.assertEqual(len(report["required_violations"]), 1)
        self.assertEqual(report["required_violations"][0]["requires"], "-")
        self.assertEqual(report["required_inactive"], [])

    def test_commit_uses_source_directory_from_cache(self):
        report = self._report()
        self.assertEqual(report["commit"], self.source_commit)

    def test_git_failure_is_null_warning_without_audit_failure_or_cwd_fallback(self):
        non_repo = self.temp / "not a source repository"
        non_repo.mkdir()
        self._cache(source=non_repo)
        report = self._report()
        self.assertIsNone(report["commit"])
        self.assertTrue(any(re.search(r"(?i)git|commit", warning) for warning in report["warnings"]))

    def test_missing_source_in_cache_is_null_warning_without_cwd_fallback(self):
        (self.build / "CMakeCache.txt").write_text(f"{MPI}:BOOL=ON\n{HDF5}:BOOL=ON\n", encoding="utf-8")
        report = self._report()
        self.assertIsNone(report["commit"])
        self.assertTrue(report["warnings"])

    def test_mpiexec_path_and_unavailable_are_recorded(self):
        try:
            auditor = importlib.import_module(MODULE)
        except ModuleNotFoundError as error:
            if error.name != MODULE:
                raise
            self.fail(f"Missing skip-audit feature: {MODULE}")
        original_which = __import__("shutil").which
        for available in (None, str(self.temp / "synthetic mpiexec")):
            with self.subTest(available=available):
                def which(command, *args, **kwargs):
                    return available if command == "mpiexec" else original_which(command, *args, **kwargs)

                with mock.patch("shutil.which", side_effect=which), \
                        contextlib.redirect_stderr(io.StringIO()), \
                        contextlib.redirect_stdout(io.StringIO()):
                    code = auditor.main(self._args())
                self.assertEqual(code, 0)
                self.assertEqual(self._read_report(0)["mpiexec"], available)

    def test_long_reason_is_preserved_and_truncated_at_500_characters(self):
        reason = "  synthetic | MPI\n缺少运行器 " + "x" * 600
        case = ET.Element("testcase", name="required_case", status="notrun", time="0.375")
        ET.SubElement(case, "skipped").text = reason
        self._xml(ET.tostring(case, encoding="unicode"))
        report = self._report(code=1)
        self.assertEqual(report["skipped"][0]["reason"], reason[:500])
        self.assertAlmostEqual(report["skipped"][0]["elapsed_seconds"], 0.375)

    def test_system_out_fallback_preserves_text_without_inferring_status(self):
        self._xml(
            '<testcase name="required_case" status="notrun" time="0.1">'
            '<skipped/><system-out>  synthetic reason\nline two  </system-out></testcase>',
            '<testcase name="passed_case" status="run"><system-out>SKIP return 77</system-out></testcase>',
        )
        report = self._report(code=1)
        self.assertEqual(report["skipped"][0]["reason"], "  synthetic reason\nline two  ")
        self.assertEqual(report["counts"], {"Passed": 1, "Failed": 0, "Skipped": 1, "NotRun": 0})

    def test_missing_or_invalid_skip_elapsed_is_null_with_warning(self):
        for timing in ("", 'time="invalid"', 'time="NaN"', 'time="-1"'):
            with self.subTest(timing=timing):
                self._xml(f'<testcase name="required_case" status="run" {timing}><skipped/></testcase>')
                report = self._report(code=1)
                self.assertIsNone(report["skipped"][0]["elapsed_seconds"])
                self.assertTrue(any("required_case" in warning for warning in report["warnings"]))

    def test_markdown_sections_escape_reasons_and_end_with_switch_snapshot(self):
        self.junit.write_bytes((DATA / "mixed.xml").read_bytes())
        document = ET.parse(self.junit)
        document.find(".//skipped").attrib["message"] = "synthetic | reason\nsecond line"
        document.write(self.junit, encoding="utf-8", xml_declaration=True)
        self._requirements(self.required.read_text(encoding="utf-8") +
                           f"absent_inactive\tCHIPCAE_ENABLE_ECAD\tSynthetic inactive\n")
        self._report(code=1)
        markdown = (self.out / "skip-audit.md").read_text(encoding="utf-8")
        for section in (r"(?i)violation|违规", r"(?i)skip|跳过", r"(?i)not.?run|未运行",
                        r"(?i)warning|警告", r"(?i)inactive|非必跑|未启用",
                        r"(?i)missing|缺失", r"(?i)commit|提交"):
            self.assertRegex(markdown, section)
        self.assertIn(r"synthetic \| reason", markdown)
        self.assertNotIn("synthetic | reason\nsecond line", markdown)
        self.assertIn(self.source_commit, markdown)
        self.assertIn("absent_inactive", markdown)
        headings = re.findall(r"(?m)^#{1,6}\s+(.+)$", markdown)
        self.assertTrue(headings, markdown)
        self.assertRegex(headings[-1], r"(?i)switch|开关")
        self.assertIn(MPI, markdown)

    def test_overwrite_preserves_unrelated_file(self):
        self.out.mkdir(parents=True)
        sentinel = self.out / "keep.bin"
        sentinel.write_bytes(b"\x00synthetic sentinel\xff")
        for filename in ("skip-audit.json", "skip-audit.md"):
            (self.out / filename).write_text("stale content", encoding="utf-8")
        self._report()
        self.assertEqual(sentinel.read_bytes(), b"\x00synthetic sentinel\xff")
        self._xml('<testcase name="required_case" status="run" time="0.25"><skipped/></testcase>')
        report = self._report(code=1)
        self.assertEqual(report["counts"]["Passed"], 0)
        self.assertEqual(sentinel.read_bytes(), b"\x00synthetic sentinel\xff")
        self.assertEqual({path.name for path in self.out.iterdir()},
                         {"skip-audit.json", "skip-audit.md", "keep.bin"})

    def test_report_lists_are_sorted_by_exact_name(self):
        self._requirements("z_case\t-\tz\na_case\t-\ta\n")
        self._xml('<testcase name="z_case" status="run" time="0"><skipped/></testcase>',
                  '<testcase name="a_case" status="run" time="0"><skipped/></testcase>',
                  '<testcase name="Z_case" status="notrun"/>',
                  '<testcase name="A_case" status="notrun"/>')
        report = self._report(code=1)
        self.assertEqual([entry["name"] for entry in report["required_violations"]], ["a_case", "z_case"])
        self.assertEqual([entry["name"] for entry in report["notrun"]], ["A_case", "Z_case"])

    def test_repository_required_list_enforces_only_six_authorized_names_and_gates(self):
        self.required = ROOT / "tests" / "vv" / "skip_audit_required.tsv"
        names = ["field_compare_ranks", "f1130_uncut_cli_e2e", "chain_ipc_uncut_chain",
                 "odbcli_odbpp_cli_e2e", "fvm_fvm_case_e2e", "vv_result_schema"]
        self._xml(*(f'<testcase name="{name}" status="run" time="0"><skipped/></testcase>'
                    for name in [*names, "synthetic_extra"]))
        all_on = (f"{MPI}:BOOL=ON\n{HDF5}:BOOL=ON\n"
                  "CHIPCAE_ENABLE_ECAD:BOOL=ON\nCHIPCAE_ENABLE_MFEM:BOOL=ON\n")
        self._cache(all_on)
        report = self._report(code=1)
        self.assertEqual([entry["name"] for entry in report["required_violations"]], sorted(names))
        for switch in (MPI, HDF5, "CHIPCAE_ENABLE_ECAD", "CHIPCAE_ENABLE_MFEM"):
            with self.subTest(off=switch):
                self._cache(all_on.replace(f"{switch}:BOOL=ON", f"{switch}:BOOL=OFF"))
                report = self._report(code=1)
                expected = ["vv_result_schema"] if switch == MPI else ["field_compare_ranks", "vv_result_schema"]
                self.assertEqual([entry["name"] for entry in report["required_violations"]], expected)
                self.assertEqual(len(report["required_inactive"]), 6 - len(expected))

    def test_500_cases_finish_under_five_seconds_with_exact_counts(self):
        self._xml(*(f'<testcase name="synthetic_{index:03d}" status="run" time="0.001"/>'
                    for index in range(500)), root="testsuites")
        start = time.monotonic()
        report = self._report(required=False)
        elapsed = time.monotonic() - start
        self.assertLess(elapsed, 5.0, f"500-test audit took {elapsed:.3f} seconds")
        self.assertEqual(report["counts"], {"Passed": 500, "Failed": 0, "Skipped": 0, "NotRun": 0})
        self.assertEqual(sum(report["counts"].values()), 500)


if __name__ == "__main__":
    unittest.main()
