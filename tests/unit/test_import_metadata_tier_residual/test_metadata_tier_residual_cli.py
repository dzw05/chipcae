"""Residual metadata acceptance, frozen before implementation at merged main.

kicad4l retains its baseline C-tier rejection (three Ovals), so it has no
CLI geometry artifacts. A successful synthetic side-outline case additionally
checks geometry bytes as required by the revised brief.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(os.environ.get("CHIPCAE_SOURCE_DIR", Path(__file__).resolve().parents[3]))
DATA = ROOT / "tests/integration/ecad/data"
MAPPING = ROOT / "tests/integration/import_cli/material_map.json"
BASELINE_SHA = "ea3da025c4deb11163a78f266887b6067527bcea"
MAPPING_SHA = 'ecd3aa2ec10f2162c30e198b5d30908104bba70f142c00580f43df61cb2dc2b2'
SIDE_XML = '<IPC-2581>\n<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>\n<CadHeader units="MILLIMETER"></CadHeader>\n<CadData>\n<Layer name="Cu" layerFunction="CONDUCTOR"/>\n<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>\n<Step name="residual">\n<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>\n<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>\n</Step></CadData></IPC-2581>\n'
FROZEN = json.loads(r'''{
  "kicad4l": {
    "report": {
      "schema_version": "import-report/1.0",
      "L": 0,
      "t_min": 0,
      "eps_weld": 0,
      "tau_A": 0,
      "tau_B": 0,
      "delta_f": 0,
      "tier_counts": {
        "Weld": 0,
        "A": 74,
        "B": 53,
        "C": 3
      },
      "b_tier_accepted": "all",
      "defects": [
        {
          "defect_id": "board/SolderMask%3AF.Mask/0",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "solder mask not modeled",
          "action": "solder mask not modeled",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/SolderMask%3AB.Mask/0",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "solder mask not modeled",
          "action": "solder mask not modeled",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/FunctionMode/4",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StepRef/5",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/6",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/7",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/8",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/9",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/10",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/11",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/12",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/13",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/14",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/15",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/16",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/17",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/LayerRef/18",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/BomRef/19",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/AvlRef/20",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/DictionaryColor/21",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/DictionaryLineDesc/22",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/EntryLineDesc/23",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "definition expressed by modeled instances",
          "action": "definition expressed by modeled instances",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/EntryLineDesc/26",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/DictionaryFillDesc/30",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/DictionaryStandard/31",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/EntryStandard/32",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Oval/33",
          "tier": "C",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "unsupported geometry must be modeled before import",
          "action": "",
          "rejection_reason": "tier C defect is rejected",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/EntryStandard/35",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Oval/36",
          "tier": "C",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "unsupported geometry must be modeled before import",
          "action": "",
          "rejection_reason": "tier C defect is rejected",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/DictionaryUser/39",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/EntryUser/40",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/EntryUser/48",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Role/59",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Enterprise/60",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Person/61",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/FileRevision/64",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/SoftwarePackage/65",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Certification/66",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/BomHeader/71",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StepRef/72",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/BomItem/74",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/RefDes/75",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Characteristics/76",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Textual/77",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/86",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/87",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/92",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/98",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/99",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Dielectric/101",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/102",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Conductor/109",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/110",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/116",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Dielectric/118",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/119",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Dielectric/121",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/122",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Conductor/129",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/130",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/136",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Dielectric/138",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/139",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Dielectric/141",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/142",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Conductor/149",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/150",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/156",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Dielectric/158",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/159",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Dielectric/161",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/162",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Conductor/169",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/170",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/176",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/177",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Dielectric/179",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/180",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/185",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/191",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Property/192",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/NonstandardAttribute/276",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PadstackPadDef/278",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PadstackPadDef/282",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PadstackPadDef/286",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PadstackPadDef/290",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PadstackPadDef/297",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PadstackPadDef/301",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PadstackPadDef/305",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PadstackPadDef/309",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Datum/314",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PickupPoint/355",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "definition expressed by modeled instances",
          "action": "definition expressed by modeled instances",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/358",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/362",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/591",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/592",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/596",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/597",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/605",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/606",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/610",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/611",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/835",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/836",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/840",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/841",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/849",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/850",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/854",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/855",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/1079",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/1080",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/1084",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/1085",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/NonstandardAttribute/1091",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Features/1092",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Polyline/1095",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Polyline/1107",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/1339",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/1340",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/StandardPrimitiveRef/1344",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/PinRef/1345",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Features/1351",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/SlotCavity/1376",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Xform/1378",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Oval/1379",
          "tier": "C",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "unsupported geometry must be modeled before import",
          "action": "",
          "rejection_reason": "tier C defect is rejected",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/Avl/1386",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/AvlHeader/1387",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/AvlItem/1388",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "non-physical metadata ignored",
          "action": "non-physical metadata ignored",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/via_plating_missing/F.Cu-B.Cu",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm missing via plating",
          "action": "confirm missing via plating",
          "rejection_reason": "",
          "material_ambiguous": false
        }
      ]
    },
    "ids": [
      "board/SolderMask%3AF.Mask/0",
      "board/SolderMask%3AB.Mask/0",
      "board/FunctionMode/4",
      "board/StepRef/5",
      "board/LayerRef/6",
      "board/LayerRef/7",
      "board/LayerRef/8",
      "board/LayerRef/9",
      "board/LayerRef/10",
      "board/LayerRef/11",
      "board/LayerRef/12",
      "board/LayerRef/13",
      "board/LayerRef/14",
      "board/LayerRef/15",
      "board/LayerRef/16",
      "board/LayerRef/17",
      "board/LayerRef/18",
      "board/BomRef/19",
      "board/AvlRef/20",
      "board/DictionaryColor/21",
      "board/DictionaryLineDesc/22",
      "board/EntryLineDesc/23",
      "board/EntryLineDesc/26",
      "board/DictionaryFillDesc/30",
      "board/DictionaryStandard/31",
      "board/EntryStandard/32",
      "board/Oval/33",
      "board/EntryStandard/35",
      "board/Oval/36",
      "board/DictionaryUser/39",
      "board/EntryUser/40",
      "board/EntryUser/48",
      "board/Role/59",
      "board/Enterprise/60",
      "board/Person/61",
      "board/FileRevision/64",
      "board/SoftwarePackage/65",
      "board/Certification/66",
      "board/BomHeader/71",
      "board/StepRef/72",
      "board/BomItem/74",
      "board/RefDes/75",
      "board/Characteristics/76",
      "board/Textual/77",
      "board/Property/86",
      "board/Property/87",
      "board/Property/92",
      "board/Property/98",
      "board/Property/99",
      "board/Dielectric/101",
      "board/Property/102",
      "board/Conductor/109",
      "board/Property/110",
      "board/Property/116",
      "board/Dielectric/118",
      "board/Property/119",
      "board/Dielectric/121",
      "board/Property/122",
      "board/Conductor/129",
      "board/Property/130",
      "board/Property/136",
      "board/Dielectric/138",
      "board/Property/139",
      "board/Dielectric/141",
      "board/Property/142",
      "board/Conductor/149",
      "board/Property/150",
      "board/Property/156",
      "board/Dielectric/158",
      "board/Property/159",
      "board/Dielectric/161",
      "board/Property/162",
      "board/Conductor/169",
      "board/Property/170",
      "board/Property/176",
      "board/Property/177",
      "board/Dielectric/179",
      "board/Property/180",
      "board/Property/185",
      "board/Property/191",
      "board/Property/192",
      "board/NonstandardAttribute/276",
      "board/PadstackPadDef/278",
      "board/PadstackPadDef/282",
      "board/PadstackPadDef/286",
      "board/PadstackPadDef/290",
      "board/PadstackPadDef/297",
      "board/PadstackPadDef/301",
      "board/PadstackPadDef/305",
      "board/PadstackPadDef/309",
      "board/Datum/314",
      "board/PickupPoint/355",
      "board/StandardPrimitiveRef/358",
      "board/StandardPrimitiveRef/362",
      "board/StandardPrimitiveRef/591",
      "board/PinRef/592",
      "board/StandardPrimitiveRef/596",
      "board/PinRef/597",
      "board/StandardPrimitiveRef/605",
      "board/PinRef/606",
      "board/StandardPrimitiveRef/610",
      "board/PinRef/611",
      "board/StandardPrimitiveRef/835",
      "board/PinRef/836",
      "board/StandardPrimitiveRef/840",
      "board/PinRef/841",
      "board/StandardPrimitiveRef/849",
      "board/PinRef/850",
      "board/StandardPrimitiveRef/854",
      "board/PinRef/855",
      "board/StandardPrimitiveRef/1079",
      "board/PinRef/1080",
      "board/StandardPrimitiveRef/1084",
      "board/PinRef/1085",
      "board/NonstandardAttribute/1091",
      "board/Features/1092",
      "board/Polyline/1095",
      "board/Polyline/1107",
      "board/StandardPrimitiveRef/1339",
      "board/PinRef/1340",
      "board/StandardPrimitiveRef/1344",
      "board/PinRef/1345",
      "board/Features/1351",
      "board/SlotCavity/1376",
      "board/Xform/1378",
      "board/Oval/1379",
      "board/Avl/1386",
      "board/AvlHeader/1387",
      "board/AvlItem/1388",
      "board/via_plating_missing/F.Cu-B.Cu"
    ],
    "id_sha": "6b1f9df488a3d289d416138a4440cb10934f8a1c6dfef2d510569f719b470090",
    "exit_code": 7,
    "stdout": "",
    "stderr": "IMPORT_REJECTED: unsupported board geometry\n",
    "input_sha": "01a01e4509435d445452b68d929ab76ff4a0632c9d0a1725b80b1fc152d74016",
    "geometry": {}
  },
  "unsupported": {
    "report": {
      "schema_version": "import-report/1.0",
      "L": 0,
      "t_min": 0,
      "eps_weld": 0,
      "tau_A": 0,
      "tau_B": 0,
      "delta_f": 0,
      "tier_counts": {
        "Weld": 0,
        "A": 0,
        "B": 3,
        "C": 0
      },
      "b_tier_accepted": null,
      "defects": [
        {
          "defect_id": "board/MysteryA/2",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "",
          "rejection_reason": "tier B defect requires explicit confirmation",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/MysteryB/2",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "",
          "rejection_reason": "tier B defect requires explicit confirmation",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/MysteryC/2",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "",
          "rejection_reason": "tier B defect requires explicit confirmation",
          "material_ambiguous": false
        }
      ]
    },
    "ids": [
      "board/MysteryA/2",
      "board/MysteryB/2",
      "board/MysteryC/2"
    ],
    "id_sha": "d8a062aebfb6d2dc4c292d386d9bf7fcd2b16393f44eb0b6f400870782cc42c7",
    "exit_code": 6,
    "stdout": "",
    "stderr": "IMPORT_CONFIRMATION_REQUIRED: tier B defects require confirmation\n",
    "input_sha": "025c8a965028696c404e8fe4582bfd7329cc4015b72629da759818971d23c3c4",
    "geometry": {}
  },
  "side_placed": {
    "report": {
      "schema_version": "import-report/1.0",
      "L": 0,
      "t_min": 0,
      "eps_weld": 0,
      "tau_A": 0,
      "tau_B": 0,
      "delta_f": 0,
      "tier_counts": {
        "Weld": 0,
        "A": 1,
        "B": 2,
        "C": 0
      },
      "b_tier_accepted": "all",
      "defects": [
        {
          "defect_id": "board/DictionaryLineDesc/2",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/EntryLineDesc/2",
          "tier": "B",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "confirm omitted metadata, components or text",
          "action": "confirm omitted metadata, components or text",
          "rejection_reason": "",
          "material_ambiguous": false
        },
        {
          "defect_id": "board/OtherSideView/9",
          "tier": "A",
          "kind": "IMPORT_UNSUPPORTED_ELEMENT",
          "reason": 0,
          "dimension": "element",
          "size": 1,
          "size_unit": "1",
          "suggestion": "definition expressed by modeled instances",
          "action": "definition expressed by modeled instances",
          "rejection_reason": "",
          "material_ambiguous": false
        }
      ]
    },
    "ids": [
      "board/DictionaryLineDesc/2",
      "board/EntryLineDesc/2",
      "board/OtherSideView/9"
    ],
    "id_sha": "658d19c44139b915ac71c9b03b6279cb82d462b47324a4e0cd3430b544be7573",
    "exit_code": 0,
    "stdout": "",
    "stderr": "",
    "input_sha": "079e2a2fb0e4cba01830e00c3dcb9320e5973e588f16ead6678668cb18e0b203",
    "geometry": {
      "stack.json": "864a2fb47aa6526925b36a330c6078451e7a57098dcb91c6bfd33bc4d7daa2e2",
      "board.polyset": "9c33ccd457542b80511340965443d6fc8539ec81f9724067deff1408d65f5463",
      "import_layers.json": "ac0de18e34ce03a1582683541b251cf31f510618b7ce6adee34a3f27d97e460f"
    }
  }
}''' )
PROMOTIONS = {'kicad4l': {'board/StepRef/72': 'bom', 'board/Property/86': 'spec-text', 'board/Property/87': 'spec-text', 'board/Property/92': 'spec-text', 'board/Property/98': 'spec-text', 'board/Property/99': 'spec-text', 'board/Property/116': 'spec-text', 'board/Property/136': 'spec-text', 'board/Property/156': 'spec-text', 'board/Property/176': 'spec-text', 'board/Property/177': 'spec-text', 'board/Property/185': 'spec-text', 'board/Property/191': 'spec-text', 'board/Property/192': 'spec-text'}, 'unsupported': {}, 'side_placed': {'board/DictionaryLineDesc/2': 'def-instanced', 'board/EntryLineDesc/2': 'def-instanced'}}

def sha(data):
    return hashlib.sha256(data).hexdigest()


def id_sha(ids):
    return sha(json.dumps(ids, ensure_ascii=False, separators=(',', ':')).encode('utf-8'))


def expected(name):
    report = copy.deepcopy(FROZEN[name]['report'])
    for defect in report['defects']:
        context = PROMOTIONS[name].get(defect['defect_id'])
        if context:
            assert defect['tier'] == 'B', 'baseline promotion must start at B'
            action = ('definition expressed by modeled instances' if context == 'def-instanced'
                      else 'non-physical metadata ignored')
            defect.update(tier='A', suggestion=action, action=action)
    count = len(PROMOTIONS[name])
    report['tier_counts']['A'] += count
    report['tier_counts']['B'] -= count
    return report


def invoke(executable, source, directory, accept):
    directory.mkdir()
    command = [str(executable), str(source), '-o', str(directory / 'stack.json'),
               '--polyset', str(directory / 'board.polyset'),
               '--report', str(directory / 'defects.json'), '--material-map', str(MAPPING)]
    if accept:
        command.append('--accept-b-tier')
    process = subprocess.run(command, capture_output=True, timeout=45)
    report = json.loads((directory / 'import_report.json').read_bytes())
    return process, report, directory


class ResidualMetadataCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='metadata-residual-cli-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        side = cls.directory / 'side.xml'
        side.write_bytes(SIDE_XML.encode('utf-8'))
        cls.sources = {'kicad4l': DATA / 'odbpp/kicad4l.ipc2581.xml',
                       'unsupported': DATA / 'unsupported.xml', 'side_placed': side}
        cls.runs = {name: invoke(EXECUTABLE, source, cls.directory / name,
                                name != 'unsupported') for name, source in cls.sources.items()}

    def test_frozen_input_bytes_and_ordered_id_constants(self):
        self.assertEqual(sha(MAPPING.read_bytes()), MAPPING_SHA)
        for name, source in self.sources.items():
            with self.subTest(case=name):
                self.assertEqual(sha(source.read_bytes()), FROZEN[name]['input_sha'])
                ids = [d['defect_id'] for d in FROZEN[name]['report']['defects']]
                self.assertEqual(ids, FROZEN[name]['ids'])
                self.assertEqual(id_sha(ids), FROZEN[name]['id_sha'])
        self.assertEqual(len(PROMOTIONS['kicad4l']), 14)

    def test_ordered_ids_and_every_defect_field(self):
        for name, (_, report, _) in self.runs.items():
            original = FROZEN[name]
            wanted = expected(name)
            ids = [d['defect_id'] for d in report['defects']]
            with self.subTest(case=name):
                self.assertEqual(ids, original['ids'])
                self.assertEqual(id_sha(ids), original['id_sha'])
            for actual, target in zip(report['defects'], wanted['defects']):
                with self.subTest(case=name, defect=target['defect_id']):
                    if target['defect_id'] in PROMOTIONS[name]:
                        self.assertEqual(actual['tier'], 'A',
                                         'missing residual B->A metadata classification')
                    self.assertEqual(actual, target)
            with self.subTest(case=name, field='complete report and tier deltas'):
                self.assertEqual(report, wanted)

    def test_exit_status_stdout_stderr_and_geometry_bytes(self):
        for name, (process, _, directory) in self.runs.items():
            original = FROZEN[name]
            with self.subTest(case=name):
                self.assertEqual(process.returncode, original['exit_code'])
                self.assertEqual(process.stdout, original['stdout'].encode())
                self.assertEqual(process.stderr, original['stderr'].encode())
            for file in ('stack.json', 'board.polyset', 'import_layers.json'):
                with self.subTest(case=name, artifact=file):
                    if file in original['geometry']:
                        self.assertEqual(sha((directory / file).read_bytes()),
                                         original['geometry'][file])
                    else:
                        self.assertFalse((directory / file).exists())

    def test_unknown_elements_keep_goldens_and_exit_six(self):
        process, report, directory = self.runs['unsupported']
        self.assertEqual(process.returncode, 6)
        self.assertEqual(report['tier_counts'], {'Weld': 0, 'A': 0, 'B': 3, 'C': 0})
        golden = ROOT / 'tests/integration/import_cli/golden/unsupported'
        for name in ('defects.json', 'import_report.json', 'import_report.csv'):
            with self.subTest(file=name):
                self.assertEqual((directory / name).read_bytes(), (golden / name).read_bytes())
        self.assertEqual(process.stdout, (golden / 'stdout').read_bytes())
        self.assertEqual(process.stderr, (golden / 'stderr').read_bytes())
        self.assertEqual(process.returncode, int((golden / 'exit_code').read_text()))

    def test_missing_or_nonexecutable_importer_exits_one(self):
        environment = dict(os.environ)
        environment.pop('CHIPCAE_IMPORT_EXE', None)
        nonexecutable = self.directory / 'nonexecutable'
        nonexecutable.write_text('not an importer', encoding='utf-8')
        nonexecutable.chmod(0o600)
        for candidate in (None, self.directory / 'missing', self.directory, nonexecutable):
            with self.subTest(executable=candidate):
                if candidate is not None:
                    environment['CHIPCAE_IMPORT_EXE'] = str(candidate)
                process = subprocess.run([sys.executable, str(Path(__file__).resolve())],
                                         env=environment, capture_output=True, timeout=10)
                self.assertEqual(process.returncode, 1)
                self.assertIn(b'CHIPCAE_IMPORT_EXE must name an executable importer', process.stderr)


if __name__ == '__main__':
    value = os.environ.get('CHIPCAE_IMPORT_EXE')
    if not value or not Path(value).is_file() or not os.access(value, os.X_OK):
        print('CHIPCAE_IMPORT_EXE must name an executable importer', file=sys.stderr)
        sys.exit(1)
    EXECUTABLE = Path(value).resolve()
    unittest.main(argv=[sys.argv[0]])
