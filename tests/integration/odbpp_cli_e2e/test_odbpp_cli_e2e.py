"""Synthetic ODB++/IPC CLI agreement and uncut coupled numerical acceptance.

Catches symbol-unit, layer-order, material, averaging, thermal and readback
regressions through real executables; no product behavior is replaced here.
"""
import copy
from datetime import date
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

SOURCE = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE))
FIXTURE = SOURCE / 'tests/integration/ecad/data/odbpp'
ORDER = ['f.cu', 'dielectric_1', 'in1.cu', 'dielectric_2',
         'in2.cu', 'dielectric_3', 'b.cu']
COPPER = ['f.cu', 'in1.cu', 'in2.cu', 'b.cu']
DIMENSIONS = [(10, 10), (20, 15), (30, 20), (36, 25)]
EXPECTED_FRACTIONS = [1 / 12, 1 / 4, 1 / 2, 3 / 4]
TIMINGS = {}


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def nonempty(path):
    assert path.is_file() and path.stat().st_size > 0, f'missing/empty artifact: {path}'


def fold(name):
    # Only ASCII case changes; punctuation and non-ASCII characters stay intact.
    return name.translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ',
                                        'abcdefghijklmnopqrstuvwxyz'))


def keyed(rows, field):
    result = {}
    for row in rows:
        key = fold(row[field])
        assert key not in result, f'duplicate ASCII-folded {field}: {key}'
        result[key] = row
    return result


def relative_ok(value, reference, tolerance):
    assert type(value) in (int, float) and type(reference) in (int, float)
    assert math.isfinite(value) and math.isfinite(reference), (value, reference)
    assert reference != 0, 'relative comparison requires a nonzero reference'
    return abs(value - reference) / abs(reference) <= tolerance


def relative(value, reference, tolerance, label):
    assert relative_ok(value, reference, tolerance), (
        f'{label}: actual={value:.17g}, reference={reference:.17g}, '
        f'relative_error={abs(value-reference)/abs(reference):.17g}, limit={tolerance}')


def run_step(name, argv, cwd):
    argv = [str(arg) for arg in argv]
    started = time.monotonic()
    try:
        result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.TimeoutExpired) as exc:
        TIMINGS[name] = time.monotonic() - started
        raise AssertionError(
            f'{name}: command={shlex.join(argv)}\nexit=not completed\n'
            f'stdout={getattr(exc, "stdout", None)}\n'
            f'stderr={getattr(exc, "stderr", None)}\n{exc}') from exc
    finally:
        TIMINGS[name] = time.monotonic() - started
    print(f'实测 {name}: exit={result.returncode}, seconds={TIMINGS[name]:.6f}', flush=True)
    assert result.returncode == 0, (
        f'{name}: command={shlex.join(argv)}\nexit={result.returncode}\n'
        f'stdout:\n{result.stdout}\nstderr:\n{result.stderr}')
    return result


def set_thickness(job, name, value):
    path = job / f'steps/pcb/layers/{fold(name)}/attrlist'
    text, count = re.subn(r'(?m)^\.layer_dielectric=.*$', f'.layer_dielectric={value}',
                          path.read_text())
    assert count == 1, 'fixture mutation must replace exactly one thickness'
    path.write_text(text)


def generate_odb(destination, changed_thickness=False):
    shutil.copytree(FIXTURE / 'kicad4l', destination)
    matrix = destination / 'matrix/matrix'
    matrix.write_text(re.sub(
        r'LAYER\s*\{[^}]*\}', lambda m: m[0] if re.search(
            r'TYPE=(SIGNAL|DIELECTRIC)\s', m[0]) else '', matrix.read_text()))
    dimensions = dict(zip(COPPER, DIMENSIONS, strict=True))
    for path in (destination / 'steps/pcb/layers').glob('*/features'):
        name = fold(path.parent.name)
        features = 'UNITS=MM\n'
        if name in dimensions:
            width, height = dimensions[name]
            features += f'$0 rect{width * 1000}x{height * 1000}\nP 30 -25 0 P 0 0\n'
        path.write_text(features)
    if changed_thickness:
        set_thickness(destination, 'DIELECTRIC_2', '1.07')
    return destination


def generate_ipc(destination, swap_inner=False):
    tree = ET.parse(FIXTURE / 'kicad4l.ipc2581.xml')
    root = tree.getroot()
    namespace = root.tag.partition('}')[0] + '}' if root.tag.startswith('{') else ''

    def local(element):
        return element.tag.rsplit('}', 1)[-1]

    # The initial import rejected unused Oval/UserSpecial/Circle dictionary
    # entries as C-tier; after removing all features, neither dictionary is used.
    for parent in root.iter():
        for child in list(parent):
            if local(child) in ('LayerFeature', 'Component', 'Package', 'PadStackDef',
                                    'DictionaryStandard', 'DictionaryUser'):
                parent.remove(child)
    steps = [node for node in root.iter() if local(node) == 'Step']
    assert len(steps) == 1, 'fixture must have exactly one board Step'
    dimensions = list(DIMENSIONS)
    if swap_inner:
        dimensions[1], dimensions[2] = dimensions[2], dimensions[1]
    for name, (width, height) in zip(['F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu'],
                                      dimensions, strict=True):
        feature = ET.SubElement(steps[0], namespace + 'LayerFeature', layerRef=name)
        polygon = ET.SubElement(ET.SubElement(feature, namespace + 'Set'), namespace + 'Polygon')
        x0, x1 = 30 - width / 2, 30 + width / 2
        y0, y1 = -25 - height / 2, -25 + height / 2
        for tag, (x, y) in zip(['PolyBegin'] + ['PolyStepSegment'] * 3,
                                [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], strict=True):
            ET.SubElement(polygon, namespace + tag, x=str(x), y=str(y))
    tree.write(destination, encoding='utf-8', xml_declaration=True)
    return destination


def import_board(importer, source, output, mapping, ipc=False):
    output.mkdir()
    argv = [importer, source, '-o', output / 'board.stack.json',
            '--material-map', mapping, '--report', output / 'defects.json']
    if ipc:
        argv.append('--accept-b-tier')
    try:
        run_step(f'{output.name}/import', argv, output.parent)
    except AssertionError:
        report = output / 'import_report.json'
        if report.is_file():
            print('import_report.json.defects:', json.dumps(load(report).get('defects')), flush=True)
        raise
    for name in ('board.stack.json', 'board.stack.polyset', 'import_layers.json',
                 'import_report.json', 'defects.json'):
        nonempty(output / name)
    return load(output / 'board.stack.json')


def average(output):
    run_step(f'{output.name}/copper_fraction',
             [sys.executable, SOURCE / 'tools/stack/copper_fraction.py',
              output / 'board.stack.json', '-o', output / 'solve.stack.json',
              '--report', output / 'cf.json'], output.parent)
    for name in ('solve.stack.json', 'cf.json'):
        nonempty(output / name)
    stack, report = load(output / 'solve.stack.json'), load(output / 'cf.json')
    entries = keyed(report['layers'], 'layer')
    assert len(entries) == 4 and set(entries) == set(COPPER), list(entries)
    assert all('polygons' not in layer.get('copper', {}) for layer in stack['layers'])
    return stack, report


def stack_layers(stack):
    layers = keyed(stack['layers'], 'name')
    assert list(layers) == ORDER, f'layer order/missing layers: {list(layers)}'
    return layers


def compare_thickness(odb, ipc):
    left, right = stack_layers(odb), stack_layers(ipc)
    unexpected, known = [], []
    for name in left:
        a, b = left[name]['thickness'], right[name]['thickness']
        if name in COPPER:
            if relative_ok(a, 3e-5, 1e-12) and relative_ok(b, 3.5e-5, 1e-12):
                known.append(name)
            else:
                unexpected.append(name)
        elif not relative_ok(a, b, 1e-6):
            unexpected.append(name)
    return unexpected, known


def fraction_layers(report):
    entries = keyed(report['layers'], 'layer')
    assert len(entries) == 4 and set(entries) == set(COPPER), list(entries)
    return entries


def compare_fractions(odb_cf, ipc_cf):
    left, right = fraction_layers(odb_cf), fraction_layers(ipc_cf)
    return [name for name in COPPER
            if not relative_ok(right[name]['fraction'], left[name]['fraction'], 1e-3)]


def check_geometry(output, stack, report):
    from tools.stack.polyset import read_polyset, layer_area_m2, ring_area_m2

    print(f'实测 {output.name}: thickness/material/copper={json.dumps(stack["layers"])}, '
          f'fractions={json.dumps(report["layers"])}', flush=True)
    stack_layers(stack)
    entries = fraction_layers(report)
    for name, expected in zip(COPPER, EXPECTED_FRACTIONS, strict=True):
        relative(entries[name]['fraction'], expected, 1e-6, f'{output.name}/{name}/fraction')
        relative(entries[name]['outline_area_m2'], 1.2e-3, 1e-6, f'{output.name}/outline')
    polyset = read_polyset(output / 'board.stack.polyset')
    polys = {}
    for layer in polyset.layers:
        name = fold(layer.name)
        assert name not in polys, f'duplicate folded polyset layer: {name}'
        polys[name] = layer
    for name, expected in [('f.cu', 1e-4)]:
        assert name in polys, f'missing polyset layer: {name}'
        relative(layer_area_m2(polys[name], polyset.dbu_m), expected, 1e-6,
                 f'{output.name}/{name}/decoded area m²')
    outline = tuple(tuple(point) for point in stack['outline']['polygon'])
    relative(abs(ring_area_m2(outline, 1.0)), 1.2e-3, 1e-6, f'{output.name}/outline polygon')


def material_cards():
    # Same illustrative cards and physical model as test_ipc_uncut_chain.py.
    materials = []
    for name, E, nu, alpha, k in [('mat.cu', 110e9, .34, 17e-6, 390),
                                 ('mat.fr4_eq', 25e9, .3, 1e-5, .3)]:
        def table(columns):
            return dict(temperatures=[298.15], columns={key: [value] for key, value in columns.items()},
                        interpolation='constant', extrapolation='clamp')
        materials.append(dict(
            id=name, schema_version='1', unit_system='SI', model='thermo_elastic',
            density=2000, conductivity=k,
            elastic=dict(symmetry='isotropic', table=table(dict(E=E, nu=nu))),
            thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                   stress_free_temperature=298.15, table=table(dict(alpha=alpha))),
            provenance=dict(source_type='illustrative', confidence='illustrative')))
    return dict(schema='chipcae.material_library/1', materials=materials)


def check_run(run_dir, mesh_stack, materials, fractions, preview_cells):
    import h5py
    import numpy as np

    nonempty(run_dir / 'manifest.json')
    nonempty(run_dir / 'out/step-0.vtkhdf')
    manifest = load(run_dir / 'manifest.json')
    summary = {entry['name']: entry['value'] for entry in manifest['summary']}
    for name in ('warpage_pv', 'U_mag_max', 'T_max'):
        assert type(summary[name]) in (int, float) and math.isfinite(summary[name]), (name, summary)
    for name in ('warpage_pv', 'U_mag_max'):
        assert summary[name] > 0, (name, summary)
    conductivity = {card['id']: card['conductivity'] for card in materials['materials']}
    layers = []
    by_name = fraction_layers(fractions)
    for layer in mesh_stack['layers']:
        k = conductivity[layer['material']]
        if 'copper' in layer:
            copper = layer['copper']
            f = by_name[fold(layer['name'])]['fraction']
            relative(copper['fraction'], f, 1e-12, f'{layer["name"]}/solver fraction')
            k = 1 / (f / conductivity[copper['metal']] + (1 - f) / k)
        assert math.isfinite(k) and k > 0
        assert math.isfinite(layer['thickness']) and layer['thickness'] > 0
        layers.append((layer['thickness'], k))
    heat_flux = 50 / (sum(t / k for t, k in layers) + 1 / 1000)
    with h5py.File(run_dir / 'out/step-0.vtkhdf') as file:
        cut = file.get('VTKHDF/cut')
        cut_count = 0 if cut is None else int(np.sum(cut['NumberOfCells'][:]))
        assert cut_count == 0, f'cut cells: {cut_count}'
        hexes = file['VTKHDF/hex']
        cells = int(np.sum(hexes['NumberOfCells'][:]))
        assert cells > 0 and cells == preview_cells, (cells, preview_cells)
        assert np.all(hexes['CellData/cell_kind'][:] == 0), 'nonregular hex cell kind'
        z = hexes['Points'][:, 2]
        temperatures = hexes['PointData/T'][:]
        assert z.size > 0 and temperatures.shape == z.shape, (temperatures.shape, z.shape)
        assert np.all(np.isfinite(z)) and np.all(np.isfinite(temperatures))
        assert abs(float(np.min(z))) <= 1e-15, 'stack must start at z=0'
        relative(float(np.max(z)), sum(t for t, _ in layers), 1e-12, 'stack height')
        resistance = np.zeros_like(z)
        bottom = 0.0
        for thickness, k in layers:
            resistance += np.clip(z - bottom, 0, thickness) / k
            bottom += thickness
        expected = 298.15 + heat_flux * resistance
        assert np.all(np.isfinite(expected))
        error = float(np.max(np.abs(temperatures - expected)))
        print(f'实测 {run_dir.name}: cells={cells}, cut_cells={cut_count}, '
              f'T_max_abs_error_K={error:.17g}, QoI={summary}', flush=True)
        assert math.isfinite(error) and error <= 1e-8, f'T_max_abs_error_K={error:.17g} > 1e-8'
    run_step(f'{run_dir.name}/result_summary',
             [sys.executable, SOURCE / 'tools/vv/result_summary.py', run_dir,
              '-o', run_dir / 'summary.json'], run_dir.parent)
    nonempty(run_dir / 'summary.json')
    relative(load(run_dir / 'summary.json')['warpage']['w_bfp_m'],
             summary['warpage_pv'], 1e-12, 'readback warpage')
    return dict(T_max_abs_error_K=error, **{name: summary[name]
                                         for name in ('warpage_pv', 'U_mag_max', 'T_max')})


def revision():
    provided = os.environ.get('CHIPCAE_GIT_SHA', '')
    if re.fullmatch(r'[0-9a-f]{7,40}', provided):
        return provided, []
    try:
        value = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=SOURCE,
                                        stderr=subprocess.PIPE, text=True, timeout=10).strip()
        if re.fullmatch(r'[0-9a-f]{7,40}', value):
            return value, []
    except (OSError, subprocess.SubprocessError):
        pass
    return '0000000', ['commit fallback: CHIPCAE_GIT_SHA and git HEAD unavailable; using 0000000']


def main():
    importer, previewer, solver = [os.environ.get(name) for name in (
        'CHIPCAE_IMPORT_EXE', 'CHIPCAE_PREVIEW_EXE', 'CHIPCAE_SOLVE_EXE')]
    if not all((importer, previewer, solver)):
        print('SKIP: importer/preview/solver executable environment unavailable')
        return 77
    try:
        import numpy  # noqa: F401
        import h5py  # noqa: F401
    except ImportError as exc:
        print(f'SKIP: numpy/h5py unavailable: {exc}')
        return 77
    from tools.vv.check_result_data_class import check_file

    TIMINGS.clear()
    with tempfile.TemporaryDirectory(prefix='odbcli-') as directory:
        root = Path(directory)
        mapping = root / 'map.json'
        write(mapping, {'copper': 'mat.cu', 'Copper': 'mat.cu', 'COPPER': 'mat.cu',
                        'FR4': 'mat.fr4_eq', 'fr4': 'mat.fr4_eq',
                        'layer:DIELECTRIC_1': 'mat.fr4_eq'})
        odb = import_board(importer, generate_odb(root / 'job'), root / 'odb', mapping)
        ipc = import_board(importer, generate_ipc(root / 'board.xml'), root / 'ipc', mapping, ipc=True)
        odb_average, odb_cf = average(root / 'odb')
        ipc_average, ipc_cf = average(root / 'ipc')
        for name, stack, report in [('odb', odb, odb_cf), ('ipc', ipc, ipc_cf)]:
            check_geometry(root / name, stack, report)
        left, right = stack_layers(odb), stack_layers(ipc)
        for name in ORDER:
            a, b = left[name], right[name]
            assert ('copper' in a) == ('copper' in b) == (name in COPPER), (name, a, b)
            assert a['material'] == b['material'] == ('mat.cu' if name in COPPER else 'mat.fr4_eq'), (
                f'M1 material discrepancy: {name}', a, b)
            if name in COPPER:
                assert a['copper']['metal'] == b['copper']['metal'] == 'mat.cu', (name, a, b)
        unexpected, known = compare_thickness(odb, ipc)
        print(f'已知偏差，待裁定 (M1): {known}; unexpected={unexpected}', flush=True)
        assert unexpected == [] and known == COPPER, (unexpected, known)
        assert compare_fractions(odb_cf, ipc_cf) == []
        for name in COPPER:
            relative(fraction_layers(ipc_cf)[name]['outline_area_m2'],
                     fraction_layers(odb_cf)[name]['outline_area_m2'], 1e-6, f'{name}/cross outline')
        negative = import_board(importer, generate_odb(root / 'job_thick', changed_thickness=True),
                                root / 'neg_thick', mapping)
        unexpected, known = compare_thickness(negative, ipc)
        assert unexpected == ['dielectric_2'] and known == COPPER, (unexpected, known)
        import_board(importer, generate_ipc(root / 'swap.xml', swap_inner=True),
                     root / 'neg_swap', mapping, ipc=True)
        _, swapped_cf = average(root / 'neg_swap')
        mismatches = compare_fractions(odb_cf, swapped_cf)
        assert mismatches == ['in1.cu', 'in2.cu'], mismatches
        print(f'negative comparisons: thickness={unexpected}, fractions={mismatches}', flush=True)

        mesh_stack = copy.deepcopy(odb_average)
        spacing = min(layer['thickness'] for layer in mesh_stack['layers'])
        for layer in mesh_stack['layers']:
            layer['n_elem_min'] = math.ceil(layer['thickness'] / spacing)
        mesh_path = root / 'odb/mesh.stack.json'
        write(mesh_path, mesh_stack)
        result = run_step('odb/preview', [previewer, mesh_path, '-o', root / 'preview',
                          '--nx', '4', '--ny', '4', '--levels', '0', '--max-ratio', '100'], root)
        meshes = list(root.glob('preview*.vtkhdf'))
        assert meshes and all(path.stat().st_size > 0 for path in meshes), 'no preview output'
        events = []
        for line in result.stdout.splitlines():
            if line.lstrip().startswith('{'):
                event = json.loads(line)
                if event.get('stage') == 'success':
                    events.append(event)
        assert len(events) == 1, result.stdout
        preview_cells = events[0]['cell_count']
        assert type(preview_cells) is int and preview_cells > 0, events
        print(f'实测 preview cells={preview_cells}, n_elem_min='
              f'{[layer["n_elem_min"] for layer in mesh_stack["layers"]]}', flush=True)
        materials = material_cards()
        write(root / 'materials.json', materials)
        case = dict(
            schema='chipcae.case/1.0', stack=dict(ref='odb/mesh.stack.json'),
            materials=dict(ref='materials.json'),
            mesh=dict(type='aligned_cartesian_cut', nx=4, ny=4, levels=0, max_ratio=100),
            physics=[dict(id='thermal', solver='fem.conduction', mode='steady'),
                     dict(id='mech', solver='fem.solid', temp_from='thermal', T_ref=298.15)],
            loads=[dict(type='fixed_temperature', target='face:zmin', T=298.15),
                   dict(type='convection', target='face:zmax', h=1000, T_inf=348.15)],
            constraints=dict(rigid='3-2-1'), solver=dict(linear=dict(preconditioner='direct')),
            output=dict(format='vtkhdf'))
        write(root / 'case.json', case)
        preserved = {path: path.read_bytes() for path in (
            root / 'case.json', root / 'materials.json', mesh_path,
            root / 'odb/board.stack.json', root / 'odb/solve.stack.json',
            root / 'ipc/board.stack.json', root / 'ipc/solve.stack.json')}
        launcher = shutil.which('mpiexec') or shutil.which('mpirun')
        print(f'实测 MPI launcher={launcher}', flush=True)
        if launcher is None:
            print('SKIP ranks=2: launcher not found', flush=True)
        measured = {}
        for ranks in ([1, 2] if launcher else [1]):
            profile = dict(schema='chipcae.run_profile/1.0', backend='cpu', ranks=ranks, gpus=0)
            profile_path = root / f'profile_r{ranks}.json'
            write(profile_path, profile)
            run_dir = root / f'ranks{ranks}'
            argv = [solver, '--case', root / 'case.json', '--profile', profile_path, '--out', run_dir]
            if ranks == 2:
                argv = [launcher, '-n', '2', *argv]
            run_step(f'ranks{ranks}/solve', argv, root)
            measured[ranks] = check_run(run_dir, mesh_stack, materials, odb_cf, preview_cells)
            commit, notes = revision()
            write(run_dir / 'result.json', dict(
                schema='chipcae.vv_result/0.2', case_id=f'odbcli_kicad4l_reduced_ranks{ranks}',
                data_class='synthetic', label='实测', date=date.today().strftime('%Y%m%d'),
                commit=commit, qoi=measured[ranks], verdict='pass',
                reference=dict(value=None, type='解析', source='1D 串联热阻，test_ipc_uncut_chain.py 同口径'),
                notes=notes + [f'实测 {name}: {elapsed:.6f} s' for name, elapsed in TIMINGS.items()]))
            violations = check_file(run_dir / 'result.json')
            assert violations == [], violations
            assert all(path.read_bytes() == content for path, content in preserved.items()), 'input bytes changed'
        if 2 in measured:
            for name in ('warpage_pv', 'U_mag_max', 'T_max'):
                relative(measured[2][name], measured[1][name], 1e-8, f'ranks2/ranks1 {name}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
