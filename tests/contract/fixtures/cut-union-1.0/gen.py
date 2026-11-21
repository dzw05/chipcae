#!/usr/bin/env python3
"""cut-union/1.0 (I10) fixture generator. Standard library only.

Inputs are the L/V/R/L1 vertex tables of #202 tests/unit/region_ls/test_plane_cut_union.cpp
(l_shape :329, v_shape :335, r_shape :343, L1 = l_shape(.012501, .012501) :677) and the convex
octagon of single_piece_bitwise (:714). Planes are derived exactly like the test helper piece()
(:311-322): n = (dy, -dx)/hypot, d = n.a, internal = edge not on the outline.

Expected values:
  * exact: per-cell solid fraction = |outline ∩ cell| / |cell| with fractions.Fraction on the
    decimal literals (prism, so z does not matter), solid volume = |outline| * thickness;
  * assertion constants copied verbatim from the #202 test (volume, embedded area, L1 fractions,
    L1 small_subs counts, error path/reason) and cross-checked against the exact values.

Usage: python3 gen.py            write <case>/input.json and <case>/expected.json
       python3 gen.py --stdout   print everything as one JSON document (used by check.py)
"""
import json, math, os, sys
from fractions import Fraction as F

BOX = (F(0), F(0), F("0.02"), F("0.02"))
NXY = 8                      # sp5::grid(stack(), 8): 8 x 8 columns
H = F("0.0025")              # cell size in x/y (fixture_oracles :639-640)
NZ = 2                       # thickness .001, n_elem_min 2 (stack() :252-265)
HZ = F("0.0005")
THICK = F("0.001")
SRC = "tests/unit/region_ls/test_plane_cut_union.cpp @ #202 0d97b59"

def P(*xy):
    return [(F(str(x)), F(str(y))) for x, y in xy]

def l_shape(x="0.0113", y="0.0137"):
    return dict(outline=P((0, 0), (.02, 0), (.02, y), (x, y), (x, .02), (0, .02)),
                blocks=[P((0, 0), (.02, 0), (.02, y), (x, y)), P((0, 0), (x, y), (x, .02), (0, .02))])

SHAPES = {
    "L": dict(l_shape(), src=":329-334", assert_volume="3.4519e-7", assert_area="1.5e-5"),
    "V": dict(outline=P((0, 0), (.0064, 0), (.0103, .0087), (.0142, 0), (.02, 0), (.02, .02), (0, .02)),
              blocks=[P((0, 0), (.0064, 0), (.0103, .0087), (0, .02)),
                      P((.0103, .0087), (.0142, 0), (.02, 0), (.02, .02)),
                      P((.0103, .0087), (.02, .02), (0, .02))],
              src=":335-342", assert_volume="3.6607e-7", assert_area="2*hypot(.0039,.0087)*.001"),
    "R": dict(outline=P((0, 0), (.0086, 0), (.0086, .0043), (.0043, .0043), (.0043, .0157), (.0157, .0157),
                        (.0157, .0043), (.0117, .0043), (.0117, 0), (.02, 0), (.02, .02), (0, .02)),
              blocks=[P((0, 0), (.0086, 0), (.0086, .0043), (.0043, .0043)),
                      P((0, 0), (.0043, .0043), (.0043, .0157), (0, .02)),
                      P((0, .02), (.0043, .0157), (.0157, .0157), (.02, .02)),
                      P((.02, .02), (.0157, .0157), (.0157, .0043), (.02, 0)),
                      P((.0117, 0), (.02, 0), (.0157, .0043), (.0117, .0043))],
              src=":343-363", assert_volume="2.5671e-7", assert_area="5.11e-5"),
}
L1 = dict(l_shape("0.012501", "0.012501"), src=":676-711 union_small_subs")
OCT = P((.003, 0), (.017, 0), (.02, .003), (.02, .017), (.017, .02), (.003, .02), (0, .017), (0, .003))

def area(ring):
    s = F(0)
    for i, a in enumerate(ring):
        b = ring[(i + 1) % len(ring)]
        s += a[0] * b[1] - a[1] * b[0]
    return s / 2

def on_edge(p, a, b):
    cr = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])
    return cr == 0 and min(a[0], b[0]) <= p[0] <= max(a[0], b[0]) and min(a[1], b[1]) <= p[1] <= max(a[1], b[1])

def original_edge(a, b, outline):
    return any(on_edge(a, outline[i], outline[(i + 1) % len(outline)]) and
               on_edge(b, outline[i], outline[(i + 1) % len(outline)]) for i in range(len(outline)))

def box_edge(a, b):
    return any(a[d] == b[d] == x for d in (0, 1) for x in (BOX[0], BOX[2]))

def planes(ring, outline):
    out = []
    for i, a in enumerate(ring):
        b = ring[(i + 1) % len(ring)]
        ax, ay, bx, by = float(a[0]), float(a[1]), float(b[0]), float(b[1])
        nx, ny = by - ay, ax - bx
        ln = math.hypot(nx, ny)
        n0, n1 = nx / ln, ny / ln
        out.append({"n": [n0, n1, 0.0], "d": n0 * ax + n1 * ay, "internal": not original_edge(a, b, outline),
                    "edge": [[str(a[0]), str(a[1])], [str(b[0]), str(b[1])]]})
    return out

def clip(poly, k, v, keep_ge):
    out = []
    inside = (lambda p: p[k] >= v) if keep_ge else (lambda p: p[k] <= v)
    for i, a in enumerate(poly):
        b = poly[(i + 1) % len(poly)]
        ia, ib = inside(a), inside(b)
        if ia:
            out.append(a)
        if ia != ib:
            t = (v - a[k]) / (b[k] - a[k])
            out.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])))
    return out

def cell_fraction(outline, i, j):
    x0, y0 = i * H, j * H
    p = outline
    for k, v, ge in ((0, x0, True), (0, x0 + H, False), (1, y0, True), (1, y0 + H, False)):
        p = clip(p, k, v, ge)
        if not p:
            return F(0)
    return area(p) / (H * H)

def exact_cells(outline):
    full = void = 0
    cut = []
    for k in range(NZ):
        for j in range(NXY):
            for i in range(NXY):
                f = cell_fraction(outline, i, j)
                if f == 0:
                    void += 1
                elif f == 1:
                    full += 1
                else:
                    cut.append([i, j, k, float(f), str(f)])
    return full, void, cut

def perimeter(outline):
    s = 0.0
    for i, a in enumerate(outline):
        b = outline[(i + 1) % len(outline)]
        if not box_edge(a, b):
            s += math.hypot(float(b[0] - a[0]), float(b[1] - a[1]))
    return s * float(THICK)

def ring_json(r):
    return [[str(x), str(y)] for x, y in r]

def ok_case(name, shape, extra_expected=None, options=None):
    outline, blocks = shape["outline"], shape["blocks"]
    full, void, cut = exact_cells(outline)
    vol = area(outline) * THICK
    exp = {"case": name, "ok": True, "solid_volume": float(vol), "solid_volume_exact": str(vol),
           "embedded_area": perimeter(outline), "n_cells": full + len(cut), "n_cut": len(cut),
           "n_void_removed": void, "cells": [c[:4] for c in cut], "cells_exact": [c[4] for c in cut]}
    if "assert_volume" in shape:
        exp["assert"] = {"volume": shape["assert_volume"], "embedded_area": shape["assert_area"],
                         "rel": 1e-10, "src": shape["src"] + " + volume_case :597-606"}
    if extra_expected:
        exp.update(extra_expected)
    inp = {"case": name, "source": SRC + " " + shape["src"],
           "mesh": {"box": [str(b) for b in BOX], "nx": NXY, "ny": NXY, "nz": NZ, "h_xy": str(H), "h_z": str(HZ),
                    "builder": "sp5::grid(stack(), 8), stack() :252-265"},
           "options": options or {"export_polyhedra": True},
           "extra": [],
           "outline": ring_json(outline), "outline_phi": "signed xy distance to outline per node, negative inside (test signed_distance :281-292)",
           "blocks": [ring_json(b) for b in blocks],
           "pieces": [planes(b, outline) for b in blocks]}
    return inp, exp

ERRORS = [
    ("E_empty", "pieces = {}", "cut/union", "invalid piece", ":1030-1043 which=0; plane_cut.cpp:1092-1093"),
    ("E_internal_size", "L, pieces[1].internal.pop_back()", "cut/union", "invalid piece", ":1036-1037 which=1; plane_cut.cpp:1095-1096"),
    ("E_body", "L, pieces[0].planes[0].body = entries[0].body", "cut/union", "invalid piece", ":1038-1039 which=2; plane_cut.cpp:1097-1099"),
    ("E_phi_short", "L, outline_phi.pop_back()", "cut/union", "outline_phi size mismatch", ":1044-1058; plane_cut.cpp:1101-1102"),
    ("E_phi_long_fast", "L piece 0 only, all internal=false (legacy path), outline_phi.push_back(0)", "cut/union", "outline_phi size mismatch", ":1044-1058 fast=1; plane_cut.cpp:1101-1102"),
    ("E_options", "L, options.quad_degree = 3", "cut/options", "invalid thresholds", ":1059-1079 which=2"),
    ("E_plane", "L, pieces[1].planes[0].n = {0,0,0}", "cut/plane", "nonfinite or zero normal", ":1080-1098 which=0"),
    ("E_disconnected", "two rectangles x<.0105 and x>.0115, phi=min of both", "outline.unsupported",
     "outline: disconnected solid in one cell", ":1012-1029; plane_cut.cpp:585"),
]

def build():
    cases = {}
    for name, s in SHAPES.items():
        cases[name] = ok_case(name, s)
    for th, label, n_small in (("1e-3", "L1_th1e-3", 10), ("5e-4", "L1_th5e-4", 8)):
        inp, exp = ok_case(label, L1, options={"export_polyhedra": True, "small_sub_vf": th})
        exp["n_small"] = n_small
        exp["assert"] = {"corner_fraction": "7.9984e-4", "strip_fraction": "4e-4", "corners": 2, "strips": 8,
                         "n_small": n_small, "rel": 1e-10, "src": ":676-711"}
        cases[label] = (inp, exp)
    inp, exp = ok_case("O8_convex", dict(outline=OCT, blocks=[OCT], src=":713-750 single_piece_bitwise"),
                       options={"export_polyhedra": True})
    exp["bitwise_vs_build_cut_layer"] = {"cut_equal": True, "mesh_equal": True,
        "legacy_call": "build_cut_layer(mesh, planes with body expanded to every attr_table body, options)",
        "src": ":713-750 (test also covers keep_void x quad_degree {2,6} and an extra plane)"}
    cases["O8_convex"] = (inp, exp)
    for name, setup, path, reason, src in ERRORS:
        cases[name] = ({"case": name, "setup": setup, "source": SRC + " " + src},
                       {"case": name, "ok": False, "path": path, "reason": reason, "mesh_unchanged": True})
    return cases

def main():
    cases = build()
    if "--stdout" in sys.argv:
        json.dump({k: {"input": v[0], "expected": v[1]} for k, v in cases.items()}, sys.stdout, indent=1)
        return
    here = os.path.dirname(os.path.abspath(__file__))
    for name, (inp, exp) in cases.items():
        d = os.path.join(here, name)
        os.makedirs(d, exist_ok=True)
        for fn, obj in (("input.json", inp), ("expected.json", exp)):
            with open(os.path.join(d, fn), "w") as f:
                json.dump(obj, f, indent=1)
                f.write("\n")
    print("wrote", len(cases), "cases")

if __name__ == "__main__":
    main()
