#!/usr/bin/env python3
"""Build the scale-aware STEP defect injection plan.

The OCCT integration runner consumes this file.  Values here mirror
compute_tolerances() and classify() in geometry/src/import_thresholds.cpp.
"""

import argparse
import json
import math
from pathlib import Path

KINDS = ("Gap", "OpenEdge", "SmallEdge", "SmallFace", "SliverFace",
         "DuplicateFace", "FlippedFace", "WireSelfIntersection", "NearLayerInterface")
MULTIPLIERS = (0.5, 1.0, 2.0)


def plan(scales):
    L, t_min, h_xy, dbu_m = scales
    eps_weld = max(min(1e-6 * L, 0.005 * t_min), 1e-8)
    tau_a = min(1e-5 * L, 0.02 * t_min)
    tau_b = min(1e-4 * L, 0.1 * t_min)
    values = {"eps_weld": eps_weld, "tauA": tau_a, "tauB": tau_b}

    def classify(size):
        return ("Weld" if size <= eps_weld else "A" if size <= tau_a else
                "B" if size <= tau_b else "C")

    cases = []
    for kind in KINDS:
        for threshold, boundary in values.items():
            for multiplier in MULTIPLIERS:
                size = boundary * multiplier
                cases.append({
                    "id": f"{kind}_{threshold}_{multiplier:g}x",
                    "kind": kind, "expected_kind": kind,
                    "threshold": threshold, "multiplier": multiplier,
                    "injected_size_m": size, "measured_m": size,
                    "expected_tier": classify(size),
                    "expected_face_index": None,
                    "injection_point": [L / 4, t_min / 2, 0.0],
                })
    # The remaining cases describe API-level invariants. They do not claim
    # StepDefectKind values for the thin-layer classifier or unit metadata.
    thin_width = 1e-4 * L
    cases += [
        {"id": "legal_thin_face", "kind": "LegalThinFace",
         "expected_tier": classify(thin_width), "expected_face_index": None,
         "width_m": thin_width, "should_delete": False},
        {"id": "zero_thickness_partition", "kind": "ZeroThicknessPartition",
         "expected_result": "Error", "expected_face_index": None},
        {"id": "unit_mismatch_mm_as_m", "kind": "UnitMismatch",
         "expected_kind": None, "expected_face_index": None,
         "expected_behavior": "caller_expectation_required"},
        {"id": "thin_side_10um_by_20mm", "kind": "LegalThinFace",
         "expected_tier": classify(1e-5), "expected_face_index": None,
         "width_m": 1e-5, "length_m": 2e-2,
         "minimum_angle_degrees": 1, "preserve_face_count": True,
         "merge_max_volume_rel": 1e-4},
    ]
    return {"schema": 2, "scales": {"L": L, "t_min": t_min, "h_xy": h_xy,
                                       "dbu_m": dbu_m},
            "tolerances": values, "assembly": ["cover", "substrate", "die"],
            "cases": cases}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--L", type=float, default=1.0)
    parser.add_argument("--t-min", type=float, default=1e-3)
    parser.add_argument("--h-xy", type=float, default=1e-3)
    parser.add_argument("--dbu-m", type=float, default=0.0)
    args = parser.parse_args()
    scales = (args.L, args.t_min, args.h_xy, args.dbu_m)
    if (not all(math.isfinite(x) for x in scales) or args.L <= 0 or
            args.t_min <= 0 or args.h_xy <= 0 or args.dbu_m < 0):
        parser.error("scales must be finite; L, t-min, h-xy positive and dbu-m nonnegative")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(plan(scales), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
