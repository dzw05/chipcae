"""Outline-only sidecars supply geometry and receive reference validation."""
import hashlib
from pathlib import Path
import struct
import tempfile
from test_identical import EcadDiffError, board, diff_stacks, entry, number, save


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        a = board()
        a['outline'] = {'polygon': [[0, 0], [0.01, 0], [0, 0.005]]}
        pa = save(directory / 'a.json', a)
        points = [(0, 0), (10000000, 0), (0, 5000000)]
        data = (b'CCPOLY01' + struct.pack('<dII', 1e-9, 1, 7) + b'outline'
                + struct.pack('<Iqq', 1, 0, 3)
                + b''.join(struct.pack('<qq', *p) for p in points))
        sidecar = directory / 'outline.polyset'
        sidecar.write_bytes(data)
        b = board()
        b['external_refs'] = [{'path': sidecar.name,
                               'sha256': hashlib.sha256(data).hexdigest()}]
        pb = directory / 'b.json'
        # The triangle must override its rectangular box, and also work without a box.
        for outline in ({'box': [0, 0, 0.01, 0.005]}, {}):
            b['outline'] = outline
            result = diff_stacks(pa, save(pb, b))
            assert result['summary']['hard'] == 0
            e = entry(result, 'outline')
            assert e['grade'] == 'info'
            number(e['ipc']['area'], 0.000025)
        for case, code in (('hash', 10), ('truncated', 14), ('missing', 12),
                           ('path', 14)):
            sidecar.write_bytes(data)
            b['external_refs'][0] = {'path': sidecar.name,
                                     'sha256': hashlib.sha256(data).hexdigest()}
            if case == 'hash':
                b['external_refs'][0]['sha256'] = '0' * 64
            elif case == 'truncated':
                sidecar.write_bytes(data[:-1])
                b['external_refs'][0]['sha256'] = hashlib.sha256(data[:-1]).hexdigest()
            elif case == 'missing':
                sidecar.unlink()
            else:
                b['external_refs'][0]['path'] = '../outline.polyset'
            try:
                diff_stacks(pa, save(pb, b))
            except EcadDiffError as exc:
                assert exc.code == code, (case, exc)
            else:
                raise AssertionError(f'{case}: accepted invalid outline sidecar')


if __name__ == '__main__':
    main()
