"""API codes and CLI diagnostics must agree and never leave a report."""
import copy
import hashlib
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
from test_identical import ROOT, EcadDiffError, board, diff_stacks, polyset, save, write_report


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        good = save(directory / 'good.json', board())
        for case, code, name in (
                ('missing_stack', 12, 'INPUT_REFERENCE_MISSING'),
                ('directory_stack', 12, 'INPUT_REFERENCE_MISSING'),
                ('missing_polyset', 12, 'INPUT_REFERENCE_MISSING'),
                ('symlink_file', 14, 'INPUT_INVALID'),
                ('symlink_directory', 14, 'INPUT_INVALID'),
                ('hash', 10, 'INPUT_HASH_MISMATCH'),
                ('duplicate_hash', 10, 'INPUT_HASH_MISMATCH'),
                ('duplicate_digest', 14, 'INPUT_INVALID'),
                ('relative_nonfinite', 14, 'INPUT_INVALID'),
                ('trailing', 14, 'INPUT_INVALID'),
                *[(case, 14, 'INPUT_INVALID') for case in (
                    'used_magic', 'used_truncated', 'used_dbu',
                    'used_offsets', 'used_negative', 'used_terminal',
                    'used_zero_terminal', 'used_trailing', 'used_duplicate', 'used_utf8')],
                ('delta', 14, 'INPUT_INVALID'),
                ('delta_nan', 14, 'INPUT_INVALID'),
                ('delta_inf', 14, 'INPUT_INVALID'),
                ('missing_polygon_layer', 14, 'INPUT_INVALID'),
                ('derived_nonfinite', 14, 'INPUT_INVALID'),
                ('copper_nonfinite', 14, 'INPUT_INVALID'),
                *[(case, 14, 'INPUT_INVALID') for case in (
                    'empty', 'root', 'missing_unit', 'missing_layers', 'missing_outline',
                    'bad_layers', 'bad_outline', 'bad_layer', 'missing_thickness',
                    'missing_material', 'bad_copper', 'missing_metal', 'bad_polygons',
                    'missing_polygon_file', 'bad_ref', 'missing_representation',
                    'both_representations')],
                ('unit', 14, 'INPUT_INVALID'),
                ('utf8', 14, 'INPUT_INVALID'),
                ('output_file', 14, 'INPUT_INVALID')):
            folder = directory / case
            folder.mkdir()
            b = copy.deepcopy(board())
            path = folder / 'stack.json'
            out = folder / 'reports'
            delta = {'delta': 0, 'delta_nan': float('nan'),
                     'delta_inf': float('inf')}.get(case, 1e-9)
            if case == 'derived_nonfinite':
                # Finite inputs can overflow δ × P without invalid geometry.
                delta = 1e308
                b['outline']['box'] = [0, 0, 1, 1]
            if case == 'relative_nonfinite':
                # A finite clockwise ring area divided by the other side's tiny
                # positive area overflows only the derived relative difference.
                data = (b'CCPOLY01' + struct.pack('<dII', 5e153, 1, 4) + b'F.Cu'
                        + struct.pack('<Iqq', 1, 0, 4)
                        + struct.pack('<qqqqqqqq', 0, 0, 0, 1, 1, 1, 1, 0))
                (folder / 'huge.polyset').write_bytes(data)
                b['external_refs'] = [{'path': 'huge.polyset',
                                       'sha256': hashlib.sha256(data).hexdigest()}]
                b['layers'][0]['copper'] = {'metal': 'mat.cu',
                    'polygons': {'file': 'huge.polyset', 'layer': 'F.Cu'}}
            if case == 'copper_nonfinite':
                # Each finite ring has area 2.5e307; eight rings overflow the sum.
                data = (b'CCPOLY01' + struct.pack('<dII', 5e153, 1, 4) + b'F.Cu'
                        + struct.pack('<I9q', 8, *range(0, 33, 4))
                        + struct.pack('<qqqqqqqq', 0, 0, 1, 0, 1, 1, 0, 1) * 8)
                (folder / 'huge.polyset').write_bytes(data)
                b['external_refs'] = [{'path': 'huge.polyset',
                                       'sha256': hashlib.sha256(data).hexdigest()}]
                b['layers'][0]['copper'] = {'metal': 'mat.cu',
                    'polygons': {'file': 'huge.polyset', 'layer': 'F.Cu'}}
            if case in ('missing_polyset', 'hash', 'trailing',
                        'duplicate_hash', 'duplicate_digest', 'missing_polygon_layer'):
                p = polyset(folder, b, trailing=b'x' if case == 'trailing' else b'')
                if case == 'missing_polyset':
                    p.unlink()
                if case == 'missing_polygon_layer':
                    b['layers'][0]['copper']['polygons']['layer'] = 'absent'
                if case in ('duplicate_hash', 'duplicate_digest'):
                    ref = dict(b['external_refs'][0])
                    ref['sha256'] = '0' * 64 if case == 'duplicate_hash' else 'invalid'
                    b['external_refs'].append(ref)
                if case == 'hash':
                    b['external_refs'][0]['sha256'] = '0' * 64
            if case.startswith('symlink_'):
                outside = directory / (case + '_outside')
                outside.mkdir()
                target = polyset(outside, b)
                if case == 'symlink_directory':
                    (folder / 'linked').symlink_to(outside, target_is_directory=True)
                    reference = 'linked/' + target.name
                else:
                    (folder / target.name).symlink_to(target)
                    reference = target.name
                b['external_refs'][0]['path'] = reference
                b['layers'][0]['copper']['polygons']['file'] = reference
            if case.startswith('used_'):
                header = b'CCPOLY01' + struct.pack('<dI', 1e-9, 0)
                layer = (struct.pack('<I', 4) + b'F.Cu' + struct.pack('<Iqq', 1, 0, 3)
                         + struct.pack('<qqqqqq', 0, 0, 1, 0, 0, 1))
                malformed = {
                    'used_utf8': (b'CCPOLY01' + struct.pack('<dI', 1e-9, 1)
                                  + layer.replace(b'F.Cu', b'F.\xffu')),
                    'used_magic': b'BADMAGIC' + header[8:],
                    'used_truncated': header[:-1],
                    'used_dbu': b'CCPOLY01' + struct.pack('<dI', 0, 0),
                    'used_offsets': (b'CCPOLY01' + struct.pack('<dI', 1e-9, 1)
                                       + layer[:12] + struct.pack('<qq', 0, 2)
                                       + struct.pack('<qqqq', 0, 0, 1, 0)),
                    'used_negative': (b'CCPOLY01' + struct.pack('<dI', 1e-9, 1)
                                        + layer[:12] + struct.pack('<qq', 0, -3)),
                    'used_terminal': (b'CCPOLY01' + struct.pack('<dI', 1e-9, 1)
                                        + layer[:12] + struct.pack('<qq', 0, 4)),
                    'used_zero_terminal': (b'CCPOLY01' + struct.pack('<dI', 1e-9, 1)
                                             + layer[:8] + struct.pack('<Iq', 0, -1)),
                    'used_trailing': header + b'x',
                    'used_duplicate': (b'CCPOLY01' + struct.pack('<dI', 1e-9, 2)
                                         + layer + layer),
                }[case]
                (folder / 'unused.polyset').write_bytes(malformed)
                b['external_refs'] = [{'path': 'unused.polyset',
                                       'sha256': hashlib.sha256(malformed).hexdigest()}]
                b['layers'][0]['copper'] = {'metal': 'mat.cu',
                    'polygons': {'file': 'unused.polyset', 'layer': 'F.Cu'}}
            if case == 'unit':
                b['unit'] = 'mm'
            if case == 'root':
                b = []
            if case.startswith('missing_') and case[8:] in ('unit', 'layers', 'outline'):
                del b[case[8:]]
            if case == 'bad_layers':
                b['layers'] = {}
            if case == 'bad_outline':
                b['outline'] = []
            if case == 'bad_layer':
                b['layers'][0] = []
            if case in ('missing_thickness', 'missing_material'):
                del b['layers'][0][case[8:]]
            if case == 'bad_copper':
                b['layers'][0]['copper'] = []
            if case == 'missing_metal':
                del b['layers'][0]['copper']['metal']
            if case in ('bad_polygons', 'missing_polygon_file'):
                b['layers'][0]['copper'] = {
                    'metal': 'mat.cu', 'polygons': [] if case == 'bad_polygons' else {}}
            if case == 'missing_representation':
                del b['layers'][0]['copper']['fraction']
            if case == 'both_representations':
                b['layers'][0]['copper']['polygons'] = {
                    'file': 'missing.polyset', 'layer': 'F.Cu'}
            if case == 'bad_ref':
                b['external_refs'] = [None]
            if case != 'missing_stack':
                save(path, b)
            if case == 'directory_stack':
                path.unlink()
                path.mkdir()
            if case == 'empty':
                path.write_bytes(b'')
            if case == 'utf8':
                path.write_bytes(b'{"unit": "m", "bad": "\xff"}')
            if case == 'output_file':
                out.write_text('existing file', encoding='utf-8')
            try:
                result = diff_stacks(good, path, delta)
                assert case not in ('derived_nonfinite', 'copper_nonfinite'), (case, result)
                write_report(result, out)
            except EcadDiffError as exc:
                assert exc.code == code and exc.name == name, (case, exc)
                if case == 'used_utf8':
                    assert str(exc) == 'invalid polyset utf-8'
                if case in ('missing_stack', 'directory_stack'):
                    assert f'cannot read stack: {path}:' in str(exc)
                if case == 'missing_polyset':
                    assert f'cannot read polyset: {p}:' in str(exc)
            else:
                raise AssertionError(f'{case}: API accepted invalid input')
            command = [sys.executable, str(ROOT / 'tools/vv/ecad_diff.py'),
                       '--odb', str(good), '--ipc', str(path), '-o', str(out),
                       '--delta-m', str(delta)]
            proc = subprocess.run(command, capture_output=True, text=True)
            assert proc.returncode == code, (case, proc.stderr)
            assert len(proc.stderr.splitlines()) == 1, proc.stderr
            assert proc.stderr.startswith(f'ecad_diff: {name}: '), proc.stderr
            if case in ('missing_stack', 'directory_stack'):
                assert f'cannot read stack: {path}:' in proc.stderr
            if case == 'missing_polyset':
                assert f'cannot read polyset: {p}:' in proc.stderr
            if case == 'used_utf8':
                assert proc.stderr == 'ecad_diff: INPUT_INVALID: invalid polyset utf-8\n'
            assert not (out / 'diff.json').exists()
            assert not (out / 'diff.md').exists()
            if case == 'output_file':
                assert out.read_text(encoding='utf-8') == 'existing file'
                # Output validation precedes missing, malformed and hash-invalid inputs.
                for bad_input in (folder / 'missing.json', path):
                    for contents in (b'', b'not JSON'):
                        path.write_bytes(contents)
                        command[command.index('--ipc') + 1] = str(bad_input)
                        proc = subprocess.run(command, capture_output=True, text=True)
                        assert proc.returncode == 14, proc.stderr
                        assert proc.stderr.startswith('ecad_diff: INPUT_INVALID: '), proc.stderr
                        assert 'output' in proc.stderr, proc.stderr
                        assert len(proc.stderr.splitlines()) == 1, proc.stderr
                        assert out.read_text(encoding='utf-8') == 'existing file'
                polyset(folder, b)
                b['external_refs'][0]['sha256'] = '0' * 64
                save(path, b)
                command[command.index('--ipc') + 1] = str(path)
                proc = subprocess.run(command, capture_output=True, text=True)
                assert proc.returncode == 14 and 'output' in proc.stderr, proc.stderr
            else:
                assert not out.exists()

        out = directory / 'nonnumeric_reports'
        proc = subprocess.run(
            [sys.executable, str(ROOT / 'tools/vv/ecad_diff.py'),
             '--odb', str(good), '--ipc', str(good), '-o', str(out),
             '--delta-m', 'not-a-number'], capture_output=True, text=True)
        assert proc.returncode == 14, proc.stderr
        assert proc.stderr == (
            'ecad_diff: INPUT_INVALID: delta must be a finite positive number\n')
        assert not out.exists()

        # Without an explicit polygon, every reference is consulted for outline
        # discovery and must pass the same validation as copper sidecars.
        for kind, code, name in (
                ('missing', 12, 'INPUT_REFERENCE_MISSING'),
                ('malformed', 14, 'INPUT_INVALID'),
                ('symlink', 14, 'INPUT_INVALID'),
                ('directory_symlink', 14, 'INPUT_INVALID'),
                ('hash', 10, 'INPUT_HASH_MISMATCH'),
                ('valid', 0, None)):
            folder = directory / ('discovery_' + kind)
            folder.mkdir()
            b = board()
            fixture = board()
            sidecar = polyset(folder, fixture)
            data = sidecar.read_bytes()
            reference = sidecar.name
            digest = hashlib.sha256(data).hexdigest()
            if kind == 'missing':
                sidecar.unlink()
            elif kind == 'malformed':
                sidecar.write_bytes(b'not a polyset')
                digest = hashlib.sha256(sidecar.read_bytes()).hexdigest()
            elif kind in ('symlink', 'directory_symlink'):
                outside = directory / (kind + '_discovery_outside')
                outside.mkdir()
                target = polyset(outside, board())
                sidecar.unlink()
                if kind == 'symlink':
                    sidecar.symlink_to(target)
                else:
                    (folder / 'linked').symlink_to(outside, target_is_directory=True)
                    reference = 'linked/' + target.name
            elif kind == 'hash':
                digest = '0' * 64
            b['external_refs'] = [{'path': reference, 'sha256': digest}]
            path = save(folder / 'stack.json', b)
            out = folder / 'reports'
            try:
                result = diff_stacks(good, path)
            except EcadDiffError as exc:
                assert code and (exc.code, exc.name) == (code, name), (kind, exc)
            else:
                assert code == 0, (kind, result)
                assert result['summary']['hard'] == result['summary']['discuss'] == 0
            command = [sys.executable, str(ROOT / 'tools/vv/ecad_diff.py'),
                       '--odb', str(good), '--ipc', str(path), '-o', str(out)]
            proc = subprocess.run(command, capture_output=True, text=True)
            assert proc.returncode == code, (kind, proc.stderr)
            if code:
                assert len(proc.stderr.splitlines()) == 1, proc.stderr
                assert proc.stderr.startswith(f'ecad_diff: {name}: '), proc.stderr
                assert not out.exists()
            else:
                assert (out / 'diff.json').exists() and (out / 'diff.md').exists()
            # An explicit outline does not consult unrelated references.
            b['outline']['polygon'] = [[0, 0], [0.01, 0], [0.01, 0.005], [0, 0.005]]
            save(path, b)
            result = diff_stacks(good, path)
            assert result['summary']['hard'] == result['summary']['discuss'] == 0

        for kind in ('file', 'directory', 'dangling'):
            target = directory / ('output_target_' + kind)
            if kind == 'file':
                target.write_bytes(b'preserve existing file')
            elif kind == 'directory':
                target.mkdir()
                (target / 'sentinel').write_bytes(b'preserve directory')
            out = directory / ('output_symlink_' + kind)
            out.symlink_to(target, target_is_directory=kind == 'directory')
            try:
                write_report(diff_stacks(good, good), out)
            except EcadDiffError as exc:
                assert exc.code == 14 and exc.name == 'INPUT_INVALID', exc
            else:
                raise AssertionError(f'{kind}: API accepted output symlink')
            # Reject the output before trying even a missing or malformed input.
            for input_path in (good, directory / 'missing.json', path):
                path.write_bytes(b'not JSON')
                proc = subprocess.run(
                    [sys.executable, str(ROOT / 'tools/vv/ecad_diff.py'),
                     '--odb', str(good), '--ipc', str(input_path), '-o', str(out)],
                    capture_output=True, text=True)
                assert proc.returncode == 14, (kind, proc.stderr)
                assert len(proc.stderr.splitlines()) == 1, proc.stderr
                assert proc.stderr.startswith('ecad_diff: INPUT_INVALID: '), proc.stderr
                assert 'output' in proc.stderr, proc.stderr
            assert out.is_symlink() and out.readlink() == target
            if kind == 'file':
                assert target.read_bytes() == b'preserve existing file'
            elif kind == 'directory':
                assert list(target.iterdir()) == [target / 'sentinel']
                assert (target / 'sentinel').read_bytes() == b'preserve directory'
            else:
                assert not target.exists()


if __name__ == '__main__':
    main()
