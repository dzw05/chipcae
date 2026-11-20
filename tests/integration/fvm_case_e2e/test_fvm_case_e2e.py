"""Public FVM case/1.0 acceptance; this test-first revision is expected to be red."""

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
    def test_elastic_log_requires_one_rank_zero_level_one_strict_finite_record(self):
        valid = 'FVM_ELASTIC {"iterations":2,"residual":1e-18,"warpage_pv":0.012345678901234567,"U_mag_max":0.02}'
        decoded = u.elastic(u.decode_events(log_record(valid)))
        self.assertEqual(decoded['iterations'], 2)
        self.assertAlmostEqual(decoded['warpage_pv'], .012345678901234567, delta=1e-18)
        zero = 'FVM_ELASTIC {"iterations":0,"residual":0,"warpage_pv":0,"U_mag_max":0}'
        self.assertAlmostEqual(u.elastic(u.decode_events(log_record(zero)))['warpage_pv'], 0)
        invalid = [log_record('unrelated'), log_record(valid) + log_record(valid, seq=2),
                   log_record(valid, level=2), log_record(valid, rank=1),
                   log_record(valid + ' trailing'), log_record(valid + '\n'),
                   log_record(valid + '\r'), log_record('FVM_ELASTIC []')]
        fields = dict(iterations='2', residual='1e-18', warpage_pv='0.012345678901234567',
                      U_mag_max='0.02')
        for key, value in fields.items():
            token = f'"{key}":{value}'
            invalid.append(log_record(valid.replace(token, token + ',' + token)))
            missing = json.loads(valid[len(u.ELASTIC_PREFIX):])
            del missing[key]
            invalid.append(log_record(u.ELASTIC_PREFIX + json.dumps(missing)))
            for replacement in ('true', 'false', 'null', '"1"', '-1', 'NaN', 'Infinity', '1e999'):
                invalid.append(log_record(valid.replace(token, f'"{key}":{replacement}')))
        invalid.extend([log_record(valid.replace('"iterations":2', '"iterations":2.0')),
                        log_record(valid.replace('"iterations":2', '"iterations":2,"extra":0'))])
        for index, raw in enumerate(invalid):
            with self.subTest(index=index), self.assertRaises(AssertionError):
                u.elastic(u.decode_events(raw))

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
                    'PointData/T': np.concatenate([np.full(8, c[2]) for c in chunks]),
                    'PointData/U': np.concatenate([np.tile([c[0]*1e-6, 0, -2e-6], (8, 1))
                                                   for c in chunks])}
        a, b = block(False), block(True)
        u.compare_parallel(dict(hex=a, cut=a), dict(hex=b, cut=b))
        b['PointData/U'][0, 2] += 1e-6
        with self.assertRaisesRegex(AssertionError, 'MPI global U'):
            u.compare_parallel(dict(hex=a, cut=a), dict(hex=b, cut=b))
        b = block(True)
        b['PointData/T'][0] += .01
        with self.assertRaises(AssertionError):
            u.compare_parallel(dict(hex=a, cut=a), dict(hex=b, cut=b))

    def test_curvature_sign_ignores_rigid_translation_and_tilt(self):
        import numpy as np
        points = np.array([(x, y, .1) for x in (0., .5, 1.) for y in (0., .5, 1.)])
        def blocks(sign, tilt):
            displacement = np.zeros((len(points), 3))
            displacement[:, 2] = sign * (points[:, 0]**2 + points[:, 1]**2) * 1e-4
            displacement[:, 2] += tilt * (3 + 2*points[:, 0] - points[:, 1])
            return dict(hex={'Points': points, 'PointData/U': displacement},
                        cut={'Points': np.empty((0, 3)), 'PointData/U': np.empty((0, 3))})
        self.assertGreater(u.curvature(blocks(1, 0)), 0)
        self.assertGreater(u.curvature(blocks(1, -10)), 0)
        self.assertLess(u.curvature(blocks(-1, 10)), 0)


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
                # Full public structure, frozen before any production edits at 6eca460.
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
            summaries = {x['name']: x for x in manifest['summary']}
            self.assertEqual(summaries['T_max']['method'], 'FVM cell centers')
            # Equal CTEs permit zero bending, while free thermal expansion is nonzero.
            u.inspect_elastic(out, blocks, manifest, nonzero=False)
            self.assertGreater(summaries['U_mag_max']['value'], 0)

    def test_solid_only_publishes_solved_elastic_log_residual_and_displacement(self):
        case = u.bimaterial_strip(self.root)
        out, _ = u.solve(self.root, case, 'solid-elastic')
        # Volume publication has its own unchanged thermal gate. Isolate this
        # mechanics gate so the upstream missing hex volume cannot mask U=0.
        manifest, blocks = u.read_result(out, require_volume=False)
        u.inspect_elastic(out, blocks, manifest)

    def test_uniform_bimaterial_strip_matches_fem_warpage_and_curvature(self):
        case = u.bimaterial_strip(self.root)
        results = {}
        for family in ('fvm', 'fem'):
            with self.subTest(family=family):
                candidate = deepcopy(case)
                candidate['physics'][0]['solver'] = family + '.solid'
                candidate['solver']['linear']['preconditioner'] = 'direct' if family == 'fem' else 'jacobi'
                out, _ = u.solve(self.root, candidate, 'strip-' + family)
                results[family] = u.read_result(out, require_volume=False)
        self.assertEqual(set(results), {'fvm', 'fem'})
        fvm, fem = results['fvm'], results['fem']
        u.compare_mechanics(fvm[0], fem[0], fvm[1], fem[1])
        u.inspect_elastic(self.root / 'strip-fvm', fvm[1], fvm[0])

    def test_zero_temperature_rise_has_zero_U_and_warpage_with_normal_methods(self):
        import numpy as np
        case = u.bimaterial_strip(self.root)
        case['loads'][0]['T_target'] = 300
        out, _ = u.solve(self.root, case, 'strip-zero')
        manifest, blocks = u.read_result(out, require_volume=False)
        # Largest fixture dimension is exactly 1 m; zero-rise warpage <= 1e-14 L.
        values = u.summary(manifest)
        self.assertLessEqual(abs(values['warpage_pv']), 1e-14 * 1.0)
        self.assertLessEqual(abs(values['U_mag_max']), 1e-14)
        for name, block in blocks.items():
            self.assertTrue(np.all(np.abs(block['PointData/U']) <= 1e-14), name)
        u.inspect_elastic(out, blocks, manifest, nonzero=False)

    def test_solid_only_np1_np2_preserve_displacement_and_warpage(self):
        case = u.bimaterial_strip(self.root)
        results = {}
        for ranks in (1, 2):
            out, _ = u.solve(self.root, case, f'strip-np{ranks}', ranks)
            results[ranks] = u.read_result(out, require_volume=False)
        u.compare_parallel(results[1][1], results[2][1])
        first, second = (u.summary(results[r][0])['warpage_pv'] for r in (1, 2))
        self.assertGreater(first, 0)
        self.assertLessEqual(abs(first-second) / first, 1e-8)
        for ranks in (1, 2):
            u.inspect_elastic(self.root / f'strip-np{ranks}', results[ranks][1], results[ranks][0])

    def test_rigid321_fixed_bottom_and_fixed_nonfirst_layer_supports(self):
        import numpy as np
        case = u.bimaterial_strip(self.root)
        for name, constraint, height in (
                ('rigid321', dict(rigid='3-2-1'), None),
                ('fixed-bottom', dict(fixed=[dict(target='face:zmin')]), 0),
                ('fixed-layer-b', dict(fixed=[dict(target='layer:b')]), .1)):
            with self.subTest(support=name):
                candidate = deepcopy(case)
                candidate['constraints'] = constraint
                out, _ = u.solve(self.root, candidate, name)
                manifest, blocks = u.read_result(out, require_volume=False)
                if height is not None:
                    block = blocks['hex']
                    mask = np.abs(block['Points'][:, 2] - height) <= 1e-14
                    self.assertTrue(np.any(mask), 'no vertices on prescribed exterior faces')
                    self.assertTrue(np.all(np.abs(block['PointData/U'][mask]) <= 1e-14),
                                    'fixed/free edge vertices must retain prescribed zero U')
                u.inspect_elastic(out, blocks, manifest)

    def test_solid_only_nonconvergence_exits_one_with_residual_and_no_output(self):
        case = u.bimaterial_strip(self.root)
        case['solver']['linear'].update(max_iter=1, rtol=1e-14)
        u.probe(self.root, case)
        out, result = u.solve(self.root, case, 'solid-nonconverged', expected=None)
        self.assertEqual(result.returncode, 1, result.stderr)
        events = u.decode_events((out / 'events.bin').read_bytes())
        residuals = [p for e in events if e['kind'] == 18 for p in e['points']
                     if p['solver'] == 'fvm.solid' and p['field'] == 'U']
        self.assertTrue(residuals, 'mechanics failure must publish measured residuals')
        self.assertTrue(all(math.isfinite(p['norm']) and p['norm'] >= 0 for p in residuals))
        self.assertFalse(any(e['kind'] == 22 for e in events))
        self.assertFalse((out / 'out/step-0.vtkhdf').exists())
        if (out / 'manifest.json').exists():
            self.assertEqual(u.read_json(out / 'manifest.json')['steps'], [])
        self.assertEqual((events[-1]['kind'], events[-1]['exit_code']), (19, 1))

    def test_cut_elastic_rejects_unsupported_geometry_or_publishes_solved_cut_U(self):
        import numpy as np
        case = u.bimaterial_strip(self.root)
        stack = u.read_json(self.root / 'stack.json')
        stack['outline']['polygon'] = [[.1, 0], [1, 0], [1, .2], [0, .2], [0, .1]]
        u.write_json(self.root / 'stack.json', stack)
        u.probe(self.root, case)  # Reject fixture/schema mistakes before exercising mechanics.
        out, result = u.solve(self.root, case, 'cut-elastic', expected=None)
        if result.returncode == 13:
            # The frozen library currently rejects embedded faces. Preserve the
            # explicit CUT_UNSUPPORTED outcome until the upstream cut path exists.
            self.assertIn('unsupported', result.stderr.lower().replace('not supported', 'unsupported'))
            events = u.decode_events((out / 'events.bin').read_bytes())
            self.assertFalse(any(e['kind'] == 22 for e in events))
            self.assertEqual((events[-1]['kind'], events[-1]['exit_code']), (19, 13))
            self.assertFalse((out / 'out/step-0.vtkhdf').exists())
            if (out / 'manifest.json').exists():
                self.assertEqual(u.read_json(out / 'manifest.json')['steps'], [])
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest, blocks = u.read_result(out, require_volume=False)
            self.assertGreater(int(blocks['cut']['NumberOfCells'].sum()), 0)
            self.assertGreater(int(blocks['eb_surface']['Polygons/NumberOfCells'].sum()), 0)
            self.assertTrue(np.any(blocks['cut']['PointData/U'] != 0))
            self.assertTrue(np.any(blocks['eb_surface']['PointData/U'] != 0))
            u.inspect_elastic(out, blocks, manifest)

    def test_solid_only_stop_is_acknowledged_once_without_successful_output(self):
        case = u.bimaterial_strip(self.root)
        control = json.dumps(dict(cmd_seq='1', action='STOP')) + '\n'
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                out, _ = u.solve(self.root, case, f'solid-stop{ranks}', ranks, control=control*2)
                events = u.decode_events((out / 'events.bin').read_bytes())
                acks = [e for e in events if e['kind'] == 17 and e['status'] == 1]
                self.assertEqual(len(acks), 1)
                self.assertEqual(acks[0]['cmd_seq'], 1)
                self.assertEqual((events[-1]['kind'], events[-1]['reason']), (19, 'user_stop'))
                self.assertFalse(any(e['kind'] == 22 for e in events))
                self.assertFalse((out / 'out/step-0.vtkhdf').exists())
                if (out / 'manifest.json').exists():
                    self.assertEqual(u.read_json(out / 'manifest.json')['steps'], [])

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'deterministic checkpoint fixture requires POSIX FIFO')
    def test_stop_queued_after_first_checkpoint_prevents_mechanics_publication(self):
        case = u.bimaterial_strip(self.root)
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                out, _ = u.solve_with_late_stop(self.root, case, f'late-stop{ranks}', ranks)
                events = u.decode_events((out / 'events.bin').read_bytes())
                acks = [e for e in events if e['kind'] == 17]
                self.assertEqual([(e['cmd_seq'], e['status']) for e in acks], [(1, 3), (2, 1)],
                                 'STOP queued during mechanics needs a second control checkpoint')
                self.assertEqual((events[-1]['kind'], events[-1]['reason']), (19, 'user_stop'))
                self.assertFalse(any(e['kind'] == 22 for e in events))
                self.assertFalse((out / 'out/step-0.vtkhdf').exists())
                if (out / 'manifest.json').exists():
                    self.assertEqual(u.read_json(out / 'manifest.json')['steps'], [])

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
        for case in (u.thermal_case(), u.solid_case()):
            for reverse in (False, True):
                with self.subTest(physics=case['physics'], reverse=reverse):
                    candidate = deepcopy(case)
                    # The brief permits only solver-name enums in case schema;
                    # parameters.schema.json still restricts explicit FEM preconditioners.
                    # Exercise FVM preconditioners through the real loader/CLI above.
                    candidate.pop('solver')
                    if reverse:
                        candidate['physics'].reverse()
                    self.assertIn('warpage', candidate['output'])
                    errors = list(validator.iter_errors(candidate))
                    self.assertFalse(errors, '\n'.join(e.message for e in errors))

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
                    u.inspect_fvm(out, blocks, manifest)
                results[family, ranks] = manifest, blocks
        for name, content in originals.items():
            self.assertEqual((self.root / name).read_bytes(), content)
        self.assertEqual(len(stack['layers']), 5)
        self.assertEqual(set(results), {('fvm', 1), ('fvm', 2), ('fem', 1)})
        u.compare_parallel(results['fvm', 1][1], results['fvm', 2][1])
        fvm = u.summary(results['fvm', 1][0])['T_max']
        fem = u.summary(results['fem', 1][0])['T_max']
        self.assertGreater(fem - 298.15, 0)
        error = abs(fvm - fem) / (fem - 298.15)
        print(f'实测 board Tmax fvm={fvm:.17g} fem={fem:.17g} '
              f'rise_relative={error:.17g} absolute_T_relative={abs(fvm-fem)/abs(fem):.17g}', flush=True)
        self.assertLessEqual(error, .01)
        fvm_manifest, fvm_blocks = results['fvm', 1]
        fem_manifest, fem_blocks = results['fem', 1]
        u.compare_mechanics(fvm_manifest, fem_manifest, fvm_blocks, fem_blocks)
        a, b = (u.summary(results['fvm', n][0])['warpage_pv'] for n in (1, 2))
        self.assertGreater(a, 0)
        self.assertLessEqual(abs(a-b) / a, 1e-8)
        for ranks in (1, 2):
            u.inspect_elastic(self.root / f'fvm{ranks}', results['fvm', ranks][1], results['fvm', ranks][0])

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
            _, blocks, ledger, _ = self.successful_thermal(case, 'demo-embedded')
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
