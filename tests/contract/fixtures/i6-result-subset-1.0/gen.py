#!/usr/bin/env python3
"""Generate i6-result-subset/1.0 fixtures. Standard library only.

  python3 gen.py --out DIR
Writes manifests, vv_result samples, negatives, expected.json, and copies of
gen.py / check.py. Two runs on the same DIR must be byte-identical.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCHEMA_RESULTS = "chipcae.results/1.0"
SCHEMA_VV = "chipcae.vv_result/0.2"
MESH_SHA = hashlib.sha256(b"i6-result-subset-1.0-synthetic-mesh").hexdigest()
DIGESTS = [
    hashlib.sha256(b"i6-np1-r1").hexdigest(),
    hashlib.sha256(b"i6-np1-r2").hexdigest(),
    hashlib.sha256(b"i6-np1-r3").hexdigest(),
]
DIGESTS2 = [
    hashlib.sha256(b"i6-np2-r1").hexdigest(),
    hashlib.sha256(b"i6-np2-r2").hexdigest(),
    hashlib.sha256(b"i6-np2-r3").hexdigest(),
]
RES_T = 1.5e-12
RES_U = 2.25e-12
METHODS = {
    "fem": {
        "warpage_pv": "fem_q2_nodes",
        "T_max": "fem_q2_nodes",
        "U_mag_max": "fem_q2_nodes",
    },
    "fvm": {
        "warpage_pv": "fvm_cell_centers_lsq",
        "T_max": "fvm_cell_centers",
        "U_mag_max": "fvm_cell_centers",
    },
}


def dump(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(obj, indent=2, sort_keys=True, ensure_ascii=False)
    path.write_text(text + "\n", encoding="utf-8")


def fields():
    def f(name, location, components, dtype, unit, reserved=None):
        item = {
            "name": name,
            "location": location,
            "components": components,
            "dtype": dtype,
            "unit": unit,
        }
        if reserved is not None:
            item["reserved"] = reserved
        return item

    return [
        f("T", "point", 1, "float64", "K"),
        f("U", "point", 3, "float64", "m"),
        f("layer_id", "cell", 1, "int32", "1"),
        f("material_id", "cell", 1, "int32", "1"),
        f("cell_kind", "cell", 1, "int32", "1"),
        f("elem_gid", "cell", 1, "int64", "1"),
        f("volume", "cell", 1, "float64", "m3"),
        f("sigma_vm", "cell", 1, "float64", "Pa", True),
    ]


def manifest(family: str) -> dict:
    methods = METHODS[family]
    return {
        "schema_version": SCHEMA_RESULTS,
        "units": "SI",
        "stackup": "stackup.json",
        "steps": [{"n": 0, "file": "out/step-0.vtkhdf"}],
        "partitions": {"per_step": [1], "solver_fixed_per_run": True},
        "fields": fields(),
        "mesh": {
            "nx0": 2,
            "ny0": 2,
            "L": 0,
            "padded_cells": 0,
            "nz": 2,
            "mesh_sha256": MESH_SHA,
        },
        "summary": [
            {
                "name": "warpage_pv",
                "value": 1.25e-6,
                "unit": "m",
                "step": 0,
                "method": methods["warpage_pv"],
            },
            {
                "name": "T_max",
                "value": 370.0,
                "unit": "K",
                "step": 0,
                "method": methods["T_max"],
            },
            {
                "name": "U_mag_max",
                "value": 2.5e-6,
                "unit": "m",
                "step": 0,
                "method": methods["U_mag_max"],
            },
        ],
    }


def base_vv() -> dict:
    return {
        "schema": SCHEMA_VV,
        "case_id": "i6-subset-synthetic",
        "data_class": "synthetic",
        "label": "计划值",
        "date": "20261120",
        "commit": "e8e86ce",
        "qoi": {"warpage_pv": 1.25e-6, "T_max": 370.0, "U_mag_max": 2.5e-6},
        "verdict": "pass",
    }


def nightly_vv() -> dict:
    obj = base_vv()
    obj.update(
        {
            "reference": {
                "value": None,
                "type": "回归",
                "source": "synthetic i6-result-subset-1.0 nightly sample",
            },
            "run": {"np": [1, 2]},
            "iterations": {
                "T": 5,
                "T.residual": RES_T,
                "U": 8,
                "U.residual": RES_U,
            },
            "timing": {"stages": {"mesh": {"cell_count": 8}}},
            "scale": {
                "cells": 8,
                "cells_source": "vtkhdf",
                "solver": {"T": "fvm.conduction", "U": "fvm.solid"},
                "dofs": {"T": 8, "U": 24},
                "dofs_source": "log:dofs",
                "rel_residual": {"T": RES_T, "U": RES_U},
                "abs_residual": {"T": 1e-15, "U": 1e-15},
            },
            "determinism": {
                "np1_digest": DIGESTS,
                "np2_digest": DIGESTS2,
                "np_violations": [[], [], []],
                "np_bitwise_pairs": [True, True, True],
                "np_bitwise_equal": True,
            },
        }
    )
    return obj


def scale_inconsistent() -> dict:
    obj = nightly_vv()
    obj["case_id"] = "i6-subset-scale-inconsistent"
    obj["scale"]["cells"] = 8
    obj["scale"]["cells_source"] = None
    return obj


def run_np_empty() -> dict:
    obj = nightly_vv()
    obj["case_id"] = "i6-subset-run-np-empty"
    obj["run"] = {"np": []}
    return obj


def d03_real() -> dict:
    obj = base_vv()
    obj["case_id"] = "i6-subset-d03-real"
    obj["data_class"] = "real"
    obj["label"] = "实测"
    return obj


def write_duplicate_keys(path: Path) -> None:
    # Intentionally invalid JSON object with a repeated key. Not produced by json.dumps.
    path.parent.mkdir(parents=True, exist_ok=True)
    body = (
        '{\n'
        '  "schema_version": "chipcae.results/1.0",\n'
        '  "schema_version": "chipcae.results/0.9",\n'
        '  "steps": [{"n": 0, "file": "out/step-0.vtkhdf"}],\n'
        '  "fields": [],\n'
        '  "partitions": {"per_step": [1], "solver_fixed_per_run": true},\n'
        '  "summary": []\n'
        '}\n'
    )
    path.write_text(body, encoding="utf-8")


def missing_tmax() -> dict:
    obj = manifest("fem")
    obj["summary"] = [e for e in obj["summary"] if e["name"] != "T_max"]
    return obj


def bad_schema() -> dict:
    obj = manifest("fem")
    obj["schema_version"] = "chipcae.results/0.9"
    return obj


def expected_doc() -> dict:
    return {
        "positives": [
            "manifest_fem/fem.manifest.json",
            "manifest_fvm/fvm.manifest.json",
            "vv_result_nightly/nightly.vv_result.json",
        ],
        "negatives": [
            {
                "file": "negative/schema_version.manifest.json",
                "kind": "manifest",
                "error": "expected manifest schema chipcae.results/1.0",
            },
            {
                "file": "negative/missing_tmax.manifest.json",
                "kind": "manifest",
                "error": "summary: missing T_max",
            },
            {
                "file": "negative/scale_cells_inconsistent.vv_result.json",
                "kind": "subset",
                "error": "scale.cells: inconsistent with cells_source",
            },
            {
                "file": "negative/run_np_empty.vv_result.json",
                "kind": "subset",
                "error": "run.np: empty array",
            },
            {
                "file": "negative/duplicate_keys.manifest.json",
                "kind": "json",
                "error": "duplicate JSON key: schema_version",
            },
            {
                "file": "negative/d03_real.vv_result.json",
                "kind": "d03",
                "check_file": ["real result must stay local (D03)"],
            },
        ],
    }


def generate(out: Path) -> None:
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    dump(out / "manifest_fem/fem.manifest.json", manifest("fem"))
    dump(out / "manifest_fvm/fvm.manifest.json", manifest("fvm"))
    dump(out / "vv_result_nightly/nightly.vv_result.json", nightly_vv())
    dump(out / "negative/schema_version.manifest.json", bad_schema())
    dump(out / "negative/missing_tmax.manifest.json", missing_tmax())
    dump(out / "negative/scale_cells_inconsistent.vv_result.json", scale_inconsistent())
    dump(out / "negative/run_np_empty.vv_result.json", run_np_empty())
    write_duplicate_keys(out / "negative/duplicate_keys.manifest.json")
    dump(out / "negative/d03_real.vv_result.json", d03_real())
    dump(out / "expected.json", expected_doc())
    for name in ("gen.py", "check.py"):
        src = HERE / name
        dst = out / name
        if src.exists():
            data = src.read_bytes()
            if dst.resolve() != src.resolve():
                dst.write_bytes(data)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=HERE)
    args = parser.parse_args(argv)
    if not (HERE / "check.py").exists() and args.out.resolve() == HERE.resolve():
        sys.stderr.write("check.py missing next to gen.py\n")
        return 2
    generate(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
