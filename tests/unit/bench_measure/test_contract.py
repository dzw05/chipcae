"""Synthetic acceptance data; never a measured benchmark artifact.

Test seam for the planned tool: main(argv, *, runner, clock) returns an int.
runner follows subprocess.run; clock follows time.monotonic. CTest registration
uses JSON, while execution uses CTest's JUnit report (show-only JSON contains
no execution statuses). Reports must be requested on each timed invocation.
"""
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "tools/bench_solve/measure.py"


def load_tool():
    if not TOOL.is_file():
        raise AssertionError(f"missing benchmark CLI: {TOOL}")
    spec = importlib.util.spec_from_file_location("bench_measure_under_test", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SyntheticRunner:
    """In-process subprocess.run double; no PATH changes or fake executables."""

    def __init__(self, names=None, outcomes=None, discovery_rc=0, malformed=False):
        self.names = ["solve_mesh", "solve_pipeline"] if names is None else names
        self.outcomes = iter(outcomes or [(0, "passed")])
        self.discovery_rc = discovery_rc
        self.malformed = malformed
        self.calls = []
        self.events = []

    def __call__(self, argv, **kwargs):
        argv = list(argv)
        self.calls.append((argv, kwargs))
        if "--show-only=json-v1" in argv:
            self.events.append("discovery")
            payload = {"kind": "ctestInfo", "version": {"major": 1, "minor": 0},
                       "tests": [{"name": name} for name in self.names]}
            stdout = "invalid synthetic JSON" if self.malformed else json.dumps(payload)
            return subprocess.CompletedProcess(argv, self.discovery_rc, stdout, "")
        self.events.append("run")
        outcome = next(self.outcomes)
        if isinstance(outcome, Exception):
            raise outcome
        rc, status = outcome
        # CTest stdout/stderr are inherited; result status comes from a report.
        report = Path(argv[argv.index("--output-junit") + 1])
        if status == "malformed":
            report.write_text("<testsuite>", encoding="utf-8")
            return subprocess.CompletedProcess(argv, rc, None, None)
        suite = ET.Element("testsuite", name="synthetic", tests="0" if status == "empty" else "1")
        if status != "empty":
            name = argv[argv.index("-R") + 1][1:-1]
            case = ET.SubElement(suite, "testcase", name=name, status="notrun" if status == "skipped" else "run")
            if status == "failed":
                ET.SubElement(case, "failure", message="synthetic failure")
            elif status == "skipped":
                ET.SubElement(case, "skipped", message="synthetic skip")
        ET.ElementTree(suite).write(report, encoding="utf-8", xml_declaration=True)
        return subprocess.CompletedProcess(argv, rc, None, None)


class Contract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = load_tool()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="benchmeasure-synthetic-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.build = self.base / "build with spaces"
        self.build.mkdir()
        self.out = self.base / "synthetic.json"

    def invoke(self, runner=None, ticks=(10.0, 10.25), extra=(), build=None):
        runner = runner if runner is not None else SyntheticRunner()
        times = iter(ticks)

        def clock():
            runner.events.append("clock")
            return next(times)

        argv = ["--build-dir", str(self.build) if build is None else build,
                "--test", "solve_mesh", "--out", str(self.out), *extra]
        rc = self.tool.main(argv, runner=runner, clock=clock)
        return rc, runner

    def payload(self):
        return json.loads(self.out.read_text(encoding="utf-8"))

    def check_payload(self, data, repeat, passed, kind="实测", test="solve_mesh"):
        self.assertEqual(set(data), {"schema_version", "kind", "test", "repeat", "runs", "all_passed", "error"})
        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(data["kind"], kind)
        self.assertEqual(data["test"], test)
        self.assertEqual(data["repeat"], repeat)
        self.assertIs(data["all_passed"], passed)
        self.assertIsInstance(data["error"], str)
        self.assertEqual(data["error"] == "", passed)
        for run in data["runs"]:
            self.assertEqual(set(run), {"index", "returncode", "status", "wall_seconds", "error"})
            self.assertIn(run["status"], {"passed", "failed", "skipped"})
            self.assertTrue(math.isfinite(run["wall_seconds"]))
            self.assertGreaterEqual(run["wall_seconds"], 0)
            self.assertIsInstance(run["error"], str)
            if run["status"] == "passed":
                self.assertEqual(run["error"], "")
            elif run["status"] == "failed":
                self.assertTrue(run["error"])

    def test_success_and_default_repeat(self):
        rc, runner = self.invoke()
        self.assertEqual(rc, 0)
        data = self.payload()
        self.check_payload(data, 1, True)
        self.assertEqual(data["runs"], [{"index": 1, "returncode": 0, "status": "passed",
                                         "wall_seconds": 0.25, "error": ""}])
        self.assertEqual(runner.events, ["discovery", "clock", "run", "clock"])

    def test_failure_and_skip(self):
        for outcome, expected in [((8, "failed"), "failed"), ((8, "passed"), "failed"),
                                  ((0, "failed"), "failed"), ((0, "skipped"), "skipped"),
                                  ((8, "skipped"), "skipped")]:
            with self.subTest(outcome=outcome):
                self.out.unlink(missing_ok=True)
                rc, _ = self.invoke(SyntheticRunner(outcomes=[outcome]))
                self.assertEqual(rc, 1)
                data = self.payload()
                self.check_payload(data, 1, False)
                self.assertEqual(len(data["runs"]), 1)
                self.assertEqual(data["runs"][0]["status"], expected)
                self.assertEqual(data["runs"][0]["returncode"], outcome[0])

    def test_unregistered_has_no_run_or_clock(self):
        rc, runner = self.invoke(SyntheticRunner(names=[]), ticks=())
        self.assertEqual(rc, 1)
        data = self.payload()
        self.assertEqual(data["kind"], "未测量")
        self.assertEqual(data["runs"], [])
        self.assertIs(data["all_passed"], False)
        self.assertEqual(data["error"], "test not registered")
        self.assertEqual(runner.events, ["discovery"])

    def test_discovery_errors_never_time_a_run(self):
        for runner in [SyntheticRunner(names=["solve_mesh", "solve_mesh"]),
                       SyntheticRunner(discovery_rc=1), SyntheticRunner(malformed=True)]:
            with self.subTest(runner=runner):
                self.out.unlink(missing_ok=True)
                rc, _ = self.invoke(runner, ticks=())
                self.assertEqual(rc, 1)
                data = self.payload()
                self.assertEqual(data["runs"], [])
                self.assertIs(data["all_passed"], False)
                self.assertTrue(data["error"])
                self.assertEqual(runner.events, ["discovery"])

    def test_each_repeat_has_its_own_time_and_index(self):
        rc, _ = self.invoke(SyntheticRunner(outcomes=[(8, "failed"), (0, "passed")]),
                            ticks=(1.0, 1.5, 8.0, 10.0), extra=("--repeat", "2"))
        self.assertEqual(rc, 1)
        data = self.payload()
        self.check_payload(data, 2, False)
        self.assertEqual([r["index"] for r in data["runs"]], [1, 2])
        self.assertEqual([r["wall_seconds"] for r in data["runs"]], [0.5, 2.0])
        self.assertEqual([r["status"] for r in data["runs"]], ["failed", "passed"])

    def test_three_repeats_allowed(self):
        rc, _ = self.invoke(SyntheticRunner(outcomes=[(0, "passed")] * 3),
                            ticks=(0, 1, 2, 3, 4, 5), extra=("--repeat", "3"))
        self.assertEqual(rc, 0)
        data = self.payload()
        self.check_payload(data, 3, True)
        self.assertEqual([r["index"] for r in data["runs"]], [1, 2, 3])

    def test_launch_exceptions_are_failed_measured_attempts(self):
        for error in [FileNotFoundError("synthetic missing ctest"), OSError("synthetic launch error")]:
            with self.subTest(error=error):
                self.out.unlink(missing_ok=True)
                rc, _ = self.invoke(SyntheticRunner(outcomes=[error]))
                self.assertEqual(rc, 1)
                data = self.payload()
                self.check_payload(data, 1, False)
                self.assertEqual(len(data["runs"]), 1)
                run = data["runs"][0]
                self.assertEqual(run["index"], 1)
                self.assertEqual(run["status"], "failed")
                self.assertIsNone(run["returncode"])
                self.assertEqual(run["wall_seconds"], 0.25)
                self.assertIn(type(error).__name__, run["error"])
                self.assertIn(str(error), run["error"])

    def test_malformed_report_errors_are_recorded(self):
        rc, runner = self.invoke(SyntheticRunner(outcomes=[(0, "malformed")]))
        self.assertEqual(rc, 1)
        data = self.payload()
        self.check_payload(data, 1, False)
        self.assertEqual(len(data["runs"]), 1)
        run = data["runs"][0]
        self.assertEqual(run["status"], "failed")
        self.assertEqual(run["returncode"], 0)
        self.assertIn("ParseError", run["error"])
        self.assertEqual(run["wall_seconds"], 0.25)
        self.assertEqual(runner.events, ["discovery", "clock", "run", "clock"])

    def test_empty_execution_results_cannot_pass(self):
        rc, _ = self.invoke(SyntheticRunner(outcomes=[(0, "empty")]))
        self.assertEqual(rc, 1)
        self.assertIs(self.payload()["all_passed"], False)
        self.assertTrue(self.payload()["error"])

    def test_invalid_arguments_do_not_launch(self):
        for extra in [("--test", "Solve_mesh"), ("--test", "unknown"),
                      ("--repeat", "0"), ("--repeat", "4"), ("--repeat", "bad")]:
            with self.subTest(extra=extra):
                runner = SyntheticRunner()
                try:
                    rc, _ = self.invoke(runner, ticks=(), extra=extra)
                except SystemExit as exc:
                    rc = exc.code
                self.assertEqual(rc, 2)
                self.assertEqual(runner.calls, [])
                self.assertFalse(self.out.exists())

    def test_empty_build_dir_is_unmeasured(self):
        for build in ("", "   "):
            with self.subTest(build=build):
                self.out.unlink(missing_ok=True)
                rc, runner = self.invoke(build=build, ticks=())
                self.assertEqual(rc, 1)
                self.assertEqual(runner.calls, [])
                self.assertEqual(runner.events, [])
                data = self.payload()
                self.check_payload(data, 1, False, kind="未测量")
                self.assertEqual(data["runs"], [])
                self.assertEqual(data["error"], "build directory must not be empty")

    def test_existing_output_is_preserved_without_launch(self):
        self.out.write_bytes(b"synthetic existing output\n")
        rc, runner = self.invoke(ticks=())
        self.assertEqual(rc, 2)
        self.assertEqual(runner.calls, [])
        self.assertEqual(self.out.read_bytes(), b"synthetic existing output\n")

    def test_output_creation_error(self):
        self.out = self.base / "missing parent" / "synthetic.json"
        rc, _ = self.invoke()
        self.assertEqual(rc, 2)
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
