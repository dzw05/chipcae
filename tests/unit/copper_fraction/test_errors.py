"""Every rejected input must fail with its protocol code, before writing."""
import unittest
from test_fraction import *


class Errors(unittest.TestCase):
    def test_protocol_errors(self):
        names = {10: 'INPUT_HASH_MISMATCH', 12: 'INPUT_REFERENCE_MISSING', 13: 'CUT_UNSUPPORTED', 14: 'INPUT_INVALID'}
        cases = ['missing_stack', 'missing_sidecar', 'missing_reference', 'hash', 'conflicting_hash',
                 'magic', 'offsets', 'negative_offset', 'oversized_offset', 'short_ring', 'trailing', 'truncated', 'empty_sidecar', 'utf8',
                 'zero_dbu', 'nan_dbu', 'first_offset', 'case', 'missing_layer', 'duplicates',
                 'too_large', 'negative', 'unit', 'same_path', 'two_outlines', 'zero_outlines',
                 'zero_outline', 'empty_json', 'invalid_json',
                 'missing_unit', 'missing_layers', 'missing_outline', 'bad_outline',
                 'empty_outline', 'bad_box', 'bad_polygon', 'missing_refs',
                 'bad_refs', 'bad_ref', 'missing_ref_path', 'missing_ref_hash']
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                data = polyset([('F.Cu', (RECT,)), ('Empty', ())])
                code = 14
                if case == 'magic': data = b'BADMAGIC' + data[8:]
                if case == 'offsets':
                    data = polyset([('F.Cu', (RECT[:3],))])
                    data = data[:32] + struct.pack('<qqq', 0, 4, 3) + data[48:]
                    # ring_count follows the four-byte F.Cu name.
                    data = data[:28] + struct.pack('<I', 2) + data[32:]
                if case == 'negative_offset': data = data[:40] + struct.pack('<q', -1) + data[48:]
                if case == 'oversized_offset': data = data[:40] + struct.pack('<q', 10000) + data[48:]
                if case == 'first_offset': data = data[:32] + struct.pack('<q', 1) + data[40:]
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
                if case in ('two_outlines', 'zero_outlines'):
                    data = polyset([('F.Cu', (RECT,)), ('outline', (RECT, RECT) if case == 'two_outlines' else ())])
                    code = 13
                stack = fixture(root, data)
                stack['layers'] = stack['layers'][:1]
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
                if case == 'zero_outline': stack['outline']['box'] = [0, 0, 0, 0]
                if case in ('missing_unit', 'missing_layers', 'missing_outline', 'missing_refs'):
                    stack.pop({'missing_refs': 'external_refs'}.get(case, case.removeprefix('missing_')))
                    code = 12
                if case == 'bad_outline': stack['outline'] = []
                if case == 'empty_outline': stack['outline'] = {}
                if case == 'bad_box': stack['outline'] = {'box': [0, 1]}
                if case == 'bad_polygon': stack['outline'] = {'polygon': [None]}
                if case in ('bad_refs', 'bad_ref', 'missing_ref_path', 'missing_ref_hash'):
                    if case == 'bad_refs': stack['external_refs'] = {}
                    if case == 'bad_ref': stack['external_refs'] = [None]
                    if case == 'missing_ref_path': stack['external_refs'][0].pop('path')
                    if case == 'missing_ref_hash': stack['external_refs'][0].pop('sha256')
                    code = 12
                source, output = root / 'input.json', root / 'output.json'
                source.write_text(json.dumps(stack))
                if case == 'empty_json': source.write_text('')
                if case == 'invalid_json': source.write_text('{bad')
                if case == 'missing_stack': source.unlink()
                before = source.read_bytes() if source.exists() else None
                if case == 'same_path': output = source
                result = subprocess.run([sys.executable, str(SOURCE / 'tools/stack/copper_fraction.py'), str(source), '-o', str(output)], capture_output=True, text=True, cwd=root, timeout=5)
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertIn('copper_fraction: ' + names[code] + ':', result.stderr)
                self.assertEqual(len(result.stderr.splitlines()), 1)
                if case == 'same_path':
                    self.assertEqual(source.read_bytes(), before)
                else:
                    self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
