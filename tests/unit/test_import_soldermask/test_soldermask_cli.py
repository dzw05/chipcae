"""Exercise solder mask opt-in through the real single-board IPC importer."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests/integration/ecad/data/odbpp/kicad4l.ipc2581.xml"
MASK_IDS = {"board/SolderMask%3AF.Mask/0", "board/SolderMask%3AB.Mask/0"}
NOTICE = "solder mask modeled as full-coverage homogenized layer: "
ARTIFACTS = ("stack.json", "board.polyset", "import_report.json", "import_report.csv",
             "defects.json", "import_layers.json")
BASE_MAP = {"FR4": "core_eq", "copper": "cu"}


def invoke(executable, directory, mapping, accept=False, fixture=FIXTURE):
    directory.mkdir()
    map_file = directory / "map.json"
    map_file.write_text(json.dumps(mapping), encoding="utf-8")
    command = [str(executable), str(fixture), "-o", str(directory / "stack.json"),
               "--polyset", str(directory / "board.polyset"),
               "--report", str(directory / "defects.json"), "--material-map", str(map_file)]
    if accept:
        command.append("--accept-b-tier")
    process = subprocess.run(command, capture_output=True, timeout=45)
    return process, {name: (directory / name).read_bytes() if (directory / name).exists() else None
                     for name in ARTIFACTS}


def document(products, name):
    assert products[name] is not None, f"missing output {name}"
    return json.loads(products[name])


def ids(report):
    return {defect["defect_id"] for defect in report["defects"]}


def source_layers():
    """Read source keys to catch report rows shifted by inserted masks."""
    tree = ET.parse(FIXTURE)
    specs = {}
    for spec in tree.findall(".//{*}Spec"):
        for prop in spec.findall("./{*}General[@type='MATERIAL']/{*}Property"):
            if prop.get("text"):
                specs[spec.get("name")] = prop.get("text")
                break
    result = {}
    for entry in tree.findall(".//{*}StackupLayer"):
        ref = entry.find("{*}SpecRef")
        result[entry.get("layerOrGroupRef")] = entry.get(
            "matDes", specs.get(ref.get("id") if ref is not None else None, ""))
    return result


class SoldermaskCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = Path(os.environ["CHIPCAE_IMPORT_EXE"])
        cls.temporary = tempfile.TemporaryDirectory(prefix="soldermask-cli-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        # Preserve the original fixture in the repository. Its three unsupported
        # Ovals independently force exit 7; remove only those tags in a temporary
        # copy to exercise mask opt-in and B-tier confirmation with exits 0/6.
        # Replacing tags with spaces preserves source line numbers for defect IDs.
        text, removed = re.subn(r"<Oval\b[^>]*/>", lambda match: " " * len(match[0]),
                                FIXTURE.read_text(encoding="utf-8"))
        if removed != 3:
            raise AssertionError(f"expected exactly three pre-existing Ovals, found {removed}")
        cls.fixture = cls.directory / FIXTURE.name
        cls.fixture.write_text(text, encoding="utf-8")
        # Within this test process compare current-branch outputs. The brief's
        # pre-change byte comparison belongs to the separate acceptance harness.
        cls.unmapped = {}
        for accept in (False, True):
            cls.unmapped[accept] = invoke(cls.executable, cls.directory / f"unmapped-{accept}",
                                         BASE_MAP, accept, cls.fixture)

    def assert_no_masks(self, products):
        for name in ("import_report.json", "defects.json"):
            self.assertFalse(any(value.startswith("board/SolderMask%3A")
                                 for value in ids(document(products, name))), name)

    def assert_masks(self, products, original):
        layers = document(products, "stack.json")["layers"]
        self.assertEqual([layer["name"] for layer in layers],
                         ["F.Mask"] + [layer["name"] for layer in original] + ["B.Mask"])
        for mask in (layers[0], layers[-1]):
            self.assertEqual(mask["kind"], "Homogenized")
            self.assertEqual(mask["material"], "mask_eq")
            self.assertLessEqual(abs(mask["thickness"] / 1e-5 - 1), 1e-12)
        self.assertEqual(layers[1]["material"], "core_eq")
        self.assertEqual(layers[-2]["material"], "mask_eq")
        for restored, prior in zip(layers[1:-1], original):
            self.assertEqual(restored["name"], prior["name"])
            self.assertEqual(restored["kind"], prior["kind"])
            self.assertAlmostEqual(restored["thickness"], prior["thickness"], delta=1e-15)
            if prior["name"] != "B.Cu":
                self.assertEqual(restored["material"], prior["material"])

    def assert_defect_delta(self, products, baseline):
        report = document(products, "import_report.json")
        original = document(baseline, "import_report.json")
        self.assertEqual(ids(report), ids(original) - MASK_IDS)
        expected = dict(original["tier_counts"])
        expected["B"] -= 2
        self.assertEqual(report["tier_counts"], expected)
        self.assert_no_masks(products)

    def test_mapped_accepted_stack_warnings_and_report_alignment(self):
        process, products = invoke(self.executable, self.directory / "mapped-accepted",
                                   dict(BASE_MAP, SOLDERMASK="mask_eq"), True, self.fixture)
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        baseline = self.unmapped[True][1]
        original = document(baseline, "stack.json")["layers"]
        self.assert_masks(products, original)
        self.assert_defect_delta(products, baseline)
        notices = [line for line in process.stderr.decode().splitlines() if line.startswith(NOTICE)]
        self.assertEqual(notices, [NOTICE + "F.Mask", NOTICE + "B.Mask"])
        self.assertNotIn(NOTICE, process.stdout.decode())
        # Modeling notices belong only on stderr, including after layer-report publication.
        for name in ARTIFACTS:
            if products[name] is not None:
                self.assertNotIn(NOTICE.encode(), products[name], name)
        rows = [row for row in document(products, "import_layers.json")["layers"]
                if row["status"] == "kept"]
        layers = document(products, "stack.json")["layers"]
        self.assertEqual([row["name"] for row in rows], [layer["name"] for layer in layers])
        sources = source_layers()
        for row, layer in zip(rows, layers):
            self.assertEqual(row["material_key"], sources[layer["name"]])
            expected_id = "mask_eq" if layer["name"].endswith(".Mask") else (
                "cu" if layer["kind"] == "Patterned" else "core_eq")
            self.assertEqual(row["material_id"], expected_id)
            self.assertEqual(row["thickness_source"], "ipc")
            self.assertAlmostEqual(row["thickness_m"], layer["thickness"], delta=1e-15)

    def test_mapped_without_acceptance_still_requires_confirmation(self):
        process, products = invoke(self.executable, self.directory / "mapped-confirmation",
                                   dict(BASE_MAP, SOLDERMASK="mask_eq"), fixture=self.fixture)
        self.assertEqual(self.unmapped[False][0].returncode, 6)
        self.assertEqual(process.returncode, self.unmapped[False][0].returncode,
                         process.stderr.decode())
        self.assertEqual(document(products, "defects.json")["code"], "IMPORT_CONFIRMATION_REQUIRED")
        self.assert_defect_delta(products, self.unmapped[False][1])
        for name in ("F.Mask", "B.Mask"):
            self.assertEqual(process.stderr.decode().splitlines().count(NOTICE + name), 1)
        self.assertIsNone(products["stack.json"])
        self.assertIsNone(products["board.polyset"])
        self.assertIsNone(products["import_layers.json"])

    def test_unmapped_defects_and_accepted_stack_are_unchanged(self):
        for accept, (reference, reference_products) in self.unmapped.items():
            with self.subTest(accept=accept):
                process, products = invoke(
                    self.executable, self.directory / f"unmapped-current-{accept}",
                    BASE_MAP, accept, self.fixture)
                self.assertEqual(process.returncode, 0 if accept else 6, process.stderr.decode())
                report = document(products, "import_report.json")
                mask_defects = [d for d in report["defects"]
                                if d["defect_id"].startswith("board/SolderMask%3A")]
                self.assertEqual({d["defect_id"] for d in mask_defects}, MASK_IDS)
                for defect in mask_defects:
                    self.assertEqual(defect["tier"], "B")
                    self.assertEqual(defect["suggestion"], "solder mask not modeled")
                self.assertNotIn(NOTICE, process.stderr.decode())
                if accept:
                    layers = document(products, "stack.json")["layers"]
                    self.assertFalse(any(layer["name"].endswith(".Mask") for layer in layers))
                self.assert_outputs_identical(process, products, reference, reference_products)

    def assert_outputs_identical(self, process, products, reference, reference_products):
        self.assertEqual(process.returncode, reference.returncode)
        self.assertEqual(process.stdout, reference.stdout)
        self.assertEqual(process.stderr, reference.stderr)
        for name in ARTIFACTS:
            self.assertEqual(products[name], reference_products[name], name)

    def test_missing_thickness_precedes_invalid_material_map(self):
        for mapping in (None, "not JSON", "{}"):
            results = []
            for masks in (False, True):
                with self.subTest(masks=masks, mapping=mapping):
                    fixture = self.directory / "missing-thickness.xml"
                    text = self.fixture.read_text(encoding="utf-8")
                    if not masks:
                        text = text.replace('layerFunction="SOLDERMASK"',
                                            'layerFunction="DOCUMENT"')
                    text, removed = re.subn(
                        r'(<StackupLayer\b[^>]*layerOrGroupRef="F.Cu"[^>]*?)'
                        r'thickness="[^"]*"', r'\1', text)
                    self.assertEqual(removed, 1)
                    fixture.write_text(text, encoding="utf-8")
                    directory = self.directory / f"thickness-{masks}-{mapping}"
                    directory.mkdir()
                    map_file = directory / "map.json"
                    if mapping is not None:
                        map_file.write_text(mapping, encoding="utf-8")
                    process = subprocess.run(
                        [str(self.executable), str(fixture), "-o", str(directory / "stack.json"),
                         "--polyset", str(directory / "board.polyset"),
                         "--report", str(directory / "defects.json"),
                         "--material-map", str(map_file)],
                        capture_output=True, timeout=45)
                    products = {name: (directory / name).read_bytes()
                                if (directory / name).exists() else None for name in ARTIFACTS}
                    self.assertEqual(process.returncode, 7)
                    self.assertEqual(process.stdout, b"")
                    self.assertEqual(process.stderr,
                                     b"IMPORT_REJECTED: missing thickness for layers: F.Cu\n")
                    self.assertEqual(document(products, "defects.json")["code"], "IMPORT_REJECTED")
                    for name in ARTIFACTS:
                        if name != "defects.json":
                            self.assertIsNone(products[name], name)
                    results.append((process, products))
            # Mask capture must not change rejection precedence or any output.
            self.assertEqual(len(results), 2)
            self.assert_outputs_identical(*results[1], *results[0])

    def test_synthetic_board_restores_full_coverage_masks(self):
        # No material parameters: callers supply the library entries for these IDs.
        fixture = self.directory / "synthetic.xml"
        fixture.write_text('''<IPC-2581><Ecad><CadData>
<Layer name="F.Mask" layerFunction="SOLDERMASK"/><Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Layer name="Core" layerFunction="DIELCORE"/><Layer name="B.Cu" layerFunction="CONDUCTOR"/>
<Layer name="B.Mask" layerFunction="SOLDERMASK"/>
<Stackup><StackupGroup>
<StackupLayer layerOrGroupRef="F.Mask" thickness="0.010" matDes="SOLDERMASK"/>
<StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/>
<StackupLayer layerOrGroupRef="Core" thickness="1.6" matDes="FR4"/>
<StackupLayer layerOrGroupRef="B.Cu" thickness="0.035"/>
<StackupLayer layerOrGroupRef="B.Mask" thickness="0.010" matDes="SOLDERMASK"/>
</StackupGroup></Stackup><Step name="board"><Profile><Polygon>
<PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/>
<PolyStepSegment x="10" y="5"/><PolyStepSegment x="0" y="5"/>
</Polygon></Profile></Step></CadData></Ecad></IPC-2581>''', encoding="utf-8")
        process, products = invoke(self.executable, self.directory / "synthetic-out",
                                   dict(BASE_MAP, SOLDERMASK="mask_eq"), True, fixture)
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        layers = document(products, "stack.json")["layers"]
        self.assertEqual([layer["name"] for layer in layers],
                         ["F.Mask", "F.Cu", "Core", "B.Cu", "B.Mask"])
        self.assertEqual(layers[1]["material"], "core_eq")
        self.assertEqual(layers[-2]["material"], "mask_eq")
        self.assert_no_masks(products)

    def test_raw_fixture_keeps_existing_oval_rejections_and_removes_mask_defects(self):
        # The untouched public fixture retains one C-tier SlotCavity Oval record.
        # Mask restoration must preserve these records and the rejection exit code.
        for accept in (False, True):
            with self.subTest(accept=accept):
                baseline_process, baseline = invoke(
                    self.executable, self.directory / f"raw-unmapped-{accept}", BASE_MAP, accept)
                process, products = invoke(
                    self.executable, self.directory / f"raw-mapped-{accept}",
                    dict(BASE_MAP, SOLDERMASK="mask_eq"), accept)
                self.assertEqual(baseline_process.returncode, 7)
                self.assertEqual(process.returncode, baseline_process.returncode)
                raw = document(baseline, "import_report.json")
                current = document(products, "import_report.json")
                self.assertEqual(raw["tier_counts"]["C"], 1)
                self.assertEqual([d for d in current["defects"] if d["tier"] == "C"],
                                 [d for d in raw["defects"] if d["tier"] == "C"])
                self.assert_defect_delta(products, baseline)


def main():
    executable = os.environ.get("CHIPCAE_IMPORT_EXE")
    if not executable or not Path(executable).is_file() or not os.access(executable, os.X_OK):
        print("CHIPCAE_IMPORT_EXE must name an executable importer", file=sys.stderr)
        return 1
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SoldermaskCLI)
    try:
        result = unittest.TextTestRunner(verbosity=2).run(suite)
    except OSError as error:
        print(f"cannot execute importer: {error}", file=sys.stderr)
        return 1
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
