# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
import contextlib
import io
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
from fixtures import fixture, save, h5py, np
from tools.vv.result_summary import ResultSummaryError, main, summarize


class Errors(unittest.TestCase):
    def assert_invalid(self, run, message):
        with self.assertRaises(ResultSummaryError) as caught:
            summarize(run)
        self.assertEqual(caught.exception.code, 14)
        self.assertEqual(caught.exception.name, 'INPUT_INVALID')
        self.assertIn(message, str(caught.exception))
        err, out = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
            self.assertEqual(main([str(run)]), 14)
        self.assertEqual(out.getvalue(), '')
        self.assertTrue(err.getvalue().startswith('result_summary: INPUT_INVALID: '))
        self.assertEqual(len(err.getvalue().splitlines()), 1)

    def test_dataset_ranks(self):
        paths = ('hex/NumberOfPoints', 'hex/NumberOfCells', 'hex/Points',
                 'hex/PointData/U', 'hex/PointData/T', 'hex/Types',
                 'hex/CellData/layer_id', 'hex/CellData/material_id',
                 'hex/CellData/cell_kind', 'hex/CellData/elem_gid',
                 'cut/PointData/T', 'cut/Types',
                 'hex/FieldData/layer_names', 'cut/FieldData/material_names',
                 'eb_surface/NumberOfPoints', 'eb_surface/Polygons/NumberOfCells')
        for path in paths:
            for scalar in (True, False):
                with self.subTest(path=path, scalar=scalar), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        full = 'VTKHDF/' + path
                        original = f[full][...]
                        dtype = f[full].dtype
                        value = ('bad' if dtype.kind == 'O' else 0) if scalar else original.reshape((1,) + original.shape)
                        del f[full]
                        f.create_dataset(full, data=value, dtype=dtype)
                    self.assert_invalid(run, path.split('/')[-1])

    def test_nonfinite_displacement_components(self):
        for component in range(3):
            for value in (np.nan, np.inf, -np.inf):
                with self.subTest(component=component, value=value), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        f['VTKHDF/hex/PointData/U'][5, component] = value
                    self.assert_invalid(run, 'PointData/U: nonfinite values')

    def test_empty_summary_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            _, manifest = fixture(run)
            manifest['summary'][0]['name'] = ''
            save(run, manifest)
            self.assert_invalid(run, 'invalid summary entry')

    def test_invalid_steps(self):
        for steps in ({}, [], [None], [{'file': 123}]):
            with self.subTest(steps=steps), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                _, manifest = fixture(run)
                manifest['steps'] = steps
                save(run, manifest)
                with self.assertRaises(ResultSummaryError) as caught:
                    summarize(run)
                self.assertEqual(caught.exception.code, 14)
                self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                err, out = io.StringIO(), io.StringIO()
                with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                    self.assertEqual(main([str(run)]), 14)
                self.assertEqual(out.getvalue(), '')
                self.assertEqual(err.getvalue(),
                                 'result_summary: INPUT_INVALID: expected nonempty steps[0].file string\n')

    def test_warpage_unit_diagnostic(self):
        for unit in (None, 'mm', ''):
            with self.subTest(unit=unit), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                _, manifest = fixture(run)
                manifest['summary'][1]['unit'] = unit
                save(run, manifest)
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    self.assertEqual(main([str(run)]), 14)
                self.assertEqual(err.getvalue(),
                                 'result_summary: INPUT_INVALID: summary[1].unit (warpage_pv): expected "m"\n')

    def test_manifest_invalid_utf8(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            (run / 'manifest.json').write_bytes(b'{"schema_version": "\xff"}')
            with self.assertRaises(ResultSummaryError) as caught:
                summarize(run)
            self.assertEqual(caught.exception.code, 14)
            self.assertEqual(caught.exception.name, 'INPUT_INVALID')
            self.assertEqual(str(caught.exception), 'manifest.json: invalid UTF-8')
            err, out = io.StringIO(), io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                self.assertEqual(main([str(run)]), 14)
            self.assertEqual(out.getvalue(), '')
            self.assertEqual(err.getvalue(),
                             'result_summary: INPUT_INVALID: manifest.json: invalid UTF-8\n')

    def test_manifest_must_be_object(self):
        for manifest in (None, [], 'manifest', 1, True):
            with self.subTest(manifest=manifest), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                save(run, manifest)
                self.assert_invalid(run, 'manifest must be an object')

    def test_unresolvable_dataset_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                block = f['VTKHDF/hex']
                del block['Points']
                block['Points'] = h5py.ExternalLink('missing-data.h5', '/Points')
            # The step file opens, but its required geometry cannot be resolved.
            self.assert_invalid(run, 'open')

    def test_hdf5_failures(self):
        for stage, target, code, name in (
                ('open', 'h5py.File', 12, 'INPUT_REFERENCE_MISSING'),
                ('read', '_read', 14, 'INPUT_INVALID')):
            for error_type in (OSError, RuntimeError, EOFError):
                with self.subTest(stage=stage, error=error_type), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    fixture(run)
                    message = 'HDF5 failure\nwith details'
                    with patch('tools.vv.result_summary.' + target,
                               side_effect=error_type(message)):
                        with self.assertRaises(ResultSummaryError) as caught:
                            summarize(run)
                        self.assertEqual(caught.exception.code, code)
                        self.assertEqual(caught.exception.name, name)
                    err, out = io.StringIO(), io.StringIO()
                    with patch('tools.vv.result_summary.' + target,
                               side_effect=error_type(message)) as cli_failure, \
                            contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                        self.assertEqual(main([str(run)]), code)
                    self.assertEqual(out.getvalue(), '')
                    self.assertEqual(err.getvalue(),
                                     f'result_summary: {name}: HDF5 failure with details\n')
                    cli_failure.assert_called_once()
                    self.assertEqual(summarize(run)['blocks']['hex']['points'], 11)

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
                with self.subTest(block=block, field=field), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = fixture(run, cut=True)
                    path = f'VTKHDF/{block}/{field}'
                    with h5py.File(step, 'r+') as f:
                        count = 'NumberOfPoints' if field == 'PointData/U' else 'NumberOfCells'
                        self.assertGreater(int(f[f'VTKHDF/{block}/{count}'][...].sum()), 0)
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

    def test_types_validation(self):
        for block in ('hex', 'cut'):
            for empty in (False, True):
                for invalid in ('missing', 'dtype', 'length', 'rank', 'valid'):
                    with self.subTest(block=block, empty=empty, invalid=invalid), tempfile.TemporaryDirectory() as tmp:
                        run = Path(tmp)
                        step, _ = fixture(run, cut=True)
                        with h5py.File(step, 'r+') as f:
                            group = f['VTKHDF/' + block]
                            if empty:
                                group['NumberOfCells'][...] = 0
                                for field in list(group['CellData']):
                                    dtype = group['CellData/' + field].dtype
                                    del group['CellData/' + field]
                                    group['CellData/' + field] = np.empty(0, dtype=dtype)
                            count = int(group['NumberOfCells'][...].sum())
                            del group['Types']
                            shape = (count, 1) if invalid == 'rank' else (count + (invalid == 'length'),)
                            if invalid != 'missing':
                                group['Types'] = np.full(shape, 12 if block == 'hex' else 42,
                                                         dtype='int32' if invalid == 'dtype' else 'uint8')
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                            self.assertEqual(main([str(run)]), 0 if invalid == 'valid' else 14)
                        if invalid == 'missing':
                            self.assertEqual(err.getvalue(),
                                             f'result_summary: INPUT_INVALID: /VTKHDF/{block}/Types: required field missing\n')
                            with self.assertRaises(ResultSummaryError) as caught:
                                summarize(run)
                            self.assertEqual(caught.exception.code, 14)
                            self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                        elif invalid != 'valid':
                            self.assertIn(f'{block}/Types: expected', err.getvalue())

    def test_cell_type_values(self):
        for block, expected in (('hex', 12), ('cut', 42)):
            for value in (0, 10, 12, 42, 255):
                with self.subTest(block=block, value=value), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = fixture(run, cut=True)
                    with h5py.File(step, 'r+') as f:
                        # Keep one valid entry to detect mixed valid/invalid types.
                        f[f'VTKHDF/{block}/Types'][1] = value
                    if value == expected:
                        self.assertEqual(summarize(run)['blocks'][block]['cells'], 2)
                        continue
                    with self.assertRaises(ResultSummaryError) as caught:
                        summarize(run)
                    self.assertEqual(caught.exception.code, 14)
                    self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                    err, out = io.StringIO(), io.StringIO()
                    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                        self.assertEqual(main([str(run)]), 14)
                    self.assertEqual(out.getvalue(), '')
                    self.assertEqual(err.getvalue(),
                                     f'result_summary: INPUT_INVALID: /VTKHDF/{block}/Types: expected cell type {expected}\n')

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
                if case == 'duplicate':
                    self.assertEqual(err.getvalue(),
                                     'result_summary: INPUT_INVALID: summary[2].name: duplicate summary name: warpage_pv\n')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
