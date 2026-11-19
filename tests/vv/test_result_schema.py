# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Acceptance tests for the V&V result schema and D03 repository gate."""

from __future__ import annotations

import importlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "data" / "result_schema"
CHECKER_MODULE = "tools.vv.check_result_data_class"
SCHEMA_PATH = ROOT / "tools" / "vv" / "schema" / "vv-result-0.2.schema.json"
SCHEMA_ID = "chipcae.vv_result/0.2"
REAL_REJECTION = "real result must stay local (D03)"
MAX_BYTES = 2 * 1024 * 1024
ALLOWED_DIRS = ("cases/benchmarks/", "tests/vv/data/")
NON_OBJECTS = ([], None, False, 17, 1.25, "synthetic")


def _load_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def _field_variants(base, field, valid, invalid, *, required=False):
    parent, _, key = field.rpartition(".")

    def changed(value):
        if parent:
            return {**base, parent: {**base[parent], key: value}}
        return {**base, key: value}

    if required:
        obj = dict(base)
        if parent:
            obj[parent] = dict(base[parent])
            del obj[parent][key]
        else:
            del obj[key]
        yield field, "missing", obj, (f"missing {field}",)
    for value in valid:
        yield field, repr(value), changed(value), ()
    for value in invalid:
        yield field, repr(value), changed(value), (f"invalid {field}",)


def _required_variants(base):
    non_strings = (None, False, 17, 1.25, [], {})
    fields = {
        "data_class": (
            ("synthetic", "public"),
            ("Synthetic", "REAL", "private", "", *non_strings),
        ),
        "schema": ((SCHEMA_ID,), ("chipcae.vv_result/0.1", "", *non_strings)),
        "case_id": (
            ("a", "9", "A9_.-x"),
            ("", ".case", "_case", "-case", "a/b", "a\\b", "a b", "a@b",
             "éclair", "a\n", *non_strings),
        ),
        "label": (("实测", "参考值", "计划值"), ("measured", "", *non_strings)),
        "date": (
            ("20261118", "20261301"),
            ("2026111", "202611180", "２０２６１１１８", "20261118\n", "", *non_strings),
        ),
        "commit": (
            ("0123abc", "abcdef0123" * 4),
            ("123abc", "a" * 41, "0123ABC", "0123abg", "0123abc\n", "", *non_strings),
        ),
        "qoi": (
            ({}, {"custom": [None, False, 1.25, {"nested": "text"}]}),
            ("metric", None, False, 17, 1.25, []),
        ),
        "verdict": (("pass", "fail", "需讨论", "skip"), ("PASS", "unknown", "", *non_strings)),
    }
    for field, (valid, invalid) in fields.items():
        yield from _field_variants(base, field, valid, invalid, required=True)

    yield "required", "ordered reasons", {
        "notes": 17,
        "verdict": "unknown",
        "qoi": [],
        "commit": "abcdef",
        "date": "2026-11-18",
        "label": "unknown",
        "schema": "chipcae.vv_result/0.1",
        "data_class": "Synthetic",
    }, (
        "invalid data_class",
        "invalid schema",
        "missing case_id",
        "invalid label",
        "invalid date",
        "invalid commit",
        "invalid qoi",
        "invalid verdict",
        "invalid notes",
    )


def _optional_variants(base):
    non_objects = (False, 17, 1.25, "text", [])
    number_values = (None, -2, 1.25)
    non_numbers = (False, True, "1.25", [], {})
    reference = {"value": 1, "type": "解析", "source": "Fabricated example"}
    yield from _field_variants(
        base, "reference", (None, reference, {**reference, "extra": [False]}), non_objects
    )
    yield "reference", "empty object", {**base, "reference": {}}, (
        "missing reference.value", "missing reference.type", "missing reference.source",
    )
    nested_base = {**base, "reference": reference}
    yield from _field_variants(
        nested_base, "reference.value", number_values, non_numbers, required=True
    )
    yield from _field_variants(
        nested_base, "reference.type", ("解析", "独立代码", "原型", "回归"),
        ("Ansys", "", None, False, 17, [], {}), required=True
    )
    yield from _field_variants(
        nested_base, "reference.source", ("Fabricated example",),
        ("", None, False, 17, [], {}), required=True
    )
    yield "reference", "invalid children", {
        **base, "reference": {"source": "", "type": "Ansys", "value": True},
    }, ("invalid reference.value", "invalid reference.type", "invalid reference.source")

    for field in ("error", "tolerance"):
        yield from _field_variants(
            base, field, (None, {"value": 0}, {"value": None, "extra": [False]}), non_objects
        )
        yield field, "empty object", {**base, field: {}}, (f"missing {field}.value",)
        nested_base = {**base, field: {"value": 0, "metric": "absolute"}}
        yield from _field_variants(
            nested_base, f"{field}.value", number_values, non_numbers, required=True
        )
        yield from _field_variants(
            nested_base, f"{field}.metric", ("", "relative"), (None, False, 17, [], {})
        )
        yield field, "invalid children", {**base, field: {"metric": [], "value": False}}, (
            f"invalid {field}.value", f"invalid {field}.metric",
        )

    object_fields = ("timing", "memory", "machine", "build", "run", "iterations", "caps", "cap_checks")
    for field in object_fields:
        yield from _field_variants(
            base, field, (None, {}, {"extra": [None, 1, False]}), non_objects
        )
    yield from _field_variants(
        base, "notes", ([], ["", "Fabricated note"]),
        (None, False, 17, "x", {}, ["ok", 1], [False, None, {}])
    )
    yield from _field_variants(
        base, "src_dirty", (True, False), (None, 0, 1, 1.25, "true", [], {})
    )
    yield "extra", "unknown top-level key", {**base, "extra": [None, False, {}]}, ()

    fields = ("reference", "error", "tolerance", *object_fields, "notes", "src_dirty")
    obj = {**base, **dict.fromkeys(reversed(fields), "wrong type")}
    yield "optional", "ordered reasons", obj, tuple(f"invalid {field}" for field in fields)


class ResultSchemaTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.temp = Path(temp.name).resolve()

    def _checker(self):
        # Import inside tests so missing deliverables still leave CLI and schema tests runnable.
        try:
            return importlib.import_module(CHECKER_MODULE)
        except ModuleNotFoundError as error:
            if error.name != CHECKER_MODULE:
                raise
            self.fail(f"Missing result checker module {CHECKER_MODULE}")

    def _write_json(self, obj):
        path = self.temp / "variant.json"
        path.write_text(json.dumps(obj, ensure_ascii=False), encoding="utf-8")
        return path

    def _assert_reasons(self, actual, *expected):
        self.assertIsInstance(actual, list)
        self.assertEqual(len(actual), len(expected), actual)
        for reason, substring in zip(actual, expected):
            self.assertIn(substring, reason)
        self.assertEqual(len(actual), len(set(actual)), actual)

    def _temp_repo(self):
        repo = self.temp / "repo"
        repo.mkdir()
        self._git(repo, "init", "--quiet")
        (repo / "tests/vv/data").mkdir(parents=True)
        return repo

    def _git(self, repo, *args):
        result = subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def _cli(self, cwd, *args):
        env = os.environ.copy()
        # The module is in the task checkout even when Git discovery starts in a temp repo.
        env["PYTHONPATH"] = str(ROOT)
        python = [sys.executable]
        if sys.flags.no_site:
            python.append("-S")
        return subprocess.run(
            [*python, "-m", CHECKER_MODULE, *map(str, args)],
            cwd=cwd, env=env, capture_output=True, text=True,
        )

    def _assert_cli_success(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")

    def _assert_cli_violation(self, result, reason):
        self.assertEqual(result.returncode, 1, result.stderr)
        lines = result.stderr.splitlines()
        self.assertEqual(len(lines), 1, result.stderr)
        path, separator, diagnostic = lines[0].partition(": ")
        self.assertTrue(path and separator, result.stderr)
        self.assertIn(reason, diagnostic)

    def test_valid_fixtures(self):
        repo = self._temp_repo()
        sample = repo / "tests/vv/data/sample.result.json"
        shutil.copyfile(FIXTURES / "valid_synthetic.json", sample)
        self._assert_cli_success(self._cli(repo, "--repo", repo, sample.relative_to(repo)))

        check_file = self._checker().check_file
        for name in ("valid_synthetic.json", "valid_public.json"):
            with self.subTest(fixture=name):
                self._assert_reasons(check_file(FIXTURES / name))
        outside_repo = self.temp / "valid.json"
        shutil.copyfile(FIXTURES / "valid_synthetic.json", outside_repo)
        self._assert_reasons(check_file(outside_repo))

    def test_not_json(self):
        path = FIXTURES / "bad_not_json.json"
        self.assertEqual(path.read_text(encoding="utf-8").strip(), '{"schema":')
        check_file = self._checker().check_file
        self._assert_reasons(check_file(path), "not valid JSON")

        invalid = self.temp / "invalid.json"
        invalid.write_bytes(b'{"qoi":"\xff"}')
        self._assert_reasons(check_file(invalid), "not valid JSON")
        for constant in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(constant=constant):
                invalid.write_text('{"future": [{"value": ' + constant + '}]}', encoding="utf-8")
                self._assert_reasons(check_file(invalid), "not valid JSON")
        self._assert_reasons(check_file(self.temp / "missing.json"), "cannot read file")

    def test_empty(self):
        path = FIXTURES / "bad_empty.json"
        self.assertEqual(path.stat().st_size, 0)
        self._assert_reasons(self._checker().check_file(path), "not valid JSON")

    def test_toplevel_array(self):
        path = FIXTURES / "bad_toplevel_array.json"
        self.assertEqual(_load_json(path), [])
        check_file = self._checker().check_file
        self._assert_reasons(check_file(path), "top-level not an object")
        for obj in NON_OBJECTS[1:]:
            with self.subTest(obj=obj):
                self._assert_reasons(check_file(self._write_json(obj)), "top-level not an object")

    def test_missing_data_class(self):
        self._assert_reasons(
            self._checker().check_file(FIXTURES / "bad_missing_data_class.json"),
            "missing data_class",
        )

    def test_real_rejected(self):
        path = FIXTURES / "bad_real.json"
        check_file = self._checker().check_file
        self._assert_reasons(check_file(path), REAL_REJECTION)
        repo = self._temp_repo()
        sample = repo / "tests/vv/data/result.json"
        shutil.copyfile(path, sample)
        self._assert_cli_violation(self._cli(repo, sample.relative_to(repo)), REAL_REJECTION)

    def test_case_variant_rejected(self):
        path = FIXTURES / "bad_data_class_case.json"
        obj = _load_json(path)
        self.assertEqual(obj["data_class"], "Synthetic")
        check_file = self._checker().check_file
        self._assert_reasons(check_file(path), "invalid data_class")
        self._assert_reasons(
            check_file(self._write_json({**obj, "data_class": "REAL"})), "invalid data_class"
        )

    def test_missing_other_field(self):
        check_file = self._checker().check_file
        self._assert_reasons(check_file(FIXTURES / "bad_missing_case_id.json"), "missing case_id")
        base = _load_json(FIXTURES / "valid_synthetic.json")
        for field, variant, obj, expected in _required_variants(base):
            with self.subTest(field=field, variant=variant):
                self._assert_reasons(check_file(self._write_json(obj)), *expected)

    def test_reference_type_ansys(self):
        self._assert_reasons(
            self._checker().check_file(FIXTURES / "bad_reference_ansys.json"),
            "invalid reference.type",
        )

    def test_outside_allowed_dir(self):
        repo = self._temp_repo()
        disallowed = repo / "other/result.json"
        disallowed.parent.mkdir()
        outside = self.temp / "result.json"
        alias = repo / "tests/vv/data/result.json"
        for target in (disallowed, outside):
            shutil.copyfile(FIXTURES / "valid_synthetic.json", target)
        alias.symlink_to(disallowed)
        paths = {
            "disallowed": disallowed.relative_to(repo),
            "outside repo": outside,
            "dot-dot escape": Path("tests/vv/data/../../../other/result.json"),
            "symlink escape": alias.relative_to(repo),
        }
        for scenario, path in paths.items():
            with self.subTest(scenario=scenario):
                self._assert_cli_violation(self._cli(repo, path), "outside allowed dirs")
        self.assertEqual(self._checker().ALLOWED_DIRS, ALLOWED_DIRS)

    def test_size_limit(self):
        repo = self._temp_repo()
        sample = repo / "cases/benchmarks/result.json"
        sample.parent.mkdir(parents=True)
        content = (FIXTURES / "valid_synthetic.json").read_bytes()
        sample.write_bytes(content.ljust(MAX_BYTES, b" "))
        self.assertEqual(sample.stat().st_size, MAX_BYTES)
        self._assert_cli_success(self._cli(repo, sample.relative_to(repo)))
        with sample.open("ab") as output:
            output.write(b" ")
        self.assertEqual(sample.stat().st_size, MAX_BYTES + 1)
        self._assert_cli_violation(self._cli(repo, sample.relative_to(repo)), "exceeds 2 MiB")
        checker = self._checker()
        self.assertEqual(checker.MAX_BYTES, MAX_BYTES)
        self._assert_reasons(checker.check_file(sample))

    def test_repo_clean(self):
        with self.subTest(scenario="task repo"):
            self._assert_cli_success(self._cli(ROOT))

        repo = self._temp_repo()
        data = Path("tests/vv/data")
        invalid = FIXTURES / "bad_missing_data_class.json"
        upper = data / "Result.json"
        lower = data / "result.json"
        with self.subTest(scenario="empty index"):
            result = self._cli(repo)
            self._assert_cli_success(result)
            self.assertEqual(result.stdout, "0 result.json checked\n")
        with self.subTest(scenario="uppercase tracked basename"):
            shutil.copyfile(invalid, repo / upper)
            self._git(repo, "add", "--", str(upper))
            result = self._cli(repo)
            self._assert_cli_success(result)
            self.assertEqual(result.stdout, "0 result.json checked\n")
        # Case-insensitive worktrees cannot hold both names at once.
        self._git(repo, "rm", "--cached", "--", str(upper))
        (repo / upper).unlink()
        shutil.copyfile(invalid, repo / lower)
        with self.subTest(scenario="untracked lowercase basename"):
            result = self._cli(repo)
            self._assert_cli_success(result)
            self.assertEqual(result.stdout, "0 result.json checked\n")
        with self.subTest(scenario="lowercase tracked basename"):
            self._git(repo, "add", "--", str(lower))
            self._assert_cli_violation(self._cli(repo), "missing data_class")
        with self.subTest(scenario="aliases from a subdirectory"):
            alias = data / "alias.json"
            (repo / alias).symlink_to(lower.name)
            result = self._cli(repo / "tests/vv", lower, repo / lower, alias)
            self._assert_cli_violation(result, "missing data_class")
        with self.subTest(scenario="invalid --repo"):
            result = self._cli(repo, "--repo", self.temp / "missing-repo")
            self.assertEqual(result.returncode, 2, result.stderr)

    def test_schema_agrees_with_checker(self):
        try:
            import jsonschema
        except ModuleNotFoundError as error:
            if error.name != "jsonschema":
                raise
            self.skipTest("jsonschema not installed")

        self.assertTrue(SCHEMA_PATH.is_file(), f"Missing result schema {SCHEMA_PATH}")
        schema = _load_json(SCHEMA_PATH)
        self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertEqual(schema["$id"], SCHEMA_ID)
        jsonschema.Draft202012Validator.check_schema(schema)
        validator = jsonschema.Draft202012Validator(schema)
        self.assertTrue(validator.is_valid(_load_json(FIXTURES / "bad_real.json")))

        checker = self._checker()
        self.assertEqual(checker.SCHEMA_PATH, SCHEMA_PATH)
        excluded_fixtures = {"bad_empty.json", "bad_not_json.json", "bad_real.json"}
        for path in sorted(FIXTURES.glob("*.json")):
            if path.name in excluded_fixtures:
                continue
            with self.subTest(fixture=path.name):
                self.assertEqual(validator.is_valid(_load_json(path)), checker.check_file(path) == [])

        base = _load_json(FIXTURES / "valid_synthetic.json")
        for variants in (_required_variants(base), _optional_variants(base)):
            for field, variant, obj, _ in variants:
                with self.subTest(field=field, variant=variant):
                    path = self._write_json(obj)
                    self.assertEqual(validator.is_valid(_load_json(path)), checker.check_file(path) == [])
        for obj in NON_OBJECTS:
            with self.subTest(top_level=obj):
                path = self._write_json(obj)
                self.assertFalse(validator.is_valid(_load_json(path)))
                self.assertEqual(validator.is_valid(_load_json(path)), checker.check_file(path) == [])

    def test_optional_type(self):
        check_file = self._checker().check_file
        self._assert_reasons(check_file(FIXTURES / "bad_notes_type.json"), "invalid notes")
        base = _load_json(FIXTURES / "valid_synthetic.json")
        for field, variant, obj, expected in _optional_variants(base):
            with self.subTest(field=field, variant=variant):
                self._assert_reasons(check_file(self._write_json(obj)), *expected)
