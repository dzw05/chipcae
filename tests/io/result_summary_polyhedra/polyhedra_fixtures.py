# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic outward cubes and assertions for the frozen summary protocol."""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))
# Import dependencies through the unchanged fixture (missing dependencies exit 77).
from fixtures import fixture, fixed_keys, h5py, np, SCRIPT
from tools.vv import result_summary as rs

COUNTS = ('NumberOfFaces', 'NumberOfFaceConnectivityIds',
          'NumberOfPolyhedronToFaceIds')
ARRAYS = ('FaceConnectivity', 'FaceOffsets', 'PolyhedronToFaces', 'PolyhedronOffsets')
TOPOLOGY = ARRAYS + COUNTS
CUBE = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0],
                 [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]], dtype='float64')
# Same outward rings as chipcae_write_results; analytic signed volume is side**3.
RINGS = np.array([[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4],
                  [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]], dtype='int64')


def replace(block, name, values, dtype=None):
    if name in block:
        del block[name]
    block[name] = np.asarray(values, dtype=dtype)


def invoke(run, subprocess_cli=False):
    if subprocess_cli:
        completed = subprocess.run([sys.executable, str(SCRIPT), str(run)],
                                   capture_output=True, timeout=20)
        return completed.returncode, completed.stdout, completed.stderr
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = rs.main([str(run)])
    return code, out.getvalue().encode('utf-8'), err.getvalue().encode('utf-8')


def success(test, run, baseline=None, subprocess_cli=False):
    code, content, err = invoke(run, subprocess_cli)
    test.assertEqual(code, 0, err.decode())
    test.assertEqual(err, b'')
    result = rs.summarize(run)
    fixed_keys(test, result)
    prescribed = (json.dumps(result, indent=2, sort_keys=False, ensure_ascii=False,
                            allow_nan=False) + '\n').encode('utf-8')
    test.assertEqual(content, prescribed)
    test.assertEqual(invoke(run, subprocess_cli), (0, content, b''))
    if baseline is not None:
        test.assertEqual(content, baseline)
    return result, content


def invalid(test, run, dataset, *reasons, subprocess_cli=False):
    code, out, err = invoke(run, subprocess_cli)
    test.assertEqual(code, 14, f'expected INPUT_INVALID for {dataset}; '
                     f'got exit {code}, stdout bytes={len(out)}, stderr={err!r}')
    test.assertEqual(out, b'')
    test.assertEqual(len(err.splitlines()), 1, err.decode())
    prefix = f'result_summary: INPUT_INVALID: /VTKHDF/cut/{dataset}: '
    test.assertTrue(err.startswith(prefix.encode()), err.decode())
    test.assertNotIn(b'Traceback', err)
    for reason in reasons:
        test.assertIn(reason.encode(), err)
    with test.assertRaises(rs.ResultSummaryError) as caught:
        rs.summarize(run)
    test.assertIs(type(caught.exception), rs.ResultSummaryError)
    test.assertEqual(caught.exception.code, 14)
    test.assertEqual(caught.exception.name, 'INPUT_INVALID')
    test.assertTrue(str(caught.exception).startswith(f'/VTKHDF/cut/{dataset}: '))
    for reason in reasons:
        test.assertIn(reason, str(caught.exception))


def cube_fixture(test, run, cells=(1, 0), side=1., translation=0., topology=True):
    """Each partition numbers its own disjoint cubes from point/face zero."""
    step, _ = fixture(run, cut=True)
    points = [CUBE * side + translation + np.array([3 * c * side, 0., 0.])
              for count in cells for c in range(count)]
    points = np.concatenate(points) if points else np.empty((0, 3), dtype='float64')
    total = sum(cells)
    with h5py.File(step, 'r+') as f:
        b = f['VTKHDF/cut']
        for name, values, dtype in [
                ('Points', points, 'float64'),
                ('NumberOfPoints', [8 * c for c in cells], 'int64'),
                ('NumberOfCells', cells, 'int64'),
                ('NumberOfConnectivityIds', [8 * c for c in cells], 'int64'),
                ('Offsets', [i * 8 for c in cells for i in range(c + 1)], 'int64'),
                ('Connectivity', [i for c in cells for i in range(8 * c)], 'int64'),
                ('Types', np.full(total, 42), 'uint8'),
                ('PointData/T', np.full(len(points), 300.), 'float64'),
                ('PointData/U', np.zeros((len(points), 3)), 'float64')]:
            replace(b, name, values, dtype)
        for name in ('layer_id', 'material_id', 'cell_kind', 'elem_gid'):
            replace(b, 'CellData/' + name, np.ones(total),
                    'int64' if name == 'elem_gid' else 'int32')
    # Only topology/volume change after this capture: all summary inputs are identical.
    _, baseline = success(test, run)
    if topology:
        with h5py.File(step, 'r+') as f:
            b = f['VTKHDF/cut']
            face_ids = [(RINGS + 8 * i).ravel() for c in cells for i in range(c)]
            for name, values in [
                    ('NumberOfFaces', [6 * c for c in cells]),
                    ('NumberOfFaceConnectivityIds', [24 * c for c in cells]),
                    ('NumberOfPolyhedronToFaceIds', [6 * c for c in cells]),
                    ('FaceConnectivity', np.concatenate(face_ids) if face_ids else []),
                    ('FaceOffsets', [4 * i for c in cells for i in range(6 * c + 1)]),
                    ('PolyhedronToFaces', [i for c in cells for i in range(6 * c)]),
                    ('PolyhedronOffsets', [6 * i for c in cells for i in range(c + 1)])]:
                replace(b, name, values, 'int64')
            replace(b, 'CellData/volume', np.full(total, side ** 3), 'float64')
    return step, baseline
