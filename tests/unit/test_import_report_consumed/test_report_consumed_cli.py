"""Exercise consumed-report filtering through the real IPC importer.

Snapshots live in the C++ acceptance test so both entry points share the exact
public-fixture expectations, including all eleven serialized fields. The original
snapshots retain the explicit consumed deletions; A expectations follow the public
XML whitelist audit (88 kicad4l / 51 kicad10 promotions, Circle instances only).
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(os.environ.get("CHIPCAE_SOURCE_DIR", Path(__file__).resolve().parents[3]))
DATA = ROOT / "tests/integration/ecad/data"
MAPPING = ROOT / "tests/integration/import_cli/material_map.json"
SNAPSHOTS = Path(__file__).with_name("test_report_consumed.cpp")
FIXTURES = {
    "kicad4l": DATA / "odbpp/kicad4l.ipc2581.xml",
    "kicad10": DATA / "kicad_like/board_kicad10/board.xml",
}
# Captured at adcbe340aa73b06daccc271809e8d3cf26c33eef, identical output names.
KICAD10_HASHES = {
    "stack.json": "fac500f5730339ccbac8ce7dd33c61f873c9f803bb8fe54940c6265b5261bc9a",
    "board.polyset": "f3c6ebf37498b9005a71716686b6ce8a4a7021025a63a4c309db80e59892b04f",
    "import_layers.json": "fdef57a3f12c521a1f460a1584426a85bcba0bd6524409c55f0eaddf5c0a023c",
}


def baseline(name, accept):
    source = SNAPSHOTS.read_text(encoding="utf-8")
    mode = "accepted" if accept else "rejected"
    block = source.split(f"// BEGIN {name} {mode}", 1)[1].split(
        f"// END {name} {mode}", 1)[0]
    report = json.loads(re.search(r'R"REPORT\((.*?)\)REPORT"', block, re.S).group(1))
    deletion_block = re.search(
        rf"const std::vector<Deletion> {name}_deletions = \{{(.*?)\n\}};",
        source, re.S).group(1)
    deleted = set(re.findall(r'\{\d+, "([^"]+)"\}', deletion_block))
    before = {d["defect_id"]: d for d in report["defects"]}
    assert deleted <= before.keys(), "frozen deletion IDs must exist in baseline"
    for defect_id in deleted:
        report["tier_counts"][before[defect_id]["tier"]] -= 1
    report["defects"] = [d for d in report["defects"] if d["defect_id"] not in deleted]
    return report, deleted


def invoke(executable, directory, name, accept):
    directory.mkdir()
    command = [str(executable), str(FIXTURES[name]), "-o", str(directory / "stack.json"),
               "--polyset", str(directory / "board.polyset"),
               "--report", str(directory / "defects.json"), "--material-map", str(MAPPING)]
    if accept:
        command.append("--accept-b-tier")
    process = subprocess.run(command, capture_output=True, text=True, timeout=45)
    report = json.loads((directory / "import_report.json").read_text(encoding="utf-8"))
    public = json.loads((directory / "defects.json").read_text(encoding="utf-8"))
    return process, report, public


class ConsumedReportCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="report-consumed-cli-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.runs = {}
        for name in FIXTURES:
            for accept in (False, True):
                directory = cls.directory / f"{name}-{accept}"
                cls.runs[name, accept] = invoke(EXECUTABLE, directory, name, accept)

    def test_missing_or_nonexecutable_importer_fails_without_skip(self):
        environment = dict(os.environ)
        environment.pop("CHIPCAE_IMPORT_EXE", None)
        candidates = [None, self.directory / "missing-importer", self.directory]
        nonexecutable = self.directory / "not-executable"
        nonexecutable.write_text("not an importer", encoding="utf-8")
        nonexecutable.chmod(0o600)
        if not os.access(nonexecutable, os.X_OK):
            candidates.append(nonexecutable)
        for candidate in candidates:
            with self.subTest(executable=candidate):
                if candidate is not None:
                    environment["CHIPCAE_IMPORT_EXE"] = str(candidate)
                process = subprocess.run(
                    [sys.executable, str(Path(__file__).resolve())], env=environment,
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(process.returncode, 1, process.stderr)
                self.assertIn("CHIPCAE_IMPORT_EXE must name an executable importer", process.stderr)

    def test_original_fixture_rejection_and_unsupported_ovals_are_unchanged(self):
        for accept in (False, True):
            with self.subTest(accept=accept):
                process, report, public = self.runs["kicad4l", accept]
                self.assertEqual(process.returncode, 7, process.stderr)
                self.assertEqual(process.stderr, "IMPORT_REJECTED: unsupported board geometry\n")
                self.assertEqual(public["defects"], [])
                expected, _ = baseline("kicad4l", accept)
                retained_ids = {"board/DictionaryStandard/31", "board/EntryStandard/32",
                                "board/EntryStandard/35", "board/Oval/33", "board/Oval/36",
                                "board/Oval/1379"}
                self.assertEqual([d for d in report["defects"] if d["defect_id"] in retained_ids],
                                 [d for d in expected["defects"] if d["defect_id"] in retained_ids])
                self.assertEqual(report["tier_counts"]["C"], 3)
                directory = self.directory / f"kicad4l-{accept}"
                self.assertFalse((directory / "stack.json").exists())
                self.assertFalse((directory / "board.polyset").exists())

    def test_exact_surviving_defects_and_every_tier_count(self):
        for name in FIXTURES:
            for accept in (False, True):
                with self.subTest(fixture=name, accept=accept):
                    _, report, public = self.runs[name, accept]
                    expected, _ = baseline(name, accept)
                    self.assertEqual(public["defects"], [])
                    for field in ("defects", "tier_counts"):
                        with self.subTest(field=field):
                            self.assertEqual(report[field], expected[field])
                    self.assertTrue(all(len(d) == 11 and "size_unit" in d
                                        for d in report["defects"]))

    def test_explicit_consumed_ids_are_absent(self):
        for name in FIXTURES:
            for accept in (False, True):
                _, report, _ = self.runs[name, accept]
                _, deleted = baseline(name, accept)
                actual = {d["defect_id"] for d in report["defects"]}
                with self.subTest(fixture=name, accept=accept):
                    self.assertEqual(actual & deleted, set(), "consumed element IDs remain")

    def test_existing_successful_fixture_retains_geometry_bytes(self):
        # kicad4l's three unsupported Ovals prevent successful output even with B accepted.
        # Exercise the other authorized public fixture without altering either source.
        process, _, public = self.runs["kicad10", True]
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(public["defects"], [])
        directory = self.directory / "kicad10-True"
        for name, digest in KICAD10_HASHES.items():
            with self.subTest(artifact=name):
                self.assertEqual(hashlib.sha256((directory / name).read_bytes()).hexdigest(), digest)
        rejected, _, _ = self.runs["kicad10", False]
        self.assertEqual(rejected.returncode, 6, rejected.stderr)


if __name__ == "__main__":
    configured = os.environ.get("CHIPCAE_IMPORT_EXE")
    if not configured or not Path(configured).is_file() or not os.access(configured, os.X_OK):
        print("CHIPCAE_IMPORT_EXE must name an executable importer", file=sys.stderr)
        sys.exit(1)
    EXECUTABLE = Path(configured)
    unittest.main(argv=[sys.argv[0]])
