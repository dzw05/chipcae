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
    def test_every_mechanical_residual_point_requires_solver_and_field(self):
        good = dict(solver='fvm.solid', field='U', iteration=1, norm=1e-9)
        thermal = dict(solver='fvm.conduction', field='T', iteration=1, norm=1e-10)
        events = [dict(kind=18, points=[thermal, good])]
        residuals = u.mechanical_residuals(events)
        self.assertEqual(len(residuals), 1)
        self.assertIs(residuals[0], good)
        for change in (dict(solver='fem.solid'), dict(solver=''), dict(field='T'),
                       dict(norm=float('nan')), dict(norm=float('inf')), dict(norm=-1)):
            with self.subTest(change=change), self.assertRaises(AssertionError):
                u.mechanical_residuals([dict(kind=18, points=[good, dict(good, **change)])])
        with self.assertRaises(AssertionError):
            u.mechanical_residuals([dict(kind=18, points=[thermal])])

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


class AdapterAcceptance(unittest.TestCase):
    """Test the real private helpers, including deliberately non-GID input order."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='fvm-adapter-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        fixture = Path(os.environ.get('CHIPCAE_FVM_I14_FIXTURE_DIR',
                                      SOURCE.parent / 'handoff/fixtures/fvm-elastic-api-1.0'))
        cls.contract = fixture if (fixture / 'cases.json').is_file() else None
        cls.adapter, cls.contract_probe, cls.replay = u.build_adapter_probe(cls.root, SOURCE, cls.contract)

    def actual(self, mode, *args):
        return json.loads(u.command([self.adapter, mode, *args], self.root).stdout)

    def test_mesh_preserves_regular_cut_merged_order_and_physical_faces(self):
        import numpy as np
        got = self.actual('mesh')
        self.assertEqual(got['gid'], [81, 17, 45])
        self.assertEqual(got['rank'], [1, 0, 1])
        self.assertEqual(got['layer'], [1, 2, 0])
        self.assertEqual(got['is_cut'], [0, 1, 1])
        np.testing.assert_allclose(got['volume'], [3, 2, 7], rtol=0, atol=1e-14)
        np.testing.assert_allclose(got['centroid'], [[.2, .3, 1], [.4, .6, 2], [.8, 1, 3]],
                                   rtol=0, atol=1e-14)
        self.assertAlmostEqual(got['z_top'], 9, delta=1e-14)
        self.assertEqual(got['is_embedded'], [0, 1, 0])
        for face, indices, area, normal, center in zip(
                got['faces'], [(2, 0, -1), (1, -1, -1), (0, -1, 5)], [2, 5, 3],
                [[1, 0, 0], [0, .6, .8], [0, 0, 1]],
                [[.3, .5, 2], [.4, .5, 9], [.2, .3, 9]], strict=True):
            self.assertEqual((face['lo'], face['hi'], face['side']), indices)
            self.assertAlmostEqual(face['area'], area, delta=1e-14)
            np.testing.assert_allclose(face['normal'], normal, rtol=0, atol=1e-14)
            np.testing.assert_allclose(face['centroid'], center, rtol=0, atol=1e-14)

    def test_problem_laminate_engineering_order_temperature_and_options(self):
        import numpy as np
        got = self.actual('problem')
        # Independent I1 1.2 compliance oracle, inverted by numpy, not solve::mixture.
        # Parallel normal moduli, harmonic xz/yz shear; engineering order xx,yy,zz,yz,xz,xy.
        for cell, fraction in enumerate((0., .25, .75)):
            matrix, metal = 25e9, 110e9
            nm, nc, am, ac = .2, .34, 1e-5, 3e-5
            rest = 1-fraction
            exy = ez = rest*matrix + fraction*metal
            nu = rest*nm + fraction*nc
            gxy = exy/(2*(1+nu))
            gz = 1/(rest/(matrix/(2*(1+nm))) + fraction/(metal/(2*(1+nc))))
            compliance = np.diag([1/exy, 1/exy, 1/ez, 1/gz, 1/gz, 1/gxy])
            compliance[0, 1] = compliance[1, 0] = -nu/exy
            compliance[0, 2] = compliance[2, 0] = -nu/exy
            compliance[1, 2] = compliance[2, 1] = -nu/exy
            axy = ((rest*matrix*am/(1-nm) + fraction*metal*ac/(1-nc)) /
                   (rest*matrix/(1-nm) + fraction*metal/(1-nc)))
            az = (rest*matrix*am + fraction*metal*ac)/ez
            with self.subTest(fraction=fraction, quantity='stiffness'):
                np.testing.assert_allclose(np.asarray(got['C'][cell]).reshape(6, 6),
                                           np.linalg.inv(compliance), rtol=1e-12, atol=1e-5)
            with self.subTest(fraction=fraction, quantity='CTE'):
                np.testing.assert_allclose(got['alpha'][cell], [axy, axy, az],
                                           rtol=1e-12, atol=1e-16)
        np.testing.assert_allclose(got['dT'], [10, 25, -8], rtol=0, atol=1e-14)
        np.testing.assert_allclose(got['uniform_dT'], [57, 57, 57], rtol=0, atol=1e-14)
        self.assertAlmostEqual(got['rtol'], 2e-9, delta=1e-23)
        self.assertEqual(got['max_iter'], 37)
        self.assertEqual(got['preconditioner'], 'gamg')
        self.assertFalse(got['fixed'])
        self.assertEqual(got['fixed_side'], [])
        self.assertEqual(got['fixed_layer'], [])

    def test_problem_maps_all_sides_and_nonfirst_split_alias_by_stack_name(self):
        for side, name in enumerate(('xmin', 'xmax', 'ymin', 'ymax', 'zmin', 'zmax')):
            with self.subTest(side=name):
                got = self.actual('problem', 'face:' + name)
                self.assertTrue(got['fixed'])
                self.assertEqual(got['fixed_side'], [side])
                self.assertEqual(got['fixed_layer'], [])
        for targets, layers in ((['layer:support#0'], [2]), (['layer:support'], [1, 2]),
                                (['layer:base', 'layer:support#1'], [0, 1])):
            with self.subTest(targets=targets):
                got = self.actual('problem', *targets)
                self.assertTrue(got['fixed'])
                self.assertEqual(got['fixed_side'], [])
                self.assertEqual(got['fixed_layer'], layers)

    def test_merged_constituents_keep_parent_gid_and_subcell_ordinal(self):
        self.assertEqual(self.actual('lookup'),
                         [[17, 0, 1], [45, 0, 2], [45, 3, 2], [81, 0, 0], [93, 1, 2]])

    def test_embedded_polygon_membership_includes_edges_and_excludes_box_notch(self):
        self.assertEqual(self.actual('ring'), [True, False, True, True, True, False])

    def test_top_result_validation_uses_i14_embedded_normal_and_height_predicate(self):
        # Regular top + embedded within tolerance + embedded above z_top.
        # Exclude exactly nz=.99, a lower embedded face, and an interior face.
        self.assertEqual(self.actual('top'), [3])

    def test_both_internal_prefixes_and_ordinary_input_and_solve_errors(self):
        for reason, code in (('INTERNAL: geometry', 1),
                             ('INPUT_INVALID: INTERNAL: geometry', 1),
                             ('INPUT_INVALID: invalid embedded metadata', 14),
                             ('elastic solver did not converge', 1)):
            with self.subTest(reason=reason):
                actual = self.actual('error', reason)
                self.assertEqual(actual['code'], code)
                self.assertEqual(actual['message'], 'fvm.elastic: ' + reason)

    def test_obsolete_embedded_rejection_uses_ordinary_input_error_mapping(self):
        reason = 'INPUT_INVALID: embedded faces require cut-cell support'
        actual = self.actual('error', reason)
        self.assertEqual(actual['message'], 'fvm.elastic: ' + reason)
        self.assertEqual(actual['code'], 14,
                         'frozen I14 supports embedded mechanics: remove the obsolete exit-13 special case')

    def test_fraction_column_index_and_cut_merged_constituent_volume_weighting(self):
        import numpy as np
        for geometry in ('regular', 'cut', 'merged'):
            with self.subTest(geometry=geometry):
                root = self.root / geometry
                stack = u.fixture(root)
                if geometry != 'regular':
                    stack['outline']['polygon'] = [[.1, 0], [1, 0], [1, 1], [0, 1], [0, .1]]
                u.write_json(root / 'stack.json', stack)
                case = u.solid_case()
                case['mesh'].update(nx=2, ny=2)
                u.write_json(root / 'case.json', case)
                args = [root / 'case.json', root / 'profile1.json']
                got = self.actual('fractions', *args, *(['merge'] if geometry == 'merged' else []))
                self.assertIn({'regular': 0, 'cut': 1, 'merged': 2}[geometry], got['kind'])
                parents = {gid: (layer, volume, vf) for gid, layer, volume, vf in zip(
                    got['parent_gid'], got['parent_layer'], got['parent_volume'],
                    got['parent_fraction'], strict=True)}
                expected = []
                for begin, end in zip(got['off'], got['off'][1:]):
                    volumes, fractions = [], []
                    for gid, ordinal in zip(got['constituent_gid'][begin:end],
                                            got['constituent_subcell'][begin:end], strict=True):
                        self.assertEqual(ordinal, 0)
                        layer, volume, vf = parents[gid]
                        # Deliberately reversed copper-layer map: stack 0 -> copper 1.
                        phi = .05 + .1*(1-layer) + .02*(gid//got['nz'])
                        volumes.append(volume*vf)
                        fractions.append(min(1., phi/vf))
                    expected.append(np.average(fractions, weights=volumes))
                np.testing.assert_allclose(got['fraction'], expected, rtol=1e-12, atol=1e-14)

    def test_vertex_weights_deduplicate_merged_constituents_and_copied_coordinates(self):
        import numpy as np
        root = self.root / 'reconstruct'
        stack = u.fixture(root)
        stack['layers'][0]['thickness'] = .02
        stack['layers'][1]['thickness'] = .08
        u.write_json(root / 'stack.json', stack)
        case = u.solid_case()
        case['mesh'].update(nx=2, ny=2)
        u.write_json(root / 'case.json', case)
        actual = self.actual('reconstruct', root / 'case.json', root / 'profile1.json')
        for mode, vertices in actual.items():
            with self.subTest(mode=mode):
                values = {tuple(v['key']): np.asarray(v['U']) for v in vertices}
                self.assertEqual(len(values), 27)
                np.testing.assert_allclose([v['U'] for v in vertices], [v['before'] for v in vertices],
                                           rtol=1e-9, atol=1e-14)
                # Center: (3*1 + 2+3+4+5+6+7+8)/(3+7) = 3.8 micrometers.
                # Top edge: two top faces (.25 m^2) and two xmin faces (.04 m^2).
                for key, x in (((500000000000, 500000000000, 200000000000), 3.8e-6),
                               ((500000000000, 500000000000, 1000000000000), 6e-6),
                               ((0, 500000000000, 1000000000000), (6*.5+.08)/.58*1e-6)):
                    np.testing.assert_allclose(values[key], [x, 2*x, -3*x], rtol=1e-9, atol=1e-14)
                fixed = [value for key, value in values.items() if key[2] == 0]
                self.assertEqual(len(fixed), 9)
                np.testing.assert_allclose(fixed, 0, rtol=0, atol=1e-14)

    def test_frozen_i14_fourteen_cases_including_embedded_top_and_bad_metadata(self):
        if self.contract is None:
            self.skipTest('set CHIPCAE_FVM_I14_FIXTURE_DIR to the frozen I14 fixture package')
        cases = u.read_json(self.contract / 'cases.json')['cases']
        self.assertEqual(len(cases), 14)
        self.assertEqual(cases['P5_cut_step_embedded_top'], 'positive')
        self.assertEqual(cases['E_embedded_face_metadata'], 'negative')
        result = u.command([self.contract_probe, *(self.contract / name for name in sorted(cases))],
                           self.root)
        actual = json.loads(result.stdout)
        self.assertEqual(set(actual), set(cases))
        for name, value in actual.items():
            if value['ok']:
                diagnostic = {key: value[key] for key in (
                    'min_dn_ratio', 'min_dn_gid', 'gradient_corrections', 'max_identity_defect')}
                print(f'实测 I14 {name} diagnostics={json.dumps(diagnostic)}', flush=True)
        path = self.root / 'i14-actual.json'
        u.write_json(path, actual)
        checked = u.command([sys.executable, self.contract / 'check.py', '--actual', path], self.root)
        print(checked.stdout, flush=True)


class CaseAcceptance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe_temp = tempfile.TemporaryDirectory(prefix='fvm-replay-')
        cls.addClassCleanup(cls.probe_temp.cleanup)
        cls.adapter, _, cls.replay = u.build_adapter_probe(Path(cls.probe_temp.name), SOURCE)

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

    def test_adapter_removes_thermal_only_placeholder(self):
        adapter = SOURCE / 'apps/chipcae-solve/src/fvm_pipeline.cpp'
        self.assertFalse(u.THERMAL_ONLY in adapter.read_text(encoding='utf-8'),
                         'the brief requires removing the thermal-only fallback from the adapter')

    def test_uniform_bimaterial_strip_matches_direct_library_and_fem_curvature(self):
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
        direct = u.direct_elastic(self.root, case, 'strip', self.adapter, self.replay)
        u.compare_replay(fvm[0], direct)
        u.compare_mechanics_sign(fvm[0], fem[0], fvm[1], fem[1])
        u.inspect_elastic(self.root / 'strip-fvm', fvm[1], fvm[0])

    def test_prescribed_temperature_bilayer_matches_direct_library_and_fem_curvature(self):
        case = u.prescribed_temperature_strip(self.root)
        results = {}
        for family in ('fvm', 'fem'):
            candidate = deepcopy(case)
            for physics in candidate['physics']:
                physics['solver'] = family + ('.conduction' if physics['id'] == 'thermal' else '.solid')
            candidate['solver']['linear']['preconditioner'] = 'direct' if family == 'fem' else 'jacobi'
            out, _ = u.solve(self.root, candidate, 'prescribed-' + family)
            results[family] = u.read_result(out, require_volume=False)
        fvm, fem = results['fvm'], results['fem']
        direct = u.direct_elastic(self.root, case, 'prescribed', self.adapter, self.replay)
        u.compare_replay(fvm[0], direct)
        u.compare_mechanics_sign(fvm[0], fem[0], fvm[1], fem[1])
        u.inspect_elastic(self.root / 'prescribed-fvm', fvm[1], fvm[0])

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

    def test_uniform_expansion_reconstructs_interior_volume_and_boundary_area_weights(self):
        import numpy as np
        # Equal CTEs give the analytic stress-free affine displacement. Unequal
        # layer thicknesses distinguish CV volume weighting from a plain mean.
        self.stack['layers'][0].update(thickness=.02, n_elem_min=2)
        self.stack['layers'][1].update(thickness=.08, n_elem_min=2)
        u.write_json(self.root / 'stack.json', self.stack)
        case = u.solid_case()
        case['solver']['linear'].update(rtol=1e-12)
        out, _ = u.solve(self.root, case, 'affine-reconstruction')
        manifest, blocks = u.read_result(out, require_volume=False)
        block = blocks['hex']
        points = block['Points']
        # This is a serial uncut fixture, so each hexahedron has eight local IDs.
        cells = points[block['Connectivity'].reshape(-1, 8).astype(int)]
        lo, hi = cells.min(axis=1), cells.max(axis=1)
        centers, widths = (lo+hi)/2, hi-lo
        origin = centers.mean(axis=0)  # I14 Rigid321 removes arithmetic translation.
        volume = widths.prod(axis=1)
        domain_lo, domain_hi = points.min(axis=0), points.max(axis=0)
        strain = 1e-5 * 50
        expected = []
        unequal_interior = False
        for point in points:
            incident = np.all((point >= lo-1e-14) & (point <= hi+1e-14), axis=1)
            face_centers, areas = [], []
            for cell in np.flatnonzero(incident):
                for axis in range(3):
                    for plane in (domain_lo[axis], domain_hi[axis]):
                        if (abs(point[axis]-plane) <= 1e-14
                                and (abs(lo[cell, axis]-plane) <= 1e-14
                                     or abs(hi[cell, axis]-plane) <= 1e-14)):
                            center = centers[cell].copy()
                            center[axis] = plane
                            face_centers.append(center)
                            areas.append(np.prod(np.delete(widths[cell], axis)))
            if areas:
                location = np.average(face_centers, axis=0, weights=areas)
            else:
                location = np.average(centers[incident], axis=0, weights=volume[incident])
                unequal_interior |= np.linalg.norm(location-centers[incident].mean(axis=0)) > 1e-6
            expected.append(strain * (location-origin))
        self.assertTrue(unequal_interior, 'fixture must resolve unequal incident CV weights')
        np.testing.assert_allclose(block['PointData/U'], expected, rtol=1e-9, atol=1e-14)
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
                    if name == 'fixed-layer-b':
                        x, y, z = block['Points'].T
                        exterior = ((np.abs(x) <= 1e-14) | (np.abs(x-1) <= 1e-14)
                                    | (np.abs(y) <= 1e-14) | (np.abs(y-.2) <= 1e-14)
                                    | (np.abs(z-.1) <= 1e-14))
                        mask = (z >= .05-1e-14) & exterior
                    self.assertTrue(np.any(mask), 'no vertices on prescribed exterior faces')
                    self.assertTrue(np.all(np.abs(block['PointData/U'][mask]) <= 1e-14),
                                    'fixed/free edge vertices must retain prescribed zero U')
                # Layer b includes the entire top surface: prescribed zero top
                # displacement gives zero top-face warpage even with nonzero dT.
                u.inspect_elastic(out, blocks, manifest, nonzero=name != 'fixed-layer-b')
                self.assertGreater(u.summary(manifest)['U_mag_max'], 0)
                self.assertTrue(np.any(blocks['hex']['PointData/U'] != 0))
                if name == 'fixed-layer-b':
                    self.assertLessEqual(abs(u.summary(manifest)['warpage_pv']), 1e-14)

    def test_solid_only_nonconvergence_preserves_library_reason_without_success(self):
        case = u.bimaterial_strip(self.root)
        case['solver']['linear'].update(max_iter=1, rtol=1e-14)
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                direct = u.direct_elastic(self.root, case, f'solid-failure{ranks}',
                                          self.adapter, self.replay, ranks)
                out, result = u.solve(self.root, case, f'solid-nonconverged{ranks}', ranks,
                                      expected=None)
                u.inspect_elastic_failure(out, result, direct)

    def test_thermal_success_then_mechanical_failure_preserves_library_reason(self):
        case = u.prescribed_temperature_strip(self.root)
        # Pin every thermal CV: the thermal solve completes within one iteration
        # while the mismatched CTEs still need a mechanical solve.
        case['solver']['linear'].update(max_iter=1, rtol=1e-14)
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                direct = u.direct_elastic(self.root, case, f'coupled-failure{ranks}',
                                          self.adapter, self.replay, ranks)
                out, result = u.solve(self.root, case, f'coupled-nonconverged{ranks}', ranks,
                                      expected=None)
                events = u.inspect_elastic_failure(out, result, direct)
                u.energy(events)  # Prove this failure occurs after thermal success.
                thermal = [p for e in events if e['kind'] == 18 for p in e['points']
                           if (p['solver'], p['field']) == ('fvm.conduction', 'T')]
                self.assertTrue(thermal, 'fixture must reach successful thermal publication')
                self.assertTrue(all(math.isfinite(p['norm']) for p in thermal))

    def test_pipeline_maps_opaque_i14_failures_after_thermal_success_without_mechanics_output(self):
        # A private fail() unit check cannot catch dropping mechanical=true at
        # the run_fvm call site, rewriting the error event, or publishing success.
        case = u.prescribed_temperature_strip(self.root)
        u.write_json(self.root / 'case.json', case)
        probe = self.adapter.parent / 'pipeline_failure_probe'
        reasons = (
            ('input', 'INPUT_INVALID: invalid embedded metadata', 14),
            ('internal', 'INTERNAL: invalid face geometry', 1),
            ('double-prefix', 'INPUT_INVALID: INTERNAL: invalid face geometry', 1),
            ('nonconvergence', 'elastic solver did not converge; opaque producer detail: Ω', 1),
            ('obsolete-cut', 'INPUT_INVALID: embedded faces require cut-cell support', 14),
        )
        for ranks in (1, 2):
            for name, reason, code in reasons:
                with self.subTest(ranks=ranks, reason=reason):
                    out = self.root / f'pipeline-error-{name}-np{ranks}'
                    result = u.command(u.mpi_command([
                        probe, self.root / 'case.json', self.root / f'profile{ranks}.json',
                        out, reason], ranks), self.root, expected=None)
                    events = u.decode_events((out / 'events.bin').read_bytes())
                    u.energy(events)  # The failure follows a real successful thermal solve.
                    thermal = [p for e in events if e['kind'] == 18 for p in e['points']
                               if (p['solver'], p['field']) == ('fvm.conduction', 'T')]
                    self.assertTrue(thermal, 'fixture did not reach the mechanics boundary')
                    u.no_mechanical_publication(out, events)
                    errors = [e for e in events if e['kind'] == 20 and e['level'] == 3]
                    self.assertEqual(len(errors), 1, errors)
                    self.assertEqual(errors[0]['rank'], 0)
                    self.assertEqual(errors[0]['text'], 'fvm.elastic: ' + reason)
                    self.assertEqual(errors[0]['error_code'], code)
                    exits = [e for e in events if e['kind'] == 19]
                    self.assertEqual(len(exits), 1, exits)
                    self.assertIs(exits[0], events[-1])
                    self.assertEqual((exits[0]['exit_code'], exits[0]['reason']), (code, 'error'))
                    self.assertEqual(result.returncode, code, result.stderr)

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'deterministic checkpoint fixture requires POSIX FIFO')
    def test_thermal_success_stop_checkpoints_precede_mechanical_success_and_failure(self):
        case = u.prescribed_temperature_strip(self.root)
        stop = json.dumps(dict(cmd_seq='1', action='STOP')) + '\n'
        for ranks in (1, 2):
            for phase in ('before-mechanics', 'during-success', 'during-failure'):
                with self.subTest(ranks=ranks, phase=phase):
                    candidate = deepcopy(case)
                    if phase == 'during-failure':
                        candidate['solver']['linear'].update(max_iter=1, rtol=1e-14)
                    name = f'coupled-stop-{phase}{ranks}'
                    if phase == 'before-mechanics':
                        out, _ = u.solve(self.root, candidate, name, ranks, control=stop*2)
                        expected_acks = [(1, 1)]
                    else:
                        out, _ = u.solve_with_late_stop(self.root, candidate, name, ranks)
                        expected_acks = [(1, 3), (2, 1)]
                    events = u.decode_events((out / 'events.bin').read_bytes())
                    u.energy(events)  # The STOP must be handled after a real thermal success.
                    self.assertEqual([(e['cmd_seq'], e['status']) for e in events if e['kind'] == 17],
                                     expected_acks)
                    self.assertEqual((events[-1]['kind'], events[-1]['exit_code'], events[-1]['reason']),
                                     (19, 0, 'user_stop'))
                    u.no_mechanical_publication(out, events)
                    self.assertFalse(any(e['kind'] == 18 and any(p['field'] == 'U' for p in e['points'])
                                         for e in events), 'STOP wins before mechanical publication')

    def test_fixed_support_maps_all_six_exterior_faces(self):
        import numpy as np
        # A translated rectangular outline distinguishes all four lateral sides.
        self.stack['outline']['box'] = [-.3, .4, .7, .9]
        u.write_json(self.root / 'stack.json', self.stack)
        for face, axis, coordinate in (('xmin', 0, -.3), ('xmax', 0, .7),
                                       ('ymin', 1, .4), ('ymax', 1, .9),
                                       ('zmin', 2, 0.), ('zmax', 2, .1)):
            with self.subTest(face=face):
                case = u.solid_case()
                case['constraints'] = dict(fixed=[dict(target='face:' + face)])
                out, _ = u.solve(self.root, case, 'fixed-' + face)
                manifest, blocks = u.read_result(out, require_volume=False)
                block = blocks['hex']
                mask = np.abs(block['Points'][:, axis] - coordinate) <= 1e-14
                self.assertTrue(np.any(mask), 'fixture has no vertices on the fixed face')
                self.assertTrue(np.all(np.abs(block['PointData/U'][mask]) <= 1e-14),
                                face + ': prescribed face and fixed/free edges must remain zero')
                u.inspect_elastic(out, blocks, manifest, nonzero=False)
                self.assertGreater(u.summary(manifest)['U_mag_max'], 0)
                self.assertTrue(np.any(np.abs(block['PointData/U'][~mask]) > 1e-14),
                                'clamping one face must not erase all thermal expansion')

    def test_fixed_layer_alias_matches_explicit_nonfirst_stack_indices(self):
        import numpy as np
        self.stack['layers'][0]['thickness'] = .04
        second = self.stack['layers'][1]
        second.update(name='support#1', thickness=.03)
        last = deepcopy(second)
        last['name'] = 'support#0'  # Suffix order deliberately differs from stack order.
        self.stack['layers'].append(last)
        u.write_json(self.root / 'stack.json', self.stack)
        results = {}
        for name, targets in (('alias', ['support']), ('explicit', ['support#0', 'support#1'])):
            with self.subTest(targets=name):
                case = u.solid_case()
                case['constraints'] = dict(fixed=[dict(target='layer:' + t) for t in targets])
                u.probe(self.root, case)
                out, _ = u.solve(self.root, case, 'layer-' + name)
                manifest, blocks = u.read_result(out, require_volume=False)
                block = blocks['hex']
                x, y, z = block['Points'].T
                exterior = ((np.abs(x) <= 1e-14) | (np.abs(x-1) <= 1e-14)
                            | (np.abs(y) <= 1e-14) | (np.abs(y-1) <= 1e-14)
                            | (np.abs(z-.1) <= 1e-14))
                fixed = (z >= .04-1e-14) & exterior
                self.assertTrue(np.any(fixed))
                self.assertTrue(np.all(np.abs(block['PointData/U'][fixed]) <= 1e-14))
                self.assertTrue(np.any(np.abs(block['PointData/U'][z < .04-1e-14]) > 1e-14),
                                'nonfirst fixed layers must leave the bottom layer free to expand')
                u.inspect_elastic(out, blocks, manifest, nonzero=False)
                results[name] = manifest, blocks
        self.assertEqual(set(results), {'alias', 'explicit'})
        u.compare_parallel(results['alias'][1], results['explicit'][1])

    def test_uniform_mechanics_uses_temperature_rise_instead_of_absolute_temperature(self):
        import numpy as np
        case = u.bimaterial_strip(self.root)
        results = []
        for offset in (0, 125):
            candidate = deepcopy(case)
            candidate['loads'][0].update(T_ref=300+offset, T_target=350+offset)
            out, _ = u.solve(self.root, candidate, f'reference-offset{offset}')
            manifest, blocks = u.read_result(out, require_volume=False)
            u.inspect_elastic(out, blocks, manifest)
            for block in blocks.values():
                np.testing.assert_allclose(block['PointData/T'], 350+offset, rtol=0, atol=1e-10)
            results.append((manifest, blocks))
        first, shifted = results
        for name in first[1]:
            np.testing.assert_allclose(shifted[1][name]['Points'], first[1][name]['Points'],
                                       rtol=0, atol=1e-14)
            np.testing.assert_allclose(shifted[1][name]['PointData/U'], first[1][name]['PointData/U'],
                                       rtol=1e-9, atol=1e-14)
        for key in ('warpage_pv', 'U_mag_max'):
            a, b = u.summary(first[0])[key], u.summary(shifted[0])[key]
            self.assertLessEqual(abs(a-b), 1e-9 * abs(a), key)

    def test_cut_elastic_matches_direct_library_in_both_output_modes(self):
        import numpy as np
        case = u.bimaterial_strip(self.root)
        stack = u.read_json(self.root / 'stack.json')
        stack['outline']['polygon'] = [[.1, 0], [1, 0], [1, .2], [0, .2], [0, .1]]
        u.write_json(self.root / 'stack.json', stack)
        u.probe(self.root, case)  # Reject fixture/schema mistakes before exercising mechanics.
        direct = u.direct_elastic(self.root, case, 'cut', self.adapter, self.replay)
        results = {}
        for mode, extra in (('polyhedron', ()), ('hex', ('--cut-output', 'hex'))):
            with self.subTest(cut_output=mode):
                out, _ = u.solve(self.root, case, 'cut-elastic-' + mode, extra=extra)
                manifest, blocks = u.read_result(out, require_volume=False)
                self.assertGreater(int(blocks['eb_surface']['Polygons/NumberOfCells'].sum()), 0)
                if mode == 'polyhedron':
                    self.assertGreater(int(blocks['cut']['NumberOfCells'].sum()), 0)
                    self.assertTrue(np.any(blocks['cut']['PointData/U'] != 0))
                else:
                    self.assertEqual(int(blocks['cut']['NumberOfCells'].sum()), 0)
                    mask = blocks['hex']['CellData/cell_kind'] == 1
                    self.assertTrue(np.any(mask), 'hex output lost cut parent cells')
                    cells = u.canonical_cells(blocks['hex'], 'U')
                    cut_gids = set(blocks['hex']['CellData/elem_gid'][mask])
                    self.assertTrue(any(np.any(np.asarray(values) != 0)
                                        for (gid, _), values in cells.items()
                                        if gid in cut_gids), 'hex cut parent U is zero')
                self.assertTrue(np.any(blocks['eb_surface']['PointData/U'] != 0))
                u.inspect_elastic(out, blocks, manifest)
                u.compare_replay(manifest, direct)
                results[mode] = manifest, blocks
        self.assertEqual(set(results), {'polyhedron', 'hex'})
        poly, hexa = (results[mode] for mode in ('polyhedron', 'hex'))
        np.testing.assert_allclose(poly[1]['eb_surface']['Points'],
                                   hexa[1]['eb_surface']['Points'], rtol=0, atol=1e-14)
        np.testing.assert_allclose(poly[1]['eb_surface']['PointData/U'],
                                   hexa[1]['eb_surface']['PointData/U'], rtol=1e-12, atol=1e-14)
        for key in ('warpage_pv', 'U_mag_max'):
            a, b = u.summary(poly[0])[key], u.summary(hexa[0])[key]
            self.assertLessEqual(abs(a-b), 1e-12 * abs(a))
        fem_case = deepcopy(case)
        fem_case['physics'][0]['solver'] = 'fem.solid'
        fem_case['solver']['linear']['preconditioner'] = 'direct'
        fem_out, _ = u.solve(self.root, fem_case, 'cut-fem')
        fem_manifest, fem_blocks = u.read_result(fem_out, require_volume=False)
        u.compare_mechanics_sign(poly[0], fem_manifest, poly[1], fem_blocks)

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

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'deterministic checkpoint fixture requires POSIX FIFO')
    def test_stop_after_failed_mechanics_precedes_failure_publication(self):
        case = u.bimaterial_strip(self.root)
        case['solver']['linear'].update(max_iter=1, rtol=1e-14)
        for ranks in (1, 2):
            with self.subTest(ranks=ranks):
                out, _ = u.solve_with_late_stop(self.root, case, f'failed-late-stop{ranks}', ranks)
                events = u.decode_events((out / 'events.bin').read_bytes())
                self.assertEqual([(e['cmd_seq'], e['status']) for e in events if e['kind'] == 17],
                                 [(1, 3), (2, 1)])
                self.assertEqual((events[-1]['kind'], events[-1]['exit_code'], events[-1]['reason']),
                                 (19, 0, 'user_stop'))
                self.assertFalse(any(e['kind'] == 22 or
                                     (e['kind'] == 20 and e['text'].startswith(u.ELASTIC_PREFIX))
                                     for e in events))
                self.assertFalse((out / 'out/step-0.vtkhdf').exists())

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
                # Collect mechanics independently so missing upstream volume or
                # a failed thermal gate cannot hide the missing elastic solve.
                manifest, blocks = u.read_result(out, require_volume=False)
                results[family, ranks] = manifest, blocks
                self.assertEqual(int(blocks['hex']['NumberOfCells'].sum()), preview['cell_count'])
                self.assertEqual(int(blocks['cut']['NumberOfCells'].sum()), 0)
                self.assertTrue(np.all(blocks['hex']['CellData/cell_kind'] == 0))
                for field in ('nx0', 'ny0', 'L', 'nz', 'padded_cells', 'mesh_sha256'):
                    self.assertEqual(manifest['mesh'][field], preview[field])
                if family == 'fvm':
                    with self.subTest(gate='thermal-output'):
                        u.read_result(out)  # Keep the existing physical-volume/copper requirements.
                        u.inspect_fvm(out, blocks, manifest)
        for name, content in originals.items():
            self.assertEqual((self.root / name).read_bytes(), content)
        self.assertEqual(len(stack['layers']), 5)
        self.assertEqual(set(results), {('fvm', 1), ('fvm', 2), ('fem', 1)})
        u.compare_parallel(results['fvm', 1][1], results['fvm', 2][1])
        with self.subTest(gate='direct-mechanics'):
            direct = u.direct_elastic(self.root, case, 'board', self.adapter, self.replay)
            u.compare_replay(results['fvm', 1][0], direct)
        fvm_manifest, fvm_blocks = results['fvm', 1]
        fem_manifest, fem_blocks = results['fem', 1]
        with self.subTest(gate='mechanics-comparison'):
            u.compare_mechanics_sign(fvm_manifest, fem_manifest, fvm_blocks, fem_blocks)
        a, b = (u.summary(results['fvm', n][0])['warpage_pv'] for n in (1, 2))
        with self.subTest(gate='mechanics-mpi'):
            self.assertGreater(a, 0)
            self.assertLessEqual(abs(a-b) / a, 1e-8)
        for ranks in (1, 2):
            with self.subTest(gate='mechanics-output', ranks=ranks):
                u.inspect_elastic(self.root / f'fvm{ranks}', results['fvm', ranks][1], results['fvm', ranks][0])
        fvm = float(results['fvm', 1][1]['hex']['PointData/T'].max())
        fem = float(results['fem', 1][1]['hex']['PointData/T'].max())
        self.assertGreater(fem - 298.15, 0)
        error = abs(fvm - fem) / (fem - 298.15)
        fvm_summary = u.summary(results['fvm', 1][0])['T_max']
        fem_summary = u.summary(results['fem', 1][0])['T_max']
        print(f'实测 board vertex_Tmax fvm={fvm:.17g} fem={fem:.17g} '
              f'summary_Tmax fvm={fvm_summary:.17g} fem={fem_summary:.17g} '
              f'rise_relative={error:.17g} absolute_T_relative={abs(fvm-fem)/abs(fem):.17g}', flush=True)
        self.assertLessEqual(error, .02)  # 临时门值 0.02（rulings/pr236.md 架构师 14:14）：solve-fvm-vertex-recon 合入后恢复 0.01

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

        def fvm_run(case, name, extra=()):
            direct = u.direct_elastic(self.root, case, name, self.adapter, self.replay)
            out, process = u.solve(self.root, case, name, extra=extra, expected=None)
            if not direct['ok']:
                u.inspect_elastic_failure(out, process, direct)
                return None
            self.assertEqual(process.returncode, 0, process.stderr)
            manifest, blocks = u.read_result(out)
            u.inspect_fvm(out, blocks, manifest)
            return manifest, blocks

        # The regular and cut thermal libraries are present in this baseline: no stage-B skip.
        for family in ('fem', 'fvm'):
            with self.subTest(family=family):
                case = deepcopy(original)
                if family == 'fvm':
                    for p in case['physics']:
                        p['solver'] = p['solver'].replace('fem.', 'fvm.')
                    case['solver']['linear']['preconditioner'] = 'jacobi'
                # Only the two solver names and preconditioner differ from DEMO-GATE.
                if family == 'fvm':
                    result = fvm_run(case, 'demo-fvm')
                    if result is None:
                        continue
                    manifest, blocks = result
                else:
                    out, _ = u.solve(self.root, case, 'demo-fem')
                    manifest, blocks = u.read_result(out, require_volume=False)
                self.assertGreater(int(blocks['cut']['NumberOfCells'].sum()), 0)
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
            result = fvm_run(case, 'demo-hex', extra=('--cut-output', 'hex'))
            if result is not None:
                manifest, blocks = result
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
            result = fvm_run(case, 'demo-embedded')
            if result is not None:
                manifest, blocks = result
                ledger = u.energy(u.decode_events((self.root / 'demo-embedded/events.bin').read_bytes()))
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
                               for cls in (DecoderAcceptance, AdapterAcceptance, CaseAcceptance))
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
