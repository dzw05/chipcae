"""Regressions for empty copper and transactional report failures."""
import hashlib
import os
import subprocess
import sys
import struct
from pathlib import Path
import tempfile
from unittest.mock import patch
from test_identical import board, save, diff_stacks, entry, number, polyset, write_report, EcadDiffError


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # A no-outline sidecar still requires a valid reference digest.
        data = b'CCPOLY01' + struct.pack('<dI', 1e-9, 0)
        (root / 'unused.polyset').write_bytes(data)
        for digest, code in ((None, 14), ('invalid', 14), ('0' * 64, 10)):
            unused = board()
            ref = {'path': 'unused.polyset'}
            if digest is not None:
                ref['sha256'] = digest
            unused['external_refs'] = [ref]
            path = save(root / 'unused.json', unused)
            try:
                diff_stacks(path, path)
            except EcadDiffError as exc:
                assert exc.code == code, (digest, exc)
            else:
                raise AssertionError('accepted invalid unused reference digest')
        # The fraction estimate exceeds the explicit copper perimeter here.
        fraction, polygon = board(), board()
        fraction['outline']['box'] = [0, 0, 0.1, 0.0005]
        polyset(root, polygon, width=9990000)
        pf = save(root / 'fraction.json', fraction)
        pp = save(root / 'polygon.json', polygon)
        for left, right in ((pf, pp), (pp, pf)):
            e = entry(diff_stacks(left, right, delta_m=1e-6),
                      'copper_area', '0:F.Cu|F.Cu')
            number(e['tol'], 1e-6 * 0.201)
            assert e['grade'] == 'info', e
        # Exercise the actual process exit rather than main() alone.
        environment = dict(os.environ)
        environment.pop('CHIPCAE_IMPORT_EXE', None)
        proc = subprocess.run([sys.executable, str(Path(__file__).with_name('test_import_pair.py'))],
                              env=environment, capture_output=True, text=True)
        assert proc.returncode == 77, (proc.returncode, proc.stderr)
        a, b = board(), board()
        a['layers'][0]['copper']['fraction'] = 0
        b['layers'][0]['copper']['fraction'] = 0.5
        result = diff_stacks(save(root / 'a.json', a), save(root / 'b.json', b))
        e = entry(result, 'copper_area', '0:F.Cu|F.Cu')
        assert e['grade'] == 'hard' and e['tol'] > 0
        data = (b'CCPOLY01' + struct.pack('<dII', 1e-9, 1, 4) + b'F.Cu'
                + struct.pack('<Iq', 0, 0))
        (root / 'empty.polyset').write_bytes(data)
        a['external_refs'] = [{'path': 'empty.polyset',
                               'sha256': hashlib.sha256(data).hexdigest()}]
        a['layers'][0]['copper'] = {'metal': 'mat.cu',
                                   'polygons': {'file': 'empty.polyset', 'layer': 'F.Cu'}}
        result = diff_stacks(save(root / 'a.json', a), save(root / 'b.json', b))
        e = entry(result, 'copper_area', '0:F.Cu|F.Cu')
        assert e['grade'] == 'hard' and e['tol'] > 0
        b['layers'][0]['copper']['fraction'] = 0
        both_empty = diff_stacks(save(root / 'a.json', a), save(root / 'b.json', b))
        e = entry(both_empty, 'copper_area', '0:F.Cu|F.Cu')
        assert e['grade'] == 'info' and e['tol'] > 0
        out = root / 'reports'
        for existing in (False, True):
            if existing:
                write_report(result, out)
            before = {p.name: p.read_bytes() for p in out.iterdir()} if out.exists() else {}
            original = Path.write_text

            def fail_markdown(path, *args, **kwargs):
                if path.name == 'diff.md':
                    raise OSError('injected Markdown write failure')
                return original(path, *args, **kwargs)

            with patch.object(Path, 'write_text', fail_markdown):
                try:
                    write_report(result, out)
                except EcadDiffError as exc:
                    assert exc.code == 14
                else:
                    raise AssertionError('report write unexpectedly succeeded')
            assert {p.name: p.read_bytes() for p in out.iterdir()} == before
            replace = Path.replace

            def fail_publish(path, target):
                if path.name == 'diff.md':
                    raise OSError('injected Markdown publication failure')
                return replace(path, target)

            with patch.object(Path, 'replace', fail_publish):
                try:
                    write_report(both_empty, out)
                except EcadDiffError as exc:
                    assert exc.code == 14
                else:
                    raise AssertionError('publication unexpectedly succeeded')
            assert {p.name: p.read_bytes() for p in out.iterdir()} == before


if __name__ == '__main__':
    main()
