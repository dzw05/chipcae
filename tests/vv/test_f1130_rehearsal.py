# SPDX-License-Identifier: Apache-2.0
"""Acceptance contracts for the F-1130 rehearsal CLI (synthetic data only)."""

import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

from tools.vv import f1130_rehearsal


NAMES = (
    "f1130_uncut_cli_e2e",
    "f1130_uncut_cli_records",
    "chain_ipc_uncut_chain",
    "odbcli_odbpp_cli_e2e",
)
KEYS = (
    "CHIPCAE_ENABLE_ECAD",
    "CHIPCAE_ENABLE_MFEM",
    "CHIPCAE_ENABLE_MPI",
    "CHIPCAE_ENABLE_HDF5",
)
COMMIT = "1234567890abcdef1234567890abcdef12345678"
REQUIRED = {
    "commit", "build_dir", "backends", "mpiexec", "ranks",
    "allow_missing_backend", "preflight_errors", "ctest_listing",
    "ctest_listing_returncode", "ctest_command", "ctest_returncode",
    "tests", "wall_time_s", "data_class", "overall",
}


def junit(names=NAMES, *, status="run", tag=None, message="", output=""):
    root = ET.Element("testsuite", name="CTest")
    for index, name in enumerate(names):
        case = ET.SubElement(root, "testcase", name=name, time="0.125")
        if status is not None:
            case.set("status", status)
        if index == 0 and tag:
            diagnostic = ET.SubElement(case, tag, message=message, type="synthetic-type")
            diagnostic.text = "original diagnostic body"
        if index == 0 and output:
            ET.SubElement(case, "system-out").text = output
            ET.SubElement(case, "system-err").text = "original stderr"
    return ET.tostring(root, encoding="unicode")


class RehearsalAcceptance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build with spaces"
        self.build.mkdir()
        self.out = self.root / "reports with spaces"
        self.cache = dict.fromkeys(KEYS, "ON")
        self.write_cache()
        self.mpi = str(self.root / "mpi bin" / "mpiexec")
        self.listing = "\n".join(
            f"  Test #{index}: {name}" for index, name in enumerate(NAMES, 1)
        ) + "\n"
        self.listing_stderr = "synthetic discovery stderr\n"
        self.listing_rc = 0
        self.execution_rc = 0
        self.execution_stdout = "All tests Skipped"
        self.xml = junit()
        self.calls = []
        self.stdout = io.StringIO()
        self.stderr = io.StringIO()
        self.which_calls = []

    def write_cache(self):
        (self.build / "CMakeCache.txt").write_text(
            "\n".join(f"{key}:BOOL={value}" for key, value in self.cache.items())
            + "\nCHIPCAE_ENABLE_UNRELATED:BOOL=OFF\n",
            encoding="utf-8",
        )

    def fake_which(self, name):
        self.which_calls.append(name)
        # An available mpirun must never substitute for missing mpiexec.
        return self.mpi if name == "mpiexec" else str(self.root / "mpirun")

    def fake_run(self, args, **kwargs):
        args = [str(arg) for arg in args]
        self.calls.append((args, kwargs))
        self.assertFalse(kwargs.get("shell", False))
        if args == ["git", "rev-parse", "HEAD"]:
            self.assertEqual(
                Path(kwargs["cwd"]).resolve(), Path(__file__).resolve().parents[2]
            )
            return subprocess.CompletedProcess(args, 0, COMMIT + "\n", "")
        self.assertEqual(args[0], "ctest", f"Unexpected subprocess: {args}")
        if "-N" in args:
            self.assertEqual(args, ["ctest", "--test-dir", str(self.build), "-N"])
            return subprocess.CompletedProcess(
                args, self.listing_rc, self.listing, self.listing_stderr
            )
        self.assertEqual(args, self.execution_command())
        # The driver must remove its old result before invoking CTest.
        self.assertFalse((self.out / "junit.xml").exists(), "stale JUnit was not removed")
        if self.xml is not None:
            (self.out / "junit.xml").write_text(self.xml, encoding="utf-8")
        # Console output deliberately contradicts XML and must not set verdicts.
        return subprocess.CompletedProcess(args, self.execution_rc, self.execution_stdout, "")

    def execution_command(self):
        return [
            "ctest", "--test-dir", str(self.build), "-R",
            "^(f1130_uncut_cli_e2e|f1130_uncut_cli_records|chain_ipc_uncut_chain|odbcli_odbpp_cli_e2e)$",
            "-j2", "--output-on-failure", "--output-junit", str(self.out / "junit.xml"),
        ]

    def invoke(self, expected, extra=(), build=None):
        for stream in (self.stdout, self.stderr):
            stream.seek(0)
            stream.truncate()
        args = ["--build-dir", str(self.build) if build is None else build,
                "--out", str(self.out), *extra]
        with patch("subprocess.run", side_effect=self.fake_run), \
                patch("shutil.which", side_effect=self.fake_which), \
                patch("time.monotonic", side_effect=[100.0, 103.25]), \
                contextlib.redirect_stdout(self.stdout), \
                contextlib.redirect_stderr(self.stderr):
            try:
                result = f1130_rehearsal.main(args)
            except SystemExit as exc:
                result = exc.code
        self.assertEqual(result, expected, "main() must return the specified rehearsal exit code")

    def report(self):
        report = json.loads((self.out / "rehearsal.json").read_text(encoding="utf-8"))
        self.assertTrue(REQUIRED <= report.keys(), REQUIRED - report.keys())
        self.assertEqual(report["data_class"], "synthetic")
        self.assertEqual([case["name"] for case in report["tests"]], list(NAMES))
        for case in report["tests"]:
            self.assertTrue({"name", "status", "time_s", "skip_or_fail_message"} <= case.keys())
        return report

    def ctest_calls(self):
        return [args for args, _ in self.calls if args[0] == "ctest"]

    def diagnostics(self):
        return self.stdout.getvalue() + self.stderr.getvalue()

    def test_four_run_results_pass_and_report_full_provenance(self):
        self.invoke(0)
        report = self.report()
        self.assertEqual(report["overall"], "PASS")
        self.assertEqual([c["status"] for c in report["tests"]], ["Passed"] * 4)
        self.assertEqual(report["commit"], COMMIT)
        self.assertEqual(report["build_dir"], str(self.build.resolve()))
        self.assertEqual(report["backends"], self.cache)
        self.assertEqual(report["mpiexec"], self.mpi)
        self.assertEqual(self.which_calls, ["mpiexec"])
        self.assertEqual(report["ranks"], [1, 2])
        self.assertFalse(report["allow_missing_backend"])
        self.assertEqual(report["preflight_errors"], [])
        self.assertIn(self.listing.strip(), report["ctest_listing"])
        self.assertIn(self.listing_stderr.strip(), report["ctest_listing"])
        self.assertEqual(report["ctest_listing_returncode"], 0)
        self.assertEqual(report["ctest_command"], self.execution_command())
        self.assertEqual(report["ctest_returncode"], 0)
        self.assertAlmostEqual(report["wall_time_s"], 3.25)
        for case in report["tests"]:
            self.assertAlmostEqual(case["time_s"], 0.125)
        markdown = (self.out / "rehearsal.md").read_text(encoding="utf-8")
        # Table rows, rather than names in the command, establish display order.
        rows = [line for line in markdown.splitlines()
                if line.lstrip().startswith("|") and any(name in line for name in NAMES)]
        self.assertEqual(len(rows), 4)
        for row, name in zip(rows, NAMES):
            self.assertIn(name, row)
        for value in (COMMIT, str(self.build), self.mpi, "synthetic", "PASS", "--output-junit",
                      "-j2", self.listing.strip(), self.listing_stderr.strip()):
            self.assertIn(value, markdown)

    def test_skipped_is_failure_even_when_ctest_succeeds_and_keeps_diagnostics(self):
        message = "依赖缺失 | original skip\nsecond line"
        self.xml = junit(tag="skipped", message=message, output="original skip output")
        self.execution_stdout = "100% tests passed"
        self.invoke(1)
        report = self.report()
        self.assertEqual(report["overall"], "FAIL")
        self.assertEqual(report["tests"][0]["status"], "Skipped")
        detail = report["tests"][0]["skip_or_fail_message"]
        for text in (message, "original diagnostic body", "synthetic-type",
                     "original skip output", "original stderr"):
            self.assertIn(text, detail)
        markdown = (self.out / "rehearsal.md").read_text(encoding="utf-8")
        self.assertIn("```", markdown)
        self.assertIn(message, markdown)
        self.assertIn("original skip output", markdown)
        self.assertIn("\\|", markdown)

    def test_failure_and_error_never_pass(self):
        for tag in ("failure", "error"):
            with self.subTest(tag=tag):
                self.xml = junit(tag=tag, message="original failure", output="solver output")
                self.invoke(1)
                report = self.report()
                self.assertEqual(report["overall"], "FAIL")
                self.assertEqual(report["tests"][0]["status"], "Failed")
                for text in ("original failure", "original diagnostic body", "synthetic-type",
                             "solver output", "original stderr"):
                    self.assertIn(text, report["tests"][0]["skip_or_fail_message"])

    def test_non_run_or_absent_status_never_passes(self):
        for status in ("notrun", "disabled", "", None):
            with self.subTest(status=status):
                self.xml = junit(status=status)
                self.invoke(1)
                self.assertEqual(self.report()["overall"], "FAIL")
                self.assertEqual(self.report()["tests"][0]["status"], "Not Run")

    def test_each_missing_or_off_backend_blocks_all_ctest_calls(self):
        for key in KEYS:
            for value in (None, "OFF"):
                with self.subTest(key=key, value=value):
                    self.cache = dict.fromkeys(KEYS, "ON")
                    if value is None:
                        del self.cache[key]
                    else:
                        self.cache[key] = value
                    self.write_cache()
                    self.invoke(2)
                    self.assertEqual(self.ctest_calls(), [])
                    self.assertIn(key, self.diagnostics())
                    report = self.report()
                    self.assertTrue(any(key in error for error in report["preflight_errors"]))
                    self.assertEqual(report["overall"], "NOT_RUN")
                    for case in report["tests"]:
                        self.assertEqual(case["status"], "Not Run")
                        self.assertIsNone(case["time_s"])

    def test_cmake_boolean_forms_preserve_raw_values(self):
        for values in (("YES", "TRUE", "Y", "1"), ("on", "true", "yes", "2")):
            with self.subTest(values=values):
                self.cache = dict(zip(KEYS, values))
                self.write_cache()
                self.invoke(0)
                self.assertEqual(self.report()["backends"], self.cache)
        for value in ("NO", "FALSE", "N", "0", "IGNORE", "NOTFOUND", "x-NOTFOUND", ""):
            with self.subTest(value=value):
                self.calls.clear()
                self.cache[KEYS[0]] = value
                self.write_cache()
                self.invoke(2)
                self.assertEqual(self.ctest_calls(), [])

    def test_missing_mpiexec_does_not_fall_back_to_mpirun(self):
        self.mpi = None
        self.invoke(2)
        self.assertEqual(self.ctest_calls(), [])
        self.assertEqual(self.which_calls, ["mpiexec"])
        self.assertIn("mpiexec", self.diagnostics())
        self.assertIsNone(self.report()["mpiexec"])

    def test_reports_every_missing_prerequisite(self):
        self.cache = {}
        self.write_cache()
        self.mpi = None
        self.invoke(2)
        self.assertEqual(self.ctest_calls(), [])
        for text in (*KEYS, "mpiexec"):
            self.assertIn(text, self.diagnostics())

    def test_allow_missing_runs_but_never_passes(self):
        self.cache[KEYS[0]] = "OFF"
        self.write_cache()
        self.mpi = None
        self.invoke(1, ["--allow-missing-backend"])
        report = self.report()
        self.assertEqual(len(self.ctest_calls()), 2)
        self.assertTrue(report["allow_missing_backend"])
        self.assertEqual(report["overall"], "FAIL")
        self.assertIn("Skipped", [case["status"] for case in report["tests"]])
        for text in (KEYS[0], "mpiexec"):
            self.assertIn(text, json.dumps(report["preflight_errors"], ensure_ascii=False))

    def test_missing_discovery_name_prevents_execution(self):
        self.listing = self.listing.replace(NAMES[0], "unrelated")
        self.invoke(2)
        self.assertEqual(len(self.ctest_calls()), 1)
        self.assertIn("-N", self.ctest_calls()[0])
        self.assertIn(NAMES[0], self.diagnostics())
        self.assertEqual(self.report()["overall"], "NOT_RUN")

    def test_readiness_override_retains_raw_junit_status_and_failure(self):
        self.cache[KEYS[0]] = "OFF"
        self.write_cache()
        self.xml = junit(tag="failure", message="original failure")
        self.invoke(1, ["--allow-missing-backend"])
        report = self.report()
        self.assertEqual([case["status"] for case in report["tests"]],
                         ["Failed", "Passed", "Skipped", "Skipped"])
        for case in report["tests"]:
            self.assertEqual(case["junit_status"], "run")
        self.assertIn("original failure", report["tests"][0]["skip_or_fail_message"])
        self.assertIn(KEYS[0], report["tests"][2]["skip_or_fail_message"])

    def test_allow_missing_mpi_records_global_gap_without_test_mapping(self):
        self.mpi = None
        self.invoke(1, ["--allow-missing-backend"])
        report = self.report()
        self.assertEqual(report["overall"], "FAIL")
        self.assertEqual([case["status"] for case in report["tests"]], ["Passed"] * 4)
        self.assertEqual(report["readiness_gaps"][0]["status"], "Skipped")
        self.assertIn("mpiexec", report["readiness_gaps"][0]["reason"])

    def test_discovery_requires_exact_case_sensitive_registration_lines(self):
        for replacement in (NAMES[0] + "_suffix", NAMES[0].upper(), "other"):
            with self.subTest(replacement=replacement):
                self.calls.clear()
                self.listing = "\n".join(
                    f"  Test #{i}: {replacement if i == 1 else name}"
                    for i, name in enumerate(NAMES, 1)
                ) + f"\nWarning: {NAMES[0]}\n"
                self.invoke(2)
                self.assertEqual(len(self.ctest_calls()), 1)
                self.assertIn(NAMES[0], self.diagnostics())

    def test_discovery_nonzero_prevents_execution_and_retains_output(self):
        self.listing_rc = 8
        self.invoke(2)
        self.assertEqual(len(self.ctest_calls()), 1)
        report = self.report()
        self.assertEqual(report["ctest_listing_returncode"], 8)
        self.assertIn(self.listing_stderr.strip(), report["ctest_listing"])

    def test_missing_empty_or_malformed_junit_is_not_run(self):
        for xml in (None, "", "<testsuite>"):
            with self.subTest(xml=xml):
                self.xml = xml
                self.execution_rc = 9
                self.invoke(1)
                report = self.report()
                self.assertEqual(report["overall"], "NOT_RUN")
                self.assertEqual(report["ctest_returncode"], 9)
                self.assertTrue(all(c["status"] == "Not Run" for c in report["tests"]))

    def test_missing_duplicate_and_inexact_junit_names_are_diagnosed(self):
        for names in ((), NAMES[1:], (*NAMES, NAMES[0]),
                      (NAMES[0] + "_suffix", *NAMES[1:]),
                      (NAMES[0].upper(), *NAMES[1:])):
            with self.subTest(names=names):
                self.xml = junit(names)
                self.invoke(1)
                report = self.report()
                self.assertNotEqual(report["overall"], "PASS")
                self.assertNotEqual(report["tests"][0]["status"], "Passed")
                self.assertIn(NAMES[0], report["tests"][0]["skip_or_fail_message"])

    def test_xml_order_does_not_change_manifest_order(self):
        self.xml = junit(tuple(reversed(NAMES)))
        self.invoke(0)
        self.report()

    def test_nonzero_ctest_with_passing_xml_is_failure(self):
        self.execution_rc = 8
        self.invoke(1)
        report = self.report()
        self.assertEqual(report["overall"], "FAIL")
        self.assertEqual(report["ctest_returncode"], 8)
        self.assertEqual([c["status"] for c in report["tests"]], ["Passed"] * 4)

    def test_invalid_ranks_are_parameter_errors_without_ctest(self):
        for ranks in ("3", "", ",", "1,3", "2,no", "1,", ",2"):
            with self.subTest(ranks=ranks):
                self.calls.clear()
                self.invoke(2, ["--ranks", ranks])
                self.assertEqual(self.ctest_calls(), [])

    def test_valid_ranks_only_change_metadata(self):
        for ranks, expected in (("1", [1]), ("2", [2]), ("1,2", [1, 2])):
            with self.subTest(ranks=ranks):
                self.calls.clear()
                self.invoke(0, ["--ranks", ranks])
                self.assertEqual(self.report()["ranks"], expected)
                self.assertEqual(self.ctest_calls()[-1], self.execution_command())

    def test_empty_nonexistent_and_file_build_paths_rejected(self):
        file_path = self.root / "file"
        file_path.write_text("not a directory", encoding="utf-8")
        for build in ("", str(self.root / "absent"), str(file_path)):
            with self.subTest(build=build):
                self.invoke(2, build=build)
                self.assertEqual(self.ctest_calls(), [])

    def test_missing_cache_rejected_with_diagnostic_report(self):
        (self.build / "CMakeCache.txt").unlink()
        self.invoke(2)
        self.assertEqual(self.ctest_calls(), [])
        report = self.report()
        self.assertEqual(report["overall"], "NOT_RUN")
        self.assertIn("CMakeCache.txt", json.dumps(report["preflight_errors"]))

    def test_empty_output_argument_rejected(self):
        with patch("subprocess.run", side_effect=self.fake_run):
            try:
                result = f1130_rehearsal.main(["--build-dir", str(self.build), "--out", ""])
            except SystemExit as exc:
                result = exc.code
        self.assertEqual(result, 2)
        self.assertEqual(self.ctest_calls(), [])

    def test_unreadable_cache_rejected_with_diagnostic_report(self):
        original_open = open
        cache_path = self.build / "CMakeCache.txt"

        def open_with_unreadable_cache(file, *args, **kwargs):
            if not isinstance(file, int) and Path(file) == cache_path:
                raise PermissionError("synthetic unreadable CMakeCache.txt")
            return original_open(file, *args, **kwargs)

        # Both built-in open() and pathlib's io.open() boundary see the same denial,
        # even when these tests run with privileges that bypass chmod permissions.
        with patch("builtins.open", side_effect=open_with_unreadable_cache), \
                patch("io.open", side_effect=open_with_unreadable_cache):
            self.invoke(2)
        self.assertEqual(self.ctest_calls(), [])
        report = self.report()
        self.assertEqual(report["overall"], "NOT_RUN")
        self.assertIn("CMakeCache.txt", json.dumps(report["preflight_errors"]))

    def test_existing_output_preserves_sentinel_and_cannot_reuse_stale_xml(self):
        self.out.mkdir()
        sentinel = self.out / "unrelated.bin"
        sentinel.write_bytes(b"\x00\xffkeep\n")
        (self.out / "junit.xml").write_text(junit(), encoding="utf-8")
        (self.out / "rehearsal.json").write_text("stale", encoding="utf-8")
        (self.out / "rehearsal.md").write_text("stale", encoding="utf-8")
        self.xml = None
        self.invoke(1)
        self.assertEqual(self.report()["overall"], "NOT_RUN")
        self.assertEqual(sentinel.read_bytes(), b"\x00\xffkeep\n")
        self.assertNotIn("stale", (self.out / "rehearsal.md").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
