"""IPC import → polygon trace map → uncut coupled solve; illustrative materials."""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

SOURCE = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE))
sys.path.insert(0, str(SOURCE / 'tests/integration/f1130_uncut_cli'))


def main():
    importer, solver = os.environ.get('CHIPCAE_IMPORT_EXE'), os.environ.get('CHIPCAE_SOLVE_EXE')
    if not importer or not solver:
        print('SKIP: importer/solver executable environment unavailable')
        return 77
    try:
        import numpy as np
        import h5py
    except ImportError:
        return 77
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        started = time.monotonic()

        def write(name, obj):
            (root / name).write_text(json.dumps(obj), encoding='utf-8')

        def run(args):
            result = subprocess.run([str(arg) for arg in args], cwd=root, capture_output=True, text=True, timeout=600)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)

        # Exercise the CLI fixture's material ids through the solver's material schema.
        material_map = SOURCE / 'tests/integration/import_cli/material_map.json'
        run([importer, SOURCE / 'tests/integration/ecad/data/board_2layer.xml', '-o', 'stack.json', '--polyset', 'stack.polyset', '--report', 'defects.json', '--material-map', material_map])
        stack = json.loads((root / 'stack.json').read_text())
        # tensor_grid.cpp accumulates stack layers from z=0 in array order.
        # Lock that physical order to the fixture's IPC StackupLayer sequence.
        assert [layer['name'] for layer in stack['layers']] == ['F.Cu', 'Core', 'B.Cu']
        for layer, thickness in zip(stack['layers'], [0.000035, 0.0016, 0.000035], strict=True):
            assert math.isclose(layer['thickness'], thickness, rel_tol=1e-12)
        for layer in stack['layers']:
            if layer['kind'] == 'Patterned':
                assert layer['copper']['metal'] == 'cu', layer
                assert 'polygons' in layer['copper'] and 'fraction' not in layer['copper'], layer
                assert layer['material'] == 'fr4_eq', layer
        materials = []
        for name, E, nu, alpha, k in [('cu', 110e9, .34, 17e-6, 390), ('fr4_eq', 25e9, .3, 1e-5, .3)]:
            def table(columns):
                return dict(temperatures=[298.15], columns={key: [value] for key, value in columns.items()}, interpolation='constant', extrapolation='clamp')
            materials.append(dict(id=name, schema_version='1', unit_system='SI', model='thermo_elastic', density=2000, conductivity=k,
                                  elastic=dict(symmetry='isotropic', table=table(dict(E=E, nu=nu))),
                                  thermal_expansion=dict(symmetry='isotropic', cte_type='tangent', stress_free_temperature=298.15, table=table(dict(alpha=alpha))),
                                  provenance=dict(source_type='illustrative', confidence='illustrative')))
        write('materials.json', dict(schema='chipcae.material_library/1', materials=materials))
        case = dict(schema='chipcae.case/1.0', stack=dict(ref='stack.json'), materials=dict(ref='materials.json'),
                    mesh=dict(type='aligned_cartesian_cut', nx=2, ny=2, levels=0, max_ratio=100),
                    physics=[dict(id='thermal', solver='fem.conduction', mode='steady'), dict(id='mech', solver='fem.solid', temp_from='thermal', T_ref=298.15)],
                    loads=[dict(type='fixed_temperature', target='face:zmin', T=298.15), dict(type='convection', target='face:zmax', h=1000, T_inf=348.15)],
                    constraints=dict(rigid='3-2-1'), solver=dict(linear=dict(preconditioner='direct', rtol=1e-12)), output=dict(format='vtkhdf'))
        write('case.json', case)
        write('profile.json', dict(schema='chipcae.run_profile/1.0', backend='cpu', ranks=1, gpus=0))
        shared = {name: (root / name).read_bytes() for name in
                  ('stack.json', 'stack.polyset', 'materials.json', 'case.json')}
        run([solver, '--case', 'case.json', '--profile', 'profile.json', '--out', 'run'])
        for name, content in shared.items():
            assert (root / name).read_bytes() == content, f'shared input changed: {name}'
        run([sys.executable, SOURCE / 'tools/vv/result_summary.py', 'run', '-o', 'summary.json'])
        manifest = json.loads((root / 'run/manifest.json').read_text())
        summary = {entry['name']: entry['value'] for entry in manifest['summary']}
        for key in ('warpage_pv', 'U_mag_max', 'T_max'):
            assert math.isfinite(summary[key]), (key, summary)
        for key in ('warpage_pv', 'U_mag_max'):
            assert summary[key] > 0, (key, summary)
        import trace_map_ref as ref
        rings = ref.read_polyset(root / 'stack.polyset')
        with h5py.File(root / 'run/out/step-0.vtkhdf') as file:
            assert int(np.sum(file['VTKHDF/cut/NumberOfCells'][:])) == 0
            mesh = ref.read_hex_mesh(file['VTKHDF/hex'], stack)
        count = len(mesh['gids'])
        measurements = {}
        ref.check_solution(mesh, rings, stack, measurements,
                           expected_areas={'F.Cu': 6., 'B.Cu': 12.})
        readback = json.loads((root / 'summary.json').read_text())['warpage']['manifest_warpage_pv_m']
        assert math.isfinite(readback)
        assert math.isclose(readback, summary['warpage_pv'], rel_tol=1e-12, abs_tol=0)
        print(f"实测 warpage_pv={summary['warpage_pv']}, T_max={summary['T_max']}, cells={count}, wall_seconds={time.monotonic() - started:.6f}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
