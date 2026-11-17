"""Freeze parsed report fields, including row order and replacement semantics."""

import csv
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile


COLUMNS = [
    "defect_id", "tier", "kind", "reason", "dimension", "size",
    "suggestion", "action", "rejection_reason", "material_ambiguous",
]
TIERS = ["Weld", "A", "B", "C"]


def expected_rows(mode):
    ids = ["caseA", "casea", "duplicate", "duplicate"]
    return [dict(zip(COLUMNS, [
        ids[i] if mode == "four" else "replacement", TIERS[i], f"kind_{i}",
        0, f"L{i}", 0.125 + i * 0.25, f"repair_{i}", f"action_{i}",
        f"rejection_{i}", bool(i % 2),
    ])) for i in range(4 if mode == "four" else 0 if mode == "empty" else 1)]


def exact(actual, expected, location):
    if isinstance(expected, float):
        assert type(actual) in (int, float), (location, actual)
        assert math.isfinite(actual) and abs(actual - expected) <= 1e-15, (
            location, actual, expected)
    elif isinstance(expected, dict):
        assert type(actual) is dict and actual.keys() == expected.keys(), location
        for key, value in expected.items():
            exact(actual[key], value, f"{location}.{key}")
    elif isinstance(expected, list):
        assert type(actual) is list and len(actual) == len(expected), location
        for i, (value, wanted) in enumerate(zip(actual, expected)):
            exact(value, wanted, f"{location}[{i}]")
    else:
        assert type(actual) is type(expected) and actual == expected, (
            location, actual, expected)


def verify(writer, directory, mode):
    result = subprocess.run([writer, str(directory), mode], check=True,
                            capture_output=True, text=True)
    paths = result.stdout.splitlines()
    assert paths == [str(directory / "import_report.json"),
                     str(directory / "import_report.csv")], result.stdout
    rows = expected_rows(mode)
    expected = dict(L=1.0, t_min=0.1, eps_weld=0.000001, tau_A=0.00002,
                    tau_B=0.0003, delta_f=0.004,
                    tier_counts={tier: sum(row["tier"] == tier for row in rows)
                                 for tier in TIERS},
                    b_tier_accepted="all" if mode == "replacement" else None,
                    defects=rows)
    with Path(paths[0]).open(encoding="ascii") as stream:
        exact(json.load(stream), expected, mode)
    with Path(paths[1]).open(encoding="ascii", newline="") as stream:
        reader = csv.reader(stream)
        assert next(reader) == COLUMNS, "CSV header differs from frozen columns"
        csv_rows = list(reader)
    assert len(csv_rows) == len(rows), (mode, csv_rows)
    for index, (actual, wanted) in enumerate(zip(csv_rows, rows)):
        assert len(actual) == len(COLUMNS), (mode, index, actual)
        for column, value in zip(COLUMNS, actual):
            expected_value = wanted[column]
            if column == "size":
                exact(float(value), expected_value, f"CSV {index}.{column}")
            else:
                expected_text = (str(expected_value).lower()
                                 if type(expected_value) is bool else str(expected_value))
                assert value == expected_text, (mode, index, column, value, expected_text)
    return paths


def main():
    writer = os.environ["CHIPCAE_REPORTFIELDS_WRITER"]
    assert Path(writer).is_absolute() and Path(writer).is_file(), writer
    with tempfile.TemporaryDirectory(prefix="chipcae-reportfields-") as temporary:
        directory = Path(temporary) / "reports"
        first_paths = verify(writer, directory, "four")
        assert verify(writer, directory, "replacement") == first_paths
        assert verify(writer, directory, "empty") == first_paths


if __name__ == "__main__":
    main()
