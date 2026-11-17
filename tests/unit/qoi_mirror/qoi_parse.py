# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Analytic fixture constants and pure reference helper copied from test_warpage_qoi."""
import numpy as np

k = 1.2e-3
L = 40.0  # mm
SAMPLE_COUNT = 81


def _rel(got, ref):
    return abs(got / ref - 1.0)


def sorted_rows(rows):
    return rows[np.lexsort((rows[:, 2], rows[:, 1], rows[:, 0]))]
