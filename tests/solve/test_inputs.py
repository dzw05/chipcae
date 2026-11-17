"""CLI validation and end-to-end input acceptance."""
import copy
import hashlib
import json
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time

exe, source, backend = sys.argv[1:]
backend = backend == "1"
start = time.perf_counter()
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

    def run(c, p, expected, diagnostic=''):
        shutil.rmtree(root / 'out', ignore_errors=True)
        (root / 'case.json').write_text(json.dumps(c))
        (root / 'profile.json').write_text(json.dumps(p))
        result = subprocess.run([exe, '--case', str(root / 'case.json'), '--profile',
                                 str(root / 'profile.json'), '--out', str(root / 'out')],
                                capture_output=True, text=True, timeout=120)
        assert result.returncode == expected, (result.returncode, result.stderr)
        assert diagnostic in result.stderr, result.stderr
        if expected == 0:
            assert (root / 'out/manifest.json').exists()

    if backend:
        run(case, profile, 0)
    missing = copy.deepcopy(case)
    missing['stack'] = {}
    run(missing, profile, 12, 'missing ref')
    altered = copy.deepcopy(case)
    altered['stack']['sha256'] = hashlib.sha256((root / 'stack.json').read_bytes()).hexdigest()
    if backend:
        run(altered, profile, 0)
    (root / 'stack.json').write_text(json.dumps(stack) + '\n')
    run(altered, profile, 10, 'hash mismatch')
    invalid = dict(profile, ranks=2)
    run(case, invalid, 14, 'profile')
    cut = copy.deepcopy(stack)
    cut['layers'][0]['kind'] = 'Explicit'
    cut['layers'][0]['regions'] = [{'name': 'die', 'material': 'organic_eq',
                                  'footprint': {'box': [0.005, 0.005, 0.015, 0.015]}}]
    (root / 'stack.json').write_text(json.dumps(cut))
    if backend:
        run(case, profile, 0)
    cut['layers'][0]['regions'][0]['footprint']['polygon'] = [[0.005, 0.005], [0.015, 0.005], [0.01, 0.015]]
    (root / 'stack.json').write_text(json.dumps(cut))
    run(case, profile, 13, 'cut not supported in O2')
    (root / 'stack.json').write_text(json.dumps(stack))
    for mutation in ('duplicate', 'temperature', 'target', 'linear'):
        invalid = copy.deepcopy(case)
        if mutation == 'duplicate':
            invalid['loads'][0]['id'] = 'same'
            invalid['constraints']['id'] = 'same'
        elif mutation == 'temperature':
            invalid['physics'].append({'id': 'thermal', 'solver': 'fem.conduction', 'mode': 'steady'})
        elif mutation == 'target':
            invalid['constraints'] = {'fixed': [{'target': 'layer:missing'}]}
        else:
            invalid['solver'] = {'linear': {'rtol': 0}}
        run(invalid, profile, 14)
    # Schema accepts temperature ordering; the loader must reject it with context.
    for section, properties in (('elastic', ('E', 'nu')),
                                ('thermal_expansion', ('alpha',))):
        for temperatures in ([300, 300], [400, 250]):
            bad = copy.deepcopy(library)
            tab = bad['materials'][0][section]['table']
            tab['temperatures'] = temperatures
            tab['interpolation'] = 'linear'
            for prop in properties:
                tab['columns'][prop] *= 2
            (root / 'material.json').write_text(json.dumps(bad))
            run(case, profile, 14, f'material organic_eq/{properties[0]}: temperatures must be strictly increasing')
    (root / 'material.json').write_text(json.dumps(library))
    # Schema-valid unsupported modes must fail before the solve.
    for section, properties in (('elastic', ('E', 'nu')),
                                ('thermal_expansion', ('alpha',))):
        for mode in ('pchip', 'constant'):
            bad = copy.deepcopy(library)
            tab = bad['materials'][0][section]['table']
            tab['temperatures'] = [250, 325, 500]
            tab['interpolation'] = mode
            for prop in properties:
                tab['columns'][prop] *= 3
            (root / 'material.json').write_text(json.dumps(bad))
            run(case, profile, 14, f'material organic_eq/{properties[0]}: unsupported interpolation')
    bad = copy.deepcopy(library)
    tab = bad['materials'][0]['elastic']['table']
    tab.update(temperatures=[325, 500], interpolation='linear', extrapolation='error')
    for prop in ('E', 'nu'):
        tab['columns'][prop] *= 2
    (root / 'material.json').write_text(json.dumps(bad))
    run(case, profile, 14, 'material organic_eq/E: requested temperature outside table range')
    (root / 'material.json').write_text(json.dumps(library))
    # Every integer converted to int is tested on both signed/unsigned overflow paths.
    for value in (4294967297, 2**63-1, 2**64-1, -2**63, 1.5, '1', True):
        for field in ('ranks', 'gpus', 'threads_per_rank'):
            run(case, dict(profile, **{field: value}), 14)
        for field in ('nx', 'ny', 'levels', 'cell_budget', 'min_elems_per_layer'):
            invalid = copy.deepcopy(case)
            invalid['mesh'][field] = value
            run(invalid, profile, 14)
        invalid = copy.deepcopy(case)
        invalid['solver'] = {'linear': {'max_iter': value}}
        run(invalid, profile, 14)
        run(case, dict(profile, output_interval={'steps': value}), 14)
    for args in ('bad', [1], {}, [True]):
        run(case, dict(profile, mpi_args=args), 14)
    for interval in ('bad', {}, {'steps': 1, 'time': 2}, {'steps': 0},
                     {'time': 0}, {'time': '2'}, {'time': True}, {'other': 2}):
        run(case, dict(profile, output_interval=interval), 14)
    for field in ('stack', 'materials'):
        invalid = copy.deepcopy(case)
        invalid[field]['ref'] = str(root / ('stack.json' if field == 'stack' else 'material.json'))
        run(invalid, profile, 14, 'relative')
    stack_ref = copy.deepcopy(stack)
    stack_ref['external_refs'] = [{'path': str(root / 'material.json'), 'sha256': '0'*64}]
    (root / 'stack.json').write_text(json.dumps(stack_ref))
    run(case, profile, 14, 'relative')
    (root / 'stack.json').write_text(json.dumps(stack))
    for bad_material in ({}, dict(library, materials=[]), dict(library, materials=[{}, material])):
        (root / 'material.json').write_text(json.dumps(bad_material))
        run(case, profile, 14)
    (root / 'material.json').write_text(json.dumps(dict(library, materials=[material, material])))
    run(case, profile, 11, 'duplicate material id')
    for field in ('id', 'schema_version', 'unit_system', 'model', 'density', 'elastic',
                  'thermal_expansion', 'provenance'):
        bad = copy.deepcopy(library)
        del bad['materials'][0][field]
        (root / 'material.json').write_text(json.dumps(bad))
        run(case, profile, 14)
    bad = copy.deepcopy(library)
    bad['materials'][0]['id'] = 'missing'
    (root / 'material.json').write_text(json.dumps(bad))
    run(case, profile, 9, 'mapping missing')
    (root / 'material.json').write_text(json.dumps(library))
    if backend:
        run(case, dict(profile, mpi_args=['--bind-to', 'none'], output_interval={'steps': 1}), 0)
        # Native block YAML, including a quoted scalar, flow collection and sequence.
        yaml_case = """schema: chipcae.case/1.0
stack: {ref: stack.json}
materials: {ref: material.json}
mesh:
  type: aligned_cartesian_cut
physics:
  - id: mech
    solver: fem.solid
loads:
  - type: uniform_temperature
    T_ref: 300
    T_target: 400
constraints: {rigid: "3-2-1"}
output: {format: vtkhdf}
"""
        for extension in ('yaml', 'yml'):
            path = root / ('case.' + extension)
            path.write_text(yaml_case)
            result = subprocess.run([exe, '--case', str(path), '--profile', str(root / 'profile.json'),
                                     '--out', str(root / ('out-' + extension))], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
    for args in ([], ['--case'], ['--help', 'extra'], ['--wat', 'value'],
                 ['--case', 'a', '--case', 'b', '--out', 'c']):
        assert subprocess.run([exe, *args], capture_output=True).returncode == 14
    assert subprocess.run([exe, '--help'], capture_output=True).returncode == 0
print(f'Input and CLI assertions passed; wall_seconds={time.perf_counter() - start:.6f}')
