"""Thresholds are inclusive; binary-exact inputs isolate the equality boundary."""
import copy
from pathlib import Path
import tempfile
from test_identical import (AREA, DELTA, PERIMETER, board, diff_stacks, entry,
                            number, polyset, save, tolerance)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        a = board()
        # Check the public default and an exactly representable equality boundary.
        for delta, thickness in ((DELTA, 1e-3), (2.0 ** -30, 2.0 ** -10)):
            a['layers'][1]['thickness'] = thickness
            pa = save(directory / 'a.json', a)
            for scale, grade in ((0.5, 'info'), (1, 'info'), (2, 'hard')):
                b = copy.deepcopy(a)
                b['layers'][1]['thickness'] += scale * delta
                r = diff_stacks(pa, save(directory / 'b.json', b), delta)
                e = entry(r, 'thickness', '1:core|core')
                assert e['grade'] == grade
                number(e['odb'], thickness)
                number(e['ipc'], b['layers'][1]['thickness'])
                number(e['delta'], abs(b['layers'][1]['thickness'] - thickness))
                if delta != DELTA:
                    number(e['delta'], scale * delta)
                tolerance(e, delta)
                total = entry(r, 'total_thickness')
                assert total['grade'] == grade
                tolerance(total, delta)
        for polygon, grade in (([[0, 0], [0.01 + 2 * DELTA, 0],
                                  [0.01, 0.005], [0, 0.005]], 'hard'),
                                ([[0, 0], [0.005, 0], [0.01, 0],
                                  [0.01, 0.005], [0, 0.005]], 'discuss')):
            b = copy.deepcopy(a)
            b['outline']['polygon'] = polygon
            e = entry(diff_stacks(pa, save(directory / 'b.json', b)), 'outline')
            assert e['grade'] == grade
            p = sum(((polygon[i][0] - polygon[i-1][0]) ** 2 +
                     (polygon[i][1] - polygon[i-1][1]) ** 2) ** 0.5
                    for i in range(len(polygon)))
            tolerance(e, DELTA, max(PERIMETER, p))
            assert 'arcs already discretized by readers; e_chord not included' in e['tol_derivation']
        for width, grade in ((9950000, 'discuss'), (9800000, 'hard')):
            b = copy.deepcopy(a)
            polyset(directory, b, width)
            e = entry(diff_stacks(pa, save(directory / 'b.json', b)),
                      'copper_area', '0:F.Cu|F.Cu')
            assert e['grade'] == grade
            number(e['odb'], AREA)
            number(e['ipc'], width * 1e-9 * 0.005)
            number(e['delta'], AREA - width * 1e-9 * 0.005)
            tolerance(e, DELTA, 2 * (width * 1e-9 + 0.005))
        b = copy.deepcopy(a)
        b['layers'][0]['name'] = 'f.cu'
        r = diff_stacks(pa, save(directory / 'b.json', b))
        assert entry(r, 'layers')['grade'] == 'info'
        assert entry(r, 'layers')['tol_derivation'] == 'layer names differ'
        assert entry(r, 'thickness', '0:F.Cu|f.cu')['grade'] == 'info'


if __name__ == '__main__':
    main()
