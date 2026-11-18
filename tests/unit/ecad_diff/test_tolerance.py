"""Thresholds are inclusive; binary-exact inputs isolate the equality boundary."""
import copy
import math
from pathlib import Path
import tempfile
from test_identical import (AREA, DELTA, PERIMETER, board, diff_stacks, entry,
                            number, polyset, save, tolerance)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        a = board()
        # Check the internal default and an exactly representable equality boundary.
        for delta, thickness in ((DELTA, 1e-3), (2.0 ** -30, 2.0 ** -10)):
            a['layers'][1]['thickness'] = thickness
            pa = save(directory / 'a.json', a)
            assert entry(diff_stacks(pa, pa, delta), 'layers')['tol_derivation'] == 'identical'
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
        # Rounded endpoint addition equals y although the actual difference exceeds δ.
        a = board()
        a['layers'][1]['thickness'] = 1.0
        b = copy.deepcopy(a)
        b['layers'][1]['thickness'] = math.nextafter(1.0, math.inf)
        delta = math.nextafter(b['layers'][1]['thickness'] - 1.0, 0.0)
        for left, right in ((a, b), (b, a)):
            e = entry(diff_stacks(save(directory / 'a.json', left),
                                  save(directory / 'b.json', right), delta),
                      'thickness', '1:core|core')
            assert e['delta'] > delta and e['grade'] == 'hard'
        # Fraction area uncertainty is f times the outline area uncertainty.
        a, b = board(), board()
        a['layers'][0]['copper']['fraction'] = 0.25
        b['layers'][0]['copper']['fraction'] = 0.2500002
        for left, right in ((a, b), (b, a)):
            e = entry(diff_stacks(save(directory / 'a.json', left),
                                  save(directory / 'b.json', right)),
                      'copper_area', '0:F.Cu|F.Cu')
            tolerance(e, DELTA, 0.2500002 * PERIMETER)
            assert e['grade'] == 'discuss'
            assert 'no copper perimeter inferred' in e['tol_derivation']
        # The next representable thickness beyond the boundary must still fail.
        a = board()
        pa = save(directory / 'a.json', a)
        b = copy.deepcopy(a)
        b['layers'][1]['thickness'] = math.nextafter(
            a['layers'][1]['thickness'] + DELTA, math.inf)
        for left, right in ((pa, save(directory / 'b.json', b)),
                            (directory / 'b.json', pa)):
            assert entry(diff_stacks(left, right), 'thickness',
                         '1:core|core')['grade'] == 'hard'
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
            assert 'area tolerance δ × P' in e['tol_derivation']
            assert 'box coordinate tolerance δ' in e['tol_derivation']
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
            tolerance(e, DELTA, max(PERIMETER, 2 * (width * 1e-9 + 0.005)))
        b = copy.deepcopy(a)
        b['layers'][0]['name'] = 'f.cu'
        r = diff_stacks(pa, save(directory / 'b.json', b))
        assert entry(r, 'layers')['grade'] == 'info'
        assert entry(r, 'layers')['tol_derivation'] == 'layer names differ'
        assert entry(r, 'thickness', '0:F.Cu|f.cu')['grade'] == 'info'


if __name__ == '__main__':
    main()
