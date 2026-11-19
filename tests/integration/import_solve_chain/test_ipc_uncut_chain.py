"""IPC import → layer average → uncut coupled solve; illustrative materials."""
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
        run([sys.executable, SOURCE / 'tools/stack/copper_fraction.py', 'stack.json', '-o', 'solve.stack.json', '--report', 'cf.json'])
        report = json.loads((root / 'cf.json').read_text())
        fractions = {entry['layer']: entry['fraction'] for entry in report['layers']}
        for name, expected in [('F.Cu', .12), ('B.Cu', .24)]:
            assert name in fractions, ('importer layer discrepancy', fractions)
            assert math.isclose(fractions[name], expected, rel_tol=1e-12), ('importer geometry discrepancy', fractions)
        stack = json.loads((root / 'solve.stack.json').read_text())
        # tensor_grid.cpp accumulates stack layers from z=0 in array order.
        # Lock that physical order to the fixture's IPC StackupLayer sequence.
        assert [layer['name'] for layer in stack['layers']] == ['F.Cu', 'Core', 'B.Cu']
        for layer, thickness in zip(stack['layers'], [0.000035, 0.0016, 0.000035], strict=True):
            assert math.isclose(layer['thickness'], thickness, rel_tol=1e-12)
        for layer in stack['layers']:
            if layer['name'] in fractions:
                assert layer['copper'] == {'metal': 'cu', 'fraction': fractions[layer['name']]}, layer
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
        case = dict(schema='chipcae.case/1.0', stack=dict(ref='solve.stack.json'), materials=dict(ref='materials.json'),
                    mesh=dict(type='aligned_cartesian_cut', nx=2, ny=2, levels=0, max_ratio=100),
                    physics=[dict(id='thermal', solver='fem.conduction', mode='steady'), dict(id='mech', solver='fem.solid', temp_from='thermal', T_ref=298.15)],
                    loads=[dict(type='fixed_temperature', target='face:zmin', T=298.15), dict(type='convection', target='face:zmax', h=1000, T_inf=348.15)],
                    constraints=dict(rigid='3-2-1'), solver=dict(linear=dict(preconditioner='direct')), output=dict(format='vtkhdf'))
        write('case.json', case)
        write('profile.json', dict(schema='chipcae.run_profile/1.0', backend='cpu', ranks=1, gpus=0))
        run([solver, '--case', 'case.json', '--profile', 'profile.json', '--out', 'run'])
        manifest = json.loads((root / 'run/manifest.json').read_text())
        summary = {entry['name']: entry['value'] for entry in manifest['summary']}
        for key in ('warpage_pv', 'U_mag_max', 'T_max'):
            assert math.isfinite(summary[key]), (key, summary)
        for key in ('warpage_pv', 'U_mag_max'):
            assert summary[key] > 0, (key, summary)
        conductivity = {'cu': 390, 'fr4_eq': .3}
        layers = []
        for layer in stack['layers']:
            k = conductivity[layer['material']]
            if 'copper' in layer:
                copper = layer['copper']
                f = copper['fraction']
                k = 1 / (f / conductivity[copper['metal']] + (1 - f) / k)
            layers.append((layer['thickness'], k))
        heat_flux = 50 / (sum(t / k for t, k in layers) + 1 / 1000)
        with h5py.File(root / 'run/out/step-0.vtkhdf') as file:
            assert int(np.sum(file['VTKHDF/cut/NumberOfCells'][:])) == 0
            hexes = file['VTKHDF/hex']
            count = int(np.sum(hexes['NumberOfCells'][:]))
            assert count > 0
            assert np.all(hexes['CellData/cell_kind'][:] == 0)
            z = hexes['Points'][:, 2]
            assert math.isclose(float(np.min(z)), 0.0, abs_tol=1e-15)
            assert math.isclose(float(np.max(z)), sum(t for t, _ in layers), rel_tol=1e-12)
            resistance = np.zeros_like(z)
            bottom = float(np.min(z))
            for thickness, k in layers:
                resistance += np.clip(z - bottom, 0, thickness) / k
                bottom += thickness
            expected = 298.15 + heat_flux * resistance
            error = float(np.max(np.abs(hexes['PointData/T'][:] - expected)))
            assert error <= 1e-8, error
        print(f"实测 warpage_pv={summary['warpage_pv']}, T_max={summary['T_max']}, cells={count}, wall_seconds={time.monotonic() - started:.6f}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
