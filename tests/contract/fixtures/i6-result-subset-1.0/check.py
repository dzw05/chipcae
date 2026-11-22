#!/usr/bin/env python3
"""Check i6-result-subset/1.0 fixtures. Standard library only (jsonschema optional).

  python3 check.py --repo PATH
Exit 0 = pass, 1 = mismatch, 2 = usage / import failure.
Without jsonschema: print SKIP-SCHEMA and keep structural checks (not a failure).
Importing tools.vv.check_result_data_class.check_file is required; failure exits 1.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCHEMA_RESULTS = "chipcae.results/1.0"
SCHEMA_VV = "chipcae.vv_result/0.2"
METHOD_ENUM = frozenset(
    {
        "fem_q2_nodes",
        "fvm_cell_centers",
        "fvm_cell_centers_lsq",
        "mechanical_disabled",
        "not_solved",
    }
)
REQUIRED_SUMMARY = {
    "warpage_pv": "m",
    "T_max": "K",
    "U_mag_max": "m",
}
FIELD_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
MESH_INTS = ("nx0", "ny0", "L", "padded_cells", "nz")


class DupKey(ValueError):
    pass


class CheckError(Exception):
    def __init__(self, message: str):
        super().__init__(message)
        self.message = message


def unique_object(pairs):
    seen = set()
    out = {}
    for key, value in pairs:
        if key in seen:
            raise DupKey(f"duplicate JSON key: {key}")
        seen.add(key)
        out[key] = value
    return out


def load_json(path: Path):
    raw = path.read_text(encoding="utf-8")
    if not raw.strip():
        raise CheckError(f"{path}: empty file")
    try:
        return json.loads(raw, object_pairs_hook=unique_object)
    except DupKey as exc:
        raise CheckError(str(exc)) from exc
    except json.JSONDecodeError as exc:
        raise CheckError(f"{path}: not valid JSON ({exc})") from exc


def is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def is_number(value) -> bool:
    return (is_int(value) or isinstance(value, float)) and not isinstance(value, bool)


def fail(message: str):
    raise CheckError(message)


def check_manifest(obj) -> None:
    if not isinstance(obj, dict):
        fail("manifest must be an object")
    if obj.get("schema_version") != SCHEMA_RESULTS:
        fail("expected manifest schema chipcae.results/1.0")
    steps = obj.get("steps")
    if not isinstance(steps, list) or not steps:
        fail("steps: expected nonempty array")
    for index, record in enumerate(steps):
        if not isinstance(record, dict):
            fail(f"steps[{index}]: expected object")
        if not is_int(record.get("n")) or record["n"] < 0:
            fail(f"steps[{index}].n: expected nonnegative integer")
        if not isinstance(record.get("file"), str) or not record["file"]:
            fail(f"steps[{index}].file: expected nonempty string")
    if "partitions" not in obj:
        fail("partitions: missing")
    partitions = obj["partitions"]
    if not isinstance(partitions, dict):
        fail("partitions: expected object")
    per_step = partitions.get("per_step")
    if not isinstance(per_step, list):
        fail("partitions.per_step: expected array")
    if len(per_step) != len(steps):
        fail(f"partitions.per_step: expected length {len(steps)}")
    for index, value in enumerate(per_step):
        if not is_int(value) or value < 1:
            fail(f"partitions.per_step[{index}]: expected integer >= 1")
    fixed = partitions.get("solver_fixed_per_run")
    if not isinstance(fixed, bool):
        fail("partitions.solver_fixed_per_run: expected boolean")
    if "mesh" not in obj:
        fail("mesh: missing")
    mesh = obj["mesh"]
    if not isinstance(mesh, dict):
        fail("mesh: expected object")
    for key in MESH_INTS:
        if key not in mesh:
            fail(f"mesh.{key}: missing")
        if not is_int(mesh[key]) or mesh[key] < 0:
            fail(f"mesh.{key}: expected integer >= 0")
    if not isinstance(mesh.get("mesh_sha256"), str) or not SHA256.fullmatch(mesh["mesh_sha256"]):
        fail("mesh.mesh_sha256: expected 64 lowercase hex")
    entries = obj.get("summary")
    if not isinstance(entries, list):
        fail("summary: expected array")
    by_name = {}
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str) or not entry["name"]:
            fail(f"summary[{index}]: invalid entry")
        name = entry["name"]
        if name in by_name:
            fail(f"summary[{index}].name: duplicate summary name: {name}")
        by_name[name] = entry
        if not is_number(entry.get("value")):
            fail(f"summary[{index}].value: expected number")
        if not isinstance(entry.get("unit"), str):
            fail(f"summary[{index}].unit: expected string")
    for name, unit in REQUIRED_SUMMARY.items():
        if name not in by_name:
            fail(f"summary: missing {name}")
        entry = by_name[name]
        if entry.get("unit") != unit:
            fail(f"summary.{name}.unit: expected {unit}")
        method = entry.get("method")
        if not isinstance(method, str) or method not in METHOD_ENUM:
            fail(f"summary.{name}.method: expected enum value")
    fields = obj.get("fields")
    if not isinstance(fields, list):
        fail("fields: expected array")


def check_vv_subset(obj) -> None:
    if not isinstance(obj, dict):
        fail("vv_result must be an object")
    if obj.get("schema") != SCHEMA_VV:
        fail("schema: expected chipcae.vv_result/0.2")
    run = obj.get("run")
    if run is not None:
        if not isinstance(run, dict):
            fail("run: expected object")
        if "np" in run:
            np_val = run["np"]
            if not isinstance(np_val, list):
                fail("run.np: expected array")
            if not np_val:
                fail("run.np: empty array")
            for index, value in enumerate(np_val):
                if not is_int(value) or value < 1:
                    fail(f"run.np[{index}]: expected integer >= 1")
    iterations = obj.get("iterations")
    field_names = set()
    if iterations is not None:
        if not isinstance(iterations, dict):
            fail("iterations: expected object")
        residuals = {}
        counts = {}
        for key, value in iterations.items():
            if key.endswith(".residual"):
                name = key[: -len(".residual")]
                if not FIELD_NAME.fullmatch(name):
                    fail(f"iterations.{key}: invalid field name")
                if not is_number(value):
                    fail(f"iterations.{key}: expected number")
                residuals[name] = value
            else:
                if not FIELD_NAME.fullmatch(key):
                    fail(f"iterations.{key}: invalid field name")
                if not is_int(value) or value < 0:
                    fail(f"iterations.{key}: expected integer >= 0")
                counts[key] = value
                field_names.add(key)
        for name in residuals:
            if name not in counts:
                fail(f"iterations.{name}: missing iteration count")
    timing = obj.get("timing")
    if timing is not None:
        if not isinstance(timing, dict):
            fail("timing: expected object")
        stages = timing.get("stages")
        if stages is not None:
            if not isinstance(stages, dict):
                fail("timing.stages: expected object")
            mesh = stages.get("mesh")
            if mesh is not None:
                if not isinstance(mesh, dict):
                    fail("timing.stages.mesh: expected object")
                if "cell_count" in mesh and not is_int(mesh["cell_count"]):
                    fail("timing.stages.mesh.cell_count: expected integer")
    scale = obj.get("scale")
    if scale is not None:
        if not isinstance(scale, dict):
            fail("scale: expected object")
        cells = scale.get("cells", "MISSING")
        source = scale.get("cells_source", "MISSING")
        if cells == "MISSING" or source == "MISSING":
            fail("scale.cells: missing paired cells_source")
        if cells is None:
            if source is not None:
                fail("scale.cells: inconsistent with cells_source")
        else:
            if not is_int(cells):
                fail("scale.cells: expected integer or null")
            if source not in ("vtkhdf", "preview"):
                fail("scale.cells: inconsistent with cells_source")
        solver = scale.get("solver")
        dofs = scale.get("dofs")
        rel = scale.get("rel_residual")
        abs_res = scale.get("abs_residual")
        for key, value, kind in (
            ("solver", solver, "str"),
            ("dofs", dofs, "int"),
            ("rel_residual", rel, "number"),
            ("abs_residual", abs_res, "number"),
        ):
            if value is None:
                continue
            if not isinstance(value, dict):
                fail(f"scale.{key}: expected object")
            for field, item in value.items():
                if not FIELD_NAME.fullmatch(field):
                    fail(f"scale.{key}.{field}: invalid field name")
                if kind == "str" and not isinstance(item, str):
                    fail(f"scale.{key}.{field}: expected string")
                elif kind == "int" and not is_int(item):
                    fail(f"scale.{key}.{field}: expected integer")
                elif kind == "number" and not is_number(item):
                    fail(f"scale.{key}.{field}: expected number")
        if "dofs_source" in scale and scale["dofs_source"] not in (
            "log:dofs",
            "log:free_dofs",
            None,
        ):
            fail("scale.dofs_source: expected log:dofs, log:free_dofs, or null")
        if iterations is not None and rel is not None:
            for field, value in rel.items():
                key = f"{field}.residual"
                if key not in iterations:
                    fail(f"iterations.{key}: missing (required by scale.rel_residual)")
                if iterations[key] != value:
                    fail(f"iterations.{key}: not bitwise equal to scale.rel_residual.{field}")
        for group_name, group in (("solver", solver), ("dofs", dofs)):
            if group is None or iterations is None:
                continue
            extra = set(group) - field_names
            if extra:
                fail(f"scale.{group_name}: keys must be a subset of iterations field names")
    det = obj.get("determinism")
    if det is not None:
        if not isinstance(det, dict):
            fail("determinism: expected object")
        for key in ("np1_digest", "np2_digest"):
            values = det.get(key)
            if not isinstance(values, list) or len(values) != 3:
                fail(f"determinism.{key}: expected array of length 3")
            for index, item in enumerate(values):
                if not isinstance(item, str) or not SHA256.fullmatch(item):
                    fail(f"determinism.{key}[{index}]: expected 64 lowercase hex")
        violations = det.get("np_violations")
        if not isinstance(violations, list) or len(violations) != 3:
            fail("determinism.np_violations: expected array of length 3")
        for index, item in enumerate(violations):
            if not isinstance(item, list) or any(not isinstance(x, str) for x in item):
                fail(f"determinism.np_violations[{index}]: expected string array")
        pairs = det.get("np_bitwise_pairs")
        if not isinstance(pairs, list) or len(pairs) != 3 or any(
            not isinstance(x, bool) for x in pairs
        ):
            fail("determinism.np_bitwise_pairs: expected 3 booleans")
        equal = det.get("np_bitwise_equal")
        if not isinstance(equal, bool):
            fail("determinism.np_bitwise_equal: expected boolean")
        if equal != all(pairs):
            fail("determinism.np_bitwise_equal: must be AND of np_bitwise_pairs")
    ref = obj.get("reference")
    if ref is not None:
        if not isinstance(ref, dict):
            fail("reference: expected object or null")
        if "value" not in ref or not (ref["value"] is None or is_number(ref["value"])):
            fail("reference.value: expected number or null")
        if ref.get("type") not in ("解析", "独立代码", "原型", "回归"):
            fail("reference.type: expected enum")
        if not isinstance(ref.get("source"), str) or not ref["source"]:
            fail("reference.source: expected nonempty string")


def try_schema(repo: Path, kind: str, obj, failures: list[str]) -> None:
    global _schema_status
    if _schema_status != "ok":
        return
    try:
        import jsonschema
    except ImportError:
        _schema_status = "skip"
        return
    name = (
        "schema/manifest-1.schema.json"
        if kind == "manifest"
        else "tools/vv/schema/vv-result-0.2.schema.json"
    )
    path = repo / name
    try:
        schema = json.loads(path.read_text(encoding="utf-8"))
        jsonschema.validate(obj, schema)
    except Exception as exc:  # noqa: BLE001 — report validator or schema errors
        failures.append(f"schema {kind}: {exc}")


_schema_status = "ok"


def load_check_file(repo: Path):
    repo = repo.resolve()
    sys.path.insert(0, str(repo))
    try:
        from tools.vv.check_result_data_class import check_file
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write(f"failed to import check_file from {repo}: {exc}\n")
        sys.exit(1)
    return check_file


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True, help="origin/main work tree")
    args = parser.parse_args(argv)
    repo = args.repo
    if not repo.exists():
        sys.stderr.write(f"--repo not found: {repo}\n")
        return 2
    check_file = load_check_file(repo)
    expected_path = HERE / "expected.json"
    if not expected_path.exists():
        sys.stderr.write("expected.json missing; run gen.py first\n")
        return 1
    try:
        expected = load_json(expected_path)
    except CheckError as exc:
        sys.stderr.write(exc.message + "\n")
        return 1

    failures: list[str] = []

    for rel in expected.get("positives", []):
        path = HERE / rel
        try:
            obj = load_json(path)
            if rel.endswith(".manifest.json"):
                check_manifest(obj)
                try_schema(repo, "manifest", obj, failures)
            elif rel.endswith(".vv_result.json"):
                reasons = check_file(path)
                if reasons:
                    failures.append(f"{rel}: check_file returned {reasons!r}, expected []")
                else:
                    check_vv_subset(obj)
                    try_schema(repo, "vv_result", obj, failures)
            else:
                failures.append(f"{rel}: unknown positive kind")
        except CheckError as exc:
            failures.append(f"{rel}: unexpected error: {exc.message}")

    for case in expected.get("negatives", []):
        rel = case["file"]
        path = HERE / rel
        kind = case["kind"]
        try:
            if kind == "d03":
                obj = load_json(path)
                reasons = check_file(path)
                want = case["check_file"]
                if reasons != want:
                    failures.append(f"{rel}: check_file={reasons!r}, expected {want!r}")
                continue
            if kind == "json":
                try:
                    load_json(path)
                    failures.append(f"{rel}: expected {case['error']!r}, got success")
                except CheckError as exc:
                    if exc.message != case["error"]:
                        failures.append(
                            f"{rel}: error {exc.message!r}, expected {case['error']!r}"
                        )
                continue
            obj = load_json(path)
            if kind == "manifest":
                try:
                    check_manifest(obj)
                    failures.append(f"{rel}: expected {case['error']!r}, got success")
                except CheckError as exc:
                    if exc.message != case["error"]:
                        failures.append(
                            f"{rel}: error {exc.message!r}, expected {case['error']!r}"
                        )
            elif kind == "subset":
                reasons = check_file(path)
                if reasons:
                    failures.append(f"{rel}: check_file {reasons!r} before subset check")
                else:
                    try:
                        check_vv_subset(obj)
                        failures.append(f"{rel}: expected {case['error']!r}, got success")
                    except CheckError as exc:
                        if exc.message != case["error"]:
                            failures.append(
                                f"{rel}: error {exc.message!r}, expected {case['error']!r}"
                            )
            else:
                failures.append(f"{rel}: unknown negative kind {kind}")
        except CheckError as exc:
            if kind in {"manifest", "subset", "json"} and exc.message == case.get("error"):
                continue
            failures.append(f"{rel}: unexpected error: {exc.message}")

    if _schema_status == "skip":
        print("SKIP-SCHEMA")
    if failures:
        for line in failures:
            print(line)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
