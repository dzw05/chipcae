"""Oval acceptance through the real CLI, with an explicit, ordered report delta.

The residual test owns the frozen pre-Oval report. Only the IDs listed below may
be removed/promoted; every other field and the surviving order must stay intact.
"""
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(os.environ.get("CHIPCAE_SOURCE_DIR", Path(__file__).resolve().parents[3]))
DATA = ROOT / "tests/integration/ecad/data"
MAPPING = ROOT / "tests/integration/import_cli/material_map.json"
DEFINITION = "definition expressed by modeled instances"
REMOVED = {
    "board/DictionaryStandard/31",  # consumed once both Oval entries are consumed
    "board/EntryStandard/32", "board/Oval/33",
    "board/EntryStandard/35", "board/Oval/36",
    *{f"board/StandardPrimitiveRef/{line}" for line in
      (591, 596, 835, 840, 1079, 1084, 1339, 1344)},
}
PROMOTED = {
    "board/DictionaryUser/39", "board/EntryUser/40", "board/EntryUser/48",
    "board/StandardPrimitiveRef/358", "board/StandardPrimitiveRef/362",
    *{f"board/PadstackPadDef/{line}" for line in (278, 282, 286, 290, 297, 301, 305, 309)},
}
SYNTHETIC = '''<IPC-2581>
<CadHeader units="MILLIMETER"/>
<DictionaryStandard><EntryStandard id="shape">
<Oval width="3" height="4"/>
</EntryStandard></DictionaryStandard>
<CadData><Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup>
<StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="copper"/>
</StackupGroup></Stackup>
<Step name="oval">
<Profile><Polygon><PolyBegin x="0" y="-40"/>
<PolyStepSegment x="40" y="-40"/><PolyStepSegment x="40" y="0"/>
<PolyStepSegment x="0" y="0"/></Polygon></Profile>
<LayerFeature layerRef="Cu"><Set><Pad padstackDefRef="PS">
<Location x="10" y="-20"/>
<StandardPrimitiveRef id="shape"/>
</Pad></Set></LayerFeature>
</Step></CadData></IPC-2581>
'''


def frozen_report():
    # Load data from the frozen literal, not expected(), which may be reconciled
    # when the feature is implemented. Apply only its explicit residual whitelist.
    source = ROOT / ("tests/unit/test_import_metadata_tier_residual/"
                     "test_metadata_tier_residual_cli.py")
    spec = importlib.util.spec_from_file_location("oval_residual_snapshot", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    report = copy.deepcopy(module.FROZEN["kicad4l"]["report"])
    residual_ids = {
        "board/StepRef/72", *{f"board/Property/{line}" for line in
                              (86, 87, 92, 98, 99, 116, 136, 156, 176, 177, 185, 191, 192)},
    }
    for defect in report["defects"]:
        if defect["defect_id"] in residual_ids:
            defect.update(tier="A", suggestion="non-physical metadata ignored",
                          action="non-physical metadata ignored")
    report["tier_counts"] = {"Weld": 0, "A": 88, "B": 39, "C": 3}
    return report


def expected_report(accept):
    report = frozen_report()
    indexed = {d["defect_id"]: d for d in report["defects"]}
    assert REMOVED <= indexed.keys() and PROMOTED <= indexed.keys()
    assert len(REMOVED) == 13 and len(PROMOTED) == 13
    for identifier in PROMOTED:
        assert indexed[identifier]["tier"] == "B", identifier
    report["defects"] = [d for d in report["defects"] if d["defect_id"] not in REMOVED]
    for defect in report["defects"]:
        if defect["defect_id"] in PROMOTED:
            defect.update(tier="A", suggestion=DEFINITION, action=DEFINITION,
                          rejection_reason="")
        if defect["tier"] == "B" and not accept:
            defect.update(action="", rejection_reason="tier B defect requires explicit confirmation")
    report["tier_counts"] = {"Weld": 0, "A": 101, "B": 15, "C": 1}
    report["b_tier_accepted"] = "all" if accept else None
    return report


def invoke(executable, source, directory, accept=True):
    directory.mkdir()
    command = [str(executable), str(source), "-o", str(directory / "stack.json"),
               "--polyset", str(directory / "board.polyset"),
               "--report", str(directory / "defects.json"), "--material-map", str(MAPPING)]
    if accept:
        command.append("--accept-b-tier")
    process = subprocess.run(command, capture_output=True, text=True, timeout=45)
    report = json.loads((directory / "import_report.json").read_text(encoding="utf-8"))
    return process, report, directory


def polyset(path):
    """Read the public CCPOLY01 format independently of the importer writer."""
    data = path.read_bytes()
    if data[:8] != b"CCPOLY01":
        raise AssertionError("invalid polyset magic")
    dbu, count = struct.unpack_from("<dI", data, 8)
    offset = 20
    layers = {}
    for _ in range(count):
        length, = struct.unpack_from("<I", data, offset)
        offset += 4
        name = data[offset:offset + length].decode("utf-8")
        offset += length
        rings, = struct.unpack_from("<I", data, offset)
        offset += 4
        indices = struct.unpack_from(f"<{rings + 1}q", data, offset)
        offset += 8 * (rings + 1)
        coordinates = struct.unpack_from(f"<{2 * indices[-1]}q", data, offset)
        offset += 16 * indices[-1]
        points = list(zip(coordinates[::2], coordinates[1::2]))
        layers[name] = [points[a:b] for a, b in zip(indices, indices[1:])]
    if offset != len(data):
        raise AssertionError("unexpected trailing polyset bytes")
    return dbu, layers


class OvalPadCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="oval-pad-cli-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.fixture = {
            accept: invoke(EXECUTABLE, DATA / "odbpp/kicad4l.ipc2581.xml",
                           cls.directory / f"kicad4l-{accept}", accept)
            for accept in (False, True)
        }

    def test_kicad4l_exact_ordered_report_for_both_policies(self):
        for accept, (process, report, directory) in self.fixture.items():
            with self.subTest(accept_b=accept):
                self.assertEqual(process.returncode, 7, process.stderr)
                self.assertEqual(process.stderr, "IMPORT_REJECTED: unsupported board geometry\n")
                self.assertEqual(report, expected_report(accept))
                self.assertEqual(json.loads((directory / "defects.json").read_text())["defects"], [])
                self.assertFalse((directory / "board.polyset").exists())
                self.assertFalse((directory / "stack.json").exists())

    def test_kicad4l_each_removed_promoted_and_retained_record(self):
        _, report, _ = self.fixture[True]
        indexed = {d["defect_id"]: d for d in report["defects"]}
        for identifier in sorted(REMOVED):
            with self.subTest(removed=identifier):
                self.assertTrue(identifier not in indexed, f"consumed record remains: {identifier}")
        for identifier in sorted(PROMOTED):
            with self.subTest(promoted=identifier):
                self.assertIn(identifier, indexed)
                defect = indexed[identifier]
                self.assertEqual(defect["tier"], "A")
                self.assertEqual(defect["suggestion"], DEFINITION)
                self.assertEqual(defect["action"], DEFINITION)
                self.assertEqual(defect["rejection_reason"], "")
        expected = expected_report(True)
        unchanged = {d["defect_id"]: d for d in expected["defects"]
                     if d["defect_id"] not in PROMOTED}
        for identifier, defect in unchanged.items():
            with self.subTest(unchanged=identifier):
                self.assertEqual(indexed[identifier], defect)
        for line in (605, 610, 849, 854):
            self.assertIn(f"board/StandardPrimitiveRef/{line}", indexed)

    def test_kicad4l_only_slot_cavity_remains_c(self):
        _, report, _ = self.fixture[True]
        self.assertEqual([d["defect_id"] for d in report["defects"] if d["tier"] == "C"],
                         ["board/Oval/1379"])
        self.assertEqual(report["tier_counts"], {"Weld": 0, "A": 101, "B": 15, "C": 1})

    def test_synthetic_oval_success_and_258_vertex_copper_polyset(self):
        source = self.directory / "oval.xml"
        source.write_text(SYNTHETIC, encoding="utf-8")
        process, report, directory = invoke(EXECUTABLE, source, self.directory / "oval")
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(report["tier_counts"]["C"], 0)
        self.assertFalse(any(d["defect_id"].startswith(("board/Oval/", "board/EntryStandard/",
                                                       "board/StandardPrimitiveRef/"))
                             for d in report["defects"]))
        dbu, layers = polyset(directory / "board.polyset")
        self.assertAlmostEqual(dbu, 1e-6, delta=1e-15)
        self.assertEqual(set(layers), {"Cu"})
        self.assertEqual(len(layers["Cu"]), 1)
        points = layers["Cu"][0]
        self.assertEqual(len(points), 258)
        self.assertEqual((min(x for x, _ in points), max(x for x, _ in points),
                          min(y for _, y in points), max(y for _, y in points)),
                         (8500, 11500, -22000, -18000))
        area = abs(sum(x * v - y * u for (x, y), (u, v) in
                       zip(points, points[1:] + points[:1]))) / 2
        expected = 3_000_000 + math.pi * 1500 ** 2
        epsilon = (1 - 128 / math.pi * math.sin(math.pi / 128)
                   + (2000 + 3000 * math.pi) * math.sqrt(2) / (2 * expected))
        self.assertLessEqual(abs(area - expected) / expected, epsilon)

    def test_unknown_elements_keep_exact_goldens_and_exit_six(self):
        process, report, directory = invoke(EXECUTABLE, DATA / "unsupported.xml",
                                            self.directory / "unsupported", False)
        golden = ROOT / "tests/integration/import_cli/golden/unsupported"
        self.assertEqual(process.returncode, 6)
        self.assertEqual(report["tier_counts"], {"Weld": 0, "A": 0, "B": 3, "C": 0})
        for name in ("defects.json", "import_report.json", "import_report.csv"):
            with self.subTest(artifact=name):
                self.assertEqual((directory / name).read_bytes(), (golden / name).read_bytes())
        self.assertEqual(process.stdout, (golden / "stdout").read_text())
        self.assertEqual(process.stderr, (golden / "stderr").read_text())
        self.assertEqual(process.returncode, int((golden / "exit_code").read_text()))

    def test_missing_or_nonexecutable_importer_exits_one(self):
        environment = dict(os.environ)
        environment.pop("CHIPCAE_IMPORT_EXE", None)
        file = self.directory / "nonexecutable"
        file.write_text("not an importer", encoding="utf-8")
        file.chmod(0o600)
        for candidate in (None, self.directory / "missing", self.directory, file):
            with self.subTest(executable=candidate):
                if candidate is not None:
                    environment["CHIPCAE_IMPORT_EXE"] = str(candidate)
                process = subprocess.run([sys.executable, str(Path(__file__).resolve())],
                                         env=environment, capture_output=True, text=True, timeout=10)
                self.assertEqual(process.returncode, 1)
                self.assertIn("CHIPCAE_IMPORT_EXE must name an executable importer", process.stderr)


if __name__ == "__main__":
    value = os.environ.get("CHIPCAE_IMPORT_EXE")
    if not value or not Path(value).is_file() or not os.access(value, os.X_OK):
        print("CHIPCAE_IMPORT_EXE must name an executable importer", file=sys.stderr)
        sys.exit(1)
    EXECUTABLE = Path(value).resolve()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
