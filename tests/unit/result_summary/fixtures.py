# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic partitioned results shared by the acceptance tests."""
import json
from pathlib import Path
import sys

try:
    import h5py
    import numpy as np
except ImportError:
    print('SKIP: h5py/numpy unavailable')
    raise SystemExit(77)

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
SCRIPT = ROOT / 'tools/vv/result_summary.py'
LAYERS = ['substrate', 'die_attach', 'Si']
MATERIALS = ['铜', 'silicon']


def fixture(run, cut=False, two=False):
    step = run / 'out/step-0.vtkhdf'
    step.parent.mkdir(parents=True, exist_ok=True)
    top = np.array([(x, y, 1.) for y in (-1., 0., 1.) for x in (-1., 0., 1.)])
    points = np.concatenate(([[0., 0., 0.]], top[:5], top[4:]))
    counts = [6, 5]
    if two:
        points = np.array([[0., 0., 0.], [-1., 0., 1.], [1., 0., 1.]])
        counts = [2, 1]
    with h5py.File(step, 'w') as f:
        root = f.create_group('VTKHDF')
        root.attrs['Version'] = [2, 8]
        root.attrs['Type'] = np.bytes_('PartitionedDataSetCollection')
        for index, name in enumerate(('hex', 'cut')):
            b = root.create_group(name)
            b.attrs['Version'] = [2, 8]
            b.attrs['Type'] = np.bytes_('UnstructuredGrid')
            b.attrs['Index'] = index
            p = points if name == 'hex' else (np.array([[2., 0., 1.], [0., 0., 0.]]) if cut else np.empty((0, 3)))
            pc = counts if name == 'hex' else ([1, 1] if cut else [0, 0])
            cc = [1, 1] if name == 'hex' or cut else [0, 0]
            b['Points'] = p.astype('float64')
            b['NumberOfPoints'] = np.array(pc, dtype='int64')
            b['NumberOfCells'] = np.array(cc, dtype='int64')
            b['Types'] = np.full(sum(cc), 12 if name == 'hex' else 42, dtype='uint8')
            b['PointData/T'] = np.where(p[:, 2] == 0., 300., 305. + p[:, 0] + 3*p[:, 1]) if name == 'hex' else np.array([999., 100.])[:len(p)]
            u = np.zeros((len(p), 3), dtype='float64')
            u[:, 2] = .125 + .25*p[:, 0] - .5*p[:, 1] - .0625*(p[:, 0]**2+p[:, 1]**2)
            b['PointData/U'] = u
            for field in ('layer_id', 'material_id', 'cell_kind', 'elem_gid'):
                b['CellData/'+field] = np.array([1, 0][:sum(cc)], dtype='int64' if field == 'elem_gid' else 'int32')
            for field, names in [('layer_names', LAYERS), ('material_names', MATERIALS)]:
                b.create_dataset('FieldData/'+field, data=names, dtype=h5py.string_dtype())
        eb = root.create_group('eb_surface')
        eb.attrs['Version'] = [2, 8]
        eb['NumberOfPoints'] = np.array([3, 4], dtype='int64')
        eb['Polygons/NumberOfCells'] = np.array([1, 2], dtype='int64')
    manifest = {'schema_version': 'chipcae.results/1.0', 'steps': [{'n': 0, 'file': 'out/step-0.vtkhdf'}],
                'mesh': {'mesh_sha256': 'synthetic-sha256'},
                'summary': [{'name': 'T_max', 'value': 309., 'unit': 'K'},
                            {'name': 'warpage_pv', 'value': .1, 'unit': 'm'}]}
    save(run, manifest)
    return step, manifest


def save(run, manifest):
    (run/'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')


def fixed_keys(test, result):
    test.assertEqual(list(result), ['schema', 'step_file', 'mesh_sha256', 'blocks', 'layer_names', 'material_names', 'temperature', 'warpage', 'manifest_summary'])
    test.assertEqual(list(result['blocks']), ['hex', 'cut', 'eb_surface'])
    for name, keys in [('hex', ['points', 'cells', 'cell_kind_counts']), ('cut', ['points', 'cells', 'type_counts']), ('eb_surface', ['points', 'polygons'])]:
        test.assertEqual(list(result['blocks'][name]), keys)
    if result['temperature'] is not None:
        test.assertEqual(list(result['temperature']), ['min', 'max', 'top_min', 'top_max', 'top_mean'])
    test.assertEqual(list(result['warpage']), ['manifest_warpage_pv_m', 'w_bfp_m', 'rel_diff', 'n_top_points'])
    test.assertEqual(result['schema'], 'chipcae.vv.result_summary/0.1')
