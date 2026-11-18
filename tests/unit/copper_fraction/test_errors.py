"""Every rejected input must fail with its protocol code, before writing."""
import unittest
from unittest.mock import patch
from test_fraction import *


class Errors(unittest.TestCase):
    def test_verified_payload_is_read_once(self):
        from tools.stack.copper_fraction import main
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stack = fixture(root)
            source, output = root / 'input.json', root / 'output.json'
            source.write_text(json.dumps(stack))
            read_bytes = Path.read_bytes
            reads = 0

            def changing(path):
                nonlocal reads
                data = read_bytes(path)
                if path == root / 'copper.polyset':
                    reads += 1
                    path.write_bytes(b'changed after verification read')
                return data

            with patch.object(Path, 'read_bytes', changing):
                code = main([str(source), '-o', str(output)])
            self.assertEqual(reads, 1)
            self.assertEqual(code, 0)
            self.assertTrue(math.isclose(json.loads(output.read_text())['layers'][0]['copper']['fraction'],
                                         .06, rel_tol=1e-12))

    def test_resource_errors_at_each_stage(self):
        from contextlib import redirect_stderr
        from io import StringIO
        from tools.stack.copper_fraction import main
        for stage in ('json.load', 'copy.deepcopy', 'json.dumps'):
            for exception in (RecursionError, MemoryError):
                with self.subTest(stage=stage, exception=exception), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    source, output, report = (root / name for name in ('input.json', 'output.json', 'report.json'))
                    source.write_text(json.dumps(fixture(root)))
                    stderr = StringIO()
                    with patch('tools.stack.copper_fraction.' + stage, side_effect=exception('too deeply nested')), redirect_stderr(stderr):
                        code = main([str(source), '-o', str(output), '--report', str(report)])
                    self.assertEqual(code, 14)
                    self.assertTrue(stderr.getvalue().startswith('copper_fraction: INPUT_INVALID:'))
                    self.assertEqual(len(stderr.getvalue().splitlines()), 1)
                    self.assertFalse(output.exists())
                    self.assertFalse(report.exists())

    def test_nonlocal_references(self):
        from tools.stack.copper_fraction import homogenize_copper, StackPrepError
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for path in ('/absolute.polyset', '../outside.polyset',
                         'nested/../../outside.polyset', 'nested/../copper.polyset'):
                with self.subTest(path=path):
                    stack = fixture(root)
                    stack['external_refs'][0]['path'] = path
                    stack['layers'][0]['copper']['polygons']['file'] = path
                    with patch.object(Path, 'read_bytes', side_effect=AssertionError('must reject before read')):
                        with self.assertRaises(StackPrepError) as caught:
                            homogenize_copper(stack, root)
                    self.assertEqual(caught.exception.code, 14)
            stack_for_symlink = fixture(root)
            outside = root.parent / (root.name + '-outside.polyset')
            try:
                outside.write_bytes(b'outside')
                (root / 'copper.polyset').unlink()
                (root / 'copper.polyset').symlink_to(outside)
                with self.assertRaises(StackPrepError) as caught:
                    homogenize_copper(stack_for_symlink, root)
                self.assertEqual(caught.exception.code, 14)
            finally:
                outside.unlink(missing_ok=True)

    def test_protocol_errors(self):
        import re
        protocol = (SOURCE / 'protocol/errors.yaml').read_text(encoding='utf-8')
        names = {int(code): name for code, name in
                 re.findall(r'\{code:\s*(\d+),\s*name:\s*([A-Z_]+)', protocol)}
        self.assertTrue({10, 12, 13, 14} <= names.keys())
        cases = ['missing_stack', 'missing_sidecar', 'missing_reference', 'hash', 'conflicting_hash',
                 'magic', 'offsets', 'negative_offset', 'oversized_offset', 'short_ring', 'trailing', 'truncated', 'empty_sidecar', 'utf8',
                 'zero_dbu', 'nan_dbu', 'first_offset', 'empty_first_offset', 'case', 'missing_layer', 'duplicates',
                 'too_large', 'negative', 'unit', 'same_path', 'two_outlines', 'zero_outlines',
                 'zero_outline', 'zero_polygon', 'empty_json', 'invalid_json',
                 'deep_json', 'deep_copy', 'nan_json', 'infinity_json', 'negative_infinity_json', 'overflow_json',
                 'inline_two_outlines', 'inline_zero_outlines',
                 'missing_unit', 'missing_layers', 'missing_outline', 'bad_outline',
                 'empty_outline', 'polygon_outside_box', 'bad_box', 'bad_polygon', 'missing_refs',
                 'bad_refs', 'bad_ref', 'missing_ref_path', 'missing_ref_hash',
                 'bad_ref_path', 'empty_ref_path', 'bad_ref_hash',
                 'output_staging', 'report_staging']
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                data = polyset([('F.Cu', (RECT,)), ('Empty', ())])
                code = 14
                if case == 'magic': data = b'BADMAGIC' + data[8:]
                if case == 'offsets':
                    data = polyset([('F.Cu', (RECT, RECT))])
                    offsets_start = 20 + 4 + len('F.Cu'.encode('utf-8')) + 4
                    # Keep the valid two-ring layout; corrupt only the final offset.
                    data = (data[:offsets_start + 16] + struct.pack('<q', 3) +
                            data[offsets_start + 24:])
                if case == 'negative_offset': data = data[:40] + struct.pack('<q', -1) + data[48:]
                if case == 'oversized_offset': data = data[:40] + struct.pack('<q', 10000) + data[48:]
                if case == 'first_offset': data = data[:32] + struct.pack('<q', 1) + data[40:]
                if case == 'empty_first_offset':
                    data = polyset([('Empty', ())])
                    data = data[:-8] + struct.pack('<q', 1) + struct.pack('<qq', 0, 0)
                if case == 'short_ring': data = polyset([('F.Cu', (((0, 0), (1, 1)),))])
                if case == 'trailing': data += b'x'
                if case == 'truncated': data = data[:-1]
                if case == 'empty_sidecar': data = b''
                if case == 'utf8': data = data[:24] + b'\xff' + data[25:]
                if case == 'zero_dbu': data = polyset([('F.Cu', (RECT,))], 0)
                if case == 'nan_dbu': data = polyset([('F.Cu', (RECT,))], float('nan'))
                if case == 'duplicates': data = polyset([('F.Cu', (RECT,)), ('F.Cu', ())])
                if case == 'too_large': data = polyset([('F.Cu', (((0, 0), (20000, 0), (20000, 20000), (0, 20000)),))])
                if case == 'negative': data = polyset([('F.Cu', (tuple(reversed(RECT)),))])
                if case in ('two_outlines', 'zero_outlines', 'inline_two_outlines', 'inline_zero_outlines'):
                    data = polyset([('F.Cu', (RECT,)), ('outline', (RECT, RECT) if case.endswith('two_outlines') else ())])
                    code = 13
                stack = fixture(root, data)
                stack['layers'] = stack['layers'][:1]
                if case.startswith('inline_'):
                    stack['outline']['polygon'] = [[0, 0], [.01, 0], [.01, .005], [0, .005]]
                if case == 'missing_stack': code = 12
                if case == 'missing_sidecar':
                    (root / 'copper.polyset').unlink()
                    code = 12
                if case == 'missing_reference':
                    stack['external_refs'] = []
                    code = 12
                if case == 'hash':
                    stack['external_refs'][0]['sha256'] = '0' * 64
                    code = 10
                if case == 'conflicting_hash':
                    stack['external_refs'].append({'path': 'copper.polyset', 'sha256': '0' * 64})
                    code = 10
                if case in ('case', 'missing_layer'):
                    stack['layers'][0]['copper']['polygons']['layer'] = 'f.cu' if case == 'case' else 'absent'
                if case == 'unit': stack['unit'] = 'mm'
                if case == 'zero_polygon':
                    stack['outline']['polygon'] = [[0, 0], [.001, .001], [.002, .002]]
                if case == 'zero_outline': stack['outline']['box'] = [0, 0, 0, 0]
                if case in ('missing_unit', 'missing_layers', 'missing_outline', 'missing_refs'):
                    stack.pop({'missing_refs': 'external_refs'}.get(case, case.removeprefix('missing_')))
                    if case == 'missing_refs': code = 12
                if case == 'bad_outline': stack['outline'] = []
                if case == 'empty_outline': stack['outline'] = {}
                if case == 'polygon_outside_box':
                    stack['outline']['polygon'] = [[0, 0], [.03, 0], [0, .02]]
                if case == 'bad_box': stack['outline'] = {'box': [0, 1]}
                if case == 'bad_polygon': stack['outline'] = {'polygon': [None]}
                if case in ('bad_refs', 'bad_ref', 'missing_ref_path', 'missing_ref_hash',
                            'bad_ref_path', 'empty_ref_path', 'bad_ref_hash'):
                    if case == 'bad_refs': stack['external_refs'] = {}
                    if case == 'bad_ref': stack['external_refs'] = [None]
                    if case == 'missing_ref_path': stack['external_refs'][0].pop('path')
                    if case == 'missing_ref_hash': stack['external_refs'][0].pop('sha256')
                    if case in ('missing_ref_path', 'missing_ref_hash'): code = 12
                    if case == 'bad_ref_path': stack['external_refs'][0]['path'] = None
                    if case == 'empty_ref_path': stack['external_refs'][0]['path'] = ''
                    if case == 'bad_ref_hash': stack['external_refs'][0]['sha256'] = None
                source, output = root / 'input.json', root / 'output.json'
                source.write_text(json.dumps(stack))
                if case == 'empty_json': source.write_text('')
                if case == 'invalid_json': source.write_text('{bad')
                constants = {'nan_json': 'NaN', 'infinity_json': 'Infinity',
                             'negative_infinity_json': '-Infinity', 'overflow_json': '1e999'}
                if case in constants:
                    # Extension values also need strict JSON, even on the passthrough path.
                    stack['layers'] = []
                    source.write_text(json.dumps(stack)[:-1] + ', "x-number": ' + constants[case] + '}')
                if case in ('deep_json', 'deep_copy'):
                    depth = 2000 if case == 'deep_json' else 600
                    source.write_text('{"unit":"m","layers":[],"x-deep":' + '[' * depth + '0' + ']' * depth + '}')
                if case == 'missing_stack': source.unlink()
                before = source.read_bytes() if source.exists() else None
                if case == 'same_path': output = source
                extra = []
                if case == 'output_staging': output = root / 'absent' / 'output.json'
                if case == 'report_staging': extra = ['--report', str(root / 'absent' / 'report.json')]
                result = subprocess.run([sys.executable, str(SOURCE / 'tools/stack/copper_fraction.py'), str(source), '-o', str(output)] + extra, capture_output=True, text=True, cwd=root, timeout=5)
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertIn('copper_fraction: ' + names[code] + ':', result.stderr)
                self.assertEqual(len(result.stderr.splitlines()), 1)
                if case == 'offsets': self.assertIn('ring offsets must be monotonic', result.stderr)
                if case == 'same_path':
                    self.assertEqual(source.read_bytes(), before)
                else:
                    self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
