# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Additional coverage for structural validation and documented edge policies."""
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from fixtures import fixture, h5py, np
from tools.vv.result_summary import ResultSummaryError, main, summarize


class Edges(unittest.TestCase):
    def test_partition_count_dtypes(self):
        paths = ('hex/NumberOfPoints', 'hex/NumberOfCells',
                 'cut/NumberOfPoints', 'cut/NumberOfCells',
                 'eb_surface/NumberOfPoints', 'eb_surface/Polygons/NumberOfCells')
        for path in paths:
            for dtype in ('int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32',
                          'uint64', 'float64', 'int64', '>i8'):
                with self.subTest(path=path, dtype=dtype), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        key = 'VTKHDF/' + path
                        values = f[key][...].astype(dtype)
                        del f[key]
                        f[key] = values
                    if np.dtype(dtype).newbyteorder('=') == np.dtype('int64'):
                        self.assertEqual(summarize(run)['blocks']['hex']['points'], 11)
                    else:
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err):
                            self.assertEqual(main([str(run)]), 14)
                        self.assertIn(f'/{key}: expected int64 partition counts', err.getvalue())

    def test_required_geometry(self):
        for name in ('hex', 'cut', 'eb_surface'):
            for empty in (False, True):
                for invalid in ('missing', 'length', 'dtype', 'partitions'):
                    with self.subTest(block=name, empty=empty, invalid=invalid), tempfile.TemporaryDirectory() as tmp:
                        run = Path(tmp)
                        step, _ = fixture(run, cut=True)
                        with h5py.File(step, 'r+') as f:
                            block = f['VTKHDF/' + name]
                            cells = 'Polygons/NumberOfCells' if name == 'eb_surface' else 'NumberOfCells'
                            if empty:
                                block['NumberOfPoints'][...] = 0
                                block[cells][...] = 0
                            n = int(block['NumberOfPoints'][...].sum())
                            del block['Points']
                            if invalid != 'missing':
                                block['Points'] = np.zeros((n + (invalid == 'length'), 3),
                                                           dtype='float32' if invalid == 'dtype' else 'float64')
                            if invalid == 'partitions':
                                del block[cells]
                                block[cells] = np.zeros(1, dtype='int64')
                        with self.assertRaises(ResultSummaryError) as caught:
                            summarize(run)
                        self.assertEqual(caught.exception.code, 14)
                        expected = 'partition count mismatch' if invalid == 'partitions' else '/Points:'
                        self.assertIn(expected, str(caught.exception))

    def test_present_fields_on_empty_blocks_are_validated(self):
        fields = {'PointData/T': 'float64', 'CellData/layer_id': 'int32',
                  'CellData/material_id': 'int32', 'CellData/cell_kind': 'int32',
                  'CellData/elem_gid': 'int64'}
        for name in ('hex', 'cut'):
            for path, dtype in fields.items():
                for invalid in ('length', 'dtype'):
                    with self.subTest(block=name, field=path, invalid=invalid), tempfile.TemporaryDirectory() as tmp:
                        run = Path(tmp)
                        step, _ = fixture(run, cut=True)
                        with h5py.File(step, 'r+') as f:
                            block = f['VTKHDF/' + name]
                            for group in ('Points', 'PointData', 'CellData', 'Types'):
                                del block[group]
                            block['Points'] = np.empty((0, 3), dtype='float64')
                            block['NumberOfPoints'][...] = 0
                            block['NumberOfCells'][...] = 0
                            # Only the empty block has T in the temperature
                            # cases: validation must precede presence detection.
                            if path == 'PointData/T':
                                other = 'cut' if name == 'hex' else 'hex'
                                del f[f'VTKHDF/{other}/PointData/T']
                            block[path] = np.zeros(1 if invalid == 'length' else 0,
                                                   dtype=dtype if invalid == 'length' else 'uint8')
                        with self.assertRaises(ResultSummaryError) as caught:
                            summarize(run)
                        self.assertEqual(caught.exception.code, 14)
                        self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                        self.assertIn(f'/VTKHDF/{name}/{path}: expected', str(caught.exception))
                        err, out = io.StringIO(), io.StringIO()
                        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                            self.assertEqual(main([str(run)]), 14)
                        self.assertEqual(out.getvalue(), '')
                        self.assertIn(f'result_summary: INPUT_INVALID: /VTKHDF/{name}/{path}: expected', err.getvalue())
                        self.assertEqual(len(err.getvalue().splitlines()), 1)

    def test_empty_leading_block_temperature_alignment(self):
        for empty_t in (False, True):
            with self.subTest(empty_t=empty_t), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run, cut=True)
                with h5py.File(step, 'r+') as f:
                    block = f['VTKHDF/hex']
                    for path in ('Points', 'PointData', 'CellData', 'Types'):
                        del block[path]
                    block['Points'] = np.empty((0, 3), dtype='float64')
                    block['NumberOfPoints'][...] = 0
                    block['NumberOfCells'][...] = 0
                    if empty_t:
                        block['PointData/T'] = np.empty(0, dtype='float64')
                    # Put the top point second so top indices differ from
                    # indices into the subset of top temperatures.
                    cut = f['VTKHDF/cut']
                    cut['Points'][...] = [[0., 0., 0.], [2., 0., 1.]]
                    cut['PointData/T'][...] = [100., 999.]
                result = summarize(run)
                self.assertEqual(result['blocks']['hex']['points'], 0)
                self.assertEqual(result['blocks']['cut']['points'], 2)
                self.assertEqual(result['warpage']['n_top_points'], 1)
                self.assertEqual(result['temperature'], dict(
                    min=100., max=999., top_min=999., top_max=999., top_mean=999.))

    def test_empty_trailing_block_temperature_alignment(self):
        for empty_t in (False, True):
            with self.subTest(empty_t=empty_t), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run, cut=True)
                with h5py.File(step, 'r+') as f:
                    block = f['VTKHDF/cut']
                    for path in ('Points', 'PointData', 'CellData', 'Types'):
                        del block[path]
                    block['Points'] = np.empty((0, 3), dtype='float64')
                    block['NumberOfPoints'][...] = 0
                    block['NumberOfCells'][...] = 0
                    if empty_t:
                        block['PointData/T'] = np.empty(0, dtype='float64')
                    # Put the top point second so top indices differ from
                    # indices into the subset of top temperatures.
                    populated = f['VTKHDF/hex']
                    del populated['Points']
                    del populated['PointData/T']
                    del populated['PointData/U']
                    populated['NumberOfPoints'][...] = [1, 1]
                    populated['PointData/U'] = np.zeros((2, 3), dtype='float64')
                    populated['Points'] = [[0., 0., 0.], [2., 0., 1.]]
                    populated['PointData/T'] = [100., 999.]
                result = summarize(run)
                self.assertEqual(result['blocks']['cut']['points'], 0)
                self.assertEqual(result['blocks']['hex']['points'], 2)
                self.assertEqual(result['warpage']['n_top_points'], 1)
                self.assertEqual(result['temperature'], dict(
                    min=100., max=999., top_min=999., top_max=999., top_mean=999.))

    def test_duplicate_temperature_correspondence(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run, cut=True)
            with h5py.File(step, 'r+') as f:
                # Change the first copy (index 5); index 6 is discarded.
                f['VTKHDF/hex/PointData/T'][5] = 5000.
            result = summarize(run)
            self.assertEqual(result['temperature']['max'], 5000.)
            self.assertEqual(result['temperature']['top_min'], 301.)
            self.assertEqual(result['temperature']['top_max'], 5000.)
            self.assertAlmostEqual(result['temperature']['top_mean'], 843.9)
            self.assertEqual(result['warpage']['n_top_points'], 10)

    def test_duplicate_across_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run, cut=True)
            with h5py.File(step, 'r+') as f:
                f['VTKHDF/cut/Points'][0] = [0., 0., 1.]
            result = summarize(run)
            self.assertEqual(result['temperature']['max'], 999.)
            self.assertEqual(result['temperature']['top_max'], 309.)
            self.assertEqual(result['temperature']['top_mean'], 305.)
            self.assertEqual(result['warpage']['n_top_points'], 9)

    def test_collinear_top_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                points = f['VTKHDF/hex/Points'][...]
                points[:, 1] = 0.
                f['VTKHDF/hex/Points'][...] = points
                u = np.zeros_like(points)
                u[:, 2] = points[:, 0] ** 2
                f['VTKHDF/hex/PointData/U'][...] = u
            result = summarize(run)
            self.assertEqual(result['warpage']['n_top_points'], 3)
            self.assertAlmostEqual(result['warpage']['w_bfp_m'], 1., places=12)
            self.assertAlmostEqual(result['warpage']['rel_diff'], 9., places=12)

    def test_required_groups(self):
        for group in ('VTKHDF', 'VTKHDF/hex', 'VTKHDF/cut'):
            for replacement in (False, True):
                with self.subTest(group=group, replacement=replacement), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        del f[group]
                        if replacement:
                            f[group] = 1
                    err = io.StringIO()
                    with contextlib.redirect_stderr(err):
                        self.assertEqual(main([str(run)]), 14)
                    self.assertIn('/' + group + ': required group missing or invalid', err.getvalue())
                    self.assertEqual(len(err.getvalue().splitlines()), 1)

    def test_fixture_cell_counts(self):
        for cut in (False, True):
            with tempfile.TemporaryDirectory() as tmp:
                step, _ = fixture(Path(tmp), cut=cut)
                with h5py.File(step, 'r') as f:
                    for name in ('hex', 'cut'):
                        block = f['VTKHDF/' + name]
                        count = int(block['NumberOfCells'][...].sum())
                        self.assertEqual(block['Types'].shape, (count,))
                        for field in block['CellData'].values():
                            self.assertEqual(field.shape, (count,))

    def test_partial_temperature(self):
        for missing in ('hex', 'cut'):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run, cut=True)
                with h5py.File(step, 'r+') as f:
                    del f[f'VTKHDF/{missing}/PointData/T']
                with self.assertRaises(ResultSummaryError) as caught:
                    summarize(run)
                self.assertEqual(caught.exception.code, 14)
                self.assertEqual(caught.exception.name, 'INPUT_INVALID')
                err = io.StringIO()
                out = io.StringIO()
                with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                    self.assertEqual(main([str(run)]), 14)
                self.assertEqual(out.getvalue(), '')
                self.assertIn('INPUT_INVALID: partial T coverage', err.getvalue())

    def test_byte_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run, cut=True)
            with h5py.File(step, 'r+') as f:
                block = f['VTKHDF/hex']
                u = block['PointData/U'][...].astype('>f8')
                del block['PointData/U']
                block['PointData/U'] = u
            result = summarize(run)
            self.assertEqual(result['temperature']['min'], 100.)
            self.assertAlmostEqual(result['temperature']['top_mean'], 374.4)
            self.assertEqual(result['warpage']['n_top_points'], 10)

    def test_empty_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = fixture(run)
            with h5py.File(step, 'r+') as f:
                b = f['VTKHDF/hex']
                for path in ('Points', 'PointData', 'CellData', 'Types'):
                    del b[path]
                b['Points'] = np.empty((0, 3), dtype='float64')
                b['NumberOfPoints'][...] = 0
                b['NumberOfCells'][...] = 0
            result = summarize(run)
            self.assertIsNone(result['temperature'])
            self.assertEqual(result['warpage']['n_top_points'], 0)
            self.assertIsNone(result['warpage']['w_bfp_m'])

    def test_empty_temperature_datasets(self):
        for empty_geometry in (False, True):
            for temperature_blocks in (('cut',), ('hex', 'cut')) if empty_geometry else (('cut',),):
                with self.subTest(empty_geometry=empty_geometry, temperature_blocks=temperature_blocks), tempfile.TemporaryDirectory() as tmp:
                    run = Path(tmp)
                    step, _ = fixture(run)
                    with h5py.File(step, 'r+') as f:
                        if empty_geometry:
                            block = f['VTKHDF/hex']
                            for path in ('Points', 'PointData', 'CellData', 'Types'):
                                del block[path]
                            block['Points'] = np.empty((0, 3), dtype='float64')
                            block['NumberOfPoints'][...] = 0
                            block['NumberOfCells'][...] = 0
                        else:
                            del f['VTKHDF/hex/PointData/T']
                        for name in ('hex', 'cut'):
                            block = f['VTKHDF/' + name]
                            if 'PointData/T' in block:
                                del block['PointData/T']
                            if name in temperature_blocks:
                                block['PointData/T'] = np.empty(0, dtype='float64')
                    result = summarize(run)
                    self.assertIsNone(result['temperature'])
                    self.assertEqual(result['warpage']['n_top_points'], 0 if empty_geometry else 9)
                    out = io.StringIO()
                    with contextlib.redirect_stdout(out):
                        self.assertEqual(main([str(run)]), 0)
                    self.assertIn('"temperature": null', out.getvalue())

    def test_structural_errors(self):
        for case in ('corrupt', 'U_components', 'negative_counts', 'cut_types', 'root_version'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = fixture(run, cut=True)
                if case == 'corrupt':
                    step.write_bytes(b'not HDF5')
                else:
                    with h5py.File(step, 'r+') as f:
                        if case == 'U_components':
                            del f['VTKHDF/hex/PointData/U']
                            f['VTKHDF/hex/PointData/U'] = np.zeros((11, 2))
                        elif case == 'negative_counts':
                            f['VTKHDF/hex/NumberOfPoints'][0] = -1
                        elif case == 'cut_types':
                            del f['VTKHDF/cut/Types']
                            f['VTKHDF/cut/Types'] = np.array([42], dtype='uint8')
                        else:
                            f['VTKHDF'].attrs['Version'] = [2, 7]
                with self.assertRaises(ResultSummaryError) as caught:
                    summarize(run)
                self.assertEqual(caught.exception.code, 12 if case == 'corrupt' else 14)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
