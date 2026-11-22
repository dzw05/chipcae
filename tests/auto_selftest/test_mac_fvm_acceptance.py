"""Exercise the Mac workflow's selection and report gates without a Mac toolchain.

The shell recorder substitutes only the external CTest process. The workflow's
shell control flow, Python discovery and JUnit validation run unchanged in temp
directories. These checks cannot replace the required branch/main Mac runs.
"""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

import yaml


SOURCE = Path(__file__).resolve().parents[2]
FVM = (
    "fvm_fvm_case_e2e",
    "solve_fvm_thermal",
    "solve_fvm_thermal_cut",
    "solve_fvm_thermal_np2",
    "solve_fvm_elastic",
    "solve_fvm_bimetal",
    "solve_fvm_elastic_np2",
)
HIGHMEM = (
    "cutbim_cut_bimetal",
    "solve_column_gmg_fcbga",
    "solve_column_gmg_fcbga_scaling",
)
FVM_REGEX = "^(" + "|".join(FVM) + ")$"
REPORT = "build/junit-fvm-acceptance.xml"


def configured_test(name, command=None, labels=()):
    return {
        "name": name,
        "command": command or ["application"],
        "properties": [{"name": "LABELS", "value": list(labels)}],
    }


def junit(names=FVM):
    root = ET.Element("testsuites")
    suite = ET.SubElement(root, "testsuite")
    for index, name in enumerate(names):
        ET.SubElement(suite, "testcase", name=name, time=f"{index + 1}.25")
    return root


def selected(arguments, tests):
    """Apply the emitted CTest filters to independent synthetic inventory."""
    filters = {flag: arguments[arguments.index(flag) + 1]
               for flag in ("-R", "-E", "-L", "-LE") if flag in arguments}
    result = set()
    for test in tests:
        labels = next(prop["value"] for prop in test["properties"]
                      if prop["name"] == "LABELS")
        if "-R" in filters and not re.search(filters["-R"], test["name"]):
            continue
        if "-E" in filters and re.search(filters["-E"], test["name"]):
            continue
        if "-L" in filters and not any(re.search(filters["-L"], x) for x in labels):
            continue
        if "-LE" in filters and any(re.search(filters["-LE"], x) for x in labels):
            continue
        result.add(test["name"])
    return result


class MacFvmAcceptance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = (SOURCE / ".github/workflows/mac-highmem.yml").read_text()
        cls.workflow = yaml.safe_load(cls.text)
        cls.job = cls.workflow["jobs"]["build-test"]
        cls.steps = cls.job["steps"]

    def step(self, name):
        found = [step for step in self.steps if step.get("name") == name]
        self.assertEqual(len(found), 1, f"missing or duplicate workflow step: {name}")
        return found[0]

    def python(self, step):
        blocks = re.findall(r"python3\s+-\s+<<['\"]?(\w+)['\"]?\n(.*?)\n\1",
                            step["run"], re.S)
        self.assertEqual(len(blocks), 1, "expected one executable Python heredoc")
        return blocks[0][1]

    def shell(self, script, directory, environment=None, status=0):
        # Capture only the external process boundary, never substitute discovery
        # or assertions. No real solver/build/test executable is launched here.
        recorder = (
            "import json, os, sys; from pathlib import Path; "
            "p=Path(os.environ['RECORDER_FILE']); "
            "calls=json.loads(p.read_text()) if p.exists() else []; "
            "calls.append({'args':sys.argv[1:], 'cwd':os.getcwd()}); "
            "p.write_text(json.dumps(calls))"
        )
        prefix = (
            "record_ctest() {\n"
            f"{shlex.quote(sys.executable)} -c {shlex.quote(recorder)} \"$@\"\n"
            'return "$RECORDER_STATUS"\n}\n'
        )
        script = re.sub(r"\bctest(?=\s)", "record_ctest", script)
        record = directory / "calls.json"
        record.unlink(missing_ok=True)
        env = dict(os.environ, RUNNER_TEMP=str(directory),
                   GITHUB_ENV=str(directory / "environment"),
                   GITHUB_OUTPUT=str(directory / "outputs"),
                   RECORDER_FILE=str(record), RECORDER_STATUS=str(status))
        env.update(environment or {})
        result = subprocess.run(["bash", "-e", "-o", "pipefail", "-c", prefix + script],
                                cwd=directory, env=env, capture_output=True, text=True)
        calls = json.loads(record.read_text()) if record.exists() else []
        return result, calls

    def discover(self, tests, directory):
        (directory / "mac-highmem-tests.json").write_text(json.dumps({"tests": tests}))
        result = subprocess.run(
            [sys.executable, "-c", self.python(self.step("Discover test partitions"))],
            cwd=directory, env=dict(os.environ, RUNNER_TEMP=str(directory),
                                    GITHUB_ENV=str(directory / "environment")),
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return dict(line.split("=", 1) for line in
                    (directory / "environment").read_text().splitlines())

    def assert_partition(self, step, tests, directory, env, expected):
        result, calls = self.shell(self.step(step)["run"], directory, env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(calls), 1)
        self.assertEqual(selected(calls[0]["args"], tests), expected)

    def gate(self, directory, document):
        step = self.step("Assert FVM acceptance JUnit")
        path = directory / REPORT
        path.parent.mkdir(exist_ok=True)
        path.unlink(missing_ok=True)
        if document is not None:
            path.write_text(document if isinstance(document, str)
                            else ET.tostring(document, encoding="unicode"))
        return subprocess.run([sys.executable, "-c", self.python(step)],
                              cwd=directory, capture_output=True, text=True)

    def test_workflow_guards_and_existing_highmem_gate_are_preserved(self):
        events = self.workflow.get("on", self.workflow.get(True))
        self.assertEqual(set(events), {"workflow_dispatch", "schedule"})
        self.assertEqual(events["schedule"], [{"cron": "0 18 * * *"}])
        self.assertNotRegex(self.text, r"pull_request|secrets\.")
        self.assertEqual(self.workflow["permissions"], {"contents": "read"})
        self.assertEqual(self.workflow["concurrency"],
                         {"group": "mac-selfhosted", "cancel-in-progress": False})
        self.assertEqual(self.job["runs-on"], ["self-hosted", "macOS", "ARM64"])
        self.assertEqual(self.job["if"], "github.repository == 'dzw05/chipcae'")
        self.assertEqual(self.job["timeout-minutes"], 120)
        self.assertEqual(self.step("Assert highmem JUnit")["if"], "always()")
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "build").mkdir()
            path = directory / "build/junit-highmem.xml"
            for skipped in (False, True):
                document = junit(HIGHMEM)
                if skipped:
                    ET.SubElement(document.find(".//testcase"), "skipped")
                path.write_text(ET.tostring(document, encoding="unicode"))
                result = subprocess.run(
                    [sys.executable, "-c", self.python(self.step("Assert highmem JUnit"))],
                    cwd=directory, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, not skipped)

    def test_ecad_and_python_prerequisites(self):
        configure = shlex.split(self.step("Configure")["run"].replace("\\\n", " "))
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", configure)
        for dependency in ("ECAD", "JSONCONS", "MPI", "HDF5", "MFEM"):
            with self.subTest(dependency=dependency):
                self.assertIn(f"-DCHIPCAE_ENABLE_{dependency}=ON", configure)
        self.assertNotIn("-DCHIPCAE_ENABLE_OCCT=ON", configure)
        self.assertNotIn("-DCHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE=ON", configure)
        self.assertIn("mac_highmem_deps.sh --check", self.step("Check dependencies")["run"])
        checks = [step for step in self.steps if re.search(
            r'''python3\s+-c\s+["']import numpy, h5py["']''', step.get("run", ""))]
        self.assertEqual(len(checks), 1, "missing failing numpy/h5py prerequisite check")
        self.assertFalse(checks[0].get("continue-on-error", False))
        self.assertNotRegex(checks[0]["run"], r"\|\|\s*(true|:)")
        self.assertLess(self.steps.index(self.step("Activate toolchain")),
                        self.steps.index(checks[0]))
        self.assertLess(self.steps.index(checks[0]),
                        self.steps.index(self.step("Test highmem (a, mandatory)")))

    def test_mandatory_command_status_timing_and_order(self):
        step = self.step("Test FVM acceptance (mandatory)")
        self.assertFalse(step.get("continue-on-error", False))
        self.assertNotIn("timeout-minutes", step)
        self.assertNotIn("working-directory", step)
        self.assertLess(self.steps.index(self.step("Assert highmem JUnit")),
                        self.steps.index(step))
        self.assertLess(self.steps.index(step), self.steps.index(
            self.step("Test labeled MPI (b1, informational)")))
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            for status in (0, 8):
                with self.subTest(ctest_status=status):
                    result, calls = self.shell(step["run"], directory, status=status)
                    self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                    self.assertEqual(calls, [{"cwd": str(directory), "args": [
                        "--test-dir", "build", "-R", FVM_REGEX, "--no-tests=error",
                        "-j", "1", "--output-on-failure", "--output-junit",
                        str(directory / REPORT),
                    ]}])
                    self.assertRegex(result.stdout, r"(?i)fvm[^\n]*seconds:\s*\d+")

    def test_fvm_runs_once_and_other_partitions_keep_their_tests(self):
        launcher_name = "wrapper a.b+(x)[0]"
        tests = [configured_test(name, ["mpiexec", "-n", "2", "solver"]
                                 if name.endswith("_np2") else None) for name in FVM]
        tests += [configured_test(launcher_name, ["driver", "--launcher=/bin/mpirun"]),
                  configured_test("wrapper aXbxx0"),
                  configured_test("fvm_fvm_case_e2e_extra"),
                  configured_test("prefix_solve_fvm_thermal"),
                  configured_test("solve_fvm_elastic_concave"),
                  configured_test("solve_fvm_benchmark"),
                  configured_test("labeled", ["mpiexec", "-n", "4"], ["mpi"]),
                  configured_test(HIGHMEM[0], ["mpiexec", "-n", "9"], ["highmem"])]
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            env = self.discover(tests, directory)
            self.assertEqual(env["N"], "2", "highmem must remain outside rank audit")
            self.assertEqual({name for name in [test["name"] for test in tests]
                              if re.search(env["unlabeled_regex"], name)}, {launcher_name})
            self.assertTrue(env["unlabeled_regex"].startswith("^("))
            self.assertTrue(env["unlabeled_regex"].endswith(")$"))
            self.assert_partition("Test unlabeled MPI (b2, informational)", tests,
                                  directory, env, {launcher_name})
            self.assert_partition("Test labeled MPI (b1, informational)", tests,
                                  directory, env, {"labeled"})
            self.assert_partition("Test rest (c, informational)", tests, directory, env,
                                  {"wrapper aXbxx0", "fvm_fvm_case_e2e_extra",
                                   "prefix_solve_fvm_thermal", "solve_fvm_elastic_concave",
                                   "solve_fvm_benchmark"})

    def test_empty_b2_still_excludes_all_seven_from_c(self):
        tests = [configured_test(name) for name in FVM]
        tests.append(configured_test("unrelated"))
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            env = self.discover(tests, directory)
            self.assertEqual(env["unlabeled_regex"], "")
            self.assert_partition("Test rest (c, informational)", tests,
                                  directory, env, {"unrelated"})

    def test_rank_audit_includes_excluded_fvm_launchers(self):
        tests = [configured_test("solve_fvm_thermal_np2", ["mpiexec", "-n", "8"]),
                 configured_test("unrelated", ["mpirun", "--np=4"])]
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            env = self.discover(tests, directory)
            self.assertEqual(env["N"], "1")
            self.assertIsNone(re.search(env["unlabeled_regex"], "solve_fvm_thermal_np2"))
            self.assertIsNotNone(re.search(env["unlabeled_regex"], "unrelated"))

    def test_junit_accepts_exact_seven_and_prints_every_duration(self):
        step = self.step("Assert FVM acceptance JUnit")
        self.assertEqual(step["if"], "always()")
        self.assertFalse(step.get("continue-on-error", False))
        self.assertLess(self.steps.index(self.step("Test FVM acceptance (mandatory)")),
                        self.steps.index(step))
        with tempfile.TemporaryDirectory() as temp:
            result = self.gate(Path(temp), junit())
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for index, name in enumerate(FVM):
                self.assertRegex(result.stdout, re.escape(name) +
                                 rf"[^\n]*\b{index + 1}\.25\b")

    def test_junit_rejects_missing_malformed_empty_and_wrong_membership(self):
        self.step("Assert FVM acceptance JUnit")
        documents = {
            "missing file": None,
            "malformed XML": "<testsuites>",
            "empty report": junit(()),
            "missing case": junit(FVM[:-1]),
            "extra case": junit((*FVM, "unrelated")),
            "duplicate with all names": junit((*FVM, FVM[0])),
            "duplicate replacing missing name": junit((*FVM[:-1], FVM[0])),
            "case sensitive": junit((FVM[0].upper(), *FVM[1:])),
        }
        with tempfile.TemporaryDirectory() as temp:
            for name, document in documents.items():
                with self.subTest(report=name):
                    result = self.gate(Path(temp), document)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_junit_rejects_skipped_failure_and_error_anywhere_in_xml(self):
        self.step("Assert FVM acceptance JUnit")
        with tempfile.TemporaryDirectory() as temp:
            for tag in ("skipped", "failure", "error"):
                for placement in ("testcase", "testsuite", "testsuites"):
                    with self.subTest(tag=tag, placement=placement):
                        document = junit()
                        parent = document if placement == "testsuites" else document.find(
                            ".//" + placement)
                        ET.SubElement(parent, tag)
                        result = self.gate(Path(temp), document)
                        self.assertNotEqual(result.returncode, 0,
                                            result.stdout + result.stderr)

    def test_stale_report_is_removed(self):
        cleanup = self.step("Start load sampler")["run"].replace("\\\n", " ")
        commands = [line.strip() for line in cleanup.splitlines()
                    if line.strip().startswith("rm -f ")]
        self.assertEqual(len(commands), 1)
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            path = directory / REPORT
            path.parent.mkdir()
            path.write_text("stale success")
            result = subprocess.run(["bash", "-e", "-c", commands[0]], cwd=directory,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(path.exists(), "stale FVM success must not survive checkout")

    def test_artifact_is_always_uploaded(self):
        upload = self.step("Upload test results")
        self.assertEqual(upload["if"], "always()")
        self.assertIn(REPORT, upload["with"]["path"].splitlines())

    def test_existing_mandatory_load_limit_remains_twelve(self):
        step = self.step("Stop sampler and assert load")
        self.assertEqual(step["if"], "always()")
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "build").mkdir()
            for load, passed in (("12", True), ("12.01", False)):
                with self.subTest(one_minute_load=load):
                    (directory / "build/loadavg.log").write_text(
                        "99 { 99 99 99 }\nMARK highmem-mandatory-begin 100\n"
                        f"101 {{ {load} 20 20 }}\nMARK highmem-mandatory-end 102\n"
                    )
                    result = subprocess.run([sys.executable, "-c", self.python(step)],
                                            cwd=directory, capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, passed,
                                     result.stdout + result.stderr)


if __name__ == "__main__":
    # Auto-collection passes the source directory as argv[1], not a unittest name.
    unittest.main(argv=[sys.argv[0]], verbosity=2)
