"""Regressions for empty copper and transactional report failures."""
import hashlib
import struct
from pathlib import Path
import tempfile
from unittest.mock import patch
from test_identical import board, save, diff_stacks, entry, write_report, EcadDiffError


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
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
