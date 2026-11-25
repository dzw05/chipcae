"""LU refinement diagnostics and successful tolerances through the public CLI."""
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


exe, mpirun, importer, preview = sys.argv[1:]
SOURCE = Path(__file__).resolve().parents[2]
SCI = r"[-+]?(?:[0-9]\.[0-9]{3}e[+-][0-9]+|inf)"
FAILURE = re.compile(
    rf"LU refinement failed residual tolerance: omega=({SCI}) "
    rf"\(rtol=({SCI}), eta=({SCI}), rel=({SCI}), iterations=([1-8]), "
    rf"refine_seconds=({SCI}), field=(T|U)\)"
)


def card(name, E, nu, alpha, k=1):
    def table(values):
        return {'temperatures': [300], 'columns': {key: [v] for key, v in values.items()},
                'interpolation': 'constant', 'extrapolation': 'clamp'}
    return {'id': name, 'schema_version': '1', 'unit_system': 'SI', 'model': 'thermo_elastic',
            'density': 2000, 'conductivity': k,
            'elastic': {'symmetry': 'isotropic', 'table': table({'E': E, 'nu': nu})},
            'thermal_expansion': {'symmetry': 'isotropic', 'cte_type': 'tangent',
                'stress_free_temperature': 300, 'table': table({'alpha': alpha})},
            'provenance': {'source_type': 'illustrative', 'confidence': 'illustrative'}}


def events(path):
    content = path.read_bytes()
    result = []
    while content:
        assert len(content) >= 16, 'truncated event header'
        assert content[:8] == b'CAELOG1\x00', 'invalid event magic'
        length, crc = struct.unpack_from('<II', content, 8)
        assert 17 <= length <= len(content) - 16, 'invalid event length'
        body = content[16:16 + length]
        assert zlib.crc32(body) == crc, 'invalid event CRC'
        seq, wall, kind = struct.unpack_from('<QqB', body)
        result.append((kind, body[17:]))
        content = content[16 + length:]
    return result


def log_lines(decoded_events):
    result = []
    for kind, payload in decoded_events:
        if kind != 20:
            continue
        assert len(payload) >= 12, 'truncated LogLine header'
        level, rank, length = struct.unpack_from('<iiI', payload)
        assert length <= len(payload) - 12, 'truncated LogLine message'
        message = payload[12:12 + length].decode('utf-8')
        result.append((level, rank, message))
    return result


CASE = {'schema': 'chipcae.case/1.0', 'stack': {'ref': 'stack.json'},
        'materials': {'ref': 'materials.json'},
        'mesh': {'type': 'aligned_cartesian_cut', 'nx': 6, 'ny': 6, 'levels': 1,
                 'min_elems_per_layer': 1, 'max_ratio': 10},
        'physics': [{'id': 'thermal', 'solver': 'fem.conduction', 'mode': 'steady'},
                    {'id': 'mech', 'solver': 'fem.solid', 'temp_from': 'thermal', 'T_ref': 300}],
        'loads': [{'type': 'fixed_temperature', 'target': 'face:zmin', 'T': 300},
                  {'type': 'convection', 'target': 'face:zmax', 'h': 50, 'T_inf': 400},
                  {'type': 'heat_source', 'target': 'layer:sig', 'q_vol': 2e8}],
        'constraints': {'rigid': '3-2-1'},
        'solver': {'linear': {'preconditioner': 'direct'}},
        'output': {'format': 'vtkhdf', 'fields': ['T', 'U']}}


def write_json(path, value):
    path.write_text(json.dumps(value), encoding='utf-8')


def synthetic(root):
    rings = [[(300, 400), (2700, 400), (2700, 1600), (300, 1600)],
             [(3500, 2300), (5200, 2300), (5200, 5900), (3500, 5900)],
             [(600, 3200), (2600, 3200), (600, 5600)]]
    data = b'CCPOLY01' + struct.pack('<dI', 1e-6, 1)
    data += struct.pack('<I', 3) + b'sig' + struct.pack('<I4q', 3, 0, 4, 8, 11)
    data += b''.join(struct.pack('<qq', *point) for ring in rings for point in ring)
    (root / 'copper.polyset').write_bytes(data)
    layers = [dict(name=name, kind=kind, material='fr4_eq', thickness=thickness,
                   n_elem_min=n, grading=1)
              for name, kind, thickness, n in [('core1', 'Explicit', .0004, 2),
                                               ('sig', 'Patterned', .0001, 1),
                                               ('core2', 'Explicit', .0004, 2)]]
    layers[1]['copper'] = dict(metal='cu', polygons=dict(file='copper.polyset', layer='sig'))
    write_json(root / 'stack.json', dict(id='lu-floor', unit='m',
               outline=dict(box=[0, 0, .006, .006]), symmetry='none', package_type='pcb',
               layers=layers, external_refs=[dict(path='copper.polyset',
                   sha256=hashlib.sha256(data).hexdigest())]))


def board(root):
    commands = [[importer, str(SOURCE / 'tests/integration/ecad/data/board_4layer.xml'),
                 '-o', 'stack.json', '--polyset', 'stack.polyset', '--report', 'defects.json',
                 '--material-map', str(SOURCE / 'tests/integration/f1130_uncut_cli/material_map.json')],
                [preview, 'stack.json', '-o', 'preview.vtkhdf', '--nx', '4', '--ny', '4',
                 '--levels', '0', '--max-ratio', '100']]
    for command in commands:
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, (command, result.stdout, result.stderr)
    assert (root / 'preview.vtkhdf').is_file()


def residuals(decoded):
    result = {}
    for kind, payload in decoded:
        if kind != 11:
            continue
        strings, offset = [], 0
        for _ in range(2):
            length, = struct.unpack_from('<I', payload, offset)
            offset += 4
            strings.append(payload[offset:offset + length].decode())
            offset += length
        window, iterations, value = struct.unpack_from('<QId', payload, offset)
        result[strings[1]] = (strings[0], value)
    return result


class LuRefineFloor(unittest.TestCase):
    def run_case(self, rtol, ranks=1, is_board=False, zero=False, subdivisions=None,
                 contrast=1300, load_scale=1, failing_rank=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            case = copy.deepcopy(CASE)
            if is_board:
                board(root)
                case['mesh'].update(nx=4, ny=4, levels=0, min_elems_per_layer=2, max_ratio=100)
                case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=298.15),
                                 dict(type='convection', target='face:zmax', h=1000, T_inf=348.15)]
                case['physics'][1]['T_ref'] = 298.15
            else:
                synthetic(root)
            if load_scale != 1:
                reference = case['physics'][1]['T_ref']
                for load in case['loads']:
                    if load['type'] == 'convection':
                        load['T_inf'] = reference + (load['T_inf'] - reference) * load_scale
                    elif load['type'] == 'heat_source':
                        load['q_vol'] *= load_scale
            if zero:
                case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=0)]
                case['physics'][1]['T_ref'] = 0
            if subdivisions is not None:
                case['mesh']['min_elems_per_layer'] = subdivisions
            if rtol is not None:
                case['solver']['linear']['rtol'] = rtol
            write_json(root / 'materials.json', {'schema': 'chipcae.material_library/1',
                'materials': [card('fr4_eq', 25e9, .3, 12e-6 if is_board else 1e-5, .3),
                              card('cu', 110e9, .34, 17e-6, .3 * contrast)]})
            write_json(root / 'case.json', case)
            if failing_rank is not None:
                failing_case = copy.deepcopy(case)
                failing_case['solver']['linear']['rtol'] = 1e-16
                write_json(root / 'failing_case.json', failing_case)
            write_json(root / 'profile.json', dict(schema='chipcae.run_profile/1.0',
                       ranks=ranks, backend='cpu', gpus=0))
            command = [exe, '--case', 'case.json', '--profile', 'profile.json', '--out', 'out']
            if failing_rank is not None:
                # MPMD keeps one communicator while only one rank rejects its solve.
                self.assertEqual(ranks, 2)
                self.assertIn(failing_rank, (0, 1))
                command = [mpirun]
                for rank in range(ranks):
                    if rank:
                        command.append(':')
                    case_file = 'failing_case.json' if rank == failing_rank else 'case.json'
                    command.extend(['-np', '1', exe, '--case', case_file,
                                    '--profile', 'profile.json', '--out', 'out'])
            elif ranks > 1:
                command = [mpirun, '-np', str(ranks), *command]
            result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                                    timeout=30 if failing_rank is not None else 180)
            path = root / 'out/events.bin'
            return result, events(path) if path.exists() else []

    def check_success(self, result, decoded, rtol, ranks=1, zero=False):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('LU refinement failed', result.stderr)
        timings = {}
        for level, rank, message in log_lines(decoded):
            self.assertNotIn('LU refinement failed', message)
            if ' assembly_seconds=' not in message:
                continue
            field = message.split()[0]
            values = dict(re.findall(r'(\w+)=([^ ]+)', message))
            for key in ('omega', 'eta', 'rel', 'refine_seconds'):
                self.assertIn(key, values, message)
                self.assertRegex(values[key], rf'^(?:{SCI})$')
            for key in ('omega', 'eta', 'assembly_seconds', 'PETSc_CSR_seconds',
                        'factor_seconds', 'LU_solve_seconds', 'refine_seconds'):
                self.assertTrue(math.isfinite(float(values[key])), message)
                self.assertGreaterEqual(float(values[key]), 0, message)
            self.assertLessEqual(float(values['omega']), rtol)
            self.assertIn(int(values['refine_iterations']), range(1, 9))
            if zero:
                self.assertEqual(float(values['omega']), 0.0)
                self.assertEqual(float(values['rel']), math.inf)
            timings[rank, field] = values
            print(f'measured rank={rank} {message}', flush=True)
        self.assertEqual(set(timings), {(rank, field) for rank in range(ranks) for field in ('T', 'U')})
        for field in ('T', 'U'):
            for rank in range(1, ranks):
                self.assertEqual(timings[rank, field]['omega'], timings[0, field]['omega'])
        residual = residuals(decoded)
        self.assertEqual(set(residual), {'T', 'U'})
        for field, (method, value) in residual.items():
            self.assertEqual(method, 'PETSc.LU')
            self.assertTrue(math.isfinite(value))
            self.assertGreaterEqual(value, 0)
            omega = float(timings[0, field]['omega'])
            self.assertLessEqual(omega, rtol)
            # Residual retains ||r||_2/max(||b||_2,1); rel is ||r||_2/||b||_2.
            if zero:
                self.assertEqual(value, 0.0)
            else:
                self.assertLessEqual(value, float(timings[0, field]['rel']) * 1.001)
                self.assertGreater(value, omega)

    def test_residual_event_keeps_relative_norm(self):
        # This load makes ||b||_2 > 1 for both systems, so the legacy event
        # equals rel. It must remain distinct from componentwise omega.
        for is_board in (False, True):
            with self.subTest(board=is_board):
                result, decoded = self.run_case(1e-12, load_scale=1e4, is_board=is_board)
                self.check_success(result, decoded, 1e-12)
                timings = {message.split()[0]: dict(re.findall(r'(\w+)=([^ ]+)', message))
                           for _, rank, message in log_lines(decoded)
                           if rank == 0 and ' assembly_seconds=' in message}
                for field, (_, value) in residuals(decoded).items():
                    self.assertEqual(f'{value:.3e}', timings[field]['rel'])
                    self.assertGreater(value, 10 * float(timings[field]['omega']))

    def test_trace_explicit_tolerance(self):
        self.check_success(*self.run_case(1e-14), 1e-14)

    def test_trace_default_tolerance(self):
        self.check_success(*self.run_case(None), 1e-8)

    def test_trace_mpi(self):
        self.check_success(*self.run_case(1e-14, ranks=2), 1e-14, ranks=2)

    def test_zero_load(self):
        self.check_success(*self.run_case(1e-14, zero=True), 1e-14, zero=True)

    def test_board_kz2(self):
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                self.check_success(*self.run_case(1e-12, ranks=ranks, is_board=True), 1e-12, ranks)

    def test_parameter_range(self):
        for is_board in (False, True):
            for subdivisions in (1, 2, 4):
                for contrast in (1300, 1):
                    with self.subTest(board=is_board, subdivisions=subdivisions, contrast=contrast):
                        self.check_success(*self.run_case(1e-12, is_board=is_board,
                            subdivisions=subdivisions, contrast=contrast), 1e-12)

    def test_failure_diagnostic(self):
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                self.check_failure(*self.run_case(1e-16, ranks=ranks))

    def test_mpi_rank_local_failure(self):
        for failing_rank in (0, 1):
            with self.subTest(failing_rank=failing_rank):
                result, decoded = self.run_case(1e-14, ranks=2, failing_rank=failing_rank)
                self.check_failure(result, decoded)
                self.assertFalse(any(' assembly_seconds=' in message
                                     for _, _, message in log_lines(decoded)))

    def check_failure(self, result, decoded):
        self.assertEqual(result.returncode, 1, result.stderr)
        prefix = 'chipcae-solve: '
        messages = [line[len(prefix):] for line in result.stderr.splitlines() if line.startswith(prefix)]
        self.assertEqual(len(messages), 1, result.stderr)
        failures = [(level, rank, message) for level, rank, message in log_lines(decoded) if level == 3]
        self.assertEqual(failures, [(3, 0, messages[0])])
        match = FAILURE.fullmatch(messages[0])
        self.assertIsNotNone(match, f'wrong LU refinement diagnostic: {messages[0]!r}')
        omega, rtol, eta, rel, iterations, refine_seconds, field = match.groups()
        self.assertTrue(math.isfinite(float(omega)))
        self.assertGreater(float(omega), 1e-16)
        self.assertEqual(rtol, '1.000e-16')
        self.assertTrue(math.isfinite(float(eta)))
        self.assertTrue(math.isfinite(float(rel)))
        self.assertIn(int(iterations), range(1, 9))
        self.assertTrue(math.isfinite(float(refine_seconds)))
        self.assertGreaterEqual(float(refine_seconds), 0)
        self.assertEqual(field, 'T')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
