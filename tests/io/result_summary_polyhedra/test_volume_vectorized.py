# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Public-path equivalence guards for batched polyhedron geometry.

These catch changed summation order, occurrence-weighted centers, cursor mistakes,
and reordered failures. Performance is checked separately by bench_volume.py.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import warnings

from polyhedra_fixtures import CUBE, RINGS, cube_fixture, h5py, invalid, np, rs, success


_TOPOLOGY = ('FaceConnectivity', 'FaceOffsets', 'PolyhedronToFaces',
             'PolyhedronOffsets', 'NumberOfFaces', 'NumberOfFaceConnectivityIds',
             'NumberOfPolyhedronToFaceIds')


def _replace(block, name, values):
    if name in block:
        del block[name]
    block[name] = values


def _strip_topology(block):
    for name in (*_TOPOLOGY, 'CellData/volume'):
        if name in block:
            del block[name]


def _read(step):
    with h5py.File(step, 'r') as stream:
        block = stream['VTKHDF/cut']
        return {name: block[name][...] for name in
                (*_TOPOLOGY, 'NumberOfPoints', 'NumberOfCells', 'Points')
                + (('CellData/volume',) if 'CellData/volume' in block else ())}


def _reference_volumes(data, selected=None):
    """Verbatim cell loop from 51600d4a, with test-local names supplied below.

    _require observes the original computed scalars and first failing predicate;
    it never calls product code. When selected is supplied, only those altered
    cells are evaluated (the unchanged cells already have a valid oracle result).
    This keeps the boundary matrix within the old implementation's time budget.
    """
    result_v, result_b, failures = {}, {}, []
    stored = data.get('CellData/volume')
    volumes = stored if stored is not None else np.ones(sum(data['NumberOfCells']))
    block = SimpleNamespace(name='/VTKHDF/cut')
    point_cursor = cell_base = face_cursor = ref_cursor = fo_cursor = po_cursor = 0
    for rank, (point_count, count, faces, connections, references) in enumerate(zip(
            data['NumberOfPoints'], data['NumberOfCells'], data['NumberOfFaces'],
            data['NumberOfFaceConnectivityIds'], data['NumberOfPolyhedronToFaceIds'])):
        point_count, count, faces, connections, references = map(
            int, (point_count, count, faces, connections, references))
        fo = data['FaceOffsets'][fo_cursor:fo_cursor + faces + 1]
        original_po = data['PolyhedronOffsets'][po_cursor:po_cursor + count + 1]
        ids = data['FaceConnectivity'][face_cursor:face_cursor + connections]
        original_refs = data['PolyhedronToFaces'][ref_cursor:ref_cursor + references]
        local_points = data['Points'][point_cursor:point_cursor + point_count]
        groups = ([list(range(count))] if selected is None else
                  [[c] for p, c in sorted(selected) if p == rank])
        for source_cells in groups:
            cells = len(source_cells)
            if selected is None:
                po, refs, cell_cursor = original_po, original_refs, cell_base
            else:
                target = source_cells[0]
                start, end = original_po[target:target + 2]
                refs = original_refs[start:end]
                po = np.array([0, end - start])
                cell_cursor = cell_base + target

            def _require(condition, message):
                key = rank, source_cells[cell]
                if 'nonpositive' in message:
                    result_v[key] = float(message.split('computed=')[1].rstrip(')'))
                    kind, dataset = 'nonpositive polyhedron volume', 'PolyhedronToFaces'
                else:
                    result_b[key] = float(message.split('bbox_volume=')[1])
                    kind, dataset = 'volume mismatch', 'CellData/volume'
                    if stored is None:
                        return
                if not condition:
                    failures.append((key, dataset, kind))

            for cell in range(cells):
                rings = [ids[int(fo[face]):int(fo[face + 1])]
                         for face in refs[int(po[cell]):int(po[cell + 1])]]
                vertices = local_points[np.unique(np.concatenate(rings))]
                # Overflow and invalid derived arithmetic become protocol errors below.
                # Keep face/triangle accumulation in deterministic stored order.
                with np.errstate(all='ignore'):
                    center = vertices.mean(axis=0)
                    volume = np.float64(0.)
                    for ring in rings:
                        coords = local_points[ring] - center
                        for i in range(1, len(coords) - 1):
                            volume += np.dot(coords[0], np.cross(coords[i], coords[i + 1])) / 6.
                _require(np.isfinite(volume) and volume > 0,
                         f'{block.name}/PolyhedronToFaces: partition {rank} cell {cell} '
                         f'nonpositive polyhedron volume (computed={volume})')
                if volumes is not None:
                    reference = volumes[cell_cursor + cell]
                    with np.errstate(all='ignore'):
                        bbox_volume = np.prod(vertices.max(axis=0) - vertices.min(axis=0))
                        tolerance = 1e-9 * max(abs(reference), bbox_volume)
                        difference = abs(volume - reference)
                    _require(reference > 0 and np.isfinite(bbox_volume)
                             and np.isfinite(tolerance) and np.isfinite(difference)
                             and difference <= tolerance,
                             f'{block.name}/CellData/volume: volume mismatch, '
                             f'partition {rank} cell {cell} computed={volume} '
                             f'reference={reference} bbox_volume={bbox_volume}')

        point_cursor += point_count
        cell_base += count
        fo_cursor += faces + 1
        po_cursor += count + 1
        face_cursor += connections
        ref_cursor += references
    first = min(failures, key=lambda item: item[0]) if failures else None
    return result_v, result_b, first


def _vectors(values):
    return np.array([values[key] for key in sorted(values)])


def _perturb(step, seed):
    rng = np.random.default_rng(seed)
    # Positive determinant, with shear/rotation and anisotropic scales.
    rotation, _ = np.linalg.qr(rng.normal(size=(3, 3)))
    if np.linalg.det(rotation) < 0:
        rotation[:, 0] *= -1
    affine = rotation @ np.array([[1.2, .1, .05], [0., .9, .07], [0., 0., 1.1]])
    with h5py.File(step, 'r+') as stream:
        block = stream['VTKHDF/cut']
        points = block['Points'][...]
        points = points @ affine + rng.uniform(-.12, .12, points.shape)
        block['Points'][...] = points
        del block['CellData/volume']
    volumes, bounds, failure = _reference_volumes(_read(step))
    if failure is not None:
        raise AssertionError(f'perturbed fixture must have positive volumes: {failure}')
    v, b = _vectors(volumes), _vectors(bounds)
    if not np.all(b >= v):
        raise AssertionError('bbox must bound signed volume')
    return v, b


def _reverse(block, partition, cell):
    counts = block['NumberOfCells'][...]
    start = 24 * (int(sum(counts[:partition])) + cell)
    rings = block['FaceConnectivity'][start:start + 24].reshape(6, 4)
    block['FaceConnectivity'][start:start + 24] = rings[:, ::-1].ravel()


def _collapse(block, partition, cell):
    counts = block['NumberOfCells'][...]
    start = 8 * (int(sum(counts[:partition])) + cell)
    block['Points'][start:start + 8, 2] = 0.


class VolumeVectorized(unittest.TestCase):
    def assert_reference_failure(self, run, step, selected=None, cli=False):
        _, _, first = _reference_volumes(_read(step), selected)
        self.assertIsNotNone(first)
        (partition, cell), dataset, kind = first
        # invalid() verifies exit 14, no stdout, one stderr line, and summarize().
        invalid(self, run, dataset, kind, f'partition {partition} cell {cell} ',
                subprocess_cli=cli)
        with self.assertRaises(rs.ResultSummaryError) as caught:
            rs.summarize(run)
        self.assertEqual(re.search(r'partition (\d+) cell (\d+)', str(caught.exception)).groups(),
                         (str(partition), str(cell)))

    def test_random_polyhedra_match_reference(self):
        for seed in (1729, 9211, 34061):
            with self.subTest(seed=seed), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = cube_fixture(self, run, cells=(1100, 1000))
                v, b = _perturb(step, seed)
                data = _read(step)
                with h5py.File(step, 'r+') as stream:
                    _strip_topology(stream['VTKHDF/cut'])
                _, expected = success(self, run)
                for sign in (1., -1.):
                    with h5py.File(step, 'r+') as stream:
                        block = stream['VTKHDF/cut']
                        for name in _TOPOLOGY:
                            _replace(block, name, data[name])
                        _replace(block, 'CellData/volume', v + sign * .999e-9 * b)
                    success(self, run, expected)

    def test_tolerance_boundary_rejects(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run, cells=(1100, 1000))
            v, b = _perturb(step, 1729)
            for partition, cell in ((0, 0), (0, 1023), (0, 1024), (0, 1099),
                                    (1, 0), (1, 999)):
                with self.subTest(partition=partition, cell=cell):
                    values = v.copy()
                    index = cell + (1100 if partition else 0)
                    values[index] = v[index] + 1.001e-9 * b[index]
                    with h5py.File(step, 'r+') as stream:
                        _replace(stream['VTKHDF/cut'], 'CellData/volume', values)
                    self.assert_reference_failure(run, step, [(partition, cell)])

    def test_adjacent_float_tolerance_decisions_match_reference(self):
        # A 1e-12 accuracy bound alone cannot protect a decision within one
        # representable float of the 1e-9 threshold. Probe both sides using
        # the historical oracle, without assuming where rounding puts it.
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run, cells=(3, 2))
            v, b = _perturb(step, 9211)
            passed = rejected = 0
            for sign in (1., -1.):
                boundary = v + sign * 1e-9 * b
                for direction in (-np.inf, np.inf):
                    for index in range(len(v)):
                        with self.subTest(sign=sign, direction=direction, index=index):
                            values = v.copy()
                            values[index] = np.nextafter(boundary[index], direction)
                            with h5py.File(step, 'r+') as stream:
                                _replace(stream['VTKHDF/cut'], 'CellData/volume', values)
                            _, _, first = _reference_volumes(_read(step))
                            if first is None:
                                success(self, run)
                                passed += 1
                            else:
                                self.assert_reference_failure(run, step)
                                rejected += 1
            self.assertGreater(passed, 0)
            self.assertGreater(rejected, 0)

    def test_first_failure_matches_reference(self):
        positions = sorted({0, 1, 1099} | {n for k in range(1, 11)
                                                  for n in (2 ** k - 1, 2 ** k)})
        cases = [('reverse', (0, c), None) for c in positions]
        cases += [('reverse', (1, c), None) for c in (0, 999)]
        collapse_positions = ((0, 0), (0, 1023), (0, 1024), (1, 0))
        cases += [('collapse', position, None) for position in collapse_positions]
        # Earlier mismatch must win over later nonpositive, including chunk and
        # partition transitions; also cover cell 0 of the first partition.
        cases += [('ordered', later, earlier) for earlier, later in
                  [((0, 0), (0, 1)), ((0, 1022), (0, 1023)),
                   ((0, 1023), (0, 1024)), ((0, 1099), (1, 0))]]
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run, cells=(1100, 1000))
            data = _read(step)
            self.assertIsNone(_reference_volumes(data)[2])
            for fault, position, earlier in cases:
                with self.subTest(fault=fault, position=position, earlier=earlier):
                    with h5py.File(step, 'r+') as stream:
                        block = stream['VTKHDF/cut']
                        for name in ('Points', 'FaceConnectivity', 'CellData/volume'):
                            block[name][...] = data[name]
                        (_reverse if fault == 'reverse' else _collapse)(block, *position)
                        # Collapse with stored unit volume exercises same-cell
                        # nonpositive + mismatch: the former must win.
                        if earlier is not None:
                            p, c = earlier
                            block['CellData/volume'][c + (1100 if p else 0)] = 2.
                    selected = [position] + ([earlier] if earlier is not None else [])
                    self.assert_reference_failure(run, step, selected)

    def test_overflow_and_nonclosed(self):
        for fault in ('overflow', 'nonclosed'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, _ = cube_fixture(self, run)
                with h5py.File(step, 'r+') as stream:
                    block = stream['VTKHDF/cut']
                    if fault == 'overflow':
                        block['Points'][...] = CUBE * 1e103
                    else:
                        block['FaceConnectivity'][:4] = RINGS[0, ::-1]
                with warnings.catch_warnings(record=True) as caught:
                    warnings.simplefilter('always')
                    self.assert_reference_failure(run, step, cli=True)
                self.assertEqual(caught, [])

    def test_irregular_faces_unique_vertices_and_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run, cells=(2, 1))
            with h5py.File(step, 'r+') as stream:
                block = stream['VTKHDF/cut']
                # Dent the top of each cube: nonconvex cells. Split a quad and
                # repeat IDs, so occurrence-weighted and unique centers differ.
                points = block['Points'][...]
                points.reshape(-1, 8, 3)[:, 6] += [-.55, -.55, -.7]
                block['Points'][...] = points
                face_ids, offsets, refs, poly_offsets = [], [], [], []
                for count in (2, 1):
                    local_faces, local_offsets, local_refs = [], [0], []
                    local_poly = [0]
                    for cell in range(count):
                        rings = [list(ring) for ring in RINGS]
                        rings[0] = [0, 3, 2]
                        rings.insert(1, [0, 2, 1])
                        rings[2] = [4, 5, 6, 7, 7, 4]
                        # Leave one face reversed to make the center observable
                        # even though the cell remains positive and nonclosed.
                        rings[-1].reverse()
                        start_face = len(local_offsets) - 1
                        for ring in rings:
                            local_faces.extend(np.array(ring) + 8 * cell)
                            local_offsets.append(len(local_faces))
                        local_refs.extend(start_face + np.array([4, 0, 6, 2, 5, 1, 3]))
                        local_poly.append(len(local_refs))
                    face_ids.extend(local_faces)
                    offsets.extend(local_offsets)
                    refs.extend(local_refs)
                    poly_offsets.extend(local_poly)
                for name, values in {
                        'FaceConnectivity': face_ids, 'FaceOffsets': offsets,
                        'PolyhedronToFaces': refs, 'PolyhedronOffsets': poly_offsets,
                        'NumberOfFaces': [14, 7], 'NumberOfFaceConnectivityIds': [56, 28],
                        'NumberOfPolyhedronToFaceIds': [14, 7]}.items():
                    _replace(block, name, np.asarray(values, dtype='int64'))
                del block['CellData/volume']
            v, b, failure = _reference_volumes(_read(step))
            self.assertIsNone(failure)
            with h5py.File(step, 'r+') as stream:
                block = stream['VTKHDF/cut']
                data = _read(step)
                _strip_topology(block)
            _, expected = success(self, run)
            for sign in (1., -1.):
                with h5py.File(step, 'r+') as stream:
                    block = stream['VTKHDF/cut']
                    for name in _TOPOLOGY:
                        _replace(block, name, data[name])
                    _replace(block, 'CellData/volume', _vectors(v) + sign * .999e-9 * _vectors(b))
                success(self, run, expected)

    def test_endian_optional_empty_and_no_topology(self):
        for cells in ((1, 1), (0, 2), (2, 0), (0, 0)):
            with self.subTest(cells=cells), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp)
                step, expected = cube_fixture(self, run, cells=cells)
                success(self, run, expected)
                with h5py.File(step, 'r+') as stream:
                    block = stream['VTKHDF/cut']
                    for name in (*_TOPOLOGY, 'Points', 'NumberOfPoints', 'NumberOfCells',
                                 'CellData/volume'):
                        values = block[name][...]
                        _replace(block, name, values.astype(values.dtype.newbyteorder('>')))
                success(self, run, expected)
                with h5py.File(step, 'r+') as stream:
                    del stream['VTKHDF/cut/CellData/volume']
                success(self, run, expected)
                with h5py.File(step, 'r+') as stream:
                    _strip_topology(stream['VTKHDF/cut'])
                success(self, run, expected)

    def test_stored_nonfinite_precedes_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run, cells=(2, 0))
            with h5py.File(step, 'r+') as stream:
                block = stream['VTKHDF/cut']
                _collapse(block, 0, 0)
                block['CellData/volume'][1] = np.nan
            invalid(self, run, 'CellData/volume', 'nonfinite')

    def test_repeat_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            step, _ = cube_fixture(self, run, cells=(1, 1))
            with h5py.File(step, 'r+') as stream:
                _reverse(stream['VTKHDF/cut'], 1, 0)
            script = Path(rs.__file__)
            outputs = [subprocess.run([sys.executable, str(script), str(run)],
                                      capture_output=True, check=False, timeout=20)
                       for _ in range(2)]
            for result in outputs:
                self.assertEqual(result.returncode, 14)
                self.assertEqual(result.stdout, b'')
                self.assertEqual(len(result.stderr.splitlines()), 1)
            self.assertEqual(outputs[0].stderr, outputs[1].stderr)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
