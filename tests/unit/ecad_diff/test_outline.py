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
        for filename in ('outline.polyset', 'outline.bin', 'outline'):
            sidecar = directory / filename
            sidecar.write_bytes(data)
            b = board()
            b['external_refs'] = [{'path': sidecar.name,
                                   'sha256': hashlib.sha256(data).hexdigest()}]
            # An unrelated sidecar does not affect outline discovery.
            unrelated = directory / 'unused.polyset'
            unrelated.write_bytes(b'CCPOLY01' + struct.pack('<dI', 1e-9, 0))
            b['external_refs'].append({'path': unrelated.name,
                                       'sha256': hashlib.sha256(unrelated.read_bytes()).hexdigest()})
            pb = directory / 'b.json'
            # The triangle must override its rectangular box, and also work without a box.
            for outline in ({'box': [0, 0, 0.01, 0.005]}, {}):
                b['outline'] = outline
                result = diff_stacks(pa, save(pb, b))
                assert result['summary']['hard'] == 0
                e = entry(result, 'outline')
                assert e['grade'] == 'info'
                number(e['ipc']['area'], 0.000025)
            # An explicit rectangle wins over the sidecar triangle, including for fractions.
            b['outline'] = board()['outline']
            b['outline']['polygon'] = [[0, 0], [0.01, 0], [0.01, 0.005], [0, 0.005]]
            rectangle = save(directory / 'rectangle.json', board())
            result = diff_stacks(rectangle, save(pb, b))
            assert result['summary']['hard'] == 0
            assert result['summary']['discuss'] == 0
            number(entry(result, 'outline')['ipc']['area'], 0.00005)
            number(entry(result, 'copper_area', '0:F.Cu|F.Cu')['ipc'], 0.00005)
            # Multiple distinct outline sources are ambiguous in either reference order.
            # Repeated references to one file still represent a single source.
            sidecar.write_bytes(data)
            second = directory / 'second.polyset'
            second.write_bytes(data.replace(struct.pack('<q', 10000000),
                                            struct.pack('<q', 8000000)))
            first_ref = dict(b['external_refs'][0])
            second_ref = {'path': second.name,
                          'sha256': hashlib.sha256(second.read_bytes()).hexdigest()}
            for refs in ([first_ref, second_ref], [second_ref, first_ref]):
                b['external_refs'] = refs
                b['outline'] = rectangle_outline = board()['outline']
                for left, right in ((pa, pb), (pb, pa)):
                    save(pb, b)
                    try:
                        diff_stacks(left, right)
                    except EcadDiffError as exc:
                        assert exc.code == 14 and exc.name == 'INPUT_INVALID'
                    else:
                        raise AssertionError('accepted ambiguous outline sources')
                rectangle_outline['polygon'] = [[0, 0], [0.01, 0],
                                                 [0.01, 0.005], [0, 0.005]]
                assert diff_stacks(rectangle, save(pb, b))['summary']['hard'] == 0
            b['external_refs'] = [first_ref, dict(first_ref)]
            b['outline'] = {}
            assert diff_stacks(pa, save(pb, b))['summary']['hard'] == 0
            b['external_refs'] = [first_ref]
            b['outline'] = board()['outline']
            b['outline']['polygon'] = [[0, 0], [0.01, 0], [0.01, 0.005], [0, 0.005]]
            # Even an empty outline layer is ignored when outline.polygon is explicit.
            empty = (b'CCPOLY01' + struct.pack('<dII', 1e-9, 1, 7) + b'outline'
                     + struct.pack('<Iq', 0, 0))
            sidecar.write_bytes(empty)
            b['external_refs'][0]['sha256'] = hashlib.sha256(empty).hexdigest()
            assert diff_stacks(rectangle, save(pb, b))['summary']['hard'] == 0
            b['layers'][0]['copper'] = {'metal': 'mat.cu',
                'polygons': {'file': sidecar.name, 'layer': 'outline'}}
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
                    b['external_refs'][0]['path'] = f'../{filename}'
                    b['layers'][0]['copper']['polygons']['file'] = f'../{filename}'
                try:
                    diff_stacks(pa, save(pb, b))
                except EcadDiffError as exc:
                    assert exc.code == code, (case, exc)
                else:
                    raise AssertionError(f'{case}: accepted invalid outline sidecar')


if __name__ == '__main__':
    main()
