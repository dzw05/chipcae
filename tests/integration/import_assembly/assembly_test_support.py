"""Shared black-box fixtures. No production code or golden output is replaced."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCE = Path(os.environ['CHIPCAE_SOURCE_DIR'])
BUILD = Path(os.environ['CHIPCAE_BINARY_DIR'])
CACHE = (BUILD / 'CMakeCache.txt').read_text()
# OCCT may be installed in a non-system prefix. Its transitive shared libraries
# require the same runtime search directory as the main OCCT acceptance builds.
for _line in CACHE.splitlines():
    if _line.startswith('OpenCASCADE_DIR:') and not _line.endswith('NOTFOUND'):
        _library_dir = str(Path(_line.split('=', 1)[1]).parents[1])
        os.environ['LD_LIBRARY_PATH'] = _library_dir + ':' + os.environ.get('LD_LIBRARY_PATH', '')


def enabled(name):
    return f'CHIPCAE_ENABLE_{name}:BOOL=ON' in CACHE


def require(name):
    if not enabled(name):
        print(f'SKIP: {name} is disabled')
        raise SystemExit(77)


def check(result, expected):
    assert result.returncode == expected, (
        f'expected exit {expected}, got {result.returncode}\n'
        f'command: {result.args}\nstdout: {result.stdout}\nstderr: {result.stderr}')


class Assembly:
    def __init__(self, root):
        self.root = Path(root)
        self.output = self.root / 'out.stack.json'
        self.report = self.root / 'defects.json'
        shutil.copyfile(SOURCE / 'tests/integration/ecad/data/board_2layer.xml',
                        self.root / 'board.xml')
        (self.root / 'map.json').write_text(json.dumps({'FR4': 'fr4_eq', 'copper': 'copper'}))
        self.document = {'schema': 'chipcae.assembly/0.1',
                         'board': {'path': 'board.xml', 'format': 'ipc2581',
                                   'material_map': 'map.json'}, 'parts': []}
        self.save()

    def save(self):
        (self.root / 'assembly.json').write_text(json.dumps(self.document))

    def part(self, name='lid', step='lid.step', **extra):
        value = {'id': name, 'step': step, 'material': 'copper',
                 'placement': {'attach': 'layer:B.Cu@top'}}
        value.update(extra)
        self.document['parts'].append(value)
        self.save()
        return value

    def run(self, *args):
        # Run from a different directory to exercise assembly-relative input paths.
        return subprocess.run([str(BUILD / 'kernel/chipcae-import'), '--assembly',
                               str(self.root / 'assembly.json'), '-o', str(self.output),
                               '--report', str(self.report), *map(str, args)],
                              cwd=self.root.parent, text=True, capture_output=True, timeout=25)

    def defects(self):
        return json.loads(self.report.read_text())

    def import_report(self):
        return json.loads((self.root / 'import_report.json').read_text())

    def stack(self):
        result = json.loads(self.output.read_text())
        from jsonschema import Draft202012Validator
        Draft202012Validator(json.loads((SOURCE / 'schema/stack-0.2.schema.json').read_text())).validate(result)
        return result

    def accept(self, ids):
        path = self.root / 'accept.json'
        path.write_text(json.dumps({'accept': [{'defect_id': id_, 'note': 'acceptance test'}
                                               for id_ in ids]}))
        return path

    def boxes(self, thin=False):
        if thin:
            require_assembly_geometry()
        require('OCCT')
        generator = step_generator()
        check(subprocess.run([generator, str(self.root), 'thin' if thin else 'normal'],
                             capture_output=True, text=True), 0)
        self.part()
        if thin:
            self.part('second', 'second.step')


def b_ids(report):
    return [d['defect_id'] for d in report['defects'] if d['tier'] == 'B']


def require_assembly_geometry():
    """SP2 slicing and SP3 placement/merge are required for geometry expectations."""
    capabilities = json.loads((BUILD / 'import-assets/assembly-capabilities.json').read_text())
    if capabilities['geometry'] != 'ON':
        print('SKIP: SP2/SP3 geometry implementations are not available')
        raise SystemExit(77)


def step_generator():
    """Use the OCCT fixture tool built by the main test build."""
    require('OCCT')
    generator = Path(os.environ.get('SP4_FIXTURE_GENERATOR',
                                    BUILD / 'sp4-step-fixtures/sp4_step_fixtures'))
    if not generator.is_file():
        raise AssertionError(f'OCCT fixture tool is missing from the test build: {generator}')
    return str(generator)


def solve_stack(root, stack, name):
    """Use the established FCBGA solve case with constant material tables."""
    require('MFEM')
    require('MPI')
    require('HDF5')
    root = Path(root)
    library = json.loads((SOURCE / 'schema/examples/fcbga_demo.materials.json').read_text())
    for material in library['materials']:
        for section in ('elastic', 'thermal_expansion'):
            table = material[section]['table']
            table['temperatures'] = table['temperatures'][:1]
            table['columns'] = {k: v[:1] for k, v in table['columns'].items()}
            table['interpolation'] = 'constant'
            table['extrapolation'] = 'clamp'
    import copy
    for id_ in ('fr4_eq', 'copper'):
        if any(m['id'] == id_ for m in library['materials']):
            continue
        material = copy.deepcopy(library['materials'][0])
        material['id'] = id_
        library['materials'].append(material)
    (root / 'materials.json').write_text(json.dumps(library))
    case = json.loads((SOURCE / 'schema/examples/fcbga_demo.case.json').read_text())
    case['stack'] = {'ref': Path(stack).name}
    case['materials'] = {'ref': 'materials.json'}
    case['mesh'].update(nx=4, ny=4, levels=1, max_ratio=100)
    case['solver'] = {'linear': {'preconditioner': 'direct'}}
    (root / 'case.json').write_text(json.dumps(case))
    (root / 'profile.json').write_text(json.dumps({'schema': 'chipcae.run_profile/1.0',
                                                 'ranks': 1, 'backend': 'cpu', 'gpus': 0}))
    destination = root / name
    check(subprocess.run([str(BUILD / 'apps/chipcae-solve/chipcae-solve'), '--case',
                          str(root / 'case.json'), '--profile', str(root / 'profile.json'),
                          '--out', str(destination)], text=True, capture_output=True, timeout=40), 0)
    return json.loads((destination / 'manifest.json').read_text())
