"""Median, MAD, and coefficient of variation for the perf harness."""

from __future__ import annotations

import math
from collections.abc import Sequence


def median(values: Sequence[float]) -> float:
    if not values:
        raise ValueError("median of an empty sample")
    ordered = sorted(values)
    count = len(ordered)
    mid = count // 2
    if count % 2 == 1:
        return float(ordered[mid])
    return 0.5 * (float(ordered[mid - 1]) + float(ordered[mid]))


def mad(values: Sequence[float]) -> float:
    """Median absolute deviation around the sample median."""
    center = median(values)
    return median([abs(float(value) - center) for value in values])


def coefficient_of_variation(values: Sequence[float]) -> float:
    """Sample CV (stddev with N-1). Zero when the mean is zero or N < 2."""
    count = len(values)
    if count < 2:
        return 0.0
    mean = sum(float(value) for value in values) / count
    if mean == 0.0:
        return 0.0
    variance = sum((float(value) - mean) ** 2 for value in values) / (count - 1)
    return math.sqrt(variance) / abs(mean)
