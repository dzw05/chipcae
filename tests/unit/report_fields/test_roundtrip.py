"""Freeze parsed report fields, including row order and replacement semantics."""

import csv
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import traceback


COLUMNS = [
    "defect_id", "tier", "kind", "reason", "dimension", "size", "size_unit",
    "suggestion", "action", "rejection_reason", "material_ambiguous",
]
TIERS = ["Weld", "A", "B", "C"]
TIER_COUNTS = {
    "four": {"Weld": 1, "A": 1, "B": 1, "C": 1},
    "replacement": {"Weld": 1, "A": 0, "B": 0, "C": 0},
    "empty": {"Weld": 0, "A": 0, "B": 0, "C": 0},
}


def expected_rows(mode, reason):
    ids = ["caseA", "casea", "duplicate", "duplicate#2"]
    return [dict(zip(COLUMNS, [
        ids[i] if mode == "four" else "replacement", TIERS[i], f"kind_{i}",
        reason, f"L{i}", 0.125 + i * 0.25, "1", f"repair_{i}", f"action_{i}",
        f"rejection_{i}", bool(i % 2),
    ])) for i in range(4 if mode == "four" else 0 if mode == "empty" else 1)]


def exact(actual, expected, location):
    if isinstance(expected, float):
        assert type(actual) in (int, float), (location, actual)
        assert math.isfinite(actual) and abs(actual - expected) <= 1e-15, (
            location, actual, expected)
    elif isinstance(expected, dict):
        # Freeze the schema as well as values: additional keys require explicit review.
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


def verify(writer, directory, mode, reason):
    result = subprocess.run([writer, str(directory), mode], check=True,
                            capture_output=True, text=True, timeout=10)
    paths = result.stdout.splitlines()
    assert paths == [str(directory / "import_report.json"),
                     str(directory / "import_report.csv")], result.stdout
    rows = expected_rows(mode, reason)
    expected = dict(schema_version="import-report/1.0", L=1.0, t_min=0.1, eps_weld=0.000001, tau_A=0.00002,
                    tau_B=0.0003, delta_f=0.004,
                    tier_counts=TIER_COUNTS[mode],
                    b_tier_accepted="all" if mode == "replacement" else None,
                    defects=rows)
    with Path(paths[0]).open(encoding="ascii") as stream:
        exact(json.load(stream, parse_constant=reject_constant), expected, mode)
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


def reject_constant(value):
    raise AssertionError(f"non-RFC-8259 JSON constant: {value}")


def read_report(directory):
    report = json.loads((directory / "import_report.json").read_text(),
                        parse_constant=reject_constant)
    with (directory / "import_report.csv").open(newline="") as stream:
        reader = csv.DictReader(stream)
        assert reader.fieldnames == COLUMNS, reader.fieldnames
        rows = list(reader)
    keys = {"schema_version", "L", "t_min", "eps_weld", "tau_A", "tau_B",
            "delta_f", "tier_counts", "b_tier_accepted", "defects"}
    if "welded_interfaces" in report:
        keys.add("welded_interfaces")
    assert report.keys() == keys, report.keys()
    assert report["schema_version"] == "import-report/1.0"
    assert len(rows) == len(report["defects"])
    for d, row in zip(report["defects"], rows):
        assert d.keys() == set(COLUMNS), d.keys()
        for key in COLUMNS:
            if key == "size":
                if d[key] is None:
                    assert not math.isfinite(float(row[key])), row
                else:
                    exact(float(row[key]), float(d[key]), "CSV size")
            elif key in ("reason", "material_ambiguous"):
                assert row[key] == str(d[key]).lower()
            else:
                assert row[key] == d[key], (key, row[key], d[key])
    return report


def run_mode(writer, directory, mode):
    subprocess.run([writer, str(directory), mode], check=True,
                   capture_output=True, text=True, timeout=10)
    return json.loads((directory / "import_report.json").read_text(),
                      parse_constant=reject_constant)


def extended(writer, directory, mode):
    report = run_mode(writer, directory, mode)
    ds = report["defects"]
    ids = [d["defect_id"] for d in ds]
    if mode.startswith("assembly-"):
        expected = 0 if mode == "assembly-zero" else 9007199254740993
        assert type(report.get("welded_interfaces")) is int, report
        assert report["welded_interfaces"] == expected
        assert ds == []
    else:
        assert "welded_interfaces" not in report
    if mode == "ids":
        assert ids == ["foo", "foo#3", "foo#2", "foo#4", "part/1", "part#status", "part#2"], ids
        assert run_mode(writer, directory, "ids-twice") == report
    elif mode == "generated":
        assert all(ids) and len(set(ids)) == 2, ids
    elif mode == "escaping":
        special = 'quote" backslash\\ newline\n tab\t return\r nul\x00 unit\x1f'
        for key in ("defect_id", "kind", "dimension", "suggestion", "action", "rejection_reason"):
            assert ds[0][key] == special, (key, ds[0][key])
    elif mode == "units":
        assert [d["size_unit"] for d in ds] == ["1", "1", "1", "m", "m", "m", "m", "m", "m", "1"]
        for i, d in enumerate(ds):
            exact(d["size"], 0.125 + i * 0.25 if i < 7 else None, f"unit {i}")
    read_report(directory)


def replay(writer, directory, selection):
    directory.mkdir(parents=True)
    ids = {"one": ["foo"], "both": ["foo", "foo#2"], "second": ["foo#2"]}[selection]
    (directory / "accept.json").write_text(json.dumps({"accept": [
        {"defect_id": id_} for id_ in ids]}))
    result = subprocess.run([writer, str(directory), "replay"],
                            capture_output=True, text=True, timeout=10)
    # Check exit before schema so the red phase also exposes the acceptance bug.
    assert result.returncode == (0 if selection == "both" else 6), (result.returncode, result.stderr)
    assert "unknown defect_id" not in result.stderr, result.stderr
    ds = read_report(directory)["defects"]
    assert [d["defect_id"] for d in ds] == ["foo", "foo#2"]
    # Each final ID controls only its own row, including accepting the suffix alone.
    for defect in ds:
        if defect["defect_id"] in ids:
            assert defect["action"] and not defect["rejection_reason"], defect
        else:
            assert not defect["action"], defect
            assert defect["rejection_reason"] == "tier B defect requires explicit confirmation", defect


def assembly_cli(writer, directory, mode):
    source = Path(__file__).resolve().parents[3]
    build = next(parent for parent in Path(writer).parents
                 if (parent / "CMakeCache.txt").is_file())
    os.environ["CHIPCAE_SOURCE_DIR"] = str(source)
    os.environ["CHIPCAE_BINARY_DIR"] = str(build)
    sys.path.insert(0, str(source / "tests/integration/import_assembly"))
    sys.path.insert(0, str(source))
    from assembly_test_support import Assembly, check
    from tools.vv.real_case_e2e import _bodies, _check_a1
    directory.mkdir()
    a = Assembly(directory)
    if mode == "cli-failure":
        # Schema rejection precedes merge_parts and still writes both reports.
        a.document["schema"] = "chipcae.assembly/invalid"
        a.save()
        result = a.run()
        check(result, 14)
        assert not a.output.exists()
    elif mode == "cli-box":
        capabilities = json.loads((build / "import-assets/assembly-capabilities.json").read_text())
        assert capabilities["geometry"] == "ON", (
            "STEP A1 requires assembly geometry capability; OCCT alone is insufficient",
            capabilities)
        a.boxes()
        result = a.run("--accept-b-tier")
        check(result, 0)
    else:
        result = a.run()
        check(result, 0)
    report = a.import_report()
    summary = a.defects()
    assert "welded_interfaces" not in summary, summary
    if mode != "cli-failure":
        bodies = ["F.Cu", "Core", "B.Cu"] + (["lid"] if mode == "cli-box" else [])
        a1 = _check_a1(directory / "import_report.json", a.output,
                       result.returncode, bodies, mode == "cli-box")
        actual_bodies = _bodies(json.loads(a.output.read_text()))
        assert set(bodies) <= set(actual_bodies), actual_bodies
        print(f"{mode}: exit={result.returncode}, actual_bodies={actual_bodies}, "
              f"welded_interfaces={report.get('welded_interfaces')}, A1={a1}", flush=True)
        assert a1, report
    assert type(report.get("welded_interfaces")) is int, report
    # The box's lower face coincides with B.Cu@top, and its upper face is isolated.
    assert report["welded_interfaces"] == (1 if mode == "cli-box" else 0), report
    assert report["schema_version"] == summary["schema_version"] == "import-report/1.0"
    assert isinstance(report["parts"], dict)
    for output in (report, summary):
        ids = [d["defect_id"] for d in output["defects"]]
        assert all(ids) and len(ids) == len(set(ids)), ids
        for d in output["defects"]:
            assert d["size_unit"] in ("1", "m", "m2", "m3"), d


def main():
    writer = os.environ["CHIPCAE_REPORTFIELDS_WRITER"]
    assert Path(writer).is_absolute() and Path(writer).is_file(), writer
    reason = int(subprocess.run([writer, "--reason-value"], check=True,
                                capture_output=True, text=True, timeout=10).stdout)
    failures = []
    with tempfile.TemporaryDirectory(prefix="chipcae-reportfields-") as temporary:
        directory = Path(temporary) / "reports"
        # Run all cases even while the new contract is red, retaining overwrite coverage.
        modes = ["four", "replacement", "empty", "ids", "generated", "escaping", "units",
                 "assembly-zero", "assembly-count"]
        build = next(parent for parent in Path(writer).parents
                     if (parent / "CMakeCache.txt").is_file())
        cache = (build / "CMakeCache.txt").read_text()
        # Dependency-specific cases run when their parsers are configured.
        # STEP A1 also checks the advertised geometry capability before importing.
        if "CHIPCAE_ENABLE_JSONCONS:BOOL=ON" in cache:
            modes += ["accept-one", "accept-both", "accept-second"]
            if "CHIPCAE_ENABLE_ECAD:BOOL=ON" in cache:
                modes += ["cli-no-parts", "cli-failure"]
                if "CHIPCAE_ENABLE_OCCT:BOOL=ON" in cache:
                    modes += ["cli-box"]
        for mode in modes:
            try:
                if mode in TIER_COUNTS:
                    paths = verify(writer, directory, mode, reason)
                    assert paths == [str(directory / "import_report.json"),
                                     str(directory / "import_report.csv")]
                elif mode.startswith("cli-"):
                    assembly_cli(writer, Path(temporary) / mode, mode)
                elif mode.startswith("accept-"):
                    replay(writer, Path(temporary) / mode, mode.split("-", 1)[1])
                else:
                    extended(writer, Path(temporary) / mode, mode)
            except (AssertionError, KeyError, ValueError, subprocess.SubprocessError):
                failures.append(mode)
                print(f"FAIL {mode}", flush=True)
                traceback.print_exc()
    assert not failures, f"failed acceptance cases: {failures}"


if __name__ == "__main__":
    main()
