"""Public FVM case/1.0 acceptance through the loader, CLI and results writer."""

from copy import deepcopy
import json
import math
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

import fvm_case_util as u

SOURCE = Path(__file__).resolve().parents[3]


def snapshot_cases():
    default = u.thermal_case('fem')
    default.pop('solver')
    custom = u.thermal_case('fem')
    custom['mesh'].update(nx=3, ny=5, levels=2, cell_budget=12345,
                          cut=dict(quad_degree=4, gamma_g=.25, stabilization='ghost_penalty'))
    custom['constraints'] = dict(fixed=[dict(target='face:xmin', id='clamp')])
    custom['output']['fields'] = ['U', 'T', 'elem_gid']
    custom['solver']['linear'].update(rtol=2e-9, max_iter=321, preconditioner='column_gmg')
    sources = u.thermal_case('fem')
    sources['loads'] = [dict(type='fixed_temperature', target='layer:a', T=350, id='pin'),
                        dict(type='convection', target='layer:b', h=20, T_inf=300, id='cool'),
                        dict(type='heat_source', target='layer:a', power=7, id='power'),
                        dict(type='heat_source', target='layer:b', q_vol=3, id='density')]
    return dict(default=default, custom=custom, sources=sources, solid=u.solid_case('fem'),
                copper=u.thermal_case('fem'))


def log_record(text, level=1, rank=0, seq=1):
    """Self-generated binary fixture, independent of the production event writer."""
    def string(s):
        data = s.encode('utf-8')
        return struct.pack('<I', len(data)) + data
    body = struct.pack('<QqBii', seq, 0, 20, level, rank)
    body += string(text) + string('') + struct.pack('<I', 0)
    return b'CAELOG1\0' + struct.pack('<II', len(body), zlib.crc32(body)) + body


class DecoderAcceptance(unittest.TestCase):
    # A permissive decoder could make a broken energy publisher pass the numerical gate.
    def test_corrupt_duplicate_missing_or_invalid_energy_is_rejected(self):
        valid = 'FVM_ENERGY {"heat_in":1,"heat_out":1,"imbalance":0,"iterations":2}'
        raw = log_record(valid)
        self.assertEqual(u.energy(u.decode_events(raw))['iterations'], 2)
        zero = 'FVM_ENERGY {"heat_in":0,"heat_out":0,"imbalance":0,"iterations":0}'
        self.assertAlmostEqual(u.energy(u.decode_events(log_record(zero)))['imbalance'], 0)
        invalid = [
            log_record('unrelated'), raw + log_record(valid, seq=2),
            log_record(valid, level=2), log_record(valid, rank=1),
            log_record(valid.replace('"heat_in":1', '"heat_in":1,"heat_in":1')),
            log_record(valid.replace('"iterations":2', '"iterations":2,"extra":0')),
            log_record(valid.replace('"iterations":2', '"iterations":2.0')),
            log_record(valid.replace('"iterations":2', '"iterations":true')),
            log_record(valid.replace('"iterations":2', '"iterations":-1')),
            log_record(valid.replace('"heat_in":1', '"heat_in":true')),
            log_record(valid.replace('"heat_in":1', '"heat_in":-1')),
            log_record(valid.replace('"heat_in":1', '"heat_in":NaN')),
            log_record(valid.replace('"heat_in":1', '"heat_in":1e999')),
            log_record(valid.replace('"imbalance":0', '"imbalance":1e-8')),
            log_record(valid.replace('"heat_out":1', '"heat_out":0')),
            log_record(valid + ' trailing'), log_record(valid + '\n'),
            raw[:-1], raw[:8] + b'\0\0\0\0' + raw[12:],
            raw[:-1] + bytes([raw[-1] ^ 1]), b'BADLOG1\0' + raw[8:], raw + raw,
        ]
        for index, data in enumerate(invalid):
            with self.subTest(index=index), self.assertRaises(AssertionError):
                u.energy(u.decode_events(data))

    def test_partition_local_connectivity_alignment(self):
        import numpy as np
        # Two cells sharing coordinates but stored in reversed rank order.
        def block(reverse):
            coordinates = np.array([[0., 0., 0.], [1., 0., 0.],
                                    [1., 1., 0.], [0., 1., 0.],
                                    [0., 0., 1.], [1., 0., 1.],
                                    [1., 1., 1.], [0., 1., 1.]])
            chunks = [(9, coordinates, 310.), (21, coordinates + [1, 0, 0], 320.)]
            if reverse:
                chunks.reverse()
            return {'NumberOfPoints': np.array([8, 8]), 'NumberOfCells': np.array([1, 1]),
                    'NumberOfConnectivityIds': np.array([8, 8]),
                    'Offsets': np.array([0, 8, 0, 8]), 'Connectivity': np.tile(np.arange(8), 2),
                    'Types': np.array([12, 12]), 'Points': np.concatenate([c[1] for c in chunks]),
                    'CellData/elem_gid': np.array([c[0] for c in chunks]),
                    'PointData/T': np.concatenate([np.full(8, c[2]) for c in chunks])}
        a, b = block(False), block(True)
        u.compare_parallel(dict(hex=a, cut=a), dict(hex=b, cut=b))
        b['PointData/T'][0] += .01
        with self.assertRaises(AssertionError):
            u.compare_parallel(dict(hex=a, cut=a), dict(hex=b, cut=b))


class CaseAcceptance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fvm-case-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stack = u.fixture(self.root)

    def successful_thermal(self, case, name, ranks=1, extra=()):
        out, _ = u.solve(self.root, case, name, ranks, extra)
        manifest, blocks = u.read_result(out)
        ledger, events = u.inspect_fvm(out, blocks, manifest)
        return manifest, blocks, ledger, events

    def test_fem_public_inputs_match_prechange_snapshots(self):
        for name, case in snapshot_cases().items():
            with self.subTest(case=name):
                u.snapshot_fixture(self.root, name)
                got, _ = u.probe(self.root, case)
                got = u.normalize_paths(got, self.root)
                family = got.pop('family')
                # frozen at upstream merge 75878ee (origin/main c9c1464);
                # FEM diffs vs e181d83 come from #212 / solid-k parsing upstream
                self.assertEqual(got, u.FEM_SNAPSHOTS[name])
                self.assertEqual(family, 'fem')

    def test_fem_reference_errors_precede_malformed_physics(self):
        # Early family discovery must not reorder legacy FEM validation errors.
        for malformed in ('missing_solver', 'non_array'):
            with self.subTest(physics=malformed):
                case = u.thermal_case('fem')
                case['stack']['ref'] = 'missing-stack.json'
                if malformed == 'missing_solver':
                    case['physics'][0].pop('solver')
                else:
                    case['physics'] = {}
                _, result = u.probe(self.root, case, expected=12)
                self.assertIn('cannot read reference:', result.stderr)
                self.assertIn('missing-stack.json', result.stderr)

    def test_volume_output_field_is_fvm_only(self):
        import h5py
        for family in ('fem', 'fvm'):
            with self.subTest(family=family):
                case = u.thermal_case(family)
                case['output']['fields'] = ['T', 'volume']
                if family == 'fem':
                    _, result = u.probe(self.root, case, expected=14)
                    self.assertEqual(result.stderr.strip(), 'invalid output field')
                else:
                    got, _ = u.probe(self.root, case)
                    self.assertEqual(got['fields'], ['T', 'volume'])
                    out, _ = u.solve(self.root, case, 'explicit-volume')
                    with h5py.File(out / 'out/step-0.vtkhdf', 'r') as result:
                        for block in ('hex', 'cut'):
                            self.assertIn('volume', result[f'VTKHDF/{block}/CellData'])

    def test_invalid_physics_cannot_select_fvm_before_legacy_validation(self):
        fem = u.thermal_case('fem')['physics']
        fvm = u.thermal_case()['physics']
        variants = [
            fem + [dict(id='unknown', solver='fvm.solid')],
            fem + [dict(id='thermal', solver='fvm.conduction', mode='steady')],
            [dict(id='thermal', solver='fvm.solid', mode='steady')],
            [dict(id='thermal', solver='fvm.conduction', mode='transient')],
            [fvm[0], dict(id='mech')],
            [fvm[0], dict(id='mech', solver=42)],
            [fvm[0], dict(id='mech', solver='fvm.solid', temp_from='unknown', T_ref=300)],
            [fvm[0], dict(id='mech', solver='fvm.solid', temp_from='thermal', T_ref='bad')],
        ]
        for index, physics in enumerate(variants):
            for earlier in ('reference', 'gamma_g', 'stabilization'):
                with self.subTest(physics=index, earlier=earlier):
                    case = u.thermal_case('fem')
                    case['physics'] = deepcopy(physics)
                    if earlier == 'reference':
                        case['stack']['ref'] = 'missing-stack.json'
                        _, result = u.probe(self.root, case, expected=12)
                        self.assertIn('cannot read reference:', result.stderr)
                        self.assertIn('missing-stack.json', result.stderr)
                    else:
                        case['mesh']['cut'][earlier] = -1 if earlier == 'gamma_g' else 'invalid'
                        _, result = u.probe(self.root, case, expected=14)
                        self.assertEqual(result.stderr.strip(), 'gamma_g must be nonnegative'
                                         if earlier == 'gamma_g' else 'unsupported cut stabilization')

    def test_fvm_selection_orders_defaults_and_preconditioners(self):
        for reverse in (False, True):
            for mode in ('no_solver', 'no_linear', 'no_pc', 'jacobi', 'gamg'):
                with self.subTest(reverse=reverse, mode=mode):
                    case = u.thermal_case()
                    if reverse:
                        case['physics'].reverse()
                    if mode == 'no_solver':
                        case.pop('solver')
                    elif mode == 'no_linear':
                        case['solver'] = {}
                    elif mode == 'no_pc':
                        case['solver']['linear'].pop('preconditioner')
                    else:
                        case['solver']['linear']['preconditioner'] = mode
                    got, _ = u.probe(self.root, case)
                    self.assertEqual(got['family'], 'fvm')
                    self.assertTrue(got['thermal'])
                    self.assertEqual(got['linear']['preconditioner'],
                                     'gamg' if mode == 'gamg' else 'jacobi')
        # Both accepted preconditioners must reach and complete the numerical backend.
        for pc in ('jacobi', 'gamg'):
            with self.subTest(solve_preconditioner=pc):
                case = u.thermal_case()
                case['solver']['linear']['preconditioner'] = pc
                self.successful_thermal(case, pc)

    def test_invalid_selection_diagnostics_through_public_cli(self):
        cases = []
        for first, second in (('fem.conduction', 'fvm.solid'),
                              ('fvm.conduction', 'fem.solid')):
            case = u.thermal_case()
            case['physics'][0]['solver'], case['physics'][1]['solver'] = first, second
            cases.append(('mixed-' + first, case,
                          'mixed fem/fvm physics not supported in case/1.0'))
        for pc in ('direct', 'column_gmg'):
            case = u.thermal_case()
            case['solver']['linear']['preconditioner'] = pc
            cases.append((pc, case, 'invalid preconditioner'))
        for solver in ('fvm.xxx', 'FVM.conduction'):
            case = u.thermal_case()
            case['physics'][0]['solver'] = solver
            cases.append((solver, case, 'invalid or duplicate physics'))
        case = u.thermal_case()
        case['physics'].append(deepcopy(case['physics'][0]))
        cases.append(('duplicate-thermal', case, 'invalid or duplicate physics'))
        case = u.thermal_case()
        case['physics'].append(deepcopy(case['physics'][1]))
        cases.append(('duplicate-mech', case, 'invalid or duplicate physics'))
        case = u.thermal_case()
        case['physics'][0]['mode'] = 'transient'
        cases.append(('mode', case, 'invalid or duplicate physics'))
        for name, case, reason in cases:
            with self.subTest(case=name):
                _, result = u.solve(self.root, case, name, expected=14)
                self.assertEqual(result.stderr.strip(), 'chipcae-solve: ' + reason)

    def test_temperature_source_rules_and_solid_uniform_output(self):
        import numpy as np
        for family in ('fem', 'fvm'):
            variants = []
            case = u.solid_case(family)
            case['loads'] = []
            variants.append(case)
            case = u.solid_case(family)
            case['loads'] *= 2
            variants.append(case)
            case = u.solid_case(family)
            case['physics'][0].update(temp_from='thermal', T_ref=300)
            variants.append(case)
            case = u.thermal_case(family)
            case['loads'].append(dict(type='uniform_temperature', T_ref=300, T_target=350))
            variants.append(case)
            case = u.thermal_case(family)
            case['physics'][1].pop('temp_from')
            variants.append(case)
            case = u.thermal_case(family)
            case['loads'] = [dict(type='heat_source', target='layer:a', power=1)]
            variants.append(case)
            for index, case in enumerate(variants):
                with self.subTest(family=family, source=index):
                    _, result = u.probe(self.root, case, expected=14)
                    self.assertEqual(result.stderr.strip(), 'inconsistent temperature sources')
            for invalid_source in (
                    dict(type='heat_source', target='layer:a', power=1, q_vol=1),
                    dict(type='heat_source', target='layer:a'),
                    dict(type='heat_source', target='face:zmin', power=1)):
                with self.subTest(family=family, heat_source=invalid_source):
                    case = u.thermal_case(family)
                    case['loads'].append(invalid_source)
                    _, result = u.probe(self.root, case, expected=14)
                    self.assertEqual(result.stderr.strip(), 'invalid heat source')
        with self.subTest(solid_uniform=True):
            out, _ = u.solve(self.root, u.solid_case(), 'uniform')
            manifest, blocks = u.read_result(out)
            self.assertAlmostEqual(u.summary(manifest)['T_max'], 350, delta=1e-10)
            for data in blocks.values():
                self.assertTrue(np.allclose(data['PointData/T'], 350, rtol=0, atol=1e-10))
                self.assertTrue(np.allclose(data['PointData/U'], 0, rtol=0, atol=1e-15))
            summaries = {x['name']: x for x in manifest['summary']}
            self.assertEqual(summaries['T_max']['method'], 'FVM cell centers')
            for key in ('warpage_pv', 'U_mag_max'):
                self.assertAlmostEqual(summaries[key]['value'], 0, delta=1e-15)
                self.assertEqual(summaries[key]['method'], u.THERMAL_ONLY)
            events = u.decode_events((out / 'events.bin').read_bytes())
            self.assertTrue(any(e['kind'] == 20 and e['level'] == 2 and
                                e['text'] == u.THERMAL_ONLY for e in events))

    def test_fvm_ignores_fem_stabilization_retains_quadrature_and_constraints(self):
        for family, expected in (('fem', 14), ('fvm', 0)):
            with self.subTest(family=family):
                case = u.thermal_case(family)
                case['mesh']['cut'] = dict(stabilization='not-a-fem-stabilization', gamma_g=-2,
                                           quad_degree=4)
                got, result = u.probe(self.root, case, expected=expected)
                if expected == 0:
                    self.assertEqual(got['cut_options']['quad_degree'], 4)
                else:
                    self.assertEqual(result.stderr.strip(), 'gamma_g must be nonnegative')
        for family in ('fem', 'fvm'):
            with self.subTest(family=family, invalid_quadrature=True):
                case = u.thermal_case(family)
                case['mesh']['cut']['quad_degree'] = 3
                _, result = u.probe(self.root, case, expected=14)
                self.assertEqual(result.stderr.strip(), 'cut quad_degree must be 2, 4, 6 or 8')
        for target_type in ('load', 'constraint'):
            with self.subTest(embedded=target_type):
                case = u.thermal_case()
                if target_type == 'load':
                    case['loads'][0]['target'] = 'boundary:embedded'
                else:
                    case['constraints'] = dict(fixed=[dict(target='boundary:embedded')])
                _, result = u.probe(self.root, case, expected=13)
                self.assertIn('embedded fixed ' + ('temperature' if target_type == 'load'
                                                  else 'displacement') + ' is not supported',
                              result.stderr)

    def test_fvm_cases_validate_against_case_schema(self):
        try:
            import jsonschema
        except ImportError as error:
            self.skipTest(f'only schema validation skipped: jsonschema unavailable: {error}')
        from referencing import Registry, Resource
        schema = u.read_json(SOURCE / 'schema/case-1.0.schema.json')
        parameters = u.read_json(SOURCE / 'schema/parameters.schema.json')
        registry = Registry().with_resource(parameters['$id'], Resource.from_contents(parameters))
        validator = jsonschema.validators.validator_for(schema)(schema, registry=registry)
        explicit_volume = u.thermal_case()
        explicit_volume['output']['fields'] = ['T', 'volume']
        for case in (u.thermal_case(), u.solid_case(), explicit_volume,
                     u.thermal_case('fem'), u.solid_case('fem')):
            family = case['physics'][0]['solver'].split('.')[0]
            accepted = ('jacobi', 'gamg') if family == 'fvm' else ('direct', 'column_gmg')
            for reverse, pc in ((reverse, pc) for reverse in (False, True)
                                for pc in (*accepted, None)):
                with self.subTest(physics=case['physics'], reverse=reverse, preconditioner=pc):
                    candidate = deepcopy(case)
                    if pc is None:
                        candidate['solver']['linear'].pop('preconditioner')
                    else:
                        candidate['solver']['linear']['preconditioner'] = pc
                    if reverse:
                        candidate['physics'].reverse()
                    self.assertIn('warpage', candidate['output'])
                    errors = list(validator.iter_errors(candidate))
                    self.assertFalse(errors, '\n'.join(e.message for e in errors))
                    got, _ = u.probe(self.root, candidate)
                    default_pc = 'jacobi' if family == 'fvm' else 'column_gmg'
                    self.assertEqual(got['linear']['preconditioner'], pc or default_pc)
            rejected = ('direct', 'column_gmg', 'unknown') if family == 'fvm' else (
                'jacobi', 'gamg', 'unknown')
            for pc in rejected:
                with self.subTest(family=family, rejected_preconditioner=pc):
                    candidate = deepcopy(case)
                    candidate['solver']['linear']['preconditioner'] = pc
                    self.assertTrue(list(validator.iter_errors(candidate)))
            for field, value in (('rtol', 0), ('max_iter', 0), ('unknown', True)):
                with self.subTest(family=family, invalid_linear_field=field):
                    candidate = deepcopy(case)
                    candidate['solver']['linear'][field] = value
                    self.assertTrue(list(validator.iter_errors(candidate)))

    def test_layer_fixed_temperatures_allow_roundoff_but_reject_conflicts(self):
        import numpy as np
        cases = ((0., 5e-13), (300., math.nextafter(300., math.inf)),
                 (300., math.nextafter(300., -math.inf)), (300., 300. + 1e-10))
        for index, (temperature, duplicate) in enumerate(cases):
            for ranks in (1, 2):
                with self.subTest(temperature=temperature, duplicate=duplicate, ranks=ranks):
                    case = u.thermal_case()
                    case['loads'] = [
                        dict(type='fixed_temperature', target='layer:a', T=temperature),
                        dict(type='fixed_temperature', target='layer:a', T=duplicate),
                        dict(type='convection', target='face:zmax', h=20,
                             T_inf=temperature + 50)]
                    _, blocks, ledger, _ = self.successful_thermal(
                        case, f'fixed-roundoff-{index}-{ranks}', ranks)
                    # The first prescription is retained. R = .025/2 + .05/4 + 1/20.
                    self.assertAlmostEqual(ledger['heat_in'], 50 / .075, delta=1e-7)
                    self.assertAlmostEqual(ledger['heat_out'], 50 / .075, delta=1e-7)
                    self.assertTrue(np.all(np.isfinite(blocks['hex']['PointData/T'])))
        for index, (temperature, duplicate) in enumerate(((0., 2e-12),
                                                         (300., 300. + 1e-8),
                                                         (300., 350.))):
            with self.subTest(conflict=(temperature, duplicate)):
                case = u.thermal_case()
                case['loads'] = [dict(type='fixed_temperature', target='layer:a', T=value)
                                 for value in (temperature, duplicate)]
                out, result = u.solve(self.root, case, f'fixed-conflict-{index}', expected=14)
                self.assertIn('conflicting fixed CV', result.stderr)
                self.assertFalse((out / 'out/step-0.vtkhdf').exists())

    def test_nonconvergence_never_publishes_a_successful_step(self):
        case = u.thermal_case()
        case['solver']['linear'].update(max_iter=1, rtol=1e-14)
        u.probe(self.root, case)  # Establish that input validation succeeded first.
        out, result = u.solve(self.root, case, 'nonconverged', expected=None)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((out / 'out/step-0.vtkhdf').exists())
        if (out / 'manifest.json').exists():
            self.assertEqual(u.read_json(out / 'manifest.json')['steps'], [])
        events = u.decode_events((out / 'events.bin').read_bytes())
        self.assertFalse(any(e['kind'] == 22 for e in events))
        self.assertEqual(events[-1]['kind'], 19)
        self.assertNotEqual(events[-1]['exit_code'], 0)

    def test_zero_energy_sources_and_layer_fixed_reactions(self):
        import numpy as np
        with self.subTest(zero=True):
            case = u.thermal_case()
            case['loads'][1]['T_inf'] = 300
            _, blocks, ledger, _ = self.successful_thermal(case, 'zero')
            for key in ('heat_in', 'heat_out', 'imbalance'):
                self.assertAlmostEqual(ledger[key], 0, delta=1e-12)
            self.assertTrue(np.allclose(blocks['hex']['PointData/T'], 300, rtol=0, atol=1e-10))
        results = []
        for source in ('power', 'q_vol'):
            with self.subTest(source=source):
                case = u.thermal_case()
                case['loads'][1]['T_inf'] = 300
                # Layer a volume = 1 * 1 * .05; densities add, including repeated targets.
                for value in ((10, 5) if source == 'power' else (200, 100)):
                    case['loads'].append(dict(type='heat_source', target='layer:a', **{source: value}))
                manifest, blocks, ledger, _ = self.successful_thermal(case, source)
                self.assertAlmostEqual(ledger['heat_in'], 15, delta=1e-9)
                self.assertAlmostEqual(ledger['heat_out'], 15, delta=1e-9)
                results.append((manifest, blocks))
        if len(results) == 2:
            u.compare_parallel(results[0][1], results[1][1])
            self.assertAlmostEqual(u.summary(results[0][0])['T_max'],
                                   u.summary(results[1][0])['T_max'], delta=1e-10)
        with self.subTest(fixed_cv=True):
            case = u.thermal_case()
            case['loads'] = [dict(type='fixed_temperature', target='layer:a', T=350),
                            dict(type='fixed_temperature', target='layer:a', T=350),
                            dict(type='convection', target='face:zmax', h=20, T_inf=300),
                            dict(type='heat_source', target='layer:a', power=7)]
            _, _, ledger, _ = self.successful_thermal(case, 'layer-fixed')
            # R from fixed a center to top ambient: .025/2 + .05/4 + 1/20 = .075.
            self.assertAlmostEqual(ledger['heat_in'], 50 / .075, delta=1e-7)
            self.assertAlmostEqual(ledger['heat_out'], 50 / .075, delta=1e-7)

    def test_layer_convection_matches_exterior_faces_including_embedded_and_split(self):
        for geometry in ('uncut', 'cut', 'split-cut'):
            with self.subTest(geometry=geometry):
                stack = deepcopy(self.stack)
                stack['layers'] = [deepcopy(stack['layers'][0])]
                if geometry != 'uncut':
                    stack['outline']['polygon'] = [[.6, 0], [1, 0], [1, 1], [0, 1], [0, .6]]
                if geometry == 'split-cut':
                    stack['layers'] *= 2
                    stack['layers'] = [dict(layer, name=f'a#{i}')
                                       for i, layer in enumerate(stack['layers'])]
                u.write_json(self.root / 'stack.json', stack)
                case = u.thermal_case()
                case['loads'] = [dict(type='fixed_temperature', target='layer:a', T=350)]
                targets = ['face:' + side for side in ('xmin', 'xmax', 'ymin', 'ymax', 'zmin', 'zmax')]
                if geometry != 'uncut':
                    targets.append('boundary:embedded')
                explicit = deepcopy(case)
                explicit['loads'] += [dict(type='convection', target=target, h=20, T_inf=300)
                                      for target in targets]
                _, expected_blocks, expected_ledger, _ = self.successful_thermal(
                    explicit, geometry + '-explicit')
                self.assertGreater(expected_ledger['heat_out'], 0)
                case['loads'].append(dict(type='convection', target='layer:a', h=20, T_inf=300))
                for ranks in (1, 2):
                    with self.subTest(ranks=ranks):
                        _, blocks, ledger, _ = self.successful_thermal(
                            case, f'{geometry}-layer{ranks}', ranks)
                        u.compare_parallel(expected_blocks, blocks)
                        for key in ('heat_in', 'heat_out'):
                            self.assertAlmostEqual(ledger[key], expected_ledger[key], delta=1e-9)

    def test_board_import_preview_fvm_fem_gate_and_mpi(self):
        import numpy as np
        stack, preview, originals = u.import_board(self.root, SOURCE)
        case = u.thermal_case()
        case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=298.15),
                         dict(type='convection', target='face:zmax', h=1000, T_inf=348.15)]
        case['physics'][1]['T_ref'] = 298.15
        # No fraction converter, matrix replacement, or polyset rewrite is permitted here.
        with self.subTest(loader_polygons=True):
            got, _ = u.probe(self.root, case)
            self.assertEqual(got['family'], 'fvm')
        results = {}
        for family, ranks in (('fvm', 1), ('fvm', 2), ('fem', 1)):
            with self.subTest(family=family, ranks=ranks):
                candidate = deepcopy(case)
                for p in candidate['physics']:
                    p['solver'] = family + ('.conduction' if p['id'] == 'thermal' else '.solid')
                candidate['solver']['linear']['preconditioner'] = 'direct' if family == 'fem' else 'jacobi'
                out, _ = u.solve(self.root, candidate, family + str(ranks), ranks)
                manifest, blocks = u.read_result(out, require_volume=family == 'fvm')
                self.assertEqual(int(blocks['hex']['NumberOfCells'].sum()), preview['cell_count'])
                self.assertEqual(int(blocks['cut']['NumberOfCells'].sum()), 0)
                self.assertTrue(np.all(blocks['hex']['CellData/cell_kind'] == 0))
                for field in ('nx0', 'ny0', 'L', 'nz', 'padded_cells', 'mesh_sha256'):
                    self.assertEqual(manifest['mesh'][field], preview[field])
                if family == 'fvm':
                    ledger, _ = u.inspect_fvm(out, blocks, manifest)
                    print(f'实测 board energy np={ranks} imbalance={ledger["imbalance"]:.17g} '
                          f'iterations={ledger["iterations"]}', flush=True)
                results[family, ranks] = manifest, blocks
        for name, content in originals.items():
            self.assertEqual((self.root / name).read_bytes(), content)
        self.assertEqual(len(stack['layers']), 5)
        self.assertEqual(set(results), {('fvm', 1), ('fvm', 2), ('fem', 1)})
        u.compare_parallel(results['fvm', 1][1], results['fvm', 2][1])
        fvm = float(results['fvm', 1][1]['hex']['PointData/T'].max())
        fem = float(results['fem', 1][1]['hex']['PointData/T'].max())
        self.assertGreater(fem - 298.15, 0)
        error = abs(fvm - fem) / (fem - 298.15)
        fvm_summary = u.summary(results['fvm', 1][0])['T_max']
        fem_summary = u.summary(results['fem', 1][0])['T_max']
        print(f'实测 board vertex_Tmax fvm={fvm:.17g} fem={fem:.17g} '
              f'summary_Tmax fvm={fvm_summary:.17g} fem={fem_summary:.17g} '
              f'rise_relative={error:.17g} absolute_T_relative={abs(fvm-fem)/abs(fem):.17g}', flush=True)
        self.assertLessEqual(error, .01)

    def test_wiener_nonzero_bottom_rise(self):
        # Source-free top Dirichlet/bottom Robin gives a meaningful nonzero bottom rise.
        # A separate half-covered layer has exact, independent conductance bounds,
        # avoiding any assumptions about the board importer's matrix semantics.
        originals = u.wiener_fixture(self.root)
        case = u.thermal_case()
        case['loads'] = [dict(type='fixed_temperature', target='face:zmax', T=400),
                         dict(type='convection', target='face:zmin', h=20, T_inf=300)]
        _, _, ledger, _ = self.successful_thermal(case, 'wiener')
        # The only outflow is the bottom Robin face, area 1 m². Its physical
        # mean surface rise is heat_out/(h*A). This avoids conflating library
        # vertex averaging at adiabatic corners with the actual Robin surface T.
        rise = ledger['heat_out'] / 20
        # Half the lower layer has k=400, half k=2; upper layer k=4.
        # R includes the Robin film. Independent columns give the lower conductance;
        # an isothermal interface/layer-average parallel conductance gives the upper.
        f, h, delta, thickness, rest = .5, 20., 100., .05, .05/4
        lower = delta / h * ((1-f) / (rest + thickness/2 + 1/h)
                            + f / (rest + thickness/400 + 1/h))
        upper = delta / h / (rest + thickness/((1-f)*2+f*400) + 1/h)
        print(f'实测 Wiener bottom mean rise={rise:.17g}, bounds=[{lower:.17g},{upper:.17g}]')
        self.assertGreater(rise, 0)
        self.assertGreaterEqual(rise, lower - 1e-9 * abs(lower))
        self.assertLessEqual(rise, upper + 1e-9 * abs(upper))
        for name, content in originals.items():
            self.assertEqual((self.root / name).read_bytes(), content)

    def test_demo_gate_original_fvm_hex_and_embedded_robin(self):
        import numpy as np
        original = u.copy_demo(self.root, SOURCE)
        results = {}
        # The regular and cut thermal libraries are present in this baseline: no stage-B skip.
        for family in ('fem', 'fvm'):
            with self.subTest(family=family):
                case = deepcopy(original)
                if family == 'fvm':
                    for p in case['physics']:
                        p['solver'] = p['solver'].replace('fem.', 'fvm.')
                    case['solver']['linear']['preconditioner'] = 'jacobi'
                # Only the two solver names and preconditioner differ from DEMO-GATE.
                out, _ = u.solve(self.root, case, 'demo-' + family)
                manifest, blocks = u.read_result(out, require_volume=family == 'fvm')
                self.assertGreater(int(blocks['cut']['NumberOfCells'].sum()), 0)
                if family == 'fvm':
                    u.inspect_fvm(out, blocks, manifest)
                results[family] = manifest, blocks
        if set(results) == {'fem', 'fvm'}:
            fem, fvm = (u.summary(results[k][0])['T_max'] for k in ('fem', 'fvm'))
            self.assertGreater(fem - 298.15, 0)
            error = abs(fvm - fem) / (fem - 298.15)
            print(f'实测 DEMO-GATE Tmax fvm={fvm:.17g} fem={fem:.17g} rise_relative={error:.17g}')
            self.assertLessEqual(error, .01)
        case = deepcopy(original)
        for p in case['physics']:
            p['solver'] = p['solver'].replace('fem.', 'fvm.')
        case['solver']['linear']['preconditioner'] = 'jacobi'
        with self.subTest(cut_output='hex'):
            manifest, blocks, _, _ = self.successful_thermal(case, 'demo-hex', extra=('--cut-output', 'hex'))
            self.assertEqual(int(blocks['cut']['NumberOfCells'].sum()), 0)
            self.assertGreater(int(blocks['eb_surface']['Polygons/NumberOfCells'].sum()), 0)
            self.assertTrue(np.any(blocks['hex']['CellData/cell_kind'] == 1))
            self.assertEqual(len(set(blocks['hex']['CellData/elem_gid'])), len(blocks['hex']['Types']))
            if 'fvm' in results:
                eb, expected = blocks['eb_surface'], results['fvm'][1]['eb_surface']
                np.testing.assert_allclose(eb['Points'], expected['Points'], rtol=0, atol=1e-14)
                np.testing.assert_allclose(eb['PointData/T'], expected['PointData/T'], rtol=1e-12, atol=1e-10)
                base = results['fvm'][1]
                parent_gids = set(base['hex']['CellData/elem_gid']) | set(base['cut']['CellData/elem_gid'])
                self.assertEqual(set(blocks['hex']['CellData/elem_gid']), parent_gids)
                cut_gids = set(blocks['hex']['CellData/elem_gid'][blocks['hex']['CellData/cell_kind'] == 1])
                self.assertEqual(cut_gids, set(base['cut']['CellData/elem_gid']))
        with self.subTest(embedded_robin=True):
            case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=298.15),
                             dict(type='convection', target='boundary:embedded', h=500, T_inf=348.15)]
            case['solver']['linear']['rtol'] = 1e-12
            _, blocks, ledger, _ = self.successful_thermal(case, 'demo-embedded')
            print(f'实测 DEMO embedded_robin imbalance={ledger["imbalance"]:.17g} '
                  f'iterations={ledger["iterations"]}', flush=True)
            self.assertGreater(ledger['heat_in'], 0)
            self.assertGreater(ledger['heat_out'], 0)
            self.assertGreater(int(blocks['eb_surface']['Polygons/NumberOfCells'].sum()), 0)

    def test_cut_point_reconstruction_physical_volume_and_parallel_ownership(self):
        import numpy as np
        # Remove a right triangle of area .6*.6/2 from the unit square.
        # Whole void parents, partial parents and regular hexes all occur.
        self.stack['outline']['polygon'] = [[.6, 0], [1, 0], [1, 1], [0, 1], [0, .6]]
        u.write_json(self.root / 'stack.json', self.stack)
        library = u.read_json(self.root / 'materials.json')
        for card in library['materials']:
            if card['id'] in ('a', 'b'):
                card['conductivity'] = 2
        u.write_json(self.root / 'materials.json', library)
        case = u.thermal_case()
        case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=300),
                         dict(type='fixed_temperature', target='face:zmax', T=400)]
        results = {}
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                manifest, blocks, _, _ = self.successful_thermal(case, f'cut-linear{ranks}', ranks)
                self.assertGreater(int(blocks['cut']['NumberOfCells'].sum()), 0)
                self.assertGreater(int(blocks['eb_surface']['Polygons/NumberOfCells'].sum()), 0)
                self.assertAlmostEqual(u.summary(manifest)['T_max'], 375, delta=1e-8)
                physical_volume = sum(float(blocks[name]['CellData/volume'].sum())
                                      for name in ('hex', 'cut'))
                self.assertAlmostEqual(physical_volume, .082, delta=1e-12)
                for name in ('cut', 'eb_surface'):
                    points = blocks[name]['Points']
                    expected = 300 + 1000 * points[:, 2]
                    np.testing.assert_allclose(blocks[name]['PointData/T'], expected,
                                               rtol=0, atol=1e-8, err_msg=name)
                for name in ('hex', 'cut'):
                    for index, (gid, coordinates) in enumerate(u.canonical_cells(blocks[name])):
                        self.assertGreaterEqual(gid, 0)
                        center = np.mean(np.asarray(coordinates), axis=0)
                        self.assertGreaterEqual(center[0] + center[1], .6 - 1e-12)
                        expected_layer = 1 if center[2] < .05 else 2
                        self.assertEqual(int(blocks[name]['CellData/layer_id'][index]), expected_layer)
                        self.assertEqual(int(blocks[name]['CellData/material_id'][index]), expected_layer)
                results[ranks] = blocks
        self.assertEqual(set(results), {1, 2})
        u.compare_parallel(results[1], results[2])

    def test_conflicting_dirichlet_corners_use_library_area_weighting(self):
        import numpy as np
        case = u.thermal_case()
        case['loads'] = [dict(type='fixed_temperature', target='face:xmin', T=300),
                         dict(type='fixed_temperature', target='face:zmin', T=320),
                         dict(type='convection', target='face:zmax', h=20, T_inf=400)]
        _, blocks, _, _ = self.successful_thermal(case, 'corner-precedence')
        points = blocks['hex']['Points']
        mask = (np.abs(points[:, 0]) <= 1e-14) & (np.abs(points[:, 2]) <= 1e-14)
        self.assertTrue(np.any(mask))
        # Dirichlet overrides Robin/adiabatic contributions; conflicting Dirichlet
        # values follow the library's incident-face-area weighted convention.
        expected = (300 * (.25*.05) + 320 * (.25*.25)) / (.25*.05 + .25*.25)
        np.testing.assert_allclose(blocks['hex']['PointData/T'][mask], expected, rtol=0, atol=1e-10)

    def test_stop_and_control_line_handling_collectively(self):
        control = ('bad JSON\n' + json.dumps(dict(cmd_seq='1', action='STOP')) + '\n'
                   + json.dumps(dict(cmd_seq='1', action='STOP')) + '\n'
                   + json.dumps(dict(cmd_seq='2', action='STOP')))
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                out, _ = u.solve(self.root, u.thermal_case(), f'stop{ranks}', ranks, control=control)
                manifest = u.read_json(out / 'manifest.json')
                _, events = u.inspect_fvm(out, {}, manifest, stopped=True)
                acks = [e for e in events if e['kind'] == 17]
                self.assertEqual(len(acks), 1)
                self.assertEqual((acks[0]['cmd_seq'], acks[0]['status']), (1, 1))
                self.assertFalse((out / 'out/step-0.vtkhdf').exists())
        with self.subTest(duplicate_and_partial=True):
            control = (json.dumps(dict(cmdSeq='1', action='PAUSE')) + '\n'
                       + json.dumps(dict(cmd_seq='1', action='STOP')) + '\n'
                       + json.dumps(dict(cmd_seq='2', action='STOP')))
            out, _ = u.solve(self.root, u.thermal_case(), 'partial', control=control)
            manifest, blocks = u.read_result(out)
            _, events = u.inspect_fvm(out, blocks, manifest)
            acks = [e for e in events if e['kind'] == 17]
            self.assertEqual(len(acks), 1)
            self.assertEqual((acks[0]['cmd_seq'], acks[0]['status']), (1, 3))

    def test_repeat_run_directory_matches_fem_policy(self):
        observations = {}
        for family in ('fem', 'fvm'):
            with self.subTest(family=family):
                case = u.thermal_case(family)
                out, _ = u.solve(self.root, case, 'repeat-' + family)
                before = (out / 'events.bin').read_bytes()
                _, repeated = u.solve(self.root, case, 'repeat-' + family, expected=None)
                after = (out / 'events.bin').read_bytes()
                manifest = u.read_json(out / 'manifest.json')
                # Compare public outcome and recovery policy, rather than invent overwrite semantics.
                observations[family] = (repeated.returncode, len(manifest['steps']),
                                        after.startswith(before), len(after) > len(before))
        self.assertEqual(set(observations), {'fem', 'fvm'})
        self.assertEqual(observations['fvm'], observations['fem'])


def main():
    required = ('CHIPCAE_IMPORT_EXE', 'CHIPCAE_PREVIEW_EXE', 'CHIPCAE_SOLVE_EXE',
                'CHIPCAE_FVM_LOADER_PROBE', 'CHIPCAE_MPIEXEC', 'CHIPCAE_MPIEXEC_NUMPROC_FLAG')
    missing = [key for key in required if not os.environ.get(key)]
    if missing:
        print('SKIP: missing configured executables/launcher: ' + ', '.join(missing))
        return 77
    for key in required[:-1]:
        if not shutil.which(os.environ[key]):
            print(f'SKIP: {key} unavailable: {os.environ[key]}')
            return 77
    try:
        import h5py
        import numpy
    except Exception as error:
        print(f'SKIP: numpy/h5py unavailable: {error}')
        return 77
    fields = u.command([os.environ['CHIPCAE_FVM_LOADER_PROBE'], '--result-fields'], SOURCE)
    u.REQUIRE_COPPER_FIELDS = json.loads(fields.stdout)
    print(f'ResultsPartition copper fields available: {u.REQUIRE_COPPER_FIELDS}', flush=True)
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(cls)
                               for cls in (DecoderAcceptance, CaseAcceptance))
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
