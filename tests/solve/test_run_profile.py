"""Run-profile backend/GPU pairing acceptance tests; cuda_unavailable requires a CPU build."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe, source, scenario = sys.argv[1:]
backend, gpus, expected = {
    'cuda_unavailable': ('cuda', 1, 2),
    'cuda_gpus_zero': ('cuda', 0, 14),
    'cpu_gpus_nonzero': ('cpu', 1, 14),
}[scenario]
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    stack = json.loads((Path(source) / 'schema/examples/fcbga_demo.stack.json').read_text())
    stack['layers'] = stack['layers'][:1]
    stack.pop('activation_groups', None)
    (root / 'stack.json').write_text(json.dumps(stack))
    table = lambda columns: {'temperatures': [300], 'columns': columns,
                             'interpolation': 'constant', 'extrapolation': 'clamp'}
    material = {'id': 'organic_eq', 'schema_version': '1', 'unit_system': 'SI',
                'model': 'thermo_elastic', 'density': 2000,
                'elastic': {'symmetry': 'isotropic', 'table': table({'E': [25e9], 'nu': [0.3]})},
                'thermal_expansion': {'cte_type': 'tangent', 'symmetry': 'isotropic',
                    'stress_free_temperature': 300, 'table': table({'alpha': [15e-6]})},
                'provenance': {'source_type': 'illustrative', 'confidence': 'illustrative'}}
    library = {'schema': 'chipcae.material_library/1', 'materials': [material]}
    (root / 'material.json').write_text(json.dumps(library))
    profile = {'schema': 'chipcae.run_profile/1.0', 'ranks': 1, 'backend': 'cpu', 'gpus': 0}
    case = {'schema': 'chipcae.case/1.0', 'stack': {'ref': 'stack.json'},
            'materials': {'ref': 'material.json'},
            'mesh': {'type': 'aligned_cartesian_cut'},
            'physics': [{'id': 'mech', 'solver': 'fem.solid'}],
            'loads': [{'type': 'uniform_temperature', 'T_ref': 300, 'T_target': 400}],
            'constraints': {'rigid': '3-2-1'}, 'output': {'format': 'vtkhdf'}}

    profile.update(backend=backend, gpus=gpus)
    (root / 'case.json').write_text(json.dumps(case))
    (root / 'gpu.run-profile.json').write_text(json.dumps(profile))
    result = subprocess.run(
        [exe, '--case', str(root / 'case.json'), '--profile',
         str(root / 'gpu.run-profile.json'), '--out', str(root / 'out')],
        capture_output=True, text=True, timeout=120)
    assert result.returncode == expected, (
        f'expected exit {expected}, got {result.returncode}; stderr: {result.stderr}')
    if scenario == 'cuda_unavailable':
        assert 'chipcae-solve built without CUDA-enabled MFEM' in result.stderr, result.stderr
