#!/usr/bin/env python3
"""Generate t2m/1.0 fixtures. Standard library only.

  python3 gen.py --out DIR
Writes a 2x2x2 cell-center thermal case, nine error-semantics negatives,
expected.json, and copies of gen.py / check.py.
Two runs on the same DIR must be byte-identical.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# 2x2x2 control volumes, index = ix + 2*iy + 4*iz (x fastest).
T_CELL = [300.0 + 10.0 * i for i in range(8)]
VOLUME = [1.0] * 8
T_REF = 300.0
DT = [t - T_REF for t in T_CELL]
# Uniform volumes: weighted T = mean(T) = 335, weighted dT = 35.
WEIGHTED_T = 335.0
WEIGHTED_DT = 35.0


def dump(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(obj, indent=2, sort_keys=True, ensure_ascii=False)
    path.write_text(text + "\n", encoding="utf-8")


def valid_input() -> dict:
    return {
        "shape": [2, 2, 2],
        "index_order": "x_fastest",
        "mode": "thermal",
        "T_cell": T_CELL,
        "volume": VOLUME,
        "T_ref": T_REF,
        "temperature_unit": "K",
    }


def valid_expected() -> dict:
    return {
        "code": 0,
        "code_name": "NONE",
        "message": "",
        "dT": DT,
        "volume_weighted_T": WEIGHTED_T,
        "volume_weighted_dT": WEIGHTED_DT,
    }


def failure(code: int, name: str, message: str) -> dict:
    return {
        "code": code,
        "code_name": name,
        "message": message,
        "dT": [],
        "volume_weighted_T": 0.0,
        "volume_weighted_dT": 0.0,
    }


def generate(out: Path) -> None:
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    dump(out / "valid_2x2x2/input.json", valid_input())
    dump(out / "valid_2x2x2/expected.json", valid_expected())

    unit = valid_input()
    unit["temperature_unit"] = "C"
    dump(out / "negative/unit/input.json", unit)
    dump(out / "negative/unit/expected.json", failure(14, "INPUT_INVALID", "expected unit K"))

    missing = valid_input()
    missing["T_cell"] = []
    dump(out / "negative/missing_tcell/input.json", missing)
    dump(
        out / "negative/missing_tcell/expected.json",
        failure(12, "INPUT_REFERENCE_MISSING", "missing T_cell"),
    )

    length = valid_input()
    length["T_cell"] = T_CELL[:7]
    dump(out / "negative/length_mismatch/input.json", length)
    dump(
        out / "negative/length_mismatch/expected.json",
        failure(14, "INPUT_INVALID", "length mismatch"),
    )

    nonfinite = valid_input()
    # JSON has no NaN; the checker expands this marker to a non-finite value.
    cells = list(T_CELL)
    cells[3] = {"__nonfinite__": "nan"}
    nonfinite["T_cell"] = cells
    dump(out / "negative/nonfinite_tcell/input.json", nonfinite)
    dump(
        out / "negative/nonfinite_tcell/expected.json",
        failure(14, "INPUT_INVALID", "nonfinite T_cell"),
    )

    invalid_volume = valid_input()
    invalid_volume["volume"] = list(VOLUME)
    invalid_volume["volume"][3] = 0.0
    dump(out / "negative/invalid_volume/input.json", invalid_volume)
    dump(
        out / "negative/invalid_volume/expected.json",
        failure(14, "INPUT_INVALID", "invalid volume"),
    )

    invalid_tref = valid_input()
    invalid_tref["T_ref"] = {"__nonfinite__": "nan"}
    dump(out / "negative/invalid_tref/input.json", invalid_tref)
    dump(
        out / "negative/invalid_tref/expected.json",
        failure(14, "INPUT_INVALID", "invalid T_ref"),
    )

    inconsistent = valid_input()
    inconsistent["T_uniform"] = 400.0
    dump(out / "negative/inconsistent_sources/input.json", inconsistent)
    dump(
        out / "negative/inconsistent_sources/expected.json",
        failure(14, "INPUT_INVALID", "inconsistent temperature sources"),
    )

    invalid_tuniform = valid_input()
    invalid_tuniform["mode"] = "uniform"
    invalid_tuniform["T_cell"] = []
    invalid_tuniform["T_uniform"] = {"__nonfinite__": "nan"}
    dump(out / "negative/invalid_tuniform/input.json", invalid_tuniform)
    dump(
        out / "negative/invalid_tuniform/expected.json",
        failure(14, "INPUT_INVALID", "invalid T_uniform"),
    )

    invalid_arithmetic = valid_input()
    invalid_arithmetic["T_cell"] = list(T_CELL)
    invalid_arithmetic["T_cell"][3] = 1.7e308
    invalid_arithmetic["T_ref"] = -1.7e308
    # The ordered dTV sum overflows before the fourth cell's subtraction.
    dump(out / "negative/invalid_arithmetic/input.json", invalid_arithmetic)
    dump(
        out / "negative/invalid_arithmetic/expected.json",
        failure(14, "INPUT_INVALID", "invalid temperature arithmetic"),
    )

    catalog = {
        "positives": ["valid_2x2x2"],
        "negatives": [
            "negative/unit",
            "negative/missing_tcell",
            "negative/length_mismatch",
            "negative/nonfinite_tcell",
            "negative/invalid_volume",
            "negative/invalid_tref",
            "negative/inconsistent_sources",
            "negative/invalid_tuniform",
            "negative/invalid_arithmetic",
        ],
    }
    dump(out / "expected.json", catalog)

    for name in ("gen.py", "check.py"):
        src = HERE / name
        dst = out / name
        if src.exists() and dst.resolve() != src.resolve():
            dst.write_bytes(src.read_bytes())


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=HERE)
    args = parser.parse_args(argv)
    generate(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
