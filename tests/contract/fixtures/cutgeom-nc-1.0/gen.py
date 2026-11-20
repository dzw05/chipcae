#!/usr/bin/env python3
"""Generate synthetic cutgeom-nc/1.0 fixtures using exact decimal rationals.

Source tables: PR #202, tests/unit/region_ls/test_plane_cut_union.cpp at
0d97b5974c6f81d3b2765c631f7b4b6d4962bedf. No C++ behavior is executed here.
Run with --out DIR to write a portable, deterministic fixture copy.
"""

import argparse
import json
import math
from decimal import Decimal, localcontext
from fractions import Fraction as F
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
TIP = "0d97b5974c6f81d3b2765c631f7b4b6d4962bedf"
SOURCE = "tests/unit/region_ls/test_plane_cut_union.cpp"
CASES = ("l_shape", "v_shape", "r_shape", "l1_corner", "convex_split2",
         "chamfer_lid_octagon", "errors")
TABLES = {
    "l_shape": (
        [["0", "0"], [".02", "0"], [".02", ".0137"], [".0113", ".0137"],
         [".0113", ".02"], ["0", ".02"]],
        [[0, 1, 2, 3], [0, 3, 4, 5]]),
    "v_shape": (
        [["0", "0"], [".0064", "0"], [".0103", ".0087"], [".0142", "0"],
         [".02", "0"], [".02", ".02"], ["0", ".02"]],
        [[0, 1, 2, 6], [2, 3, 4, 5], [2, 5, 6]]),
    "r_shape": (
        [["0", "0"], [".0086", "0"], [".0086", ".0043"], [".0043", ".0043"],
         [".0043", ".0157"], [".0157", ".0157"], [".0157", ".0043"],
         [".0117", ".0043"], [".0117", "0"], [".02", "0"], [".02", ".02"],
         ["0", ".02"]],
        [[0, 1, 2, 3], [0, 3, 4, 11], [11, 4, 5, 10], [10, 5, 6, 9],
         [8, 9, 6, 7]]),
}


def points(raw):
    return [tuple(F(v) for v in p) for p in raw]


def edges(poly):
    return zip(poly, poly[1:] + poly[:1])


def cross(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def area(poly):
    return sum((a[0] * b[1] - a[1] * b[0] for a, b in edges(poly)), F(0)) / 2


def clip(poly, convex):
    for a, b in edges(convex):
        previous, poly = poly, []
        for p, q in edges(previous):
            sp, sq = cross(a, b, p), cross(a, b, q)
            if (sp < 0) != (sq < 0):
                t = sp / (sp - sq)
                poly.append(tuple(p[d] + t * (q[d] - p[d]) for d in (0, 1)))
            if sq >= 0:
                poly.append(q)
    return poly


def on_segment(p, a, b):
    return cross(a, b, p) == 0 and all(
        min(a[d], b[d]) <= p[d] <= max(a[d], b[d]) for d in (0, 1))


def external(a, b, outline):
    return any(on_segment(a, x, y) and on_segment(b, x, y) for x, y in edges(outline))


def decimal(value):
    """Serialize terminating rationals without a float conversion or rounding."""
    den = value.denominator
    twos = fives = 0
    while den % 2 == 0:
        den //= 2
        twos += 1
    while den % 5 == 0:
        den //= 5
        fives += 1
    if den != 1:
        raise ValueError("nonterminating decimal")
    places = max(twos, fives)
    integer = value.numerator * 2**(places - twos) * 5**(places - fives)
    sign = "-" if integer < 0 else ""
    digits = str(abs(integer)).zfill(places + 1)
    text = digits if places == 0 else digits[:-places] + "." + digits[-places:]
    return sign + (text.rstrip("0").rstrip(".") if places else text)


def embedded(outline):
    exact = F(0)
    irrational = Decimal(0)
    with localcontext() as context:
        context.prec = 60
        for a, b in edges(outline):
            if any(a[d] == b[d] == bound for d in (0, 1) for bound in (F(0), F(".02"))):
                continue
            length2 = sum((b[d] - a[d])**2 for d in (0, 1))
            n, d = math.isqrt(length2.numerator), math.isqrt(length2.denominator)
            if n*n == length2.numerator and d*d == length2.denominator:
                exact += F(n, d)
            else:
                irrational += (Decimal(length2.numerator) / Decimal(length2.denominator)).sqrt()
        if irrational:
            total = (irrational + Decimal(exact.numerator) / Decimal(exact.denominator)) * Decimal(".001")
            return format(total, ".17g"), False
    return decimal(exact * F(".001")), True


def l1_cells(blocks):
    h = F(".0025")
    cells = []
    for j in range(8):
        for i in range(8):
            box = [(i*h, j*h), ((i+1)*h, j*h), ((i+1)*h, (j+1)*h), (i*h, (j+1)*h)]
            fraction = sum((area(clip(p, box)) for p in blocks), F(0)) / h**2
            if 0 < fraction < 1:
                for k in range(2):
                    cells.append({"cell": [i, j, k], "sub_volfrac": decimal(fraction)})
    return cells


def build(name):
    if name in TABLES or name == "l1_corner":
        outline, indices = TABLES["l_shape" if name == "l1_corner" else name]
        outline = [p[:] for p in outline]
        if name == "l1_corner":
            outline = [[".012501" if v in (".0113", ".0137") else v for v in p] for p in outline]
        blocks = [[outline[i] for i in block] for block in indices]
    elif name == "convex_split2":
        outline = [[".003", ".002"], [".018", ".002"], [".018", ".019"], [".003", ".019"]]
        blocks = [outline[:3], [outline[0], outline[2], outline[3]]]
    elif name == "chamfer_lid_octagon":
        outline = [[".003", "0"], [".017", "0"], [".02", ".003"], [".02", ".017"],
                   [".017", ".02"], [".003", ".02"], ["0", ".017"], ["0", ".003"]]
        blocks = [outline]
    else:
        raise ValueError(name)
    ring, pieces = points(outline), [points(p) for p in blocks]
    flags = [[not external(a, b, ring) for a, b in edges(p)] for p in pieces]
    # Reject invalid source decompositions before producing any files.
    for i, p in enumerate(pieces):
        assert len(p) >= 3 and all(cross(p[j-1], p[j], p[(j+1) % len(p)]) > 0 for j in range(len(p)))
        assert area(clip(ring, p)) == area(p), "piece outside outline"
        assert all(area(clip(p, q)) == 0 for q in pieces[:i]), "overlap"
    assert sum(map(area, pieces), F(0)) == area(ring)
    closure = [sum(((b[1]-a[1], a[0]-b[0])[d] * F(".001")
                    for p, fs in zip(pieces, flags) for (a, b), f in zip(edges(p), fs) if not f), F(0))
               for d in (0, 1)]
    assert closure == [0, 0]
    embedded_area, is_exact = embedded(ring)
    inp = {"contract": "cutgeom-nc/1.0", "case": name, "units": "m", "body": 0,
           "outline": outline, "pieces": blocks, "internal": flags,
           "thickness": "0.001", "mesh_box": [["0", "0", "0"], [".02", ".02", ".001"]],
           "options": {"export_polyhedra": True, "keep_void": False, "quad_degree": 2,
                       "small_sub_vf": "0.001"}}
    exp = {"contract": "cutgeom-nc/1.0", "case": name, "area": decimal(area(ring)),
           "volume": decimal(area(ring) * F(".001")), "embedded_area": embedded_area,
           "embedded_area_exact": is_exact, "exterior_area_vector_xy": list(map(decimal, closure)),
           "piece_count": len(pieces), "internal_side_minus1_faces": 0,
           "consumer_assertions_only": ["internal_side_minus1_faces"],
           "tolerance": {"area_volume_rel": "1e-12", "embedded_area_rel": "1e-12",
                         "closure_abs": "1e-12"}}
    if name == "l1_corner":
        cells = l1_cells(pieces)
        counts = {t: sum(0 < F(c["sub_volfrac"]) < F(t) for c in cells) for t in ("0.001", "0.0005")}
        assert counts == {"0.001": 10, "0.0005": 8}
        inp["grid"] = {"h_xy": "0.0025", "nx": 8, "ny": 8, "nz": 2}
        exp.update(corner_fraction="0.00079984", strip_fraction="0.0004", corner_count=2,
                   strip_count=8, small_subs=counts, cut_cells=cells, small_subs_unique=True)
        exp["tolerance"]["sub_volfrac_rel"] = "1e-10"
        exp["consumer_assertions_only"] += ["small_subs"]
    if name == "convex_split2":
        inp.update(single_piece=[outline], single_internal=[[False]*len(outline)],
                   repeated_closing_outline=outline + outline[:1],
                   consecutive_duplicate_outline=outline[:1] + outline)
        exp.update(duplicate_points_equivalent=True)
        exp["tolerance"].update(sub_volfrac_rel="1e-14", sq_w_rel="1e-14")
        exp["consumer_assertions_only"] += ["duplicate_points_equivalent", "sub_volfrac", "sq_w"]
    if name == "chamfer_lid_octagon":
        exp["legacy_all_fields_bitwise_equal"] = True
        exp["consumer_assertions_only"] += ["legacy_all_fields_bitwise_equal"]
    return inp, exp


def error_cases():
    plane = {"n": ["1", "0", "0"], "d": ".01", "body": 0}
    triangle = [["0", "0"], [".02", "0"], ["0", ".02"]]
    inputs = [
        {"case": "empty_pieces", "pieces": [], "internal": []},
        {"case": "internal_mismatch", "pieces": [[plane]], "internal": [[]]},
        {"case": "nonzero_plane_body", "pieces": [[dict(plane, body=1)]], "internal": [[False]]},
        {"case": "outline_phi_mismatch", "node_count": 8, "outline_phi": ["0"]},
        {"case": "short_ring", "outline": [["0", "0"], [".02", "0"]]},
        {"case": "zero_area_ring", "outline": [["0", "0"], [".01", "0"], [".02", "0"]]},
        {"case": "self_intersection", "outline": [["0", "0"], [".02", ".02"], ["0", ".02"], [".015", "0"]]},
        {"case": "multiple_rings", "rings": [triangle, [[".001", ".001"], [".002", ".001"], [".001", ".002"]]]},
        {"case": "duplicate_body", "body_label": "body0", "unions": [{"body": 0}, {"body": 0}]},
        {"case": "zero_normal", "plane": dict(plane, n=["0", "0", "0"])},
        {"case": "invalid_thresholds", "options": {"small_sub_vf": "-0.001"}},
        {"case": "nonpositive_box", "mesh_box": [["0", "0", "0"], ["0", ".02", ".001"]]},
        {"case": "disconnected_cell", "pieces": [triangle,
            [[".03", ".03"], [".04", ".03"], [".03", ".04"]]],
         "cell_box": [["0", "0", "0"], [".05", ".05", ".001"]]},
    ]
    pairs = [("cut/union", "invalid piece")]*3 + [
        ("cut/union", "outline_phi size mismatch"),
        ("CUT_UNSUPPORTED", "region0: degenerate polygon"),
        ("CUT_UNSUPPORTED", "region0: degenerate polygon"),
        ("CUT_UNSUPPORTED", "region0: self-intersecting polygon"),
        ("CUT_UNSUPPORTED", "region0: multiple rings"),
        ("CUT_UNSUPPORTED", "body0: duplicate piece union"),
        ("cut/plane", "nonfinite or zero normal"), ("cut/options", "invalid thresholds"),
        ("cut/mesh", "positive affine box required"),
        ("outline.unsupported", "outline: disconnected solid in one cell")]
    return ({"contract": "cutgeom-nc/1.0", "region": "region0", "cases": inputs},
            {"contract": "cutgeom-nc/1.0", "consumer_assertions_only": True,
             "mesh_all_fields_and_order_unchanged": True,
             "cases": [{"case": inp["case"], "layer": "i11" if path == "CUT_UNSUPPORTED" else "i10",
                        "path": path, "reason": reason} for inp, (path, reason) in zip(inputs, pairs)]})


def dump(obj):
    return json.dumps(obj, ensure_ascii=False, indent=2, allow_nan=False) + "\n"


def payloads():
    out = {}
    for case in CASES:
        inp, exp = error_cases() if case == "errors" else build(case)
        out[case + "/input.json"], out[case + "/expected.json"] = inp, exp
    out["expected.json"] = {
        "contract": "cutgeom-nc/1.0", "status": "draft", "units": "m", "cases": list(CASES),
        "source": {"tip": TIP, "file": SOURCE, "tables": "l_shape/v_shape/r_shape",
                   "l1_parameters": [".012501", ".012501"], "octagon": "single_piece_bitwise synthetic ring"},
        "files": ["gen.py", "check.py", "expected.json"] + sorted(out),
        "tolerance": {"area_volume_rel": "1e-12", "embedded_area_rel": "1e-12",
                      "closure_abs": "1e-12", "l1_sub_volfrac_rel": "1e-10",
                      "convex_sub_volfrac_rel": "1e-14", "convex_sq_w_rel": "1e-14"},
        "numeric_format": "decimal strings; exact Fraction except irrational embedded areas",
        "scope": "synthetic fixture checks; runtime C++ assertions belong to consumer cards"}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=HERE)
    args = ap.parse_args()
    try:
        data = payloads()
        scripts = {name: (HERE / name).read_bytes() for name in ("gen.py", "check.py")}
        for relative, obj in data.items():
            path = args.out / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(dump(obj).encode("utf-8"))
        for name, content in scripts.items():
            (args.out / name).write_bytes(content)
    except (AssertionError, ValueError) as exc:
        print("FAIL", exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print(exc, file=sys.stderr)
        return 2
    print("cutgeom-nc/1.0: generated 7 synthetic cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
