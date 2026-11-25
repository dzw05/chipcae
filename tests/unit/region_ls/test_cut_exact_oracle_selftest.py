"""Hand-derived checks for the independent oracle; no compiled products required."""
import math
import sys
import tempfile
from pathlib import Path
import time
import unittest
from fractions import Fraction as F

import cut_exact_oracle as oracle
import cut_exact_oracle_compare as compare
from cut_exact_oracle_cases import cases, validate_case


class ExactOracleTests(unittest.TestCase):
    def test_clipped_square_moments(self):
        p = oracle.clip_halfplane([(F(0), F(0)), (F(1), F(0)),
                                  (F(1), F(1)), (F(0), F(1))], F(1), F(1), F(1))
        for powers, expected in [((0, 0), F(1, 2)), ((1, 0), F(1, 6)),
                                 ((0, 1), F(1, 6)), ((2, 0), F(1, 12))]:
            self.assertEqual(oracle.poly_monomial(p, *powers), expected)

    def test_triangle_closed_forms_through_degree_four(self):
        # Right triangles: x=u+sx*r, y=v+sy*s, r+s<=1.
        # Integrating s first and the resulting beta integral gives
        # integral(r^a*s^b)=a!*b!/(a+b+2)! independently of barycentric expansion.
        for u, v, sx, sy in [(0, 0, 1, 1), (2, 3, 1, 1), (0, 0, 3, 7)]:
            triangle = [(F(u), F(v)), (F(u+sx), F(v)), (F(u), F(v+sy))]
            for i in range(5):
                for j in range(5-i):
                    expected = sum((F(math.comb(i, a)*math.comb(j, b)
                                      * u**(i-a)*v**(j-b)*sx**a*sy**b
                                      * math.factorial(a)*math.factorial(b),
                                      math.factorial(a+b+2))
                                    for a in range(i+1) for b in range(j+1)), F(0))*sx*sy
                    with self.subTest(triangle=triangle, powers=(i, j)):
                        self.assertEqual(oracle.tri_monomial(triangle, i, j), expected)

    def test_snap_and_ambiguity_nonunit_normals(self):
        snap = F(1, 10**14)
        for n2, norm in [(F(1), F(1)), (F(25), F(5))]:
            for ratio, snapped, ambiguous in [(F(1, 2), True, False),
                                               (F(1), True, True), (F(2), False, False)]:
                for sign in [-1, 1]:
                    f = sign*ratio*snap*norm
                    self.assertEqual(oracle.snap_phi(f, n2, snap), F(0) if snapped else f)
                    self.assertEqual(oracle.ambiguous(f, n2, snap), ambiguous)

    def test_ambiguity_guard_rejects_unit_cell(self):
        snap = F(1, 10**14)
        axes = [[F(0), F(1)]]*3
        for ring in [[(snap, 0), (1, 0), (1, 1), (snap, 1)],
                     [(-3, 9*snap/4), (3*snap, -3*snap/4), (4, 4), (-3, 4)]]:
            with self.assertRaisesRegex(AssertionError, '^ambiguous snap case$'):
                oracle.check_ambiguity(axes, ring, snap)

    def test_concave_ring_rejected(self):
        with self.assertRaisesRegex(ValueError, 'convex'):
            oracle.cell_truth([[F(0), F(1)]]*3,
                              [(0, 0), (1, 0), (F(1, 2), F(1, 2)), (1, 1), (0, 1)],
                              (0, 0, 0), snap=None)

    def test_generated_vertex_ambiguity(self):
        snap = F(1, 10**14)
        axes = [[F(0), F(2)], [F(0), F(3, 2)], [F(0), F(1)]]
        ring = [(0, -1), (1, snap), (1, 2), (0, 2)]
        # Every ORIGINAL corner is unambiguous. The first plane creates
        # (1/(1+snap),0), within the forbidden band of the next plane x=1.
        for a, b, c in oracle._planes(oracle._ring_key(ring)):
            for x in axes[0]:
                for y in axes[1]:
                    self.assertFalse(oracle.ambiguous(a*x+b*y-c, a*a+b*b, snap))
        with self.assertRaisesRegex(AssertionError, '^ambiguous snap case$'):
            oracle.check_ambiguity(axes, ring, snap)

    def test_snapped_cap_keeps_edge_ownership(self):
        offset = F(1, 100)
        truth = oracle.cell_truth([[F(0), F(1)]]*3,
                                  [(0, 0), (1+offset, 0), (0, 1+offset)],
                                  (0, 0, 0), snap=F(1, 10))
        self.assertEqual(truth['cls'], 'cut')
        self.assertEqual(truth['volume'], F(1, 2))
        self.assertEqual(truth['aperture'], [1, 0, 1, 0, F(1, 2), F(1, 2)])
        self.assertEqual(len(truth['eb']), 1)
        self.assertEqual(truth['eb'][0][1], (F(1, 2), F(1, 2), F(1, 2)))
        self.assertEqual(truth['centroid'], (F(1, 3), F(1, 3), F(1, 2)))

    def test_binary64_input_and_decimal_output_conversion(self):
        self.assertNotEqual(F(.1), F('0.1'))
        truth = oracle.cell_truth([[.1, .4], [0., 1.], [0., 1.]],
                                  [(0., 0.), (.2, 0.), (.2, 1.), (0., 1.)],
                                  (0, 0, 0), snap=None)
        self.assertEqual(truth['volume'], F(.2)-F(.1))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'precise.json'
            path.write_text('{"sum":0.100000000000000000001}', encoding='utf-8')
            self.assertEqual(compare.read_json(path)['sum'], F('0.100000000000000000001'))
            for content in ['{"sum":NaN}', '{"sum":Infinity}', '{"sum":1,"sum":2}']:
                path.write_text(content, encoding='utf-8')
                with self.assertRaises(AssertionError):
                    compare.read_json(path)

    def test_thin_trapezoid_centroid_independent_antiderivative(self):
        # E3-like sliver: height e*(1-2*x). The factor e cancels from
        # (integral x*(1-2*x) dx)/(integral (1-2*x) dx).
        lo, hi, top, epsilon = F(3, 8), F(5, 12), F(1, 4), F(1, 10**12)
        area = (hi-hi*hi)-(lo-lo*lo)
        first = (hi*hi/2-2*hi**3/3)-(lo*lo/2-2*lo**3/3)
        truth = oracle.cell_truth([[lo, hi], [F(0), top], [F(0), F(1)]],
                                  [(0, top-epsilon), (1, top+epsilon), (1, 1), (0, 1)],
                                  (0, 0, 0), snap=None)
        self.assertEqual(truth['volume'], epsilon*area)
        self.assertEqual(truth['centroid'][0], first/area)
        self.assertEqual(truth['face_centroid'][5][0], first/area)

    def test_case_coverage_and_annotations(self):
        for group, count in [('levels', 8), ('variants', 16), ('aniso', 3), ('fem', 6)]:
            values = cases(group)
            self.assertEqual(len(values), count)
            self.assertEqual(len({v['name'] for v in values}), count)
            for value in values:
                validate_case(value)
        wrong = cases('variants')[0]
        wrong['truth_mode'] = 'nosnap'
        with self.assertRaises(AssertionError):
            validate_case(wrong)

    def test_f_snap_is_unambiguous_and_detectable_on_coarse_grid(self):
        case = next(c for c in cases('variants') if c['family'] == 'F_snap' and c['nx'] == 24)
        axes = [[.875, 22/24], [0., .025], [0., .025]]
        oracle.check_ambiguity(axes, case['ring'], F(case['snap_phi']))
        snapped = oracle.cell_truth(axes, case['ring'], (0, 0, 0), snap=F(case['snap_phi']))
        exact = oracle.cell_truth(axes, case['ring'], (0, 0, 0), snap=None)
        self.assertGreater(compare.snap_difference(snapped, exact), F(1, 10**13))

    def test_ring_canonicalization_and_degeneracy(self):
        ring = [(0, 0), (1, 0), (0, 1)]
        expected = oracle.cell_truth([[0, 1]]*3, ring, (0, 0, 0), snap=None)
        for rotated in [ring[1:]+ring[:1], list(reversed(ring)), ring+[ring[0]]]:
            self.assertEqual(oracle.cell_truth([[0, 1]]*3, rotated, (0, 0, 0), snap=None), expected)
        for bad in [[(0, 0), (1, 1), (2, 2)], [(0, 0), (1, 0), (1, 0), (0, 1)]]:
            with self.assertRaises(ValueError):
                oracle.cell_truth([[0, 1]]*3, bad, (0, 0, 0), snap=None)

    def test_zero_weight_records_and_missing_normals_fail(self):
        axes = [[F(0), F(1)]]*3
        truth = oracle.cell_truth(axes, [(0, 0), (1, 0), (0, 1)], (0, 0, 0), snap=None)
        normal = truth['eb'][0][2]
        record = dict(poly_vol=F(1, 2), volfrac=F(1, 2), vq_w_sum=F(1, 2),
                      mom={''.join(map(str, p)): v for p, v in truth['mom'].items()},
                      ext=truth['ext'], sq=[], eb=[], bq_n=[normal])
        for side, area in enumerate(truth['aperture']):
            centroid = truth['face_centroid'][side] if area else (0, 0, 0)
            record['sq'].append([side, int(area > 0), area, *(area*x for x in centroid)])
        record['sq'].append([-1, 1, truth['eb'][0][0], 0, 0, 0])
        record['eb'] = [[*p, *normal] for p in [(1, 0, 0), (0, 1, 0), (0, 1, 1)]]
        errors = compare.Errors()
        compare.compare_cell(errors, 'analytic_triangle', 0, (0, 0, 0), axes, record, truth, False)
        self.assertFalse(errors.failures)
        record['sq'][1][1] = 1  # No area, but one raw zero-weight record: forbidden.
        with self.assertRaisesRegex(AssertionError, 'zero aperture n_rec=1'):
            compare.compare_cell(compare.Errors(), 'zero_record', 0, (0, 0, 0),
                                 axes, record, truth, False)
        record['sq'][1][1] = 0
        record['bq_n'] = []
        with self.assertRaisesRegex(AssertionError, 'missing bq_n expected normal coverage'):
            compare.compare_cell(compare.Errors(), 'missing_normal', 0, (0, 0, 0),
                                 axes, record, truth, False)


if __name__ == '__main__':
    start = time.monotonic()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ExactOracleTests))
    # Import after the tests so a missing resource module is a failure, never a skip.
    import resource
    rss = max(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
              resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    rss /= 1024**2 if sys.platform == 'darwin' else 1024
    wall = time.monotonic()-start
    print(f'MEASURE wall_s={wall:.6f} rss_mib={rss:.3f}')
    assert wall <= 10 and rss <= 128, 'selftest resource limit'
    sys.exit(0 if result.wasSuccessful() else 1)
