# Self-test of the auto-collect convention (cmake/ChipcaeAutoTests.cmake).
chipcae_auto_tests(LIBS chipcae::core LABELS "tierA;autoglob" TIMEOUT 30)

# Inspect CTest's generated inventory, including tests owned by other directories.
# Keep the test script in the build tree: this infrastructure card permits only
# CMakeLists.txt changes, and does not change the automatic collection helper.
set(_highmem_check "${CMAKE_CURRENT_BINARY_DIR}/check_highmem.py")
file(WRITE "${_highmem_check}" [=[
import argparse
import json
from pathlib import Path
import re
import subprocess
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
        # Successful pinned RSS measurements provide independent minimum
        # membership requirements; an arbitrary nonempty subset is insufficient.
        if args.backend == "1":
            self.assertIn("solve_column_gmg_fcbga", self.highmem,
                          "pinned Release peak 1,602,996 KiB exceeds 1.5 GiB")
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
                self.assertEqual(set(props.get("LABELS", [])) - {"highmem"}, labels)
                self.assertFalse(props.get("DISABLED", False))
                self.assertFalse(props.get("RUN_SERIAL", False))
                if name.startswith("solve_column_gmg"):
                    command = self.tests[name]["command"]
                    coverage = "scaling" if name.endswith("scaling") else "reference"
                    self.assertEqual(Path(command[1]).name, "test_column_gmg.py")
                    self.assertEqual(command[-2:], ["--coverage", coverage])
                    self.assertNotIn("SKIP_RETURN_CODE", props)
                else:
                    self.assertEqual(props["SKIP_RETURN_CODE"], 77)

    def test_inventory_preserved(self):
        # Optional implementation-phase acceptance: --baseline is a JSON-v1
        # inventory from this test-first revision, and --selection is a newline
        # separated list from the measured RSS/PR table. Keep both outside source.
        self.assertIsNotNone(args.baseline, "provide --baseline inventory.json")
        self.assertIsNotNone(args.selection, "provide --selection measured-tests.txt")
        baseline = {
            test["name"]: test
            for test in json.loads(args.baseline.read_text())["tests"]
        }
        selection = set(args.selection.read_text().splitlines())
        if args.backend == "1" or solve_backend:
            self.assertTrue(selection, "the measured selection must not be empty")
        self.assertEqual(set(self.tests), set(baseline))
        self.assertEqual(self.highmem, selection)
        self.assertEqual(set(inventory("-L", "^highmem$")), selection)
        self.assertEqual({name for name, props in self.props.items()
                          if "chipcae_highmem" in props.get("RESOURCE_LOCK", [])}, selection)
        for name, old in baseline.items():
            with self.subTest(name=name):
                current = self.tests[name]
                self.assertEqual(current.get("command"), old.get("command"))
                expected = properties(old)
                if name in selection:
                    expected["LABELS"] = sorted(
                        set(expected.get("LABELS", [])) | {"highmem"}
                    )
                    expected["RESOURCE_LOCK"] = sorted(
                        set(expected.get("RESOURCE_LOCK", [])) | {"chipcae_highmem"}
                    )
                self.assertEqual(self.props[name], expected)

    def test_measurement_coverage(self):
        # A failed high-RSS candidate cannot disappear from the selection by
        # filtering for successful runs. Reconcile the complete pinned inventory,
        # including MPI/CUDA, before treating the RSS table as exhaustive.
        self.assertIsNotNone(args.measurements, "provide --measurements raw-evidence-root")
        self.assertIsNotNone(args.pinned_inventory, "provide --pinned-inventory inventory.json")
        pinned = {test["name"]: test for test in
                  json.loads(args.pinned_inventory.read_text())["tests"]}
        mandatory = {"solve_column_gmg_fcbga", "solve_column_gmg_fcbga_scaling",
                     "demogate_solve", "f1130_uncut_cli_e2e", "cutbim_cut_bimetal"}
        if args.backend == "1":
            self.assertTrue(mandatory <= pinned.keys(), "pinned full inventory omits candidates")
        recorded, successful, high_failed = set(), set(), set()
        for path in sorted(args.measurements.rglob("result.json")):
            result = json.loads(path.read_text())
            name = result.get("name")
            if result.get("revision") != "eaed3e4" or name not in pinned:
                continue
            with self.subTest(raw=str(path)):
                command = json.loads((path.parent / "command.json").read_text())
                self.assertIn("ctest", command)
                self.assertIn("-j1", command, "RSS profiling must execute serially")
                self.assertEqual(command[command.index("-R") + 1],
                                 "^" + re.escape(name) + "$")
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
                recorded.add(name)
                passed = (result.get("status") == 0
                          and result.get("outcomes") == {name: "passed"}
                          and not result.get("disabled", False)
                          and case.get("status") == "run"
                          and all(case.find(tag) is None
                                  for tag in ("skipped", "failure", "error")))
                if passed:
                    self.assertRegex(raw, r"Exit status:\s*0\s*$")
                    successful.add(name)
                    # MPI rank sums are not a single process's RSS. Qualifying
                    # wrapper peaks need independent local process observations.
                    if int(peak[1]) >= 1572864:
                        processes = json.loads((path.parent / "process-peaks.json").read_text())
                        self.assertTrue(any(process["peak_kib"] >= 1572864
                                            for process in processes.values()),
                                        "high RSS lacks per-process corroboration: " + name)
                elif (not properties(pinned[name]).get("DISABLED", False)
                      and case.find("skipped") is None and int(peak[1]) >= 1572864):
                    high_failed.add(name)
        self.assertEqual(recorded, set(pinned),
                         "every pinned registration needs a raw profiling disposition; "
                         "skips/disabled entries do not qualify as successful measurements")
        self.assertFalse(high_failed - successful,
                         "high-RSS candidates remain unresolved without a successful pinned "
                         "Release measurement: " + str(sorted(high_failed - successful)))

    def test_runtime_evidence(self):
        # Read the actual full-suite logs/JUnit, independently of report.json's
        # summaries (older summaries incorrectly counted disabled cases as passed).
        # This is manual: registering a full-suite runner would recurse into itself.
        self.assertIsNotNone(args.runtime_evidence, "provide --runtime-evidence suite-log-root")
        root = args.runtime_evidence
        outcomes, durations = {}, {}
        self.assertTrue(self.highmem, "full runtime acceptance needs a measured selection")
        expected_red = {"ctest_highmem_selection", "ctest_highmem_shared_lock"}
        baseline_inventory = json.loads((root / "baseline-inventory.json").read_text())
        self.assertEqual({test["name"] for test in baseline_inventory["tests"]}, set(self.tests))
        with self.subTest(requirement="unchanged Release cache"):
            self.assertEqual((root / "baseline-cache.txt").read_text(), cache,
                             "runtime evidence used different build options or dependencies")
        for test in baseline_inventory["tests"]:
            if test["name"] in self.highmem:
                with self.subTest(test=test["name"], requirement="unlocked baseline"):
                    self.assertNotIn("chipcae_highmem", properties(test).get("RESOURCE_LOCK", []))
                    self.assertNotIn("highmem", properties(test).get("LABELS", []))
        for name, jobs in (("baseline-j4", 4), ("after-j1", 1),
                           ("after-j4-1", 4), ("after-j4-2", 4)):
            destination = root / name
            command = json.loads((destination / "command.json").read_text())
            log = (destination / "ctest.log").read_text()
            cases = ET.parse(destination / "results.xml").findall(".//testcase")
            results = {}
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
                self.assertNotIn(case.get("name"), results, "duplicate JUnit outcome")
                results[case.get("name")] = outcome
            outcomes[name] = results
            elapsed = re.search(r"Total Test time \(real\) =\s*([\d.]+) sec", log)
            self.assertIsNotNone(elapsed, "missing CTest duration: " + name)
            durations[name] = float(elapsed[1])
            with self.subTest(run=name, requirement="full inventory and command"):
                self.assertEqual(set(results), set(self.tests))
                self.assertEqual(command[:4], ["ctest", "--test-dir", args.build, "-j" + str(jobs)])
                self.assertFalse(any(flag in command for flag in
                                     ("-R", "-E", "-L", "-LE", "--rerun-failed", "--stop-on-failure")),
                                 "runtime acceptance must execute the full inventory")
            with self.subTest(run=name, requirement="no unexpected failures"):
                failed = {test for test, outcome in results.items()
                          if outcome != "passed" and outcome != "disabled"
                          and not outcome.startswith("skipped:")}
                self.assertFalse(failed - (expected_red if name == "baseline-j4" else set()),
                                 "unexpected full-suite failures: " + str(sorted(failed)))
            active, maximum, overlaps, completed = set(), 0, [], set()
            for line in log.splitlines():
                start = re.match(r"\s*Start\s+\d+:\s+(\S+)\s*$", line)
                end = re.match(r"\s*\d+/\d+ Test\s+#\d+:\s+(\S+)\s+", line)
                if start:
                    test = start[1]
                    if test in self.highmem and active & self.highmem:
                        overlaps.append((test, sorted(active & self.highmem)))
                    active.add(test)
                    maximum = max(maximum, len(active))
                if end:
                    if end[1] in active:
                        completed.add(end[1])
                    active.discard(end[1])
            with self.subTest(run=name, requirement="observed concurrency"):
                self.assertFalse(active, "missing completion events")
                if jobs == 4:
                    self.assertGreaterEqual(maximum, 4,
                                            "four-way execution was not observed; requested -j4 "
                                            "does not prove runtime acceptance")
                else:
                    self.assertEqual(maximum, 1)
            if name != "baseline-j4":
                with self.subTest(run=name, requirement="highmem intervals"):
                    self.assertFalse(overlaps, "selected wrappers overlap: " + str(overlaps))
                    self.assertTrue(self.highmem <= completed)
                    self.assertTrue(all(results[test] == "passed" for test in self.highmem),
                                    "selected wrappers must execute and pass")
            with self.subTest(run=name, requirement="complete kernel coverage and no OOM"):
                before = (destination / "kernel-before.log").read_text()
                after = (destination / "kernel-after.log").read_text()
                self.assertTrue(before, "empty initial kernel snapshot")
                self.assertTrue(after, "empty final kernel snapshot")
                stream_path = destination / "kernel-follow.log"
                self.assertTrue(stream_path.is_file(),
                                "continuous kernel evidence missing; snapshots alone cannot "
                                "establish the required coverage: " + name)
                stream = stream_path.read_text()
                self.assertNotRegex(stream, r"(?m)^dmesg:", "kernel collector reported an error")
                self.assertIn(before, stream, "kernel stream misses its initial snapshot")
                self.assertIn(after, stream, "kernel stream misses its final snapshot")
                start, end = stream.index(before), stream.rindex(after)
                self.assertLessEqual(start, end, "kernel snapshot anchors are out of order")
                # The continuous stream bridges ring-buffer rotation; compare
                # anchors before scanning only records from this CTest run.
                delta = stream[start + len(before):end + len(after)]
                self.assertFalse([line for line in delta.splitlines()
                                  if "chipcae" in line.lower()
                                  and re.search(r"oom|out of memory|killed process", line, re.I)],
                                 "new chipcae OOM records")
        for name in ("baseline-j4", "after-j4-1", "after-j4-2"):
            with self.subTest(run=name, requirement="serial outcomes including skip/disabled"):
                expected = dict(outcomes["after-j1"])
                actual = dict(outcomes[name])
                if name == "baseline-j4":
                    for test in expected_red:
                        expected.pop(test, None)
                        actual.pop(test, None)
                self.assertEqual({test: (expected.get(test), actual.get(test))
                                  for test in expected.keys() | actual.keys()
                                  if expected.get(test) != actual.get(test)}, {},
                                 "per-test serial/parallel outcomes differ")
        for name in ("after-j4-1", "after-j4-2"):
            with self.subTest(run=name, requirement="parallel duration"):
                self.assertGreater(durations["baseline-j4"], 0)
                self.assertLessEqual(durations[name], durations["baseline-j4"] * 1.20)

    def test_measured_evidence(self):
        # Read the raw profiling artifacts, not a hand-maintained allowlist.
        # Failed/killed/skipped runs and current-revision peaks cannot justify
        # selecting a test in place of a successful pinned Release measurement.
        self.assertIsNotNone(args.measurements, "provide --measurements raw-evidence-root")
        self.assertIsNotNone(args.pr_body, "provide --pr-body pr-body.md")
        self.assertIsNotNone(args.selection, "provide --selection measured-tests.txt")
        self.assertTrue(self.highmem, "full measured acceptance needs a nonempty selection")
        selection = set(args.selection.read_text().splitlines())
        peaks = {}
        for path in sorted(args.measurements.rglob("result.json")):
            result = json.loads(path.read_text())
            name = result.get("name")
            if (result.get("revision") != "eaed3e4" or result.get("status") != 0
                    or result.get("outcomes") != {name: "passed"}
                    or result.get("disabled", False)):
                continue
            # A summary's pass flag and revision alone cannot establish a
            # successful Release run. Reuse the profiler's saved cache/JUnit.
            measured_cache = (path.parent.parent / "cache.txt").read_text()
            if not re.search(r"^CMAKE_BUILD_TYPE:STRING=Release$", measured_cache, re.M):
                continue
            cases = ET.parse(path.parent / "results.xml").findall(".//testcase")
            if (len(cases) != 1 or cases[0].get("name") != name
                    or cases[0].get("status") != "run"
                    or any(cases[0].find(tag) is not None
                           for tag in ("skipped", "failure", "error"))):
                continue
            raw = (path.parent / "time.txt").read_text()
            peak = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", raw)
            self.assertIsNotNone(peak, "missing GNU time RSS: " + str(path))
            self.assertRegex(raw, r"Exit status:\s*0\s*$")
            kib = int(peak[1])
            self.assertEqual(kib, result["time_peak_kib"], str(path))
            if kib >= 1572864:
                peaks.setdefault(name, set()).add(kib)
        with self.subTest(evidence="pinned coverage"):
            self.assertFalse(self.highmem - peaks.keys(),
                             "locked tests lack successful eaed3e4 RSS >= 1.5 GiB: "
                             + str(sorted(self.highmem - peaks.keys())))
        with self.subTest(evidence="measured selection"):
            self.assertEqual(self.highmem, set(peaks),
                             "every successfully measured high-memory test must be locked")
        with self.subTest(evidence="selection file"):
            self.assertEqual(selection, set(peaks),
                             "selection file must equal successful pinned Release measurements")
        with self.subTest(evidence="selection lock membership"):
            self.assertEqual(selection, {
                name for name, props in self.props.items()
                if "chipcae_highmem" in props.get("RESOURCE_LOCK", [])
            })
        rows = {}
        peak_column = None
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
        self.assertEqual(set(rows), self.highmem, "PR table must equal the CTest selection")
        self.assertEqual(set(rows), selection, "PR table must equal the measured selection file")
        for name, kib in rows.items():
            with self.subTest(name=name):
                self.assertIn(kib, peaks.get(name, set()),
                              "PR RSS must match successful pinned raw output: " + name)


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
import json
from pathlib import Path
import re
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


def run(name, jobs, selected, baseline=False):
    destination = args.evidence / name
    # Do not overwrite the baseline or raw evidence from a previous attempt.
    destination.mkdir()
    before = kernel()
    (destination / "kernel-before.log").write_text(before)
    command = ["ctest", "--test-dir", str(args.build), "-j" + str(jobs),
               "--output-on-failure", "--output-junit", str(destination / "results.xml")]
    (destination / "command.json").write_text(json.dumps(command, indent=2))
    active = set()
    maximum = 0
    overlaps = []
    intervals = {}
    started = time.monotonic()
    with (destination / "ctest.log").open("w") as log:
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
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
    after = kernel()
    (destination / "kernel-after.log").write_text(after)
    retained = after.startswith(before)
    delta = after[len(before):] if retained else ""
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
        for tag in ("skipped", "failure", "error"):
            child = case.find(tag)
            if child is not None:
                outcome = tag + ":" + child.get("message", "")
        outcomes[case.attrib["name"]] = outcome
    report = dict(build=str(args.build), jobs=jobs, status=status,
                  elapsed=float(elapsed[1]), outcomes=outcomes,
                  kernel_retained=retained, oom=oom, max_active=maximum,
                  overlaps=overlaps, observed_intervals=intervals)
    (destination / "report.json").write_text(json.dumps(report, indent=2))
    require(retained, "kernel log rotated; OOM delta is unverified: " + name)
    require(not oom, "new chipcae OOM records: " + name)
    failed = {test for test, value in outcomes.items()
              if value != "passed" and not value.startswith("skipped:")}
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
