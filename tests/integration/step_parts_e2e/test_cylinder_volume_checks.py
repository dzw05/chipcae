"""The cylinder verifier rejects clipping error, grid drift and excess chord error."""
import math
import unittest
import io
from contextlib import redirect_stdout
from test_e2e_cylinder_volume import check_volumes


class CylinderVolumeChecks(unittest.TestCase):
    def setUp(self):
        # These are verifier unit inputs, not solver measurements for the PR.
        quiet = redirect_stdout(io.StringIO())
        quiet.__enter__()
        self.addCleanup(quiet.__exit__, None, None, None)

    def test_inscribed_hexagon_is_exact_and_within_chord_bound(self):
        area = 3 * math.sqrt(3) / 2
        check_volumes(area, 1., 1., 1 - math.sqrt(3)/2, [(8, area), (16, area), (32, area)])

    def test_inaccurate_polygon_volume_is_rejected(self):
        area = 3 * math.sqrt(3) / 2
        with self.assertRaises(AssertionError):
            check_volumes(area, 1., 1., 1 - math.sqrt(3)/2,
                          [(n, area * (1 + 2e-10)) for n in (8, 16, 32)])

    def test_grid_drift_within_polygon_tolerance_is_rejected(self):
        area = 3 * math.sqrt(3) / 2
        with self.assertRaises(AssertionError):
            check_volumes(area, 1., 1., 1 - math.sqrt(3)/2,
                          [(8, area), (16, area), (32, area * (1 + 2e-12))])

    def test_polygon_area_outside_chord_bound_is_rejected(self):
        with self.assertRaises(AssertionError):
            check_volumes(2., 1., 1., 1 - math.sqrt(3)/2, [(8, 2.), (16, 2.), (32, 2.)])

    def test_nonfinite_volume_is_rejected(self):
        with self.assertRaises(AssertionError):
            check_volumes(2., 1., 1., .1, [(8, math.nan), (16, 2.), (32, 2.)])


if __name__ == '__main__':
    unittest.main(argv=[__file__])
