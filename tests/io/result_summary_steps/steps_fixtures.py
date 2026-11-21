# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Temporary multi-step inputs built from the unchanged result-summary fixture."""
import contextlib
import inspect
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/unit/result_summary'))
# Import dependencies through the existing fixture first so missing packages skip.
from fixtures import fixture, save, fixed_keys, h5py, SCRIPT, LAYERS, MATERIALS
from tools.vv.result_summary import ResultSummaryError, main, summarize


# Each step has a different order and values. Integers stay integers, even when
# they exceed float64 range. These are manifest values, independent of the QoI.
STEP_SUMMARIES = (
    [('T_max', 310), ('warpage_pv', .2), ('U_mag_max', .01)],
    [('U_mag_max', .02), ('T_max', 320), ('warpage_pv', .4)],
    [('warpage_pv', .6), ('U_mag_max', .03), ('T_max', 330)],
)
UNITS = {'T_max': 'K', 'warpage_pv': 'm', 'U_mag_max': 'm'}
OMITTED = object()


def steps_fixture(run):
    original, manifest = fixture(run)
    # Copy every file before changing any displacement, including step zero.
    copies = [run / f'out/step-{n}.vtkhdf' for n in (1, 2)]
    for target in copies:
        shutil.copyfile(original, target)
    for n, target in enumerate([original] + copies):
        with h5py.File(target, 'r+') as f:
            for name in ('hex', 'cut'):
                block = f[f'VTKHDF/{name}']
                points = block['Points'][...]
                x, y = points[:, 0], points[:, 1]
                u = block['PointData/U'][...]
                u[:, 2] = .125 + .25*x - .5*y - ((n+1)/16)*(x*x+y*y)
                block['PointData/U'][...] = u
    manifest['steps'] = [{'n': n, 'file': f'out/step-{n}.vtkhdf'} for n in range(3)]
    count_paths = ('hex/NumberOfCells', 'cut/NumberOfCells',
                   'eb_surface/Polygons/NumberOfCells')
    per_step = []
    for record in manifest['steps']:
        with h5py.File(run / record['file'], 'r') as f:
            counts = [f['VTKHDF/' + path].shape[0] for path in count_paths]
        assert all(count == counts[0] for count in counts), record['file']
        per_step.append(counts[0])
    manifest['partitions']['per_step'] = per_step
    # Interleave owners to make filtering preserve manifest encounter order.
    manifest['summary'] = [
        dict(name=name, value=value, unit=UNITS[name], step=n)
        for position in range(3)
        for n, entries in enumerate(STEP_SUMMARIES)
        for name, value in [entries[position]]
    ]
    save(run, manifest)
    return manifest


def cli(run, *args):
    return subprocess.run([sys.executable, str(SCRIPT), str(run), *args],
                          capture_output=True, timeout=20)


def protocol_cli(run, *args):
    """Exercise main's parser and protocol in-process for the large error matrix."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = main([str(run), *args])
        except SystemExit as exc:
            code = exc.code
    return code, out.getvalue(), err.getvalue()


def api(test, run, selector=OMITTED):
    if selector is OMITTED:
        return summarize(run)
    # This is an assertion failure on the old public interface, not an import or
    # fixture error. No production stub is needed for these Python tests.
    test.assertIn('step', inspect.signature(summarize).parameters,
                  'summarize must accept the step selector')
    return summarize(run, 1e-12, selector)


def success(test, run, args=(), selector=OMITTED):
    completed = cli(run, *args)
    test.assertEqual(completed.returncode, 0, completed.stderr.decode())
    test.assertEqual(completed.stderr, b'')
    result = json.loads(completed.stdout)
    fixed_keys(test, result)
    test.assertEqual(result, api(test, run, selector))
    repeat = cli(run, *args)
    test.assertEqual(repeat.returncode, 0, repeat.stderr.decode())
    test.assertEqual(repeat.stderr, b'')
    test.assertEqual(completed.stdout, repeat.stdout)
    return result, completed.stdout


def assert_summary(test, result, entries):
    expected = dict(entries)
    test.assertEqual(list(result['manifest_summary']), list(expected))
    test.assertEqual(result['manifest_summary'], expected)
    test.assertEqual(result['warpage']['manifest_warpage_pv_m'],
                     expected.get('warpage_pv'))
    for name, value in expected.items():
        test.assertIs(type(result['manifest_summary'][name]), type(value))


def assert_fields(test, result, n):
    test.assertEqual(result['mesh_sha256'], 'synthetic-sha256')
    test.assertEqual(result['blocks'], {
        'hex': {'points': 11, 'cells': 2, 'cell_kind_counts': {'0': 1, '1': 1}},
        'cut': {'points': 0, 'cells': 0, 'type_counts': {}},
        'eb_surface': {'points': 7, 'polygons': 3},
    })
    test.assertEqual(list(result['blocks']['hex']['cell_kind_counts']), ['0', '1'])
    test.assertEqual(result['layer_names'], LAYERS)
    test.assertEqual(result['material_names'], MATERIALS)
    temp = result['temperature']
    for key, value in [('min', 300.), ('max', 309.),
                       ('top_min', 301.), ('top_max', 309.)]:
        test.assertEqual(temp[key], value)
    test.assertLessEqual(abs(temp['top_mean']/305. - 1), 1e-14)
    # Nine symmetric unique top nodes have r^2 in [0, 2]. Removing the fitted
    # plane leaves PV = 2*c_n, calculated without calling reader/QoI code.
    expected = 2*((n+1)/16)
    warpage = result['warpage']
    test.assertEqual(warpage['n_top_points'], 9)
    test.assertLessEqual(abs(warpage['w_bfp_m']/expected - 1), 1e-12)
    pv = warpage['manifest_warpage_pv_m']
    if pv is not None:
        test.assertEqual(warpage['rel_diff'], abs(warpage['w_bfp_m']-pv)/pv)
