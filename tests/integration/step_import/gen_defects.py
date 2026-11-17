#!/usr/bin/env python3
"""Generate the expected-case manifest for the E6 STEP injection matrix."""

import argparse
import json
from pathlib import Path


KINDS = (
    "Gap",
    "OpenEdge",
    "SmallEdge",
    "SmallFace",
    "SliverFace",
    "DuplicateFace",
    "FlippedFace",
    "WireSelfIntersection",
    "NearLayerInterface",
)
THRESHOLDS = ("eps_weld", "tauA", "tauB")
MULTIPLIERS = (0.5, 1.0, 2.0)


def tier(threshold, multiplier):
    # classify() uses <=, so an exact threshold belongs to its lower tier.
    if threshold == "eps_weld":
        return "Weld" if multiplier <= 1.0 else "A"
    if threshold == "tauA":
        return "A" if multiplier <= 1.0 else "B"
    return "B" if multiplier <= 1.0 else "C"


def build_manifest():
    cases = []
    for kind in KINDS:
        for threshold in THRESHOLDS:
            for multiplier in MULTIPLIERS:
                cases.append({
                    "id": f"{kind}_{threshold}_{multiplier:g}x",
                    "kind": kind,
                    "threshold": threshold,
                    "multiplier": multiplier,
                    "expected_tier": tier(threshold, multiplier),
                    "expected_face_index": 1,
                })
    cases.extend((
        {"id": "legal_thin_face", "kind": "LegalThinFace", "expected_tier": "A",
         "expected_face_index": 1, "width_rel_L": 1e-4, "should_delete": False},
        {"id": "zero_thickness_partition", "kind": "ZeroThicknessPartition",
         "expected_tier": "C", "expected_kind": "Error", "expected_face_index": 1},
        {"id": "unit_mismatch_mm_as_m", "kind": "UnitSuspect", "expected_tier": "B",
         "expected_kind": "UnitSuspect", "expected_face_index": 1},
        {"id": "thin_side_10um_by_20mm", "kind": "LegalThinFace",
         "expected_tier": "A", "expected_face_index": 1, "width_m": 1e-5,
         "length_m": 2e-2, "minimum_angle_degrees": 1, "preserve_face_count": True},
    ))
    return {"schema": 1, "assembly": ["cover", "substrate", "die"], "cases": cases}


def validate(manifest):
    cases = manifest["cases"]
    matrix = [case for case in cases if case["kind"] in KINDS]
    if len(matrix) != len(KINDS) * len(THRESHOLDS) * len(MULTIPLIERS):
        raise ValueError("incomplete defect/threshold injection matrix")
    for case in matrix:
        if case["expected_tier"] != tier(case["threshold"], case["multiplier"]):
            raise ValueError(f"incorrect <= boundary tier: {case['id']}")
    exact = [case for case in matrix if case["multiplier"] == 1.0]
    if len(exact) != len(KINDS) * len(THRESHOLDS):
        raise ValueError("each defect kind must include each exact-threshold case")
    if not all(case["expected_face_index"] > 0 for case in cases):
        raise ValueError("each expected case must identify its source face")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    manifest = build_manifest()
    validate(manifest)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
