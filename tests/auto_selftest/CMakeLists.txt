# Self-test of the auto-collect convention (cmake/ChipcaeAutoTests.cmake).
chipcae_auto_tests(LIBS chipcae::core LABELS "tierA;autoglob" TIMEOUT 30)

# Inspect CTest's generated inventory, including tests owned by other directories.
# Keep the test script in the build tree: this infrastructure card permits only
# CMakeLists.txt changes, and does not change the automatic collection helper.
set(_highmem_check "${CMAKE_CURRENT_BINARY_DIR}/check_highmem.py")
file(WRITE "${_highmem_check}" [=[
import argparse
import ast
from contextlib import contextmanager
import itertools
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
import xml.etree.ElementTree as ET


parser = argparse.ArgumentParser()
parser.add_argument("--build", required=True)
parser.add_argument("--backend", choices=("0", "1"), required=True)
parser.add_argument("--case", required=True)
parser.add_argument("--baseline", type=Path)
parser.add_argument("--selection", type=Path)
parser.add_argument("--measurements", type=Path)
parser.add_argument("--pr-body", type=Path)
parser.add_argument("--pinned-inventory", type=Path)
parser.add_argument("--runtime-evidence", type=Path)
args = parser.parse_args()
# chipcae-solve is an unconditional top-level target. Demo/cut-bimetal use
# these backend flags without GMG's additional MFEM_USE_PETSC condition.
cache = (Path(args.build) / "CMakeCache.txt").read_text()
backend_flags = dict(re.findall(r"^CHIPCAE_ENABLE_(MFEM|MPI|HDF5):BOOL=(.*)$", cache, re.M))
solve_backend = all(
    backend_flags.get(dependency, "OFF").upper()
    not in {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}
    and not backend_flags.get(dependency, "OFF").upper().endswith("-NOTFOUND")
    for dependency in ("MFEM", "MPI", "HDF5")
)


def inventory(*filters):
    result = subprocess.run(
        ["ctest", "--test-dir", args.build, "--show-only=json-v1", *filters],
        check=True, capture_output=True, text=True,
    )
    tests = json.loads(result.stdout)["tests"]
    return {test["name"]: test for test in tests}


def properties(test):
    return {prop["name"]: prop["value"] for prop in test["properties"]}


def killed(output):
    # Wrapper exit 1/CTest exit 8 alone says nothing about a child's signal.
    return bool(re.search(
        r"\],\s*-9\s*,|\b(?:exit(?: status| code)?|returncode|return code)\s*[:=]?\s*(?:-9|137)\b"
        r"|\bSIGKILL\b|\bsignal\s+9\b", output, re.I))


def roots(cache_text):
    return [value for key in ("CMAKE_CACHEFILE_DIR", "CMAKE_HOME_DIRECTORY")
            for value in re.findall(r"^" + key + r":[^=]+=(.*)$", cache_text, re.M)]


def normalized(value, cache_text):
    if isinstance(value, str):
        for index, root in enumerate(roots(cache_text)):
            value = value.replace(root, ("<build>", "<source>")[index])
    elif isinstance(value, list):
        value = [normalized(item, cache_text) for item in value]
    elif isinstance(value, dict):
        value = {key: normalized(item, cache_text) for key, item in value.items()}
    return value


def configuration(cache_text):
    # Compare effective options/toolchains/dependencies, excluding CMake's
    # generated bookkeeping and only normalizing the two worktree roots.
    entries = {}
    for key, kind, value in re.findall(r"^([^/#\n][^:\n]*):([^=\n]+)=(.*)$",
                                       cache_text, re.M):
        if (kind != "INTERNAL" or key == "CMAKE_GENERATOR") and not key.endswith(
                ("_BINARY_DIR", "_SOURCE_DIR")):
            entries[key] = normalized(value, cache_text)
    return entries


class HighmemAcceptance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tests = inventory()
        cls.props = {name: properties(test) for name, test in cls.tests.items()}
        cls.highmem = {
            name for name, props in cls.props.items()
            if "highmem" in props.get("LABELS", [])
        }

    def test_selection(self):
        # Pinned RSS supplies an independent minimum, including the brief's
        # narrowly accepted killed scaling run. No new profiling is required.
        if args.backend == "1":
            self.assertIn("solve_column_gmg_fcbga", self.highmem,
                          "pinned Release peak 1,602,996 KiB exceeds 1.5 GiB")
            self.assertIn("solve_column_gmg_fcbga_scaling", self.highmem,
                          "pinned killed scaling peak 4,818,708 KiB exceeds 1.5 GiB")
        if solve_backend:
            self.assertIn("cutbim_cut_bimetal", self.highmem,
                          "pinned Release peak 1,882,428 KiB exceeds 1.5 GiB")
        listed = set(inventory("-L", "^highmem$"))
        self.assertEqual(listed, self.highmem)
        listing = subprocess.run(
            ["ctest", "--test-dir", args.build, "-L", "highmem", "-N"],
            check=True, capture_output=True, text=True,
        ).stdout
        self.assertEqual(set(re.findall(r"^\s*Test\s+#\d+:\s+(\S+)\s*$", listing, re.M)),
                         self.highmem, "ctest -L highmem -N must list exactly the selection")

    def test_shared_lock(self):
        locked = {
            name for name, props in self.props.items()
            if "chipcae_highmem" in props.get("RESOURCE_LOCK", [])
        }
        if args.backend == "1":
            self.assertTrue(
                locked,
                "backend-enabled inventory has no chipcae_highmem resource lock; "
                "high-memory solver tests can still overlap under parallel CTest",
            )
        self.assertEqual(locked, self.highmem,
                         "every highmem test must request the shared lock and vice versa")

    def test_existing_metadata(self):
        # Independent expectations from the pre-change registrations. This also
        # checks that conditional tests are absent rather than assigned properties
        # before registration. F1130 is collected even with the backend disabled.
        expected = {
            "solve_column_gmg_fcbga": (900, {"gmg", "numerical"}),
            "solve_column_gmg_fcbga_scaling": (900, {"gmg", "numerical"}),
            "demogate_solve": (1800, {"cut", "numerical"}),
            "f1130_uncut_cli_e2e": (1200, {"integration", "numerical", "vv"}),
            "cutbim_cut_bimetal": (2400, {"integration", "numerical", "vv"}),
        }
        for name, (timeout, labels) in expected.items():
            with self.subTest(name=name):
                if name.startswith("solve_column_gmg"):
                    present = args.backend == "1"
                elif name == "demogate_solve":
                    present = solve_backend
                else:
                    present = True
                self.assertEqual(name in self.tests, present)
                if not present:
                    continue
                props = self.props[name]
                self.assertEqual(props["TIMEOUT"], timeout)
                self.assertEqual([label for label in props.get("LABELS", [])
                                  if label != "highmem"], sorted(labels))
                self.assertFalse(props.get("DISABLED", False))
                self.assertFalse(props.get("RUN_SERIAL", False))
                if name.startswith("solve_column_gmg"):
                    command = self.tests[name]["command"]
                    coverage = "scaling" if name.endswith("scaling") else "reference"
                    self.assertEqual(Path(command[1]).name, "test_column_gmg.py")
                    self.assertEqual(command[2:5], [
                        str(Path(args.build) / "apps/chipcae-solve/chipcae-solve"),
                        re.search(r"^MPIEXEC_EXECUTABLE:FILEPATH=(.*)$", cache, re.M)[1],
                        re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache, re.M)[1]])
                    self.assertEqual(len(command), 7)
                    self.assertEqual(command[-2:], ["--coverage", coverage])
                    self.assertNotIn("SKIP_RETURN_CODE", props)
                else:
                    self.assertEqual(props["SKIP_RETURN_CODE"], 77)

    def test_inventory_preserved(self):
        # Untouched main has none of the three infrastructure checks. Common
        # commands/properties may differ only by roots and measured lock/labels.
        self.assertIsNotNone(args.baseline, "provide --baseline inventory.json")
        self.assertIsNotNone(args.selection, "provide --selection measured-tests.txt")
        baseline = {
            test["name"]: test
            for test in json.loads(args.baseline.read_text())["tests"]
        }
        selection = set(args.selection.read_text().splitlines())
        if args.backend == "1" or solve_backend:
            self.assertTrue(selection, "the measured selection must not be empty")
        added = {"ctest_highmem_selection", "ctest_highmem_shared_lock",
                 "ctest_highmem_existing_metadata"}
        self.assertEqual(set(self.tests) - set(baseline), added - set(baseline))
        self.assertFalse(set(baseline) - set(self.tests), "baseline inventory drift")
        self.assertTrue((args.baseline.parent / "baseline-cache.txt").is_file(),
                        "baseline inventory needs its saved CMake cache")
        old_cache = (args.baseline.parent / "baseline-cache.txt").read_text()
        self.assertEqual(self.highmem, selection)
        self.assertEqual(set(inventory("-L", "^highmem$")), selection)
        self.assertEqual({name for name, props in self.props.items()
                          if "chipcae_highmem" in props.get("RESOURCE_LOCK", [])}, selection)
        for name, old in baseline.items():
            with self.subTest(name=name):
                current = self.tests[name]
                self.assertEqual(normalized(current.get("command"), cache),
                                 normalized(old.get("command"), old_cache))
                expected = normalized(properties(old), old_cache)
                if name in selection:
                    expected["LABELS"] = sorted(
                        set(expected.get("LABELS", [])) | {"highmem"}
                    )
                    expected["RESOURCE_LOCK"] = sorted(
                        set(expected.get("RESOURCE_LOCK", [])) | {"chipcae_highmem"}
                    )
                self.assertEqual(normalized(self.props[name], cache), expected)

    def profile(self, path, pinned):
        result = json.loads(path.read_text())
        name = result.get("name")
        if result.get("revision") != "eaed3e4" or name not in pinned:
            return None
        command = json.loads((path.parent / "command.json").read_text())
        self.assertIn("ctest", command)
        self.assertIn("-j1", command, "RSS profiling must execute serially")
        self.assertEqual(command[command.index("-R") + 1], "^" + re.escape(name) + "$")
        measured_cache = (path.parent.parent / "cache.txt").read_text()
        self.assertRegex(measured_cache, r"(?m)^CMAKE_BUILD_TYPE:STRING=Release$")
        measured = {test["name"]: test for test in json.loads(
            (path.parent.parent / "inventory.json").read_text())["tests"]}
        self.assertEqual(measured[name].get("command"), pinned[name].get("command"))
        self.assertEqual(properties(measured[name]), properties(pinned[name]))
        cases = ET.parse(path.parent / "results.xml").findall(".//testcase")
        self.assertEqual(len(cases), 1)
        case = cases[0]
        self.assertEqual(case.get("name"), name)
        raw = (path.parent / "time.txt").read_text()
        peak = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", raw)
        self.assertIsNotNone(peak, "profiling attempt lacks raw RSS")
        kib = int(peak[1])
        self.assertEqual(kib, result["time_peak_kib"])
        self.assertRegex(raw, r"Exit status:\s*" + str(result["status"]) + r"\s*$")
        executed = (not result.get("disabled", False)
                    and not properties(pinned[name]).get("DISABLED", False)
                    and case.get("status") in {"run", "fail"}
                    and case.find("skipped") is None)
        passed = (executed and result.get("status") == 0
                  and result.get("outcomes") == {name: "passed"}
                  and case.get("status") == "run"
                  and all(case.find(tag) is None for tag in ("failure", "error")))
        exception = (executed and name == "solve_column_gmg_fcbga_scaling"
                     and result.get("status") != 0
                     and result.get("outcomes") == {name: "failed"}
                     and case.find("failure") is not None
                     and killed((path.resolve().parent / "ctest.log").read_text())
                     and killed(case.findtext("system-out", "")))
        if kib >= 1572864 and (passed or exception):
            processes = json.loads((path.parent / "process-peaks.json").read_text())
            # Never sum MPI ranks. Keep GNU time and VmHWM separate.
            self.assertTrue(any(process["peak_kib"] >= 1572864
                                for process in processes.values()),
                            "high RSS lacks per-process corroboration: " + name)
        return name, kib, passed, exception, executed

    def test_measurement_coverage(self):
        self.assertIsNotNone(args.measurements, "provide --measurements raw-evidence-root")
        self.assertIsNotNone(args.pinned_inventory, "provide --pinned-inventory inventory.json")
        pinned = {test["name"]: test for test in
                  json.loads(args.pinned_inventory.read_text())["tests"]}
        mandatory = {"solve_column_gmg_fcbga", "solve_column_gmg_fcbga_scaling",
                     "demogate_solve", "f1130_uncut_cli_e2e", "cutbim_cut_bimetal"}
        if args.backend == "1":
            self.assertTrue(mandatory <= pinned.keys(), "pinned full inventory omits candidates")
        recorded, resolved, high_failed = set(), set(), set()
        for path in sorted(args.measurements.rglob("result.json")):
            with self.subTest(raw=str(path)):
                profile = self.profile(path, pinned)
                if profile is None:
                    continue
                name, kib, passed, exception, executed = profile
                recorded.add(name)
                if passed or (exception and kib >= 1572864):
                    resolved.add(name)
                elif executed and kib >= 1572864:
                    high_failed.add(name)
        self.assertEqual(recorded, set(pinned),
                         "every pinned registration needs a raw profiling disposition; "
                         "skips/disabled entries do not qualify as successful measurements")
        self.assertFalse(high_failed - resolved,
                         "high-RSS candidates lack successful pinned Release evidence "
                         "or the scaling-only SIGKILL exception: "
                         + str(sorted(high_failed - resolved)))

    def test_runtime_evidence(self):
        self.assertIsNotNone(args.runtime_evidence, "provide --runtime-evidence suite-log-root")
        root = args.runtime_evidence
        self.assertTrue(self.highmem, "full runtime acceptance needs a measured selection")
        for filename in ("baseline-inventory.json", "baseline-cache.txt", "baseline-sha.txt",
                         "baseline-origin-main.txt", "baseline-status.txt"):
            self.assertTrue((root / filename).is_file(), "missing untouched-main evidence: " + filename)
        baseline = {test["name"]: test for test in
                    json.loads((root / "baseline-inventory.json").read_text())["tests"]}
        added = {"ctest_highmem_selection", "ctest_highmem_shared_lock",
                 "ctest_highmem_existing_metadata"}
        self.assertEqual(set(self.tests) - set(baseline), added)
        self.assertFalse(set(baseline) - set(self.tests), "baseline inventory drift")
        old_cache = (root / "baseline-cache.txt").read_text()
        for configuration_cache in (old_cache, cache):
            self.assertRegex(configuration_cache, r"(?m)^CMAKE_BUILD_TYPE:STRING=Release$")
            for dependency in ("MFEM", "MPI", "HDF5", "ECAD", "JSONCONS"):
                self.assertRegex(configuration_cache,
                                 r"(?m)^CHIPCAE_ENABLE_" + dependency + r":BOOL=ON$")
        self.assertEqual(configuration(old_cache), configuration(cache),
                         "runtime compiler/dependency/options differ")
        sha = (root / "baseline-sha.txt").read_text().strip()
        self.assertRegex(sha, r"^[0-9a-f]{40}$")
        self.assertEqual(sha, (root / "baseline-origin-main.txt").read_text().strip())
        self.assertEqual((root / "baseline-status.txt").read_text(), "",
                         "baseline must be untouched reopening origin/main")
        for name, old in baseline.items():
            expected = normalized(properties(old), old_cache)
            if name in self.highmem:
                self.assertNotIn("chipcae_highmem", expected.get("RESOURCE_LOCK", []))
                self.assertNotIn("highmem", expected.get("LABELS", []))
                expected["LABELS"] = sorted(expected.get("LABELS", []) + ["highmem"])
                expected["RESOURCE_LOCK"] = sorted(expected.get("RESOURCE_LOCK", [])
                                                   + ["chipcae_highmem"])
            self.assertEqual(normalized(self.props[name], cache), expected, name)
            self.assertEqual(normalized(self.tests[name].get("command"), cache),
                             normalized(old.get("command"), old_cache), name)
        outcomes, durations, exit_evidence = {}, {}, {}
        for run in ("baseline-j2", "after-j2"):
            destination = root / run
            for filename in ("command.json", "ctest.log", "results.xml", "report.json",
                             "Testing/Temporary/LastTest.log", "host-load.txt"):
                self.assertTrue((destination / filename).is_file(), "missing " + run + "/" + filename)
            command = json.loads((destination / "command.json").read_text())
            self.assertEqual(command[:2], ["ctest", "--test-dir"])
            self.assertEqual(command[2], roots(old_cache if run == "baseline-j2" else cache)[0])
            self.assertEqual(command[3:6], ["-j2", "--output-on-failure", "--output-junit"])
            self.assertEqual(len(command), 7, "full suite must not filter tests")
            self.assertEqual(Path(command[6]).resolve(), (destination / "results.xml").resolve())
            log = (destination / "ctest.log").read_text()
            elapsed = re.search(r"Total Test time \(real\) =\s*([\d.]+) sec", log)
            self.assertIsNotNone(elapsed, "missing CTest execution duration")
            durations[run] = float(elapsed[1])
            cases = ET.parse(destination / "results.xml").findall(".//testcase")
            temporary = (destination / "Testing/Temporary/LastTest.log").read_text()
            outcomes[run], exit_evidence[run] = {}, {}
            counts = dict(passed=0, failed=0, skipped=0, disabled=0)
            for case in cases:
                name = case.get("name")
                self.assertNotIn(name, outcomes[run], "duplicate JUnit outcome")
                outcome = ("disabled" if case.get("status") == "disabled" else
                           "skipped" if case.find("skipped") is not None else
                           "failed" if any(case.find(tag) is not None
                                           for tag in ("failure", "error")) else
                           "passed" if case.get("status") == "run" else "failed")
                outcomes[run][name] = outcome
                counts[outcome] += 1
                details = re.search(r"(?ms)^\d+/\d+ Testing: " + re.escape(name)
                                    + r"\n(.*?)(?=^\d+/\d+ Testing: |\Z)", temporary)
                exit_evidence[run][name] = killed(case.findtext("system-out", "")) or (
                    details is not None and killed(details[1]))
                if run == "after-j2" and name in self.highmem:
                    self.assertIn(case.get("status"), {"run", "fail"}, "selected test must execute")
                    self.assertIsNotNone(details, "missing selected child/output record: " + name)
                    self.assertNotIn(outcome, {"skipped", "disabled"})
                    self.assertFalse(exit_evidence[run][name], "selected test SIGKILL: " + name)
            print(run, json.dumps(counts, sort_keys=True))
            self.assertEqual(set(outcomes[run]), set(baseline) if run == "baseline-j2"
                             else set(self.tests), "full inventory differs")
            self.assertTrue((destination / "Testing/Temporary/LastTest.log").is_file(),
                            "complete Testing/Temporary logs must be retained")
            self.assertTrue((destination / "host-load.txt").read_text().strip())
            # Kernel logs are retained evidence; unrelated OOM is not a failure.
            for filename in ("kernel-before.log", "kernel-after.log", "kernel-follow.log"):
                self.assertTrue((destination / filename).is_file(), "missing " + filename)
            before = (destination / "kernel-before.log").read_text()
            after = (destination / "kernel-after.log").read_text()
            stream = (destination / "kernel-follow.log").read_text()
            self.assertTrue(before, "empty initial kernel snapshot")
            self.assertTrue(after, "empty final kernel snapshot")
            self.assertNotRegex(stream, r"(?m)^dmesg:", "kernel collector reported an error")
            self.assertIn(before, stream, "kernel stream misses initial snapshot")
            self.assertIn(after, stream, "kernel stream misses final snapshot")
            self.assertLessEqual(stream.index(before), stream.rindex(after),
                                 "kernel snapshot anchors are out of order")
            report = json.loads((destination / "report.json").read_text())
            self.assertEqual(report["jobs"], 2)
            if run == "after-j2":
                intervals = report["observed_intervals"]
                starts = re.findall(r"(?m)^\s*Start\s+\d+:\s+(\S+)\s*$", log)
                ends = re.findall(r"(?m)^\s*\d+/\d+ Test\s+#\d+:\s+(\S+)\s+", log)
                for name in self.highmem:
                    self.assertEqual(starts.count(name), 1, "missing/duplicate CTest start: " + name)
                    self.assertEqual(ends.count(name), 1, "missing/duplicate CTest end: " + name)
                    self.assertIn(name, intervals)
                    start, end = intervals[name]
                    self.assertIsNotNone(start, "missing selected start: " + name)
                    self.assertIsNotNone(end, "missing selected end: " + name)
                    self.assertGreaterEqual(end, start)
                for first, second in itertools.combinations(sorted(self.highmem), 2):
                    start1, end1 = intervals[first]
                    start2, end2 = intervals[second]
                    self.assertEqual(max(0, min(end1, end2) - max(start1, start2)), 0,
                                     "selected intervals overlap: " + first + ", " + second)
        before = {name for name, outcome in outcomes["baseline-j2"].items() if outcome == "failed"}
        after = {name for name, outcome in outcomes["after-j2"].items() if outcome == "failed"}
        for name in after - before:
            self.assertNotIn(name, self.highmem, "new selected failure: " + name)
            self.assertTrue(exit_evidence["after-j2"][name],
                            "new failure lacks explicit child SIGKILL/137 evidence: " + name)
        self.assertGreater(durations["baseline-j2"], 0)
        self.assertLessEqual(durations["after-j2"], durations["baseline-j2"] * 1.20)

    def test_measured_evidence(self):
        self.assertIsNotNone(args.measurements, "provide --measurements raw-evidence-root")
        self.assertIsNotNone(args.pr_body, "provide --pr-body pr-body.md")
        self.assertIsNotNone(args.selection, "provide --selection measured-tests.txt")
        self.assertIsNotNone(args.pinned_inventory, "provide --pinned-inventory inventory.json")
        self.assertTrue(self.highmem, "full measured acceptance needs a nonempty selection")
        pinned = {test["name"]: test for test in
                  json.loads(args.pinned_inventory.read_text())["tests"]}
        selection, peaks = set(args.selection.read_text().splitlines()), {}
        for path in sorted(args.measurements.rglob("result.json")):
            with self.subTest(raw=str(path)):
                profile = self.profile(path, pinned)
                if profile is None:
                    continue
                name, kib, passed, exception, _ = profile
                if kib >= 1572864 and (passed or exception):
                    peaks.setdefault(name, set()).add(kib)
        for name, actual in (("highmem labels", self.highmem),
                             ("selection file", selection),
                             ("shared locks", {name for name, props in self.props.items()
                                               if "chipcae_highmem" in props.get("RESOURCE_LOCK", [])})):
            with self.subTest(membership=name):
                self.assertEqual(actual, set(peaks), "must equal pinned measured selection")
        rows, notes, peak_column = {}, {}, None
        for line in args.pr_body.read_text().splitlines():
            if not line.startswith("|"):
                peak_column = None
                continue
            columns = [column.strip() for column in line.strip().strip("|").split("|")]
            if columns and columns[0] == "Locked test":
                peak_column = columns.index("Peak KiB")
                continue
            if peak_column is not None and re.fullmatch(r"`[^`]+`", columns[0]):
                name = columns[0].strip("`")
                self.assertNotIn(name, rows, "duplicate PR table row")
                rows[name] = int(columns[peak_column].replace(",", ""))
                notes[name] = " ".join(columns)
        with self.subTest(membership="PR table"):
            self.assertEqual(set(rows), self.highmem, "PR table must equal highmem labels")
        self.assertEqual(set(rows), selection, "PR table must equal measured selection")
        self.assertEqual(set(rows), set(peaks), "PR table must equal raw measured selection")
        for name, kib in rows.items():
            with self.subTest(name=name):
                self.assertIn(kib, peaks.get(name, set()), "PR RSS must match pinned raw output")
                if name == "solve_column_gmg_fcbga_scaling":
                    self.assertIn("峰值来自被 -9 终止的运行，≥ 4.8 GB", notes[name])


    def test_evidence_guards(self):
        # Small synthetic artifacts exercise the validator only; these are never
        # used as measured membership or as full-suite acceptance evidence.
        old_cache = ("CMAKE_CACHEFILE_DIR:INTERNAL=/old/build\n"
                     "CMAKE_HOME_DIRECTORY:INTERNAL=/old/source\n"
                     "CMAKE_CXX_COMPILER:FILEPATH=/toolchain/c++\n"
                     "CHIPCAE_ENABLE_MPI:BOOL=ON\nMFEM_DIR:PATH=/deps/mfem\n")
        new_cache = old_cache.replace("/old/", "/new/")
        self.assertEqual(configuration(old_cache), configuration(new_cache))
        for before, after in (("MPI:BOOL=ON", "MPI:BOOL=OFF"),
                              ("/deps/mfem", "/deps/other-mfem"),
                              ("/toolchain/c++", "/toolchain/other-c++")):
            self.assertNotEqual(configuration(old_cache),
                                configuration(new_cache.replace(before, after)))
        with tempfile.TemporaryDirectory(prefix="highmem-provenance-") as directory:
            root = Path(directory)
            name = "solve_column_gmg_fcbga_scaling"
            path = root / name / "result.json"
            path.parent.mkdir()
            registration = dict(name=name, command=["solver", "--coverage", "scaling"],
                                properties=[])
            pinned = {name: registration}
            result = dict(name=name, revision="eaed3e4", status=8,
                          outcomes={name: "failed"}, time_peak_kib=4818708)
            (root / "cache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
            (root / "inventory.json").write_text(json.dumps(dict(tests=[registration])))
            command = ["ctest", "-j1", "-R", "^" + name + "$"]
            (path.parent / "command.json").write_text(json.dumps(command))
            (path.parent / "time.txt").write_text(
                "Maximum resident set size (kbytes): 4818708\nExit status: 8\n")
            (path.parent / "process-peaks.json").write_text(
                json.dumps({"rank0": {"peak_kib": 4818756}}))
            (path.parent / "ctest.log").write_text("child exit=-9\n")
            suite = ET.Element("testsuite")
            case = ET.SubElement(suite, "testcase", name=name, status="fail")
            ET.SubElement(case, "failure", message="Failed")
            output = ET.SubElement(case, "system-out")
            output.text = "child exit=-9"
            def save():
                path.write_text(json.dumps(result))
                ET.ElementTree(suite).write(path.parent / "results.xml")
            save()
            self.assertEqual(self.profile(path, pinned), (name, 4818708, False, True, True))
            with self.subTest(guard="wrong revision"):
                result["revision"] = "current"
                save()
                self.assertIsNone(self.profile(path, pinned))
                result["revision"] = "eaed3e4"
                save()
            with self.subTest(guard="Debug provenance"):
                (root / "cache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Debug\n")
                with self.assertRaisesRegex(AssertionError, "Release"):
                    self.profile(path, pinned)
                (root / "cache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
            with self.subTest(guard="changed command"):
                (path.parent / "command.json").write_text(json.dumps(["ctest", "-j2", "-R", name]))
                with self.assertRaisesRegex(AssertionError, "serially"):
                    self.profile(path, pinned)
                (path.parent / "command.json").write_text(json.dumps(command))
            with self.subTest(guard="below threshold"):
                result["time_peak_kib"] = 1572863
                save()
                (path.parent / "time.txt").write_text(
                    "Maximum resident set size (kbytes): 1572863\nExit status: 8\n")
                _, peak, passed, exception, _ = self.profile(path, pinned)
                self.assertFalse(peak >= 1572864 and (passed or exception))
                result["time_peak_kib"] = 4818708
                save()
                (path.parent / "time.txt").write_text(
                    "Maximum resident set size (kbytes): 4818708\nExit status: 8\n")
            with self.subTest(guard="MPI rank sums"):
                (path.parent / "process-peaks.json").write_text(json.dumps(
                    {"rank0": {"peak_kib": 1000000}, "rank1": {"peak_kib": 1000000}}))
                with self.assertRaisesRegex(AssertionError, "per-process"):
                    self.profile(path, pinned)
                (path.parent / "process-peaks.json").write_text(
                    json.dumps({"rank0": {"peak_kib": 4818756}}))
            for status in ("exit=1", "exit=137", "exit status 137", "return code: -9",
                           "signal 9", "SIGKILL", "exit=-9"):
                with self.subTest(child_exit=status):
                    output.text = status
                    (path.parent / "ctest.log").write_text(status)
                    save()
                    self.assertEqual(self.profile(path, pinned)[3], status != "exit=1")
            with self.subTest(guard="disabled or skipped CUDA is not execution"):
                case.set("status", "disabled")
                save()
                self.assertFalse(self.profile(path, pinned)[3])
                case.set("status", "run")
                ET.SubElement(case, "skipped")
                save()
                self.assertFalse(self.profile(path, pinned)[3])
                case.remove(case.find("skipped"))
                case.set("status", "fail")
            with self.subTest(guard="another killed test cannot qualify"):
                other = "cutbim_cut_bimetal"
                result.update(name=other, outcomes={other: "failed"})
                case.set("name", other)
                registration["name"] = other
                (root / "inventory.json").write_text(json.dumps(dict(tests=[registration])))
                (path.parent / "command.json").write_text(json.dumps(
                    ["ctest", "-j1", "-R", "^" + other + "$"]))
                save()
                self.assertEqual(self.profile(path, {other: registration})[2:4], (False, False))

    def test_runner_contract(self):
        # Exercise the existing runner against a real tiny CTest project. Kernel
        # input is controlled so unrelated host OOM cannot make this regression
        # flaky. It is a runner regression, never a numerical acceptance run.
        script = Path(__file__).with_name("run_highmem_acceptance.py")
        module = ast.parse(script.read_text())
        functions = ast.Module(body=[node for node in module.body
                                    if isinstance(node, (ast.Import, ast.ImportFrom, ast.FunctionDef))],
                               type_ignores=[])
        with tempfile.TemporaryDirectory(prefix="highmem-runner-") as directory:
            root = Path(directory)
            source, build, evidence = root / "source", root / "build", root / "evidence"
            source.mkdir()
            evidence.mkdir()
            (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(highmem_runner_regression NONE)
enable_testing()
add_test(NAME selected COMMAND cmake -E true)
add_test(NAME existing_failure COMMAND cmake -E false)
add_test(NAME skipped COMMAND cmake -E false)
set_tests_properties(skipped PROPERTIES SKIP_RETURN_CODE 1)
add_test(NAME disabled COMMAND cmake -E true)
set_tests_properties(disabled PROPERTIES DISABLED TRUE)
''')
            subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                            "-DCMAKE_BUILD_TYPE=Release"], check=True, capture_output=True, text=True)
            namespace = dict(contextmanager=contextmanager, Path=Path, subprocess=subprocess,
                             json=json, time=time, re=re, ET=ET, signal=signal,
                             args=SimpleNamespace(build=build, evidence=evidence),
                             inventory_names={"selected", "existing_failure", "skipped", "disabled"},
                             expected_red=set())
            exec(compile(functions, str(script), "exec"), namespace)
            @contextmanager
            def kernel_input(destination):
                before = "initial kernel snapshot\n"
                delta = "chipcae unrelated host process killed process (OOM)\n" if host_oom else ""
                after = before + delta
                for filename, contents in (("kernel-before.log", before),
                                            ("kernel-after.log", after),
                                            ("kernel-follow.log", after)):
                    (destination / filename).write_text(contents)
                yield dict(before=before, after=after, stream=after)
            namespace["capture_kernel"] = kernel_input
            for host_oom in (True, False):
                rejection = None
                name = "baseline-host-oom-j2" if host_oom else "baseline-j2"
                try:
                    namespace["run"](name, 2, set(), baseline=True)
                except AssertionError as error:
                    rejection = str(error)
                with self.subTest(requirement="baseline failures are retained", host_oom=host_oom):
                    self.assertIsNone(rejection, "baseline must record its failure set and host OOM: "
                                      + str(rejection))
            destination = evidence / "baseline-j2"
            report = json.loads((destination / "report.json").read_text())
            self.assertEqual(report["outcomes"]["disabled"], "disabled")
            self.assertTrue(report["outcomes"]["skipped"].startswith("skipped"))
            self.assertTrue(report["outcomes"]["existing_failure"].startswith("failure"))
            with self.subTest(requirement="complete temporary logs"):
                self.assertTrue((destination / "Testing/Temporary/LastTest.log").is_file(),
                                "runner must retain Testing/Temporary logs")
            with self.subTest(requirement="shared host load"):
                self.assertTrue((destination / "host-load.txt").is_file(),
                                "runner must record shared host load")
            # Exercise the actual baseline phase, with a boundary assertion that
            # prevents obsolete -j4 requests from launching even in this fixture.
            phase_root = evidence / "phase"
            phase_root.mkdir()
            namespace["args"].evidence = phase_root
            namespace["args"].phase = "baseline"
            namespace["cache"] = (build / "CMakeCache.txt").read_text()
            namespace["inventory"] = subprocess.run(
                ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                check=True, capture_output=True, text=True).stdout
            for command in (["git", "init", str(source)],
                            ["git", "-C", str(source), "add", "CMakeLists.txt"],
                            ["git", "-C", str(source), "-c", "user.name=CTest",
                             "-c", "user.email=ctest@example.invalid", "commit", "-m", "fixture"],
                            ["git", "-C", str(source), "update-ref", "refs/remotes/origin/main", "HEAD"]):
                subprocess.run(command, check=True, capture_output=True, text=True)
            run = namespace["run"]
            def checked_run(name, jobs, *arguments, **options):
                self.assertEqual(jobs, 2, "baseline phase must request exactly -j2")
                return run(name, jobs, *arguments, **options)
            namespace["run"] = checked_run
            phase = [node for node in module.body if isinstance(node, ast.If)][-1]
            with self.subTest(requirement="one baseline -j2 phase"):
                exec(compile(ast.Module(body=[phase], type_ignores=[]), str(script), "exec"), namespace)
            with self.subTest(requirement="untouched main SHA recorded"):
                self.assertTrue((phase_root / "baseline-sha.txt").is_file(),
                                "runner must record the untouched origin/main revision")



unittest.main(argv=[__file__, "HighmemAcceptance.test_" + args.case], verbosity=2)
]=])
set(_highmem_backend 0)
if(CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI
        AND CHIPCAE_ENABLE_HDF5 AND MFEM_USE_PETSC AND TARGET chipcae-solve)
    set(_highmem_backend 1)
endif()
foreach(_case IN ITEMS selection shared_lock existing_metadata)
    add_test(NAME ctest_highmem_${_case}
        COMMAND "${Python3_EXECUTABLE}" "${_highmem_check}"
            --build "${PROJECT_BINARY_DIR}" --backend "${_highmem_backend}" --case "${_case}")
    set_tests_properties(ctest_highmem_${_case} PROPERTIES
        LABELS "tierA;infra;configuration" TIMEOUT 60)
endforeach()

# Run manually, never as a registered CTest (which would recursively run itself):
# python run_highmem_acceptance.py --build <build> --evidence <outside-source-dir>
#     --phase baseline
# Repeat with --phase verify --selection <measured-PR-table-test-names.txt>
#     --measurements <raw-profiling-root> --pr-body <pr-body.md>
#     --pinned-inventory <pinned-profiling-root/inventory.json> after
# implementing the properties. Raw inventories, JUnit, CTest and kernel logs stay
# in the evidence directory. No solver instrumentation or fixtures are added.
# The generated check_highmem.py also supports --case measurement_coverage and
# --case runtime_evidence (--runtime-evidence <suite-log-root>) independently, so
# invalid retained runs can be checked without launching numerical tests again.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/run_highmem_acceptance.py" [=[
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


parser = argparse.ArgumentParser()
parser.add_argument("--build", required=True, type=Path)
parser.add_argument("--evidence", required=True, type=Path)
parser.add_argument("--phase", required=True, choices=("baseline", "verify"))
parser.add_argument("--selection", type=Path)
parser.add_argument("--measurements", type=Path)
parser.add_argument("--pr-body", type=Path)
parser.add_argument("--pinned-inventory", type=Path)
args = parser.parse_args()
args.build = args.build.resolve()
args.evidence = args.evidence.resolve()
args.evidence.mkdir(parents=True, exist_ok=True)
expected_red = {"ctest_highmem_selection", "ctest_highmem_shared_lock"}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def kernel():
    return subprocess.run(["dmesg", "--color=never"], check=True,
                          capture_output=True, text=True).stdout


def wait_kernel_snapshot(path, snapshot, follower):
    require(snapshot, "empty kernel snapshot")
    deadline = time.monotonic() + 5
    while True:
        stream = path.read_text()
        require(follower.poll() is None, "continuous kernel collector exited early")
        require(not re.search(r"(?m)^dmesg:", stream), "kernel collector reported an error")
        if snapshot in stream:
            return stream
        require(time.monotonic() < deadline, "kernel stream misses a complete snapshot")
        time.sleep(.05)


@contextmanager
def capture_kernel(destination):
    # Start before the first snapshot, so both complete snapshots must occur in
    # the continuous stream. This bridges rotation without inventing a zero delta.
    path = destination / "kernel-follow.log"
    with path.open("w") as log:
        follower = subprocess.Popen(["dmesg", "--follow", "--color=never"],
                                    stdout=log, stderr=subprocess.STDOUT)
        try:
            before = kernel()
            (destination / "kernel-before.log").write_text(before)
            wait_kernel_snapshot(path, before, follower)
            captured = {"before": before}
            try:
                yield captured
            finally:
                after = kernel()
                (destination / "kernel-after.log").write_text(after)
                captured["after"] = after
                captured["stream"] = wait_kernel_snapshot(path, after, follower)
        finally:
            # Only the separate collector is stopped; CTest always runs to exit.
            if follower.poll() is None:
                follower.send_signal(signal.SIGINT)
            follower.wait(timeout=5)
            (destination / "kernel-follow-status.json").write_text(
                json.dumps({"status": follower.returncode}))


def run(name, jobs, selected, baseline=False):
    destination = args.evidence / name
    # Do not overwrite the baseline or raw evidence from a previous attempt.
    destination.mkdir()
    command = ["ctest", "--test-dir", str(args.build), "-j" + str(jobs),
               "--output-on-failure", "--output-junit", str(destination / "results.xml")]
    (destination / "command.json").write_text(json.dumps(command, indent=2))
    active = set()
    maximum = 0
    overlaps = []
    intervals = {}
    started = time.monotonic()
    with capture_kernel(destination) as captured, (destination / "ctest.log").open("w") as log:
        with subprocess.Popen(command, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True) as process:
            for line in process.stdout:
                log.write(line)
                log.flush()
                start = re.match(r"\s*Start\s+\d+:\s+(\S+)\s*$", line)
                end = re.match(r"\s*\d+/\d+ Test\s+#\d+:\s+(\S+)\s+", line)
                if start:
                    test = start[1]
                    if test in selected and active & selected:
                        overlaps.append((test, sorted(active & selected)))
                    active.add(test)
                    maximum = max(maximum, len(active))
                    intervals[test] = [time.monotonic() - started, None]
                if end:
                    active.discard(end[1])
                    if end[1] in intervals:
                        intervals[end[1]][1] = time.monotonic() - started
            status = process.wait()
    before, after, stream = captured["before"], captured["after"], captured["stream"]
    retained = after.startswith(before)
    start, end = stream.index(before), stream.rindex(after)
    covered = start <= end
    delta = stream[start + len(before):end + len(after)]
    oom = [line for line in delta.splitlines()
           if "chipcae" in line.lower()
           and re.search(r"oom|out of memory|killed process", line, re.I)]
    text = (destination / "ctest.log").read_text()
    elapsed = re.search(r"Total Test time \(real\) =\s*([\d.]+) sec", text)
    require(elapsed is not None, "CTest did not report its elapsed time: " + name)
    cases = ET.parse(destination / "results.xml").findall(".//testcase")
    outcomes = {}
    for case in cases:
        outcome = "passed"
        if case.get("status") == "disabled":
            outcome = "disabled"
        else:
            for tag in ("skipped", "failure", "error"):
                child = case.find(tag)
                if child is not None:
                    outcome = tag + ":" + child.get("message", "")
            if outcome == "passed" and case.get("status") != "run":
                outcome = "notrun"
        require(case.attrib["name"] not in outcomes, "duplicate JUnit outcome")
        outcomes[case.attrib["name"]] = outcome
    report = dict(build=str(args.build), jobs=jobs, status=status,
                  elapsed=float(elapsed[1]), outcomes=outcomes,
                  kernel_retained=retained, kernel_covered=covered, oom=oom, max_active=maximum,
                  overlaps=overlaps, observed_intervals=intervals)
    (destination / "report.json").write_text(json.dumps(report, indent=2))
    require(covered, "continuous kernel coverage is unverified: " + name)
    require(not oom, "new chipcae OOM records: " + name)
    failed = {test for test, value in outcomes.items()
              if value not in {"passed", "disabled"} and not value.startswith("skipped:")}
    # These two new tests are deliberately red in a pre-feature baseline. An
    # existing solver failure or OOM still invalidates the timing comparison.
    require(status == 0 or (baseline and status == 8 and failed == expected_red),
            "full CTest failed beyond the expected test-first assertions: " + name)
    require(not failed or (baseline and failed == expected_red),
            "unexpected failed test outcomes: " + name)
    require(set(outcomes) == set(inventory_names), "missing test outcomes: " + name)
    require(not active, "unparsed test completion events; concurrency is unverified: " + name)
    require(not overlaps, "highmem execution intervals overlap: " + name)
    require(all(test in intervals and intervals[test][1] is not None for test in selected),
            "missing highmem execution intervals: " + name)
    require(all(outcomes[test] == "passed" for test in selected),
            "a selected highmem test did not execute successfully: " + name)
    if jobs == 4:
        require(maximum >= 4, "four-way concurrency was not observed; "
                "a shim-clamped run cannot validate -j4: " + name)
    return report


inventory = subprocess.run(
    ["ctest", "--test-dir", str(args.build), "--show-only=json-v1"],
    check=True, capture_output=True, text=True,
).stdout
inventory_names = {test["name"] for test in json.loads(inventory)["tests"]}
cache = (args.build / "CMakeCache.txt").read_text()
require("CMAKE_BUILD_TYPE:STRING=Release\n" in cache, "acceptance requires a Release build")
for dependency in ("MFEM", "MPI", "HDF5", "ECAD"):
    require("CHIPCAE_ENABLE_" + dependency + ":BOOL=ON\n" in cache,
            "full acceptance requires the " + dependency + " backend")
if args.phase == "baseline":
    path = args.evidence / "baseline-inventory.json"
    with path.open("x") as output:
        output.write(inventory)
    with (args.evidence / "baseline-cache.txt").open("x") as output:
        output.write(cache)
    run("baseline-j4", 4, set(), baseline=True)
else:
    require(args.selection is not None, "provide the measured selection from the PR table")
    require(args.measurements is not None and args.pr_body is not None,
            "provide --measurements and --pr-body to verify pinned RSS and table membership")
    require(args.pinned_inventory is not None,
            "provide --pinned-inventory to reconcile every profiling candidate")
    require(cache == (args.evidence / "baseline-cache.txt").read_text(),
            "CMake cache changed; compiler/dependency/options comparison is unverified")
    selected = set(args.selection.read_text().splitlines())
    subprocess.run(
        [sys.executable, str(Path(__file__).with_name("check_highmem.py")),
         "--build", str(args.build), "--backend", "1", "--case", "inventory_preserved",
         "--baseline", str(args.evidence / "baseline-inventory.json"),
         "--selection", str(args.selection)], check=True,
    )
    subprocess.run(
        [sys.executable, str(Path(__file__).with_name("check_highmem.py")),
         "--build", str(args.build), "--backend", "1",
         "--case", "measured_evidence", "--measurements", str(args.measurements),
         "--pr-body", str(args.pr_body), "--selection", str(args.selection)], check=True,
    )
    subprocess.run(
        [sys.executable, str(Path(__file__).with_name("check_highmem.py")),
         "--build", str(args.build), "--backend", "1",
         "--case", "measurement_coverage", "--measurements", str(args.measurements),
         "--pinned-inventory", str(args.pinned_inventory)], check=True,
    )
    (args.evidence / "after-inventory.json").write_text(inventory)
    baseline = json.loads((args.evidence / "baseline-j4" / "report.json").read_text())
    failed = {test for test, outcome in baseline["outcomes"].items()
              if outcome != "passed" and not outcome.startswith("skipped:")}
    require((baseline["status"] == 0 and not failed)
            or (baseline["status"] == 8 and failed == expected_red),
            "baseline contains unexpected failures")
    require(baseline["kernel_retained"]
            and not baseline["oom"] and baseline["max_active"] >= 4,
            "baseline is not an OOM-free four-way run")
    require(baseline["jobs"] == 4 and baseline["elapsed"] > 0,
            "baseline timing is not a comparable -j4 run")
    require(baseline["build"] == str(args.build), "baseline used a different build")
    require(set(baseline["outcomes"]) == inventory_names, "baseline outcomes are incomplete")
    serial = run("after-j1", 1, selected)
    for name, outcome in baseline["outcomes"].items():
        if name not in expected_red:
            require(serial["outcomes"][name] == outcome, "baseline/serial outcome differs: " + name)
    for repeat in (1, 2):
        parallel = run("after-j4-" + str(repeat), 4, selected)
        require(parallel["outcomes"] == serial["outcomes"], "serial/parallel outcomes differ")
        require(parallel["elapsed"] <= baseline["elapsed"] * 1.20,
                "parallel duration exceeds baseline +20%")
    subprocess.run(
        [sys.executable, str(Path(__file__).with_name("check_highmem.py")),
         "--build", str(args.build), "--backend", "1", "--case", "runtime_evidence",
         "--runtime-evidence", str(args.evidence)], check=True,
    )
    print("Highmem acceptance passed; raw evidence: " + str(args.evidence))
]=])
