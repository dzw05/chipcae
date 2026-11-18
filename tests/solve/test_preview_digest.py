"""Compare the solver's canonical mesh digest with the public preview CLI."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

solve, preview, mpiexec = sys.argv[1:]


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='solve-preview-digest-') as tmp:
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
        'mesh': {'type': 'aligned_cartesian_cut', 'nx': 4, 'ny': 2, 'levels': 1,
                 'min_elems_per_layer': 1, 'max_ratio': 10},
        'physics': [{'id': 'mech', 'solver': 'fem.solid'}],
        'loads': [{'type': 'uniform_temperature', 'T_ref': 300, 'T_target': 300}],
        'constraints': {'rigid': '3-2-1'},
        'solver': {'linear': {'preconditioner': 'direct'}},
        'output': {'format': 'vtkhdf'}}
    (root / 'case.json').write_text(json.dumps(case))
    # Verified with Release solver and preview builds of baseline
    # a1510a95fc5c96a4fe37b6a512b948671832b515 at np=1 and np=2.
    baseline_digests = {
        1: b'6245600b72b8cd0d9dc213a905a417f09bd9039a5f478dba3805ee70db628e1e',
        2: b'6245600b72b8cd0d9dc213a905a417f09bd9039a5f478dba3805ee70db628e1e',
    }
    digests = []
    for ranks in (1, 2):
        launcher = [mpiexec, '-n', str(ranks)]
        profile = root / 'profile.json'
        profile.write_text(json.dumps({'schema': 'chipcae.run_profile/1.0',
                                       'ranks': ranks, 'backend': 'cpu', 'gpus': 0}))
        out = root / f'solve-{ranks}'
        run([*launcher, solve, '--case', str(root / 'case.json'),
             '--profile', str(profile), '--out', str(out)])
        kernel = json.loads((out / 'manifest.json').read_text())['mesh']
        stdout = run([*launcher, preview, str(root / 'stack.json'), '--nx', '4',
                      '--ny', '2', '--levels', '1', '-o', str(root / f'preview-{ranks}')])
        success, = [json.loads(line) for line in stdout.splitlines()
                    if json.loads(line)['stage'] == 'success']
        for key in ('nx0', 'ny0', 'L', 'nz', 'padded_cells', 'mesh_sha256'):
            assert kernel[key] == success[key], (ranks, key, kernel, success)
        assert kernel['mesh_sha256'].encode('ascii') == baseline_digests[ranks], (ranks, kernel)
        digests.append(kernel['mesh_sha256'])
        print(f'np={ranks}: kernel == preview: {digests[-1]}', flush=True)
    assert digests[0] == digests[1], digests

    # Padded solve-only case: preview padding is owned by a separate fix.
    case['mesh'].update(nx=7, ny=5, levels=2)
    (root / 'case.json').write_text(json.dumps(case))
    padded_cells = []
    for ranks in (1, 2):
        profile = root / 'profile.json'
        profile.write_text(json.dumps({'schema': 'chipcae.run_profile/1.0',
                                       'ranks': ranks, 'backend': 'cpu', 'gpus': 0}))
        out = root / f'padded-solve-{ranks}'
        run([mpiexec, '-n', str(ranks), solve, '--case', str(root / 'case.json'),
             '--profile', str(profile), '--out', str(out)])
        mesh = json.loads((out / 'manifest.json').read_text())['mesh']
        nx0, ny0, level, nz = (mesh[key] for key in ('nx0', 'ny0', 'L', 'nz'))
        assert level == case['mesh']['levels'] - 1, (ranks, mesh)
        expected = (nx0 * 2**level * ny0 * 2**level - 7 * 5) * nz
        assert mesh['padded_cells'] == expected, (ranks, mesh, expected)
        assert mesh['padded_cells'] > 0, (ranks, mesh)
        assert expected == 13 * nz, mesh
        padded_cells.append(mesh['padded_cells'])
        print(f'np={ranks}: solve padded_cells={padded_cells[-1]}', flush=True)
    assert padded_cells[0] == padded_cells[1], padded_cells
