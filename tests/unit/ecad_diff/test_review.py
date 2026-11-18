"""Regressions for empty copper and transactional report failures."""
import hashlib
import json
import os
import subprocess
import sys
import struct
from pathlib import Path
import tempfile
from unittest.mock import patch
from test_identical import board, save, diff_stacks, entry, number, polyset, write_report, EcadDiffError, DELTA, PERIMETER


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # A copper sidecar still requires a valid reference digest.
        data = (b'CCPOLY01' + struct.pack('<dII', 1e-9, 1, 4) + b'F.Cu'
                + struct.pack('<Iq', 0, 0))
        (root / 'unused.polyset').write_bytes(data)
        for digest, code in ((None, 12), ('invalid', 14), ('0' * 64, 10)):
            unused = board()
            ref = {'path': 'unused.polyset'}
            if digest is not None:
                ref['sha256'] = digest
            unused['external_refs'] = [ref]
            unused['layers'][0]['copper'] = {'metal': 'mat.cu',
                'polygons': {'file': 'unused.polyset', 'layer': 'F.Cu'}}
            path = save(root / 'unused.json', unused)
            try:
                diff_stacks(path, path)
            except EcadDiffError as exc:
                assert exc.code == code, (digest, exc)
            else:
                raise AssertionError('accepted invalid copper reference digest')
        # The propagated fraction area-error coefficient exceeds the polygon perimeter.
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
        # Absent copper retains the other side's perimeter and 100% mismatch.
        absent, present = board(), board()
        del absent['layers'][0]['copper']
        for representation in ('fraction', 'polygons'):
            if representation == 'polygons':
                polyset(root, present)
            pa = save(root / 'absent.json', absent)
            pb = save(root / 'present.json', present)
            for left, right in ((pa, pb), (pb, pa)):
                e = entry(diff_stacks(left, right), 'copper_area', '0:F.Cu|F.Cu')
                assert e['grade'] == 'hard'
                number(e['delta'], max(e['odb'], e['ipc']))
                number(e['tol'], DELTA * PERIMETER)
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
        assert e['grade'] == 'info' and e['tol'] == 0
        # Individually finite positive thicknesses may exceed float range in sum.
        huge = board()
        for layer in huge['layers']:
            layer['thickness'] = 1e308
        huge_path = save(root / 'huge.json', huge)
        huge_result = diff_stacks(huge_path, huge_path)
        total = entry(huge_result, 'total_thickness')
        assert total['grade'] == 'info' and total['delta'] == 0
        assert total['odb'] == total['ipc'] == 3 * int(1e308)
        write_report(huge_result, root / 'huge-reports')
        assert json.loads((root / 'huge-reports/diff.json').read_text()) == huge_result
        normal_path = save(root / 'normal.json', board())
        total = entry(diff_stacks(huge_path, normal_path), 'total_thickness')
        assert total['grade'] == 'hard' and total['delta'] == 3 * int(1e308)
        # Cancellation must happen before rounding overflowing totals for JSON.
        huge['layers'][2]['thickness'] = 1e-8
        huge_path = save(root / 'huge.json', huge)
        other = board()
        for layer in other['layers']:
            layer['thickness'] = 1e308
        other['layers'][2]['thickness'] = 2e-8
        other_path = save(root / 'other.json', other)
        for left, right in ((huge_path, other_path), (other_path, huge_path)):
            total = entry(diff_stacks(left, right), 'total_thickness')
            assert total['grade'] == 'hard' and total['delta'] == 1e-8
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
            assert out.exists() == existing
            assert ({p.name: p.read_bytes() for p in out.iterdir()} if out.exists() else {}) == before
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
            assert out.exists() == existing
            assert ({p.name: p.read_bytes() for p in out.iterdir()} if out.exists() else {}) == before


if __name__ == '__main__':
    main()
