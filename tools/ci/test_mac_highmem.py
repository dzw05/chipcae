"""Mac runner acceptance contracts; run: python3 -m unittest tools.ci.test_mac_highmem -v.

This test-first suite runs the future workflow's own shell/Python blocks and
installer. Synthetic homes, inventories and JUnit files live in temporary dirs.
CTest is replaced only at the workflow process boundary: it supplies inventory
and records invocations, without launching MPI or touching a real Mac runner.
No implementation of selection, JUnit validation or path validation lives here.
"""

from __future__ import annotations

import itertools
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/mac-highmem.yml"
INSTALLER = ROOT / ".github/scripts/mac_highmem_deps.sh"
REQUIRED = (
    "cutbim_cut_bimetal",
    "solve_column_gmg_fcbga",
    "solve_column_gmg_fcbga_scaling",
)
REPORTS = (
    "junit-highmem.xml", "junit-mpi.xml", "junit-mpi-unlabeled.xml", "junit-rest.xml",
)


def write(path, text="", executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if executable:
        path.chmod(0o755)


def testcase(name, labels=(), command=None):
    return {
        "name": name,
        "command": command or ["fixture-test"],
        "properties": [{"name": "LABELS", "value": list(labels)}],
    }


def junit(names=REQUIRED, child=None):
    suite = ET.Element("testsuite", tests=str(len(names)))
    for i, name in enumerate(names):
        case = ET.SubElement(suite, "testcase", name=name, time=str(1 + i))
        if child and i == 0:
            ET.SubElement(case, child)
    return ET.tostring(suite, encoding="unicode")


# A process-boundary recorder, deliberately not a second test selector. Tests
# below assert the emitted CTest arguments against literal expected selections.
CTEST_RECORDER = r'''
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ["CALL_LOG"], "a") as stream:
    stream.write(json.dumps(args) + "\n")
if "--show-only=json-v1" in args or "--show-only" in args:
    print(Path(os.environ["INVENTORY"]).read_text())
else:
    for index, arg in enumerate(args):
        if arg == "--output-junit":
            report = Path(args[index + 1])
            if not report.is_absolute():
                report = Path("build") / report
            report.parent.mkdir(parents=True, exist_ok=True)
            report.write_text(Path(os.environ["JUNIT_FIXTURE"]).read_text())
    sys.exit(int(os.environ.get("CTEST_STATUS", "0")))
'''


class WorkflowAcceptance(unittest.TestCase):
    def workflow(self):
        self.assertTrue(
            WORKFLOW.is_file(), "missing Mac highmem workflow (feature not implemented)",
        )
        text = WORKFLOW.read_text(encoding="utf-8")
        workflow = yaml.safe_load(text)
        self.assertIsInstance(workflow, dict)
        # PyYAML's YAML 1.1 resolver calls the Actions key `on` True.
        self.assertEqual(workflow["name"], "Mac highmem")
        jobs = list(workflow["jobs"].values())
        self.assertEqual(len(jobs), 1, "the sampler and all test partitions share one job")
        return workflow, jobs[0], text

    def find_step(self, predicate):
        _, job, _ = self.workflow()
        matches = [step for step in job["steps"] if predicate(step.get("run", ""))]
        self.assertEqual(len(matches), 1, "expected one workflow step for this acceptance boundary")
        return matches[0]

    def test_public_runner_isolation_and_resource_limits(self):
        workflow, job, text = self.workflow()
        triggers = workflow.get("on", workflow.get(True))
        self.assertEqual(set(triggers), {"workflow_dispatch", "schedule"})
        self.assertEqual(len(triggers["schedule"]), 1)
        cron = triggers["schedule"][0]["cron"].split()
        self.assertEqual(len(cron), 5)
        self.assertIn(int(cron[0]), range(60))
        self.assertIn(int(cron[1]), range(24))
        self.assertEqual(cron[2:], ["*", "*", "*"], "run once per day")
        self.assertNotRegex(text, r"pull_request|secrets\.")
        self.assertEqual(workflow["permissions"], {"contents": "read"})
        self.assertEqual(job["runs-on"], ["self-hosted", "macOS", "ARM64"])
        self.assertIn("github.repository == 'dzw05/chipcae'", job["if"])
        self.assertGreater(job["timeout-minutes"], 0)
        self.assertLessEqual(job["timeout-minutes"], 120)
        self.assertEqual(job.get("concurrency", workflow.get("concurrency")), {
            "group": "mac-highmem", "cancel-in-progress": False,
        })
        env = {**workflow.get("env", {}), **job.get("env", {})}
        for key, value in {
            "CMAKE_BUILD_PARALLEL_LEVEL": "6", "CTEST_PARALLEL_LEVEL": "6",
            "OMP_NUM_THREADS": "1", "OMP_PROC_BIND": "false",
        }.items():
            self.assertEqual(env[key], value, key)
        steps = job["steps"]
        checkout = next(
            i for i, s in enumerate(steps)
            if s.get("uses", "").startswith("actions/checkout@")
        )
        sampler = steps[checkout + 1].get("run", "")
        self.assertIn("vm.loadavg", sampler)
        self.assertRegex(sampler, r"sleep\s+10\b")
        self.assertIn("RUNNER_TEMP", sampler)
        artifacts = [s for s in steps if s.get("uses", "").startswith("actions/upload-artifact@v4")]
        self.assertEqual(len(artifacts), 1)
        self.assertIn("always()", artifacts[0]["if"])
        self.assertEqual(artifacts[0]["with"]["retention-days"], 7)
        for name in (*REPORTS, "loadavg.log"):
            self.assertIn("build/" + name, artifacts[0]["with"]["path"])
        check_index = next(
            i for i, s in enumerate(steps) if "mac_highmem_deps.sh" in s.get("run", "")
        )
        configure_index = next(
            i for i, s in enumerate(steps) if "CMAKE_BUILD_TYPE" in s.get("run", "")
        )
        self.assertLess(check_index, configure_index)
        self.assertIn("--check", steps[check_index]["run"])
        self.assertNotIn("--install", "\n".join(s.get("run", "") for s in steps))

    def test_highmem_is_mandatory_and_other_partitions_are_independent(self):
        _, job, _ = self.workflow()
        for report in REPORTS:
            with self.subTest(report=report):
                step = self.execution_step(report)
                self.assertEqual(bool(step.get("continue-on-error", False)), report != REPORTS[0])
        highmem = self.execution_step(REPORTS[0])
        assertion = self.junit_step()
        self.assertFalse(assertion.get("continue-on-error", False))
        self.assertIn("always()", assertion.get("if", ""))
        self.assertGreater(job["steps"].index(assertion), job["steps"].index(highmem))

    def junit_step(self):
        return self.find_step(lambda run: "ElementTree" in run and REPORTS[0] in run)

    def execution_step(self, report):
        return self.find_step(
            lambda run: "ctest" in run and "--output-junit" in run and report in run
        )

    def test_workflow_shell_blocks_parse(self):
        _, job, _ = self.workflow()
        for step in job["steps"]:
            if "run" in step:
                with self.subTest(step=step.get("name", step.get("id"))):
                    result = subprocess.run(
                        ["bash", "-n"], input=step["run"],
                        text=True, capture_output=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
        if shutil.which("actionlint"):
            result = subprocess.run(
                ["actionlint", str(WORKFLOW)], cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def run_blocks(self, scripts, directory, inventory=(), xml=None, status=0):
        """Run shipped code, preserving Actions env/output files between steps."""
        work = Path(directory)
        (work / "build").mkdir(exist_ok=True)
        env = os.environ.copy()
        env.update({
            "HOME": str(work / "home"), "RUNNER_TEMP": str(work / "runner-temp"),
            "GITHUB_WORKSPACE": str(work), "GITHUB_ENV": str(work / "github-env"),
            "GITHUB_OUTPUT": str(work / "github-output"), "CALL_LOG": str(work / "calls.jsonl"),
            "INVENTORY": str(work / "inventory.json"),
            "JUNIT_FIXTURE": str(work / "junit-fixture.xml"),
            "CTEST_STATUS": str(status),
        })
        Path(env["RUNNER_TEMP"]).mkdir(exist_ok=True)
        write(Path(env["INVENTORY"]), json.dumps({
            "kind": "ctestInfo", "version": {"major": 1, "minor": 0}, "tests": list(inventory),
        }))
        write(Path(env["JUNIT_FIXTURE"]), junit() if xml is None else xml)
        if xml is not None:
            write(work / "build/junit-highmem.xml", xml)
        recorder = work / "ctest-recorder.py"
        write(recorder, CTEST_RECORDER)
        workflow, job, _ = self.workflow()
        env.update({
            str(k): str(v)
            for k, v in {**workflow.get("env", {}), **job.get("env", {})}.items()
        })
        results, outputs = [], {}
        for step in scripts:
            run = step["run"]
            for ref, value in outputs.items():
                run = re.sub(r"\$\{\{\s*" + re.escape(ref) + r"\s*\}\}", lambda _: value, run)
            write(Path(env["GITHUB_OUTPUT"]))
            prefix = f'ctest() {{ python3 {shlex.quote(str(recorder))} "$@"; }}\n'
            result = subprocess.run(
                ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c", prefix + run],
                cwd=work, env=env, text=True, capture_output=True, timeout=20,
            )
            results.append(result)
            # These fixtures use single-line values (names/regex/rank count).
            for path, target in (
                (Path(env["GITHUB_ENV"]), env), (Path(env["GITHUB_OUTPUT"]), outputs),
            ):
                if path.exists():
                    for line in path.read_text().splitlines():
                        if "=" in line:
                            key, value = line.split("=", 1)
                            ref = key
                            if target is outputs:
                                ref = f"steps.{step.get('id', '')}.outputs.{key}"
                            target[ref] = value
            if result.returncode and not step.get("continue-on-error"):
                break
        log = Path(env["CALL_LOG"])
        calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return results, calls

    def test_junit_rejects_empty_incomplete_skipped_failed_error_and_malformed_reports(self):
        step = self.junit_step()
        invalid = [junit(()), junit(REQUIRED[:2]), junit(("other", *REQUIRED[1:])), "<testsuite>"]
        invalid += [junit(child=child) for child in ("skipped", "failure", "error")]
        for xml in invalid:
            with self.subTest(xml=xml), tempfile.TemporaryDirectory() as directory:
                results, _ = self.run_blocks([step], directory, xml=xml)
                self.assertNotEqual(results[-1].returncode, 0, results[-1].stdout)

    def test_junit_accepts_extra_highmem_cases_and_logs_each_duration(self):
        step = self.junit_step()
        for names in (REQUIRED, (*REQUIRED, "extra-highmem")):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as directory:
                results, _ = self.run_blocks([step], directory, xml=junit(names))
                self.assertEqual(results[-1].returncode, 0, results[-1].stderr)
                for name in names:
                    self.assertIn(name, results[-1].stdout)
                for seconds in range(1, len(names) + 1):
                    self.assertRegex(results[-1].stdout, rf"\b{seconds}(?:\.0+)?\b")

    def test_partition_selection_handles_wrappers_empty_lists_and_regex_names(self):
        _, job, _ = self.workflow()
        steps = [s for s in job["steps"] if "ctest" in s.get("run", "")]
        self.assertTrue(steps, "workflow must execute CTest")
        for empty in (False, True):
            with self.subTest(empty_b2=empty), tempfile.TemporaryDirectory() as directory:
                inventory = [
                    testcase(name, ["highmem"], ["mpiexec", "-n", "4", "worker"])
                    for name in REQUIRED
                ]
                inventory += [
                    testcase("future-highmem", ["highmem", "mpi"],
                             ["mpiexec", "-n", "4", "worker"]),
                    testcase("mpi-four", ["mpi"], ["mpiexec", "-n", "4", "worker"]),
                    testcase("serial"),
                    testcase("mpiexec-not-a-launcher", command=[
                        "python3", "check_mpiexec.py", "--note=mpirun",
                    ]),
                ]
                expected = [] if empty else [
                    "direct", "install_relocate", "name.+[x](a)|b^$", "wrapped",
                ]
                if not empty:
                    inventory += [
                        testcase("direct", command=["mpirun", "-np", "2", "worker"]),
                        testcase("wrapped", command=[
                            "python3", "wrapper.py", "--launcher", "/opt/mpi/bin/mpiexec",
                        ]),
                        testcase("install_relocate", command=[
                            "python3", "relocate.py", "/opt/mpi/bin/mpiexec",
                        ]),
                        testcase("name.+[x](a)|b^$", command=[
                            "env", "X=1", "mpirun", "-np", "3", "worker",
                        ]),
                    ]
                results, calls = self.run_blocks(steps, directory, inventory=inventory)
                self.assertTrue(
                    all(r.returncode == 0 for r in results),
                    "\n".join(r.stderr for r in results),
                )
                discoveries = [
                    call for call in calls if any(a.startswith("--show-only") for a in call)
                ]
                self.assertEqual(len(discoveries), 1, "capture configured inventory once")
                self.assertIn("-N", discoveries[0])
                reports = {
                    call[call.index("--output-junit") + 1].split("/")[-1]: call
                    for call in calls if "--output-junit" in call
                }
                self.assertEqual(set(reports), set(REPORTS) - ({REPORTS[2]} if empty else set()))
                for report in reports:
                    self.assertTrue((Path(directory) / "build" / report).is_file(), report)
                def option(call, key):
                    return call[call.index(key) + 1] if key in call else None
                highmem, mpi, rest = (
                    reports[name] for name in (REPORTS[0], REPORTS[1], REPORTS[3])
                )
                self.assertEqual((option(highmem, "-L"), option(highmem, "-j")), ("highmem", "1"))
                self.assertEqual(
                    (option(mpi, "-L"), option(mpi, "-LE"), option(mpi, "-j")),
                    ("mpi", "highmem", "2"),
                )
                self.assertEqual(
                    (option(rest, "-LE"), option(rest, "-j"), option(rest, "--timeout")),
                    ("highmem|mpi", "6", "900"),
                )
                if empty:
                    self.assertNotIn("-E", rest)
                    self.assertIn("无", "\n".join(r.stdout for r in results))
                else:
                    unlabeled = reports[REPORTS[2]]
                    regex = option(unlabeled, "-R")
                    self.assertEqual(regex, option(rest, "-E"))
                    self.assertTrue(regex.startswith("^") and regex.endswith("$"), regex)
                    self.assertEqual(
                        (option(unlabeled, "-LE"), option(unlabeled, "-j")), ("highmem", "2"),
                    )
                    self.assertNotIn("-L", unlabeled, "-L and -R would intersect")
                    probes = [t["name"] for t in inventory] + [
                        "prefix-direct", "direct-suffix", "nameZZxabarb",
                        "name.+[x](a)|b^$-suffix",
                    ]
                    self.assertEqual(
                        sorted(name for name in probes if re.search(regex, name)), expected,
                    )
                # Interpret only emitted filters; expected ownership is hand-derived.
                groups = []
                for call in reports.values():
                    selected = set()
                    for test in inventory:
                        labels = test["properties"][0]["value"]
                        name = test["name"]
                        include, exclude = option(call, "-L"), option(call, "-LE")
                        if include and not any(re.search(include, label) for label in labels):
                            continue
                        if exclude and any(re.search(exclude, label) for label in labels):
                            continue
                        if option(call, "-R") and not re.search(option(call, "-R"), name):
                            continue
                        if option(call, "-E") and re.search(option(call, "-E"), name):
                            continue
                        selected.add(name)
                    groups.append(selected)
                self.assertEqual(set.union(*groups), {t["name"] for t in inventory})
                for left, right in itertools.combinations(groups, 2):
                    self.assertFalse(left & right, "test partitions must be disjoint")

    def test_ctest_failures_preserve_exit_status_and_wall_time(self):
        selection = self.find_step(lambda run: "ctest" in run and "--show-only" in run)
        inventory = [testcase("wrapped", command=["python3", "wrapper.py", "mpiexec"])]
        for report in REPORTS:
            with self.subTest(report=report), tempfile.TemporaryDirectory() as directory:
                execution = self.execution_step(report)
                results, _ = self.run_blocks(
                    [selection, execution], directory, inventory=inventory, status=8,
                )
                self.assertEqual(
                    results[-1].returncode, 8, results[-1].stdout + results[-1].stderr,
                )
                log = (results[-1].stdout + results[-1].stderr).lower()
                timing = r"(?:seconds|elapsed|wall|耗时|秒)"
                self.assertRegex(log, rf"(?:{timing}[^\n]*\d+|\d+[^\n]*{timing})")

    def test_load_report_validates_samples_and_copies_artifact(self):
        _, job, _ = self.workflow()
        steps = [
            s for s in job["steps"]
            if "loadavg.log" in s.get("run", "") and "vm.loadavg" not in s.get("run", "")
        ]
        self.assertTrue(steps, "missing always-run sampler cleanup/load validation")
        cleanup = next((s for s in steps if "kill" in s["run"]), None)
        self.assertIsNotNone(cleanup, "sampler PID must be stopped")
        self.assertIn("always()", cleanup.get("if", ""))
        for text, accepted in (
            ("123 { 1.0 9.0 8.0 }\n124 { 12.0 90.0 80.0 }\n", True),
            ("123 { 12.1 1.0 1.0 }\n", False), ("", False),
            ("123 { broken }\n", False), ("123 { NaN 1.0 1.0 }\n", False),
        ):
            with self.subTest(samples=text), tempfile.TemporaryDirectory() as directory:
                temp = Path(directory) / "runner-temp"
                write(temp / "loadavg.log", text)
                results, _ = self.run_blocks(steps, directory)
                self.assertEqual(
                    all(r.returncode == 0 for r in results), accepted,
                    "\n".join(r.stdout + r.stderr for r in results),
                )
                self.assertEqual((Path(directory) / "build/loadavg.log").read_text(), text)
                if accepted:
                    self.assertRegex("\n".join(r.stdout for r in results), r"12(?:\.0+)?")


class DependencyAcceptance(unittest.TestCase):
    def setUp(self):
        self.assertTrue(
            INSTALLER.is_file(), "missing Mac dependency installer (feature not implemented)",
        )
        self.directory = tempfile.TemporaryDirectory(prefix="mac-highmem-test-")
        self.addCleanup(self.directory.cleanup)
        self.home = Path(self.directory.name) / "home"
        self.home.mkdir()
        self.envroot = self.home / "chipcae-env/env"
        self.deps = self.home / "chipcae-mac/deps"
        self.trace = Path(self.directory.name) / "tool-calls"

    def invoke(self, *args):
        env = {**os.environ, "HOME": str(self.home), "TOOL_TRACE": str(self.trace)}
        # No PATH changes. Guards replace external download/build boundaries in
        # this subprocess only; real repository builds use the supplied shim.
        guards = '\n'.join(
            f'{tool}() {{ printf "%s\\n" "{tool} $*" >> "$TOOL_TRACE"; return 97; }}'
            for tool in ("curl", "wget", "cmake", "ninja", "make", "tar")
        )
        return subprocess.run(
            ["bash", "--noprofile", "--norc", "-c",
             guards + '\nsource "$0" "$@"', str(INSTALLER), *args],
            env=env, cwd=self.home, text=True, capture_output=True, timeout=15,
        )

    def valid_environment(self):
        for name in ("cmake", "ninja", "mpicxx", "mpiexec", "clang", "clang++"):
            write(self.envroot / "bin" / name, "#!/bin/bash\nexit 0\n", executable=True)
        for path in (
            "include/petsc.h", "include/petscsys.h", "include/hdf5.h", "include/metis.h",
            "include/HYPRE.h", "lib/libpetsc.dylib", "lib/liblapack.dylib", "lib/libblas.dylib",
            "lib/libhdf5.dylib", "lib/libmetis.dylib", "lib/libHYPRE.dylib",
        ):
            write(self.envroot / path)

    def valid_mfem(self, root=None, mpi="YES", petsc="YES"):
        root = self.deps if root is None else root
        write(root / "mfem/lib/cmake/mfem/MFEMConfig.cmake", (
            f'set(MFEM_VERSION "4.8")\nset(MFEM_USE_MPI {mpi})\n'
            f'set(MFEM_USE_PETSC {petsc})\nset(MFEM_USE_METIS YES)\nset(MFEM_USE_HYPRE YES)\n'
        ))
        write(root / "mfem/include/mfem.hpp")
        write(root / "mfem/lib/libmfem.dylib")

    def valid_jsoncons(self, root=None):
        root = self.deps if root is None else root
        write(root / "jsoncons/share/cmake/jsoncons/jsonconsConfig.cmake",
              'set(jsoncons_VERSION "1.4.0")\n')
        write(root / "jsoncons/include/jsoncons/json.hpp")

    def test_bash_syntax_and_strict_arguments(self):
        result = subprocess.run(["bash", "-n", str(INSTALLER)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        for args in (
            (), ("--unknown",), ("--check", "--install"),
            ("--install", "--prefix"), ("--check", "extra"),
        ):
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.trace.exists(), "invalid arguments must fail before tools")

    def test_missing_dependencies_are_aggregated(self):
        result = self.invoke("--check")
        self.assertNotEqual(result.returncode, 0)
        output = (result.stdout + result.stderr).lower()
        for name in ("cmake", "ninja", "mpicxx", "petsc", "lapack", "blas", "mfem", "jsoncons"):
            self.assertRegex(output, rf"(?:missing[^\n]*{name}|{name}[^\n]*missing)")
        for diagnostic in (r"compiler|clang|编译", r"mpi|mpiexec|mpirun", "hdf5", "metis", "hypre"):
            self.assertRegex(output, diagnostic)
        self.assertFalse(self.trace.exists())

    def test_complete_environment_checks_without_installing(self):
        self.valid_environment()
        self.valid_mfem()
        self.valid_jsoncons()
        result = self.invoke("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("cmake", "ninja", "mpicxx", "petsc", "lapack", "blas", "mfem", "jsoncons"):
            self.assertRegex(
                result.stdout.lower(), rf"(?:ok[^\n]*{name}|{name}[^\n]*ok)",
            )
        self.assertNotIn("MISSING", result.stdout)
        self.assertFalse(self.trace.exists())

    def test_mfem_must_enable_both_mpi_and_petsc(self):
        self.valid_environment()
        self.valid_jsoncons()
        for mpi, petsc in (("NO", "YES"), ("YES", "NO"), ("NO", "NO")):
            with self.subTest(mpi=mpi, petsc=petsc):
                self.valid_mfem(mpi=mpi, petsc=petsc)
                result = self.invoke("--check")
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("MISSING", result.stdout + result.stderr)

    def test_custom_dependency_root_and_repeat_install_skip(self):
        custom = self.home / "chipcae-mac/custom dependencies"
        self.valid_environment()
        self.valid_mfem(custom)
        self.valid_jsoncons(custom)
        for mode in ("--check", "--install", "--install"):
            with self.subTest(mode=mode):
                before = {
                    str(p.relative_to(self.home)): (p.stat().st_mtime_ns, p.read_bytes())
                    for p in self.home.rglob("*") if p.is_file()
                }
                result = self.invoke(mode, "--prefix", str(custom))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(self.trace.exists(), "valid installs must not download or rebuild")
                after = {
                    str(p.relative_to(self.home)): (p.stat().st_mtime_ns, p.read_bytes())
                    for p in self.home.rglob("*") if p.is_file()
                }
                self.assertEqual(before, after, "repeat install must not rewrite existing files")

    def test_outside_and_sibling_prefixes_rejected_before_side_effects(self):
        self.valid_environment()
        for outside in (self.home / "outside", self.home / "chipcae-mac-escape/deps"):
            with self.subTest(prefix=outside):
                before = set(self.home.rglob("*"))
                result = self.invoke("--install", "--prefix", str(outside))
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(
                    (result.stdout + result.stderr).lower(),
                    r"prefix|outside|contain|path|前缀|越界|路径",
                )
                self.assertFalse(outside.exists())
                self.assertEqual(set(self.home.rglob("*")), before)
                self.assertFalse(
                    self.trace.exists(), "invalid prefix must be rejected before downloads/builds",
                )

    def test_symlinked_root_and_child_destinations_cannot_escape(self):
        self.valid_environment()
        outside = self.home / "outside"
        outside.mkdir()
        for child in ("root", "mfem", "jsoncons"):
            with self.subTest(child=child):
                if self.deps.exists() or self.deps.is_symlink():
                    if self.deps.is_symlink():
                        self.deps.unlink()
                    else:
                        shutil.rmtree(self.deps)
                self.deps.parent.mkdir(exist_ok=True)
                if child == "root":
                    self.deps.symlink_to(outside, target_is_directory=True)
                else:
                    self.deps.mkdir()
                    (self.deps / child).symlink_to(outside, target_is_directory=True)
                before = set(self.home.rglob("*"))
                result = self.invoke("--install")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(list(outside.iterdir()), [])
                self.assertEqual(set(self.home.rglob("*")), before)
                self.assertFalse(self.trace.exists(), "resolve child destinations before downloads")

    def test_each_valid_dependency_is_skipped_independently(self):
        self.valid_environment()
        for present, missing in (("mfem", "jsoncons"), ("jsoncons", "mfem")):
            with self.subTest(present=present):
                if self.deps.exists():
                    shutil.rmtree(self.deps)
                if self.trace.exists():
                    self.trace.unlink()
                if present == "mfem":
                    self.valid_mfem()
                else:
                    self.valid_jsoncons()
                before = {
                    str(p): (p.stat().st_mtime_ns, p.read_bytes())
                    for p in (self.deps / present).rglob("*") if p.is_file()
                }
                result = self.invoke("--install")
                self.assertNotEqual(
                    result.returncode, 0, "missing dependency hits guarded download",
                )
                self.assertTrue(self.trace.exists(), result.stdout + result.stderr)
                calls = self.trace.read_text().lower()
                self.assertIn(missing, calls, "download only the absent dependency")
                self.assertIn("github.com/", calls, "use the official release archive")
                self.assertIn("4.8" if missing == "mfem" else "1.4.0", calls)
                self.assertNotIn(present, calls, "valid dependency must be skipped independently")
                self.assertNotIn("cmake", calls, "do not rebuild the existing dependency")
                after = {
                    str(p): (p.stat().st_mtime_ns, p.read_bytes())
                    for p in (self.deps / present).rglob("*") if p.is_file()
                }
                self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
