"""Exercise metadata B-to-A reclassification through the real importer.

Frozen public/synthetic snapshots were measured before any production change.
The C++ test owns the complete eleven-field baseline and explicit promotion IDs.
"""
from collections import Counter
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
SNAPSHOTS = Path(__file__).with_name("test_metadata_tier.cpp")
BASELINE_SHA = "51600d4a9e89521e0874e5933c4bed8a293e512d"
ACTIONS = ("non-physical metadata ignored", "definition expressed by modeled instances")
DEFECT_KEYS = {"defect_id", "tier", "kind", "reason", "dimension", "size", "size_unit",
               "suggestion", "action", "rejection_reason", "material_ambiguous"}
BASELINE_ID_SHA256 = {
    "kicad4l": "cc82f9df7da379dbf4191e048111b50446eb981225343ced49af0304ee8da783",
    "definitions": "01d30642e3493fadda567686fff4ea8299ac242ce8cde691c43d2456adf86b23",
}
BASELINE_REPORT_SHA256 = {
    ("kicad4l", False): "352354eee52ae95f9c0b169bfabf92efdd4f22c11bf4dfdce4cb553a373e65dc",
    ("kicad4l", True): "992945e6836e0866dbc304932ee6d403e825632c5199efdfa01739d9dcbe36ae",
    ("definitions", False): "e44e2551bb7fec57e4f54a1b9ce6ee471d32e5775d7bd5d319cdcac9f05f5cc0",
    ("definitions", True): "4b31a0728aef9ae0c31446fd88edfe7a85cad7cb11d84ba6793a5261115b79c0",
}
ARTIFACT_SHA256 = {
    "stack.json": "049aa7bac38f3161cb28d419dce4a8653247d07a86bbcace1977ab6b0cd3db4b",
    "board.polyset": "e22082e4d3775aac3e97c01a16552f5948bcd20b154f6a67189e01b0814696aa",
    "import_layers.json": "ac0de18e34ce03a1582683541b251cf31f510618b7ce6adee34a3f27d97e460f",
}
FIXTURE_PROMOTIONS = {'DictionaryUser': 1, 'EntryUser': 2, 'PadstackPadDef': 8, 'StandardPrimitiveRef': 2, 'Avl': 1, 'AvlHeader': 1, 'AvlItem': 1, 'AvlRef': 1, 'BomHeader': 1, 'BomItem': 1, 'BomRef': 1, 'Certification': 1, 'Characteristics': 1, 'Conductor': 4, 'Datum': 1, 'DictionaryColor': 1, 'DictionaryFillDesc': 1, 'Dielectric': 8, 'Enterprise': 1, 'EntryLineDesc': 1, 'Features': 1, 'FileRevision': 1, 'FunctionMode': 1, 'LayerRef': 13, 'NonstandardAttribute': 1, 'Person': 1, 'PickupPoint': 1, 'PinRef': 12, 'Property': 25, 'RefDes': 1, 'Role': 1, 'SoftwarePackage': 1, 'StepRef': 2, 'Textual': 1}
DEFINITIONS_XML = '''<IPC-2581>
<DictionaryStandard><EntryStandard id="disk"><Circle diameter="1"/></EntryStandard></DictionaryStandard><DictionaryUser><EntryUser id="user"><UserSpecial><Circle diameter="1"/></UserSpecial></EntryUser></DictionaryUser><DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>

<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="metadata">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline><PickupPoint x="0" y="0"/><OtherSideView/><Pin number="1"><Location x="0" y="0"/><StandardPrimitiveRef id="disk"/></Pin></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component><PadStackDef name="PS"><PadstackPadDef layerRef="Cu"><StandardPrimitiveRef id="disk"/><UserPrimitiveRef id="user"/></PadstackPadDef></PadStackDef><LayerFeature layerRef="Cu"><Set><Pad padstackDefRef="PS"><Location x="2" y="2"/><StandardPrimitiveRef id="disk"/><PinRef componentRef="J1" pin="1"/></Pad></Set></LayerFeature>
</Step></CadData></IPC-2581>
'''

def frozen(name):
    source = SNAPSHOTS.read_text(encoding="utf-8")
    block = source.split(f"// BEGIN {name} accepted", 1)[1].split(
        f"// END {name} accepted", 1)[0]
    match = re.search(r'R"REPORT\((.*?)\)REPORT"', block, re.S)
    if match is None:
        raise AssertionError(f"missing frozen report for {name}")
    report = json.loads(match.group(1))
    block = source.split(f"// BEGIN {name} promotions", 1)[1].split(
        f"// END {name} promotions", 1)[0]
    promoted = dict(re.findall(r'\{"([^"]+)",\s*"([^"]+)"\}', block))
    return report, promoted


def expected_report(name, accept):
    report, promoted = frozen(name)
    report["b_tier_accepted"] = "all" if accept else None
    for defect in report["defects"]:
        key = promoted.get(defect["defect_id"])
        if key is not None:
            assert defect["tier"] == "B", "promotion ID must be B in frozen baseline"
            action = ACTIONS[1] if key == "def-instanced" else ACTIONS[0]
            defect.update(tier="A", suggestion=action, action=action)
        elif not accept and defect["tier"] == "B":
            defect.update(action="", rejection_reason="tier B defect requires explicit confirmation")
    report["tier_counts"]["A"] += len(promoted)
    report["tier_counts"]["B"] -= len(promoted)
    return report


def invoke(executable, source, directory, accept):
    directory.mkdir()
    command = [str(executable), str(source), "-o", str(directory / "stack.json"),
               "--polyset", str(directory / "board.polyset"),
               "--report", str(directory / "defects.json"), "--material-map", str(MAPPING)]
    if accept:
        command.append("--accept-b-tier")
    process = subprocess.run(command, capture_output=True, text=True, timeout=45)
    report = json.loads((directory / "import_report.json").read_text(encoding="utf-8"))
    public = json.loads((directory / "defects.json").read_text(encoding="utf-8"))
    return process, report, public


class MetadataTierCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="metadata-tier-cli-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        synthetic = cls.directory / "definitions.xml"
        synthetic.write_text(DEFINITIONS_XML, encoding="utf-8")
        cls.runs = {}
        for name, source in (("kicad4l", DATA / "odbpp/kicad4l.ipc2581.xml"),
                             ("definitions", synthetic)):
            for accept in (False, True):
                cls.runs[name, accept] = invoke(
                    EXECUTABLE, source, cls.directory / f"{name}-{accept}", accept)
        cls.unsupported = invoke(EXECUTABLE, DATA / "unsupported.xml",
                                 cls.directory / "unsupported", False)

    def test_00_frozen_baseline_and_ordered_id_digests(self):
        self.assertIn(BASELINE_SHA, SNAPSHOTS.read_text(encoding="utf-8"))
        for name in ("kicad4l", "definitions"):
            report, _ = frozen(name)
            with self.subTest(fixture=name):
                ids = [d["defect_id"] for d in report["defects"]]
                self.assertEqual(hashlib.sha256(json.dumps(ids, separators=(",", ":")).encode()).hexdigest(),
                                 BASELINE_ID_SHA256[name])
                self.assertEqual(report["tier_counts"]["A"], 0)
                # Verify the complete original CLI bytes encoded in our frozen constants.
                for accept in (False, True):
                    original = json.loads(json.dumps(report))
                    original["b_tier_accepted"] = "all" if accept else None
                    if not accept:
                        for defect in original["defects"]:
                            if defect["tier"] == "B":
                                defect.update(action="", rejection_reason=
                                              "tier B defect requires explicit confirmation")
                    serialized = json.dumps(original, separators=(",", ":")) + "\n"
                    self.assertEqual(hashlib.sha256(serialized.encode()).hexdigest(),
                                     BASELINE_REPORT_SHA256[name, accept])

    def test_missing_or_nonexecutable_importer_fails_without_skip(self):
        environment = dict(os.environ)
        environment.pop("CHIPCAE_IMPORT_EXE", None)
        nonexecutable = self.directory / "not-executable"
        nonexecutable.write_text("not an importer", encoding="utf-8")
        nonexecutable.chmod(0o600)
        for candidate in (None, self.directory / "missing", self.directory, nonexecutable):
            with self.subTest(executable=candidate):
                if candidate is not None:
                    environment["CHIPCAE_IMPORT_EXE"] = str(candidate)
                process = subprocess.run([sys.executable, str(Path(__file__).resolve())],
                                         env=environment, capture_output=True, text=True, timeout=10)
                self.assertEqual(process.returncode, 1, process.stderr)
                self.assertIn("CHIPCAE_IMPORT_EXE must name an executable importer", process.stderr)

    def test_ordered_ids_and_all_eleven_fields(self):
        for (name, accept), (_, actual, public) in self.runs.items():
            original, promoted = frozen(name)
            wanted = expected_report(name, accept)
            with self.subTest(fixture=name, accepted=accept):
                self.assertEqual(public["defects"], [])
                ids = [d["defect_id"] for d in actual["defects"]]
                self.assertEqual(ids, [d["defect_id"] for d in original["defects"]])
                self.assertEqual(hashlib.sha256(json.dumps(ids, separators=(",", ":")).encode()).hexdigest(),
                                 BASELINE_ID_SHA256[name])
            for before, after, expected in zip(original["defects"], actual["defects"], wanted["defects"]):
                with self.subTest(fixture=name, accepted=accept, defect=before["defect_id"]):
                    self.assertEqual(set(after), DEFECT_KEYS)
                    self.assertNotIn("context", after)
                    self.assertEqual(after, expected)
                    if accept and before["defect_id"] in promoted:
                        changed = {key for key in DEFECT_KEYS if before[key] != after[key]}
                        self.assertEqual(changed, {"tier", "suggestion", "action"})
                    elif accept:
                        self.assertEqual(after, before)
            with self.subTest(fixture=name, accepted=accept, field="complete report"):
                self.assertEqual(actual, wanted)

    def test_actual_tier_deltas_and_per_element_counts(self):
        for name in ("kicad4l", "definitions"):
            _, actual, _ = self.runs[name, True]
            original, promoted = frozen(name)
            before = original["tier_counts"]
            after = actual["tier_counts"]
            changed = [row for row in actual["defects"] if row["tier"] == "A"]
            with self.subTest(fixture=name):
                self.assertEqual(len(changed), len(promoted), "required B-to-A reclassifications missing")
                self.assertEqual(after["A"] - before["A"], len(changed))
                self.assertEqual(before["B"] - after["B"], len(changed))
                self.assertEqual((after["Weld"], after["C"]), (before["Weld"], before["C"]))
                self.assertEqual(after, expected_report(name, True)["tier_counts"])
            if name == "kicad4l":
                with self.subTest(fixture=name, field="per-element counts"):
                    self.assertEqual(dict(Counter(d["defect_id"].split("/")[1] for d in changed)),
                                     FIXTURE_PROMOTIONS)

    def test_original_kicad_ovals_still_reject_and_preserve_absent_artifacts(self):
        for accept in (False, True):
            process, report, _ = self.runs["kicad4l", accept]
            with self.subTest(accepted=accept):
                self.assertEqual(process.returncode, 7, process.stderr)
                self.assertEqual(process.stderr, "IMPORT_REJECTED: unsupported board geometry\n")
                self.assertEqual(report["tier_counts"]["C"], 1)
                directory = self.directory / f"kicad4l-{accept}"
                for name in ARTIFACT_SHA256:
                    self.assertFalse((directory / name).exists())

    def test_successful_synthetic_geometry_bytes_match_baseline(self):
        # The public fixture cannot emit geometry because of its three C Ovals.
        # This inline input exercises all definition forms using a genuinely modeled Circle.
        process, _, public = self.runs["definitions", True]
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(public["defects"], [])
        directory = self.directory / "definitions-True"
        for name, digest in ARTIFACT_SHA256.items():
            with self.subTest(artifact=name):
                self.assertEqual(hashlib.sha256((directory / name).read_bytes()).hexdigest(), digest)

    def test_whitelisted_metadata_no_longer_requires_b_confirmation(self):
        process, report, _ = self.runs["definitions", False]
        self.assertEqual(process.returncode, 0, "all ten synthetic metadata/definition defects should "
                         f"be A and import without confirmation; got {process.returncode}: {process.stderr}")
        self.assertEqual(report["tier_counts"], {"Weld": 0, "A": 10, "B": 0, "C": 0})
        directory = self.directory / "definitions-False"
        for name, digest in ARTIFACT_SHA256.items():
            self.assertEqual(hashlib.sha256((directory / name).read_bytes()).hexdigest(), digest)

    def test_unknown_elements_keep_exact_unsupported_goldens_and_exit_six(self):
        process, report, public = self.unsupported
        self.assertEqual(process.returncode, 6, process.stderr)
        self.assertEqual(report["tier_counts"], {"Weld": 0, "A": 0, "B": 3, "C": 0})
        self.assertEqual([d["defect_id"] for d in report["defects"]],
                         ["board/MysteryA/2", "board/MysteryB/2", "board/MysteryC/2"])
        self.assertEqual(public["defects"], [])
        golden = ROOT / "tests/integration/import_cli/golden/unsupported"
        directory = self.directory / "unsupported"
        for name in ("defects.json", "import_report.json", "import_report.csv"):
            with self.subTest(golden=name):
                self.assertEqual((directory / name).read_bytes(), (golden / name).read_bytes())
        self.assertEqual(process.stdout.encode(), (golden / "stdout").read_bytes())
        self.assertEqual(process.stderr.encode(), (golden / "stderr").read_bytes())
        self.assertEqual(str(process.returncode), (golden / "exit_code").read_text().strip())


if __name__ == "__main__":
    configured = os.environ.get("CHIPCAE_IMPORT_EXE")
    if not configured or not Path(configured).is_file() or not os.access(configured, os.X_OK):
        print("CHIPCAE_IMPORT_EXE must name an executable importer", file=sys.stderr)
        sys.exit(1)
    EXECUTABLE = Path(configured).resolve()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
