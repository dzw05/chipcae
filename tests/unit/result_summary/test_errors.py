# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, save, h5py, np
from tools.vv.result_summary import ResultSummaryError, main


class Errors(unittest.TestCase):
    def test_error_interface(self):
        error = ResultSummaryError(14, 'INPUT_INVALID', 'invalid fixture')
        self.assertIsInstance(error, Exception)
        self.assertEqual(error.code, 14)
        self.assertEqual(error.name, 'INPUT_INVALID')

    def test_conflicting_name_tables(self):
        for field in ('layer_names', 'material_names'):
            for cut in (False, True):
                for change in ('value', 'order', 'length', 'empty'):
                    with self.subTest(field=field, cut=cut, change=change), tempfile.TemporaryDirectory() as tmp:
                        run = Path(tmp)
                        step, _ = fixture(run, cut=cut)
                        with h5py.File(step, 'r+') as f:
                            path = 'VTKHDF/cut/FieldData/' + field
                            names = f[path].asstr()[...].tolist()
                            if change == 'value':
                                names[0] = 'conflicting name'
                            elif change == 'order':
                                names.reverse()
                            elif change == 'length':
                                names.append('extra name')
                            else:
                                names = []
                            del f[path]
                            f.create_dataset(path, data=names, dtype=h5py.string_dtype())
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err):
                            self.assertEqual(main([str(run)]), 14)
                        self.assertTrue(err.getvalue().startswith('result_summary: INPUT_INVALID:'))
                        self.assertIn(field, err.getvalue())
                        self.assertIn('conflicting', err.getvalue())
                        self.assertEqual(len(err.getvalue().splitlines()), 1)

    def test_required_field_diagnostics(self):
        for block in ('hex', 'cut'):
            for field in ('PointData/U', 'CellData/layer_id', 'CellData/material_id',
                          'CellData/cell_kind', 'CellData/elem_gid', 'Types'):
                if block == 'hex' and field == 'Types':
                    continue
                with self.subTest(block=block, field=field), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = fixture(run, cut=True)
                    path = f'VTKHDF/{block}/{field}'
                    with h5py.File(step, 'r+') as f:
                        del f[path]
                    err = io.StringIO()
                    with contextlib.redirect_stderr(err):
                        self.assertEqual(main([str(run)]), 14)
                    self.assertEqual(err.getvalue(),
                                     f'result_summary: INPUT_INVALID: /{path}: required field missing\n')

    def test_cut_types_dtype(self):
        for dtype in ('int8', 'int32', 'uint16', 'float64', 'uint8'):
            with self.subTest(dtype=dtype), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run, cut=True)
                with h5py.File(step, 'r+') as f:
                    path = 'VTKHDF/cut/Types'
                    del f[path]
                    f[path] = np.array([42, 42], dtype=dtype)
                err = io.StringIO()
                with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main([str(run)]), 0 if dtype == 'uint8' else 14)
                if dtype != 'uint8':
                    self.assertIn('cut/Types: expected uint8', err.getvalue())

    def test_name_table_dtype(self):
        for block in ('hex', 'cut'):
            for field in ('layer_names', 'material_names'):
                for dtype in ('int32', 'float64', 'S8', 'utf8'):
                    with self.subTest(block=block, field=field, dtype=dtype), tempfile.TemporaryDirectory() as tmp:
                        run = Path(tmp)
                        step, _ = fixture(run)
                        with h5py.File(step, 'r+') as f:
                            # Remove the other copy so this checks dtype alone.
                            for name in ('hex', 'cut'):
                                del f[f'VTKHDF/{name}/FieldData/{field}']
                            path = f'VTKHDF/{block}/FieldData/{field}'
                            string = dtype in ('S8', 'utf8')
                            f.create_dataset(path, data=['one', 'two'] if string else [1, 2],
                                             dtype=h5py.string_dtype() if dtype == 'utf8' else dtype)
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                            self.assertEqual(main([str(run)]), 0 if string else 14)
                        if not string:
                            self.assertIn(f'/{path}: expected a string vector', err.getvalue())

    def test_invalid_inputs(self):
        cases = ['manifest_missing', 'step_missing', 'schema', 'hex_missing', 'version', 'T_length', 'elem_dtype', 'U_missing', 'kind_missing', 'duplicate', 'unit', 'empty_manifest', 'malformed_manifest']
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, manifest = fixture(run)
                code = 12 if case in ('manifest_missing', 'step_missing') else 14
                if case == 'manifest_missing':
                    (run/'manifest.json').unlink()
                elif case == 'step_missing':
                    step.unlink()
                elif case == 'malformed_manifest':
                    (run/'manifest.json').write_text('{\n bad JSON')
                elif case == 'empty_manifest':
                    (run/'manifest.json').write_text('')
                elif case in ('schema', 'duplicate', 'unit'):
                    if case == 'schema':
                        manifest['schema_version'] = 'wrong'
                    elif case == 'duplicate':
                        manifest['summary'].append(dict(manifest['summary'][1]))
                    else:
                        manifest['summary'][1]['unit'] = 'mm'
                    save(run, manifest)
                else:
                    with h5py.File(step, 'r+') as f:
                        b = f['VTKHDF/hex']
                        if case == 'hex_missing':
                            f['VTKHDF'].move('hex', 'Hex')
                        elif case == 'version':
                            b.attrs['Version'] = [2, 7]
                        elif case == 'T_length':
                            del b['PointData/T']
                            b['PointData/T'] = np.array([300.], dtype='float64')
                        elif case == 'elem_dtype':
                            del b['CellData/elem_gid']
                            b['CellData/elem_gid'] = np.array([0, 1], dtype='int32')
                        else:
                            del b['PointData/U' if case == 'U_missing' else 'CellData/cell_kind']
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    self.assertEqual(main([str(run)]), code)
                name = 'INPUT_REFERENCE_MISSING' if code == 12 else 'INPUT_INVALID'
                self.assertTrue(err.getvalue().startswith('result_summary: '+name+':'), err.getvalue())
                self.assertEqual(len(err.getvalue().splitlines()), 1)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
