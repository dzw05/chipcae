# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Analytic kinematic symmetry checks; all lengths are mm."""
import ast
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

helper_dir = Path(__file__).resolve().parent
source_dir = helper_dir.parents[2]
for directory in (source_dir, helper_dir):
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))

from qoi_parse import np, sorted_rows
from tools.vv.warpage_qoi import mirror_quarter


def field(xs, ys):
    X, Y = np.meshgrid(xs, ys, indexing="ij")
    x, y = X.ravel(), Y.ravel()
    return np.column_stack((x, y, np.zeros(x.size),
                            x * (1 + y**2), y * (2 + x**2), x**2 + 3 * y**2))


class AxesTest(unittest.TestCase):
    def test_import_preserves_argv(self):
        self._check_isolated_imports(check_helper=False)

    def test_helper_path_from_file(self):
        self._check_isolated_imports(check_helper=True)

    def _check_isolated_imports(self, check_helper):
        test_file = Path(__file__).resolve()
        helper_dir = test_file.parent
        cases = ([], ["runner"],
                 ["runner", "--verbose", "/does-not-exist", "--quiet"],
                 ["runner", "--quiet", "--verbose", "/does-not-exist"])
        for argv in cases:
            for repeat in range(2):
                with self.subTest(argv=argv, repeat=repeat):
                    host_argv = sys.argv
                    host_path = sys.path.copy()
                    host_modules = sys.modules.copy()
                    host_cwd = Path.cwd()
                    try:
                        with tempfile.TemporaryDirectory() as temporary:
                            if check_helper:
                                os.chdir(temporary)
                                sys.modules.pop("qoi_parse", None)
                                sys.path[:] = [
                                    entry for entry in sys.path
                                    if Path(entry or host_cwd).resolve() != helper_dir
                                ]
                            sys.argv = argv.copy()
                            before = sys.argv.copy()
                            spec = importlib.util.spec_from_file_location(
                                "_qoi_mirror_isolated", test_file)
                            module = importlib.util.module_from_spec(spec)
                            try:
                                spec.loader.exec_module(module)
                            except Exception as error:
                                self.fail(f"Import must succeed for {before!r}: {error!r}")
                            self.assertEqual(sys.argv, before)
                            if check_helper:
                                self.assertEqual(
                                    Path(sys.modules["qoi_parse"].__file__).resolve(),
                                    (helper_dir / "qoi_parse.py").resolve())
                    finally:
                        os.chdir(host_cwd)
                        sys.argv = host_argv
                        sys.path[:] = host_path
                        sys.modules.clear()
                        sys.modules.update(host_modules)
        if check_helper:
# Exclude test bodies and the guarded unittest launcher.
            tree = ast.parse(test_file.read_text())
            import_time = ast.Module(body=[
                node for node in tree.body
                if not isinstance(node, (ast.FunctionDef, ast.ClassDef))
                and not (isinstance(node, ast.If)
                         and "__name__" in ast.unparse(node.test))
            ], type_ignores=[])
            self.assertTrue(any(
                isinstance(node, ast.Name) and node.id == "__file__"
                for node in ast.walk(import_time)))
            self.assertFalse(any(
                isinstance(node, ast.Attribute) and node.attr == "argv"
                for node in ast.walk(import_time)),
                "Import-time helper resolution must not read or write argv")
            self.assertFalse(any(
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and node.func.attr == "pop"
                and node.args and isinstance(node.args[0], ast.Constant)
                and node.args[0].value == 1
                for node in ast.walk(import_time)))

    def test_axes(self):
        xs, ys = np.arange(4.0), np.arange(3.0)
        quarter = field(xs, ys)
        full_x = np.arange(-3.0, 4.0)
        full_y = np.arange(-2.0, 3.0)
        expected = sorted_rows(field(full_x, full_y))
        actual = sorted_rows(mirror_quarter(quarter))
        self.assertEqual(len(actual), (2 * len(xs) - 1) * (2 * len(ys) - 1))
        self.assertEqual(len(np.unique(actual[:, :3], axis=0)), len(actual))
        self.assertLessEqual(np.max(np.abs(actual[:, :3] - expected[:, :3])), 1e-12)
        self.assertLessEqual(np.max(np.abs(actual[:, 3:] - expected[:, 3:])), 1e-12)
        for column, component in enumerate(("ux", "uy", "uz"), start=3):
            with self.subTest(component=component):
                self.assertLessEqual(
                    np.max(np.abs(actual[:, column] - expected[:, column])), 1e-12
                )
        values = {tuple(row[:3]): row[3:] for row in actual}
        for (x, y, z), u in values.items():
            across_x = values[(-x, y, z)]
            across_y = values[(x, -y, z)]
            self.assertLessEqual(abs(u[0] + across_x[0]), 1e-12)
            self.assertLessEqual(abs(u[1] + across_y[1]), 1e-12)
            self.assertLessEqual(abs(u[2] - across_x[2]), 1e-12)
            self.assertLessEqual(abs(u[2] - across_y[2]), 1e-12)


if __name__ == "__main__":
    # CTest passes the source directory as a legacy positional argument.
    argv = sys.argv.copy()
    if argv[1:2] == [str(source_dir)]:
        del argv[1]
    unittest.main(argv=argv)
