"""P2 acceptance: preview must clip before hashing, counting and writing."""
import copy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import h5py
import numpy as np

solve, preview, mpiexec, test = sys.argv[1:]
# c=.3 exceeds two grid spacings: the corner cells are completely removed,
# so the canonical node/connectivity digest detects the clipping.
CHAMFER = [[.3, 0], [.7, 0], [1, .3], [1, .7],
           [.7, 1], [.3, 1], [0, .7], [0, .3]]


def invoke(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=180)


def events(result):
    return [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]


def success(result):
    assert result.returncode == 0, (result.args, result.stdout, result.stderr)
    found = [e for e in events(result) if e['stage'] == 'success']
    assert len(found) == 1, found
    return found[0]


with tempfile.TemporaryDirectory(prefix='preview-outline-') as tmp:
    root = Path(tmp)
    stack = dict(id='digest', unit='m', outline=dict(box=[0, 0, 1, 1]),
                 symmetry='none', package_type='FCBGA', layers=[
                     dict(name='solid', kind='Explicit', material='silicon',
                          thickness=.1, n_elem_min=1, grading=1)])
    (root / 'stack.json').write_text(json.dumps(stack))
    table = lambda columns: dict(temperatures=[300], columns=columns,
                                 interpolation='constant', extrapolation='clamp')
    material = dict(
        id='silicon', schema_version='1', unit_system='SI', model='thermo_elastic',
        density=2330, elastic=dict(symmetry='isotropic',
                                  table=table({'E': [130e9], 'nu': [.3]})),
        thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                               stress_free_temperature=300,
                               table=table({'alpha': [2.6e-6]})),
        provenance=dict(source_type='illustrative', confidence='illustrative'))
    (root / 'materials.json').write_text(json.dumps(
        {'schema': 'chipcae.material_library/1', 'materials': [material]}))
    case = {
        'schema': 'chipcae.case/1.0', 'stack': {'ref': 'stack.json'},
        'materials': {'ref': 'materials.json'},
        'mesh': {'type': 'aligned_cartesian_cut', 'nx': 8, 'ny': 8, 'levels': 1,
                 'min_elems_per_layer': 1, 'max_ratio': 10},
        'physics': [{'id': 'mech', 'solver': 'fem.solid'}],
        'loads': [{'type': 'uniform_temperature', 'T_ref': 300, 'T_target': 300}],
        'constraints': {'rigid': '3-2-1'},
        'solver': {'linear': {'preconditioner': 'direct'}},
        'output': {'format': 'vtkhdf'}}
    (root / 'case.json').write_text(json.dumps(case))

    def preview_run(name, polygon=None, ranks=1, extra=()):
        data = copy.deepcopy(stack)
        if polygon is not None:
            data['outline']['polygon'] = polygon
        (root / 'stack.json').write_text(json.dumps(data))
        stem = root / name
        result = invoke([mpiexec, '-n', str(ranks), preview, str(root / 'stack.json'),
                         '--nx', '8', '--ny', '8', '--levels', '1', '-o', str(stem), *extra])
        return result, stem

    def solve_run(name, ranks):
        profile = root / 'profile.json'
        profile.write_text(json.dumps({'schema': 'chipcae.run_profile/1.0',
                                       'ranks': ranks, 'backend': 'cpu', 'gpus': 0}))
        out = root / name
        result = invoke([mpiexec, '-n', str(ranks), solve, '--case', str(root / 'case.json'),
                         '--profile', str(profile), '--out', str(out)])
        return result, out

    def file_summary(stem):
        # Preview's public background writer stores cut cells with cell_kind=1.
        with h5py.File(str(stem) + '.vtkhdf') as file:
            grid = file['VTKHDF']
            points = grid['Points'][:]
            connectivity = grid['Connectivity'][:].reshape(-1, 8)
            corners = points[connectivity]
            cell_volume = np.prod(corners.max(axis=1) - corners.min(axis=1), axis=1)
            fractions = grid['CellData/vol_frac'][:]
            cut_count = int(np.count_nonzero(grid['CellData/cell_kind'][:] == 1))
            return len(connectivity), cut_count, float(np.sum(fractions * cell_volume))

    if test in ('preview_outline_chamfer_differs', 'preview_outline_volume'):
        box, box_stem = preview_run('box')
        chamfer, chamfer_stem = preview_run('chamfer', CHAMFER)
        b, c = success(box), success(chamfer)
        bn, bc, bv = file_summary(box_stem)
        cn, cc, cv = file_summary(chamfer_stem)
        if test == 'preview_outline_chamfer_differs':
            assert cn < bn, ('outline ignored: chamfer cells must be fewer', cn, bn)
            assert c['mesh_sha256'] != b['mesh_sha256'], (b, c)
            assert c['cell_count'] == cn and b['cell_count'] == bn, (b, c, bn, cn)
            assert cc > 0 and bc == 0, (cc, bc)
            # The brief freezes the count, but does not freeze the JSON field name.
            for event, count in ((c, cc), (b, bc)):
                reported = [event[key] for key in ('cut_cell_count', 'cut_cells', 'cut_count')
                            if key in event]
                assert reported and all(type(value) is int and value == count
                                        for value in reported), (event, count)
        else:
            for label, actual, expected in [('box', bv, .1), ('chamfer', cv, .082)]:
                error = abs(actual - expected) / abs(expected)
                assert error <= 1e-12, (label, actual, expected, error)
    elif test == 'preview_solve_outline_digest':
        digests = []
        comparisons = []
        for ranks in (1, 2):
            result, _ = preview_run(f'preview-{ranks}', CHAMFER, ranks)
            digest = success(result)['mesh_sha256']
            result, out = solve_run(f'solve-{ranks}', ranks)
            assert result.returncode == 0, (result.stdout, result.stderr)
            mesh = json.loads((out / 'manifest.json').read_text())['mesh']
            comparisons.append((ranks, digest, mesh['mesh_sha256']))
            digests.append(digest)
        assert all(p == k for _, p, k in comparisons), comparisons
        assert digests[0] == digests[1], digests
    elif test == 'preview_outline_nonconvex_rejected':
        failures = []
        polygon = [[0, 0], [1, 0], [.5, .5], [1, 1], [0, 1]]
        # Keep default preview rejection covered while solve opts into general cuts.
        default, stem = preview_run('nonconvex-default', polygon)
        assert default.returncode == 13, (default.stdout, default.stderr)
        assert 'non-convex' in default.stdout + default.stderr
        default_errors = [e for e in events(default) if e['stage'] in ('error', 'failure')]
        assert default_errors, events(default)
        assert any('non-convex' in e['message'] for e in default_errors), default_errors
        assert not list(root.glob(stem.name + '*.vtkhdf'))
        # Use fixture v_shape at its physical scale for the opt-in acceptance.
        stack['outline']['box'] = [0, 0, .02, .02]
        stack['layers'][0]['thickness'] = .001
        polygon = [[0,0],[.0064,0],[.0103,.0087],[.0142,0],[.02,0],[.02,.02],[0,.02]]
        for name, ring, expected, diagnostic in (
            ('nonconvex', polygon, 0, None),
            ('bow-tie', [[0,0],[.02,.02],[.02,0],[0,.02]], 13, 'self-intersecting outline'),
        ):
            result, stem = preview_run(name, ring, extra=['--nonconvex-outline'])
            kernel, out = solve_run(name+'-solve', 1)
            try:
                assert result.returncode == expected and kernel.returncode == expected, (
                    name, 'expected', expected,
                    'preview', result.returncode, result.stdout, result.stderr,
                    'solve', kernel.returncode, kernel.stdout, kernel.stderr)
                assert result.returncode == kernel.returncode, (result.returncode, kernel.returncode)
                if expected == 0:
                    digest = success(result)['mesh_sha256']
                    mesh = json.loads((out / 'manifest.json').read_text())['mesh']
                    assert digest == mesh['mesh_sha256'], (name, digest, mesh['mesh_sha256'])
                if diagnostic:
                    assert diagnostic in result.stdout + result.stderr, (name, result.stdout, result.stderr)
                    assert diagnostic in kernel.stderr, (name, kernel.stderr)
                    assert not list(root.glob(stem.name + '*.vtkhdf'))
                    assert not list(out.glob('out/*.vtkhdf'))
            except AssertionError as error:
                failures.append(str(error))
        assert not failures, '\n'.join(failures)
    elif test == 'preview_outline_external_polyset_rejected':
        binary = b'CCPOLY01' + struct.pack('<dII', 1e-6, 1, 7) + b'outline'
        ring = [(200000, 0), (800000, 0), (1000000, 200000), (1000000, 800000),
                (800000, 1000000), (200000, 1000000), (0, 800000), (0, 200000)]
        binary += struct.pack('<Iqq', 1, 0, len(ring))
        binary += b''.join(struct.pack('<qq', *v) for v in ring)
        path = root / 'outline.polyset'
        path.write_bytes(binary)
        result, stem = preview_run('external', extra=['--polyset', str(path)])
        assert result.returncode == 13, (result.returncode, result.stdout, result.stderr)
        failures = [e for e in events(result) if e['stage'] in ('error', 'failure')]
        assert failures, events(result)
        reason = ' '.join(e['message'] for e in failures)
        assert 'outline' in reason and ('polyset' in reason or 'external' in reason), reason
        assert not list(root.glob(stem.name + '*.vtkhdf')), list(root.iterdir())
    else:
        raise AssertionError(test)
