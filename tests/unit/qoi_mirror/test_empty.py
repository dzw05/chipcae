# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Empty and repeated exact-axis inputs, determinism, and input preservation."""
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


class EmptyTest(unittest.TestCase):
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

    def test_empty(self):
        empty = np.empty((0, 6))
        before = empty.copy()
        first, second = mirror_quarter(empty), mirror_quarter(empty)
        self.assertEqual(first.shape, (0, 6))
        np.testing.assert_array_equal(first, second)
        np.testing.assert_array_equal(empty, before)

    def test_repeated_axes(self):
        points = np.array([[0., 0., 0., 0., 0., 0.],
                           [1., 0., 0., 2., 0., 3.],
                           [0., 2., 0., 0., 4., 5.]])
        repeated = np.repeat(points, 3, axis=0)
        before = repeated.copy()
        first = sorted_rows(mirror_quarter(repeated))
        second = sorted_rows(mirror_quarter(repeated))
        # Origin once, plus two reflected points for each nonzero axis point.
        self.assertEqual(len(first), 1 + 2 + 2)
        self.assertEqual(len(np.unique(first[:, :3], axis=0)), len(first))
        expected = sorted_rows(np.array([[0., 0., 0., 0., 0., 0.],
                                        [1., 0., 0., 2., 0., 3.],
                                        [-1., 0., 0., -2., 0., 3.],
                                        [0., 2., 0., 0., 4., 5.],
                                        [0., -2., 0., 0., -4., 5.]]))
        self.assertLessEqual(np.max(np.abs(first - expected)), 1e-12)
        np.testing.assert_array_equal(first, second)
        np.testing.assert_array_equal(repeated, before)


if __name__ == "__main__":
    # CTest passes the source directory as a legacy positional argument.
    argv = sys.argv.copy()
    if argv[1:2] == [str(source_dir)]:
        del argv[1]
    unittest.main(argv=argv)
