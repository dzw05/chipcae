# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Synthetic Q2 exports and analytic oracles; no solver or target-module imports."""
from itertools import product
import json
from pathlib import Path
import sys

import numpy as np


SOURCE_DIR = (Path(sys.argv[1]).resolve() if len(sys.argv) > 1
              else Path(__file__).resolve().parents[3])
sys.path.insert(0, str(SOURCE_DIR))

SEED = 270527
KAPPA = 0.37
Z0 = 0.05
X = np.array([0.0, 0.17, 0.61, 1.0])
Y = np.array([0.0, 0.07, 0.2])
Z = np.array([0.0, 0.035, 0.1])
PV_KEYS = {"pv", "n_points", "n_elements", "selfcheck_rel", "eval_seconds"}
DUAL_KEYS = {"e_exact", "e_richardson", "pass_exact", "pass_richardson", "status"}
RICHARDSON_KEYS = {"R", "p", "f_ext", "gci_fine", "status"}


def bending(points):
    x, y, z = np.asarray(points).T
    return np.column_stack((-KAPPA * x * (z - Z0), np.zeros_like(y), KAPPA * x**2 / 2))


def general(points):
    # Every component includes all required monomials with independent fixed coefficients.
    x, y, z = np.asarray(points).T
    return np.column_stack((
        0.7 + 0.3*x - 0.4*y + 0.2*z + 0.6*x*x - 0.5*y*y + 0.8*z*z
        + 0.9*x*y - 0.2*x*z + 0.4*y*z + 1.3*x*x*y*y*z*z,
        -0.9 + 0.8*x + 0.6*y - 0.3*z - 0.7*x*x + 0.4*y*y + 0.5*z*z
        - 0.2*x*y + 0.7*x*z - 0.6*y*z - 1.1*x*x*y*y*z*z,
        1.2 - 0.5*x + 0.7*y + 0.9*z + 0.4*x*x + 0.8*y*y - 0.6*z*z
        + 0.3*x*y - 0.8*x*z + 0.2*y*z + 1.7*x*x*y*y*z*z,
    ))


def cubic(points):
    x = np.asarray(points)[:, 0]
    return np.column_stack((x**3, np.zeros_like(x), np.zeros_like(x)))


def temperature(points):
    x, y, z = np.asarray(points).T
    return 1 + x**2 + y*z


def strip_field(points):
    x, y, z = np.asarray(points).T
    # Translation plus omega cross r, with omega = (0.013, -0.021, 0.017).
    return bending(points) + np.column_stack((
        0.031 - 0.021*z - 0.017*y,
        -0.023 + 0.017*x - 0.013*z,
        0.019 + 0.013*y + 0.021*x,
    ))


def elements(xs=X, ys=Y, zs=Z, field=bending, seed=SEED):
    """Generate exactly 27 nodes per box, permuting both nodes and elements."""
    rng = np.random.default_rng(seed)
    result = []
    for k, j, i in product(range(len(zs)-1), range(len(ys)-1), range(len(xs)-1)):
        axes = [(edges[n], (edges[n] + edges[n+1]) / 2, edges[n+1])
                for edges, n in ((xs, i), (ys, j), (zs, k))]
        points = np.array(list(product(*axes)), dtype=np.float64)
        order = rng.permutation(27)
        result.append({"gid": 101 + 13*len(result), "points": points[order].tolist(),
                       "U": field(points)[order].tolist(), "T": temperature(points)[order].tolist()})
    rng.shuffle(result)
    return result


def strip_elements(field=strip_field):
    return elements(np.linspace(0, 1, 13), np.linspace(0, 0.2, 5), [0, 0.1], field)


def centroids(x0, y0, x1, y1, nx, ny):
    # Deliberately scalar, x fastest, preserving the brief's operation order.
    return np.array([(x0 + (i + 0.5)*(x1 - x0)/nx,
                      y0 + (j + 0.5)*(y1 - y0)/ny)
                     for j in range(ny) for i in range(nx)], dtype=np.float64)


def analytic_pv(xy):
    # Symmetric uniform samples make centered x^2 orthogonal to both x and y.
    # Removing a + bx + cy + d(x-xbar)^2 leaves
    # d*((x-xbar)^2 - mean((x-xbar)^2)); subtracting extrema cancels the mean.
    square = (xy[:, 0] - np.mean(xy[:, 0]))**2
    return (KAPPA / 2) * (np.max(square) - np.min(square))


def sample_points():
    rng = np.random.default_rng(SEED + 1)
    random = rng.uniform(size=(500, 3)) * [1, 0.2, 0.1]
    centers = np.array(list(product((X[:-1]+X[1:])/2,
                                   (Y[:-1]+Y[1:])/2, (Z[:-1]+Z[1:])/2)))
    # Cartesian product includes shared faces, edges and vertices (including outer corners).
    boundaries = np.array(list(product(X, Y, Z)))
    face_points = []
    for axis, edges in enumerate((X, Y, Z)):
        for coordinate in edges[1:-1]:
            face = rng.uniform(size=(19, 3)) * [1, 0.2, 0.1]
            face[:, axis] = coordinate
            face_points.extend(face)
    return np.vstack((random, centers, boundaries, face_points))


def write_rank(run, rank, data):
    run = Path(run)
    run.mkdir(parents=True, exist_ok=True)
    path = run / f"accuracy-q2-rank{rank}.json"
    path.write_text(json.dumps({"elements": data}, allow_nan=False), encoding="utf-8")
    return path


def assert_field(test, actual, expected):
    actual, expected = np.asarray(actual), np.asarray(expected)
    test.assertEqual(actual.shape, expected.shape)
    test.assertEqual(actual.dtype, np.dtype("float64"))
    test.assertTrue(np.all(np.isfinite(actual)))
    tolerance = 1e-12 * float(np.max(np.abs(expected)))
    if expected.ndim == 2:
        # Each component, including analytically zero components, uses the same field scale.
        for component in range(expected.shape[1]):
            test.assertLessEqual(float(np.max(np.abs(actual[:, component]-expected[:, component]))),
                                 tolerance)
    else:
        test.assertLessEqual(float(np.max(np.abs(actual-expected))), tolerance)


def assert_prefix(test, error_class, prefix, function, *args, **kwargs):
    with test.assertRaises(error_class) as caught:
        function(*args, **kwargs)
    test.assertTrue(str(caught.exception).startswith(prefix), str(caught.exception))


def assert_bits(test, actual, expected):
    test.assertEqual(actual.shape, expected.shape)
    test.assertEqual(actual.dtype, expected.dtype)
    test.assertEqual(actual.tobytes(), expected.tobytes())
