"""Synthetic CAD covers provenance and local real-case result recording."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from parts_support import DependencyMissing, ROOT, box, dump, occt, prepare, write_step
import make_parts as generator
import test_e2e_derived_chamfer_lid as derived
import test_e2e_real_bulk_parts as real


NEUTRAL_KEYS = {
    'host_si': 'example_parts/die',
    'ihp_adh': 'example_parts/adhesive',
    'tim': 'example_parts/tim',
    'lid': 'example_parts/lid',
}
DEMO_VARS = ('CHIPCAE_DEMO_STEP_DIR', 'CHIPCAE_DEMO_BULK_STEP',
             'CHIPCAE_DEMO_BOARD_IPC', 'CHIPCAE_DEMO_BULK_BODIES')


def clean_environment():
    return {k: v for k, v in os.environ.items()
            if not k.startswith(('CHIPCAE_DEMO_', 'CHIPCAE_REAL_RUNS'))}


def execute(script, args=(), env=None):
    return subprocess.run([sys.executable, str(script), *map(str, args)],
                          env=env, capture_output=True, text=True, timeout=45)


class NeutralSamples(unittest.TestCase):
    def test_bulk_template_uses_neutral_step_and_body_keys(self):
        model = json.loads((ROOT / 'tools/samples/step_parts/bulk_parts.assembly.json').read_text())
        step = model['parts_from_step']
        self.assertTrue(step['step'] == 'example_parts.step', 'bulk STEP link must be neutral')
        self.assertTrue({v['id']: k for k, v in step['bodies'].items()} == NEUTRAL_KEYS,
                        'bulk keys must match the documented id-to-neutral-key mapping')

    def test_empty_body_negative_fixture_keeps_its_invalid_key(self):
        model = json.loads((ROOT / 'schema/examples/assembly/negative-assembly-empty_body.json').read_text())
        self.assertTrue(model['parts'][0]['step'] == model['parts_from_step']['step']
                        == 'example_parts.step', 'both STEP references must be neutral')
        self.assertTrue(model['parts'][0]['body'] == 'example_parts/lid', 'body must be neutral')
        self.assertEqual(list(model['parts_from_step']['bodies']), [''])

    def test_copper_approximation_note_uses_neutral_assembly_name(self):
        library = json.loads((ROOT / 'schema/examples/fcbga_demo.materials.json').read_text())
        copper = next(m for m in library['materials'] if m['id'] == 'copper')
        expected = '近似材料：多晶纯铜室温值，用于 example_parts 的 lid；未按实际盖板合金和工艺标定'
        self.assertTrue(copper['provenance']['notes'].encode('utf-8') == expected.encode('utf-8'),
                        'copper approximation note must use the neutral assembly name')


class DemoEnvironment(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='sp7-env-regression-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.env = clean_environment()
        self.env.update(dict(zip(DEMO_VARS, (str(self.root), 'example_parts.step',
                                            'board.ipc2581.xml', 'bodies.json'))))
        for filename in ('example_parts.step', 'board.ipc2581.xml', 'bodies.json'):
            (self.root / filename).touch()

    def test_real_scripts_skip_on_first_unset_or_empty_variable_before_dependencies(self):
        scripts = ((real, DEMO_VARS), (derived, DEMO_VARS[:2]))
        for module, variables in scripts:
            for index, missing in enumerate(variables):
                for empty in (False, True):
                    with self.subTest(script=module.__name__, missing=missing, empty=empty):
                        env = clean_environment()
                        env.update({v: self.env[v] for v in variables[:index]})
                        if empty:
                            env[missing] = ''
                        result = execute(module.__file__, [ROOT], env)
                        self.assertEqual(result.returncode, 77, result.stderr)
                        self.assertEqual(result.stdout.strip(), f'SKIP: {missing} is unset')

    def test_generator_reports_each_required_option_before_cad_generation(self):
        cases = (('chamfer', DEMO_VARS[:2]), ('bulk', DEMO_VARS))
        for case, variables in cases:
            for index, missing in enumerate(variables):
                for empty in (False, True):
                    with self.subTest(case=case, missing=missing, empty=empty):
                        env = clean_environment()
                        env.update({v: self.env[v] for v in variables[:index]})
                        if empty:
                            env[missing] = ''
                        out = self.root / f'out-{case}-{index}-{empty}'
                        result = execute(generator.__file__, ['--case', case, '--out', out], env)
                        self.assertEqual(result.returncode, 2, result.stderr)
                        self.assertIn(missing, result.stderr)
                        self.assertNotIn('example_parts.step', result.stderr)
                        self.assertFalse(out.exists(), 'missing options must fail before generation')

    def test_cpp_demo_env_resolves_only_configured_regular_files(self):
        build = os.environ.get('CHIPCAE_BINARY_DIR') or os.environ.get('CHIPCAE_TEST_BUILD_DIR')
        if not build:
            self.skipTest('C++ probes require a configured build directory')
        cache = Path(build) / 'CMakeCache.txt'
        if 'CHIPCAE_ENABLE_OCCT:BOOL=ON' not in cache.read_text():
            self.skipTest('C++ probes require CHIPCAE_ENABLE_OCCT=ON')
        for suffix, variable in (('bulk_parts', 'CHIPCAE_DEMO_BULK_STEP'),
                                 ('ihp_example', 'CHIPCAE_DEMO_IHP_STEP')):
            binary = (Path(build) / 'tests/integration/part_slice' /
                      ('chipcae_autotest_tests_integration_part_slice_occt_slice_real_' + suffix))
            self.assertTrue(binary.is_file(), 'build both real slice acceptance targets')
            target = self.root / 'example_parts.step'
            loop = self.root / ('loop-' + suffix)
            loop.symlink_to(loop.name)
            cases = (
                (str(self.root), str(target), str(target)),
                (str(self.root), target.name, str(target)),
                (None, str(target), ''), ('', str(target), ''),
                (str(self.root), None, ''), (str(self.root), '', ''),
                (str(self.root), 'missing.step', ''),
                (str(self.root), str(self.root), ''),
                (str(self.root), loop.name, ''),
            )
            for directory, value, expected in cases:
                with self.subTest(variable=variable, directory=directory, value=value):
                    env = clean_environment()
                    if directory is not None:
                        env['CHIPCAE_DEMO_STEP_DIR'] = directory
                    if value is not None:
                        env[variable] = value
                    result = subprocess.run([str(binary), '--check-demo-env', expected],
                                            env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(binary)], env=env, capture_output=True,
                                            text=True, timeout=10)
                    # A configured empty synthetic STEP reaches the existing import
                    # assertion (1); a missing configuration retains the skip (77).
                    self.assertEqual(result.returncode, 1 if expected else 77, result.stderr)


class FixtureRecording(unittest.TestCase):
    def setUp(self):
        try:
            import OCP
        except ImportError:
            self.skipTest('recording geometry requires cadquery-ocp')
        directory = tempfile.TemporaryDirectory(prefix='sp7-record-regression-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        prepare(self.source)
        (self.source / 'board.xml').rename(self.source / 'board.ipc2581.xml')
        # Original synthetic geometry, never reads private vendor inputs.
        write_step(self.source / 'example_parts.step', box(0, 0, 0, 56, 44, .5))
        dump(self.source / 'bodies.json', {key: key for key in NEUTRAL_KEYS.values()})
        self.directory = self.root / 'run'
        self.directory.mkdir()
        env = patch.dict(os.environ, {
            'CHIPCAE_DEMO_STEP_DIR': str(self.source),
            'CHIPCAE_DEMO_BULK_STEP': 'example_parts.step',
            'CHIPCAE_DEMO_BOARD_IPC': 'board.ipc2581.xml',
            'CHIPCAE_DEMO_BULK_BODIES': 'bodies.json',
        })
        env.start()
        self.addCleanup(env.stop)
        self.assertTrue((self.source / 'example_parts.step').is_file())
        self.assertTrue((self.source / 'board.ipc2581.xml').is_file())
        self.assertTrue((self.source / 'bodies.json').is_file())

    def test_derived_provenance_exists_before_importer_is_available(self):
        # SP4 is absent; stop only at that external boundary, after real generation.
        with patch.object(derived, 'imported', side_effect=DependencyMissing('SP4')):
            with self.assertRaises(DependencyMissing):
                derived.acceptance(self.directory)
        notes = json.loads((self.directory / 'fixture-notes.json').read_text())
        self.assertIn('派生件（由真实盖板四角倒角，非原始设计）', notes['labels'])
        self.assertTrue((self.directory / 'lid_chamfer_derived.step').is_file())

    def record_bulk(self, missing=None):
        # Stand in for unavailable assembly/solver backends; keep real fixture IO.
        def imported(d, model):
            dump(d / 'assembly.json', model)
            bodies = [*model['parts'], *model['parts_from_step']['bodies'].values()]
            dump(d / 'report.json', {'labels': ['近似材料'],
                                    'bodies': [p['id'] for p in bodies]})
            return {'layers': [{'regions': [{'body': p['id'], 'material': p['material']}
                                            for p in bodies if p['id'] != missing]}]}

        with patch.object(real, 'imported', side_effect=imported), \
                patch.object(real, 'solve', return_value=(1.25e-6, self.directory / 'synthetic.vtkhdf')):
            real.acceptance(self.directory)

    def test_bulk_writes_annotated_local_result(self):
        self.record_bulk()
        result = json.loads((self.directory / 'result.json').read_text())
        self.assertEqual(result['data_class'], 'real')
        self.assertAlmostEqual(result['warpage_pv'], 1.25e-6, delta=1e-18)
        self.assertIn('近似材料', result['labels'])
        self.assertIn('interposer_approx: 近似材料、近似几何', result['labels'])
        self.assertEqual(result['copper_layers'], '整层平均')

    def assert_bulk_model(self, directory, mapping):
        original = json.loads((ROOT / 'tools/samples/step_parts/bulk_parts.assembly.json').read_text())
        result = json.loads((directory / 'assembly.json').read_text())
        step = result['parts_from_step']
        self.assertEqual(step['step'], 'example_parts.step')
        self.assertEqual(list(step['bodies']), [mapping[key] for key in NEUTRAL_KEYS.values()])
        expected_values = list(original['parts_from_step']['bodies'].values())
        # Existing material selection is independent of the key remapping.
        for value in expected_values:
            if value['id'] == 'lid':
                value['material'] = 'copper'
        self.assertEqual(list(step['bodies'].values()), expected_values)
        original['parts_from_step'].update(step='example_parts.step', bodies=step['bodies'])
        self.assertEqual(result, original, 'remapping must preserve all other model fields')
        self.assertTrue((directory / 'example_parts.step').is_symlink())
        self.assertEqual((directory / 'example_parts.step').resolve(), self.source / 'example_parts.step')
        self.assertTrue((directory / 'board.xml').is_symlink())
        self.assertEqual((directory / 'board.xml').resolve(), self.source / 'board.ipc2581.xml')

    def generate_bulk(self, directory, options=()):
        result = execute(generator.__file__, ['--case', 'bulk', '--out', directory, *options])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_bulk_paths_and_mapping_preserve_values_order_and_template(self):
        template = ROOT / 'tools/samples/step_parts/bulk_parts.assembly.json'
        baseline = template.read_bytes()
        for absolute in (False, True):
            for identity in (True, False):
                mapping = dict(zip(NEUTRAL_KEYS.values(), (
                    list(NEUTRAL_KEYS.values()) if identity else
                    ('synthetic/die', 'synthetic/adhesive', 'synthetic/tim', 'synthetic/lid'))))
                dump(self.source / 'bodies.json', mapping)
                env = {key: (str(self.source / filename) if absolute else filename)
                       for key, filename in zip(DEMO_VARS[1:],
                           ('example_parts.step', 'board.ipc2581.xml', 'bodies.json'))}
                for consumer in ('real', 'generator'):
                    with self.subTest(absolute=absolute, identity=identity, consumer=consumer):
                        output = self.root / f'{consumer}-{absolute}-{identity}'
                        with patch.dict(os.environ, env):
                            if consumer == 'real':
                                self.directory = output
                                output.mkdir()
                                self.record_bulk()
                            else:
                                self.generate_bulk(output)
                            self.assert_bulk_model(output, mapping)
                        self.assertEqual(template.read_bytes(), baseline)

    def test_generator_explicit_paths_override_environment(self):
        # An explicit relative or absolute path takes precedence over env defaults.
        for absolute in (False, True):
            with self.subTest(absolute=absolute):
                paths = [str(self.source / name) if absolute else name
                         for name in ('example_parts.step', 'board.ipc2581.xml', 'bodies.json')]
                options = [arg for pair in zip(('--bulk-step', '--board-ipc', '--bulk-bodies'), paths)
                           for arg in pair]
                with patch.dict(os.environ, {key: 'unavailable' for key in DEMO_VARS[1:]}):
                    output = self.root / f'explicit-{absolute}'
                    self.generate_bulk(output, options)
                self.assert_bulk_model(output, {key: key for key in NEUTRAL_KEYS.values()})

    def test_bulk_missing_files_fail_with_only_variable_names(self):
        for variable in DEMO_VARS[1:]:
            for value in ('unavailable.step', '.'):
                with self.subTest(variable=variable, value=value):
                    with patch.dict(os.environ, {variable: value}):
                        with self.assertRaises(AssertionError) as caught:
                            real.acceptance(self.directory)
                    self.assertEqual(str(caught.exception), variable)

    def test_both_bulk_consumers_reject_invalid_body_maps_without_disclosing_targets(self):
        identity = {key: key for key in NEUTRAL_KEYS.values()}
        invalid = (
            ({}, tuple(identity)),
            ({k: v for k, v in identity.items() if k != 'example_parts/die'}, ('example_parts/die',)),
            (dict(identity, **{'example_parts/carrier': 'synthetic/extra'}), ('example_parts/carrier',)),
            (dict(identity, **{'example_parts/die': 'synthetic/duplicate',
                               'example_parts/lid': 'synthetic/duplicate'}),
             ('example_parts/die', 'example_parts/lid')),
            (dict(identity, **{'example_parts/die': ''}), ('example_parts/die',)),
            (dict(identity, **{'example_parts/die': 42}), ('example_parts/die',)),
        )
        for index, (mapping, keys) in enumerate(invalid):
            self.directory = self.root / f'invalid-real-{index}'
            self.directory.mkdir()
            dump(self.source / 'bodies.json', mapping)
            with self.subTest(consumer='real', index=index):
                with self.assertRaises(AssertionError) as caught:
                    self.record_bulk()
                message = str(caught.exception)
                for key in keys:
                    self.assertIn(key, message)
                self.assertNotIn('synthetic/', message)
                self.assertNotIn(str(self.source), message)
            with self.subTest(consumer='generator', index=index):
                output = self.root / f'invalid-{index}'
                result = execute(generator.__file__, ['--case', 'bulk', '--out', output])
                self.assertNotEqual(result.returncode, 0)
                # Check the final diagnostic; traceback source lines are not diagnostics.
                message = result.stderr.strip().splitlines()[-1]
                self.assertTrue(message.startswith('AssertionError'), message)
                for key in keys:
                    self.assertIn(key, message)
                self.assertNotIn('synthetic/', message)
                self.assertNotIn(str(self.source), message)

    def test_missing_imported_target_body_fails_coverage_instead_of_skipping(self):
        with self.assertRaisesRegex(AssertionError, 'example_parts/die'):
            self.record_bulk(missing='host_si')


if __name__ == '__main__':
    result = unittest.main(argv=[__file__], verbosity=2, exit=False).result
    if not result.wasSuccessful():
        sys.exit(1)
    # Keep the original CTest dependency skip after checking non-CAD contracts.
    occt()
