#!/usr/bin/env python3
"""Read-only cutgeom-nc/1.0 validation, optionally diagnosing #202 source drift.

Exit 0: valid (or optional git/ref diagnostic SKIP); 1: fixture/source assertion;
2: CLI/IO error. Geometry and fixed oracles are checked before regeneration.
"""

import argparse
from decimal import Decimal, localcontext
from fractions import Fraction as F
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys

# Importing the generator must leave the complete checked directory unchanged.
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
CASES = ("l_shape", "v_shape", "r_shape", "l1_corner", "convex_split2",
         "chamfer_lid_octagon", "errors")
SOURCE = "tests/unit/region_ls/test_plane_cut_union.cpp"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def pairs(poly):
    return zip(poly, poly[1:] + poly[:1])


def determinant(a, b, c):
    return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])


def measure(poly):
    return sum((a[0]*b[1] - b[0]*a[1] for a, b in pairs(poly)), F(0)) / 2


def on(p, a, b):
    return determinant(a, b, p) == 0 and all(min(a[d], b[d]) <= p[d] <= max(a[d], b[d]) for d in (0, 1))


def touching(a, b, c, d):
    turns = [determinant(a, b, c), determinant(a, b, d), determinant(c, d, a), determinant(c, d, b)]
    return (turns[0]*turns[1] < 0 and turns[2]*turns[3] < 0) or any(
        (on(p, x, y) for p, x, y in ((a, c, d), (b, c, d), (c, a, b), (d, a, b))))


def normalize(poly):
    out = []
    for p in poly:
        if not out or p != out[-1]:
            out.append(p)
    if len(out) > 1 and out[0] == out[-1]:
        out.pop()
    return out


def polygon(raw, simple=True):
    require(isinstance(raw, list), "polygon must be a list")
    require(all(isinstance(p, list) and len(p) == 2 and all(isinstance(v, str) for v in p) for p in raw),
            "coordinates must be decimal strings")
    require(all(re.fullmatch(r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?", v) for p in raw for v in p),
            "invalid decimal coordinate")
    out = [tuple(F(v) for v in p) for p in raw]
    if simple:
        out = normalize(out)
        require(len(out) >= 3 and len(set(out)) == len(out), "short ring or nonconsecutive duplicate")
        require(measure(out) > 0, "outline must have positive CCW area")
        es = list(pairs(out))
        for i, (a, b) in enumerate(es):
            for j in range(i+1, len(es)):
                if j == i+1 or (i == 0 and j == len(es)-1):
                    continue
                require(not touching(a, b, *es[j]), "self-intersecting ring")
    return out


def convex_intersection(subject, boundary):
    out = list(subject)
    for a, b in pairs(boundary):
        incoming, out = out, []
        for p, q in pairs(incoming):
            dp, dq = determinant(a, b, p), determinant(a, b, q)
            if dp >= 0:
                out.append(p)
            if (dp < 0) != (dq < 0):
                alpha = dp / (dp-dq)
                out.append(tuple(p[i] + alpha*(q[i]-p[i]) for i in (0, 1)))
    return out


def triangulate(ring):
    """Ear clipping supplies an independent containment test for concave outlines."""
    remaining, triangles = list(ring), []
    while len(remaining) > 3:
        for i, b in enumerate(remaining):
            a, c = remaining[i-1], remaining[(i+1) % len(remaining)]
            if determinant(a, b, c) <= 0:
                continue
            if any(all(determinant(x, y, p) >= 0 for x, y in pairs([a, b, c]))
                   for p in remaining if p not in (a, b, c)):
                continue
            triangles.append([a, b, c])
            remaining.pop(i)
            break
        else:
            raise AssertionError("cannot triangulate outline")
    return triangles + [remaining]


def closed(obj, keys, path):
    require(isinstance(obj, dict) and set(obj) == set(keys.split()), path + ": missing or unknown key")


def independent_schema(data):
    """Closed key sets belong to the reader, independently of generator output."""
    manifest = data["expected.json"]
    closed(manifest, "contract status units cases source files tolerance numeric_format scope", "manifest")
    closed(manifest["source"], "tip file tables l1_parameters octagon", "source")
    closed(manifest["tolerance"], "area_volume_rel embedded_area_rel closure_abs l1_sub_volfrac_rel "
           "convex_sub_volfrac_rel convex_sq_w_rel", "manifest tolerance")
    base_in = "contract case units body outline pieces internal thickness mesh_box options"
    base_out = ("contract case area volume embedded_area embedded_area_exact exterior_area_vector_xy "
                "piece_count internal_side_minus1_faces consumer_assertions_only tolerance")
    for case in CASES[:-1]:
        inp, exp = data[case + "/input.json"], data[case + "/expected.json"]
        in_extra = out_extra = tolerance_extra = ""
        if case == "l1_corner":
            in_extra = " grid"
            out_extra = " corner_fraction strip_fraction corner_count strip_count small_subs cut_cells small_subs_unique"
            tolerance_extra = " sub_volfrac_rel"
            closed(inp["grid"], "h_xy nx ny nz", "grid")
            closed(exp["small_subs"], "0.001 0.0005", "small_subs")
            for cell in exp["cut_cells"]:
                closed(cell, "cell sub_volfrac", "cut cell")
        elif case == "convex_split2":
            in_extra = " single_piece single_internal repeated_closing_outline consecutive_duplicate_outline"
            out_extra = " duplicate_points_equivalent"
            tolerance_extra = " sub_volfrac_rel sq_w_rel"
        elif case == "chamfer_lid_octagon":
            out_extra = " legacy_all_fields_bitwise_equal"
        closed(inp, base_in + in_extra, case + " input")
        closed(exp, base_out + out_extra, case + " expected")
        closed(inp["options"], "export_polyhedra keep_void quad_degree small_sub_vf", "options")
        closed(exp["tolerance"], "area_volume_rel embedded_area_rel closure_abs" + tolerance_extra, "tolerance")
    inp, exp = data["errors/input.json"], data["errors/expected.json"]
    closed(inp, "contract region cases", "errors input")
    closed(exp, "contract consumer_assertions_only mesh_all_fields_and_order_unchanged cases", "errors expected")
    fields = {
        "empty_pieces": "pieces internal", "internal_mismatch": "pieces internal",
        "nonzero_plane_body": "pieces internal", "outline_phi_mismatch": "node_count outline_phi",
        "short_ring": "outline", "zero_area_ring": "outline", "self_intersection": "outline",
        "multiple_rings": "rings", "duplicate_body": "body_label unions", "zero_normal": "plane",
        "invalid_thresholds": "options", "nonpositive_box": "mesh_box", "disconnected_cell": "pieces cell_box"}
    for record in inp["cases"]:
        require(record.get("case") in fields, "unknown error case")
        closed(record, "case " + fields[record["case"]], "error input")
        if record["case"] in ("internal_mismatch", "nonzero_plane_body"):
            for block in record["pieces"]:
                for plane in block:
                    closed(plane, "n d body", "error plane")
        if record["case"] == "zero_normal":
            closed(record["plane"], "n d body", "zero normal")
        if record["case"] == "duplicate_body":
            for union in record["unions"]:
                closed(union, "body", "error union")
        if record["case"] == "invalid_thresholds":
            closed(record["options"], "small_sub_vf", "error options")
    for record in exp["cases"]:
        closed(record, "case layer path reason", "error expected")


def schema(actual, template, path):
    """Closed recursive schema; all keys are required and case sensitive."""
    require(type(actual) is type(template), path + ": wrong type")
    if isinstance(template, dict):
        require(actual.keys() == template.keys(), path + ": missing or unknown key")
        for key in template:
            require(key == key.lower(), path + ": uppercase key")
            schema(actual[key], template[key], path + "/" + key)
    elif isinstance(template, list):
        require(len(actual) == len(template), path + ": wrong list length")
        for i, (a, t) in enumerate(zip(actual, template)):
            schema(a, t, path + "/" + str(i))


def geometry(name, inp, exp):
    ring = polygon(inp["outline"])
    blocks = [polygon(p) for p in inp["pieces"]]
    require(blocks and len(blocks) == len(inp["internal"]), "empty/mismatched pieces")
    triangles = triangulate(ring)
    require(sum(map(measure, triangles), F(0)) == measure(ring), "triangulation coverage")
    closure = [F(0), F(0)]
    for i, block in enumerate(blocks):
        require(len(block) == len(inp["pieces"][i]), "piece contains duplicate point")
        require(all(determinant(block[j-1], block[j], block[(j+1) % len(block)]) > 0 for j in range(len(block))),
                "piece must be strictly convex CCW")
        require(sum((measure(convex_intersection(t, block)) for t in triangles), F(0)) == measure(block),
                "piece outside outline")
        require(all(measure(convex_intersection(block, p)) == 0 for p in blocks[:i]), "overlapping blocks")
        flags = inp["internal"][i]
        require(len(flags) == len(block), "internal shape mismatch")
        for (a, b), internal in zip(pairs(block), flags):
            ext = any(on(a, x, y) and on(b, x, y) for x, y in pairs(ring))
            require(type(internal) is bool and internal == (not ext), "incorrect internal edge")
            if ext:
                closure[0] += (b[1]-a[1]) * F(inp["thickness"])
                closure[1] += (a[0]-b[0]) * F(inp["thickness"])
    require(sum(map(measure, blocks), F(0)) == measure(ring), "incomplete union coverage")
    require(closure == [0, 0] == [F(v) for v in exp["exterior_area_vector_xy"]], "nonzero exact closure")
    require(F(inp["thickness"]) == F(".001"), "thickness")
    require(measure(ring) == F(exp["area"]), "area oracle")
    require(measure(ring)*F(".001") == F(exp["volume"]), "volume oracle")
    constants = {"l_shape": (".00034519", ".00000034519", ".000015"),
                 "v_shape": (".00036607", ".00000036607", None),
                 "r_shape": (".00025671", ".00000025671", ".0000511")}
    if name in constants:
        a, v, embedded = constants[name]
        require(F(exp["area"]) == F(a) and F(exp["volume"]) == F(v), "published exact constants")
        if embedded:
            require(F(exp["embedded_area"]) == F(embedded), "published embedded constant")
    with localcontext() as context:
        context.prec = 60
        length = Decimal(0)
        for a, b in pairs(ring):
            if any(a[d] == b[d] == bound for d in (0, 1) for bound in (F(0), F(".02"))):
                continue
            squared = sum((b[d]-a[d])**2 for d in (0, 1))
            length += (Decimal(squared.numerator) / Decimal(squared.denominator)).sqrt()
        want = length * Decimal(".001")
        require(abs(Decimal(exp["embedded_area"])/want - 1) <= Decimal("1e-12"), "embedded area")
        if name == "v_shape":
            oracle = 2*(Decimal(".0039")**2 + Decimal(".0087")**2).sqrt()*Decimal(".001")
            require(abs(Decimal(exp["embedded_area"])/oracle - 1) <= Decimal("1e-12"), "V irrational oracle")
            require(len(Decimal(exp["embedded_area"]).as_tuple().digits) == 17, "V significant digits")
    # Float checks are separate from the exact equalities above.
    for exact, recorded in ((measure(ring), exp["area"]), (measure(ring)*F(".001"), exp["volume"])):
        require(abs(float(exact)/float(recorded)-1) <= 1e-12, "floating area/volume cross-check")
    float_embedded = sum(math.hypot(float(b[0]-a[0]), float(b[1]-a[1]))
                         for a, b in pairs(ring)
                         if not any(a[d] == b[d] == bound for d in (0, 1)
                                    for bound in (F(0), F(".02")))) * .001
    require(abs(float_embedded/float(exp["embedded_area"])-1) <= 1e-12,
            "floating embedded-area cross-check")
    if name == "l1_corner":
        h, fractions = F(".0025"), {}
        for i in range(8):
            for j in range(8):
                cell = [(i*h, j*h), ((i+1)*h, j*h), ((i+1)*h, (j+1)*h), (i*h, (j+1)*h)]
                f = sum((measure(convex_intersection(p, cell)) for p in blocks), F(0)) / h**2
                for k in range(2):
                    fractions[i, j, k] = f
        require([f for (i, j, k), f in fractions.items() if i == j == 5] == [F(".00079984")]*2, "L1 corners")
        require([f for (i, j, k), f in fractions.items() if (i >= 6 and j == 5) or (i == 5 and j >= 6)] == [F(".0004")]*8,
                "L1 strips")
        require(exp["corner_count"] == 2 and exp["strip_count"] == 8, "L1 counts")
        recorded = {tuple(c["cell"]): F(c["sub_volfrac"]) for c in exp["cut_cells"]}
        require(len(recorded) == len(exp["cut_cells"]) and recorded == {c: f for c, f in fractions.items() if 0 < f < 1},
                "L1 cut cell set")
        for threshold, count in (("0.001", 10), ("0.0005", 8)):
            require(sum(0 < f < F(threshold) for f in fractions.values()) == exp["small_subs"][threshold] == count,
                    "L1 completed union small_subs")
    if name == "convex_split2":
        require([polygon(p) for p in inp["single_piece"]] == [ring] and len(blocks) == 2, "convex variants")
        require(normalize(polygon(inp["repeated_closing_outline"], False)) == ring, "closing equivalence")
        require(normalize(polygon(inp["consecutive_duplicate_outline"], False)) == ring, "adjacent equivalence")
        require(exp["duplicate_points_equivalent"] is True, "consumer duplicate expectation")
        require(all(F(exp["tolerance"][k]) == F("1e-14") for k in ("sub_volfrac_rel", "sq_w_rel")), "convex tolerance")
    if name == "chamfer_lid_octagon":
        require(len(ring) == 8 and blocks == [ring] and inp["internal"] == [[False]*8], "octagon fast path input")
        require(measure(ring) == F(".000382") and exp["legacy_all_fields_bitwise_equal"] is True, "octagon oracle")
    require(exp["internal_side_minus1_faces"] == 0, "consumer internal face expectation")


def invalid_inputs(inp, exp):
    ins = {c["case"]: c for c in inp["cases"]}
    outs = {c["case"]: c for c in exp["cases"]}
    require(len(ins) == len(inp["cases"]) and ins.keys() == outs.keys(), "error case linkage")
    require(ins["empty_pieces"]["pieces"] == [], "empty input")
    c = ins["internal_mismatch"]
    require(any(len(p) != len(f) for p, f in zip(c["pieces"], c["internal"])), "mismatch input")
    require(ins["nonzero_plane_body"]["pieces"][0][0]["body"] != 0, "plane body input")
    c = ins["outline_phi_mismatch"]
    require(len(c["outline_phi"]) != c["node_count"], "phi input")
    require(len(set(polygon(ins["short_ring"]["outline"], False))) < 3, "short input")
    require(measure(polygon(ins["zero_area_ring"]["outline"], False)) == 0, "degenerate input")
    ring = polygon(ins["self_intersection"]["outline"], False)
    require(measure(ring) != 0, "self-intersection precedence")
    es = list(pairs(ring))
    require(any(determinant(a,b,c)*determinant(a,b,d) < 0 and determinant(c,d,a)*determinant(c,d,b) < 0
                for i, (a,b) in enumerate(es) for c,d in es[i+1:]), "crossing input")
    require(len(ins["multiple_rings"]["rings"]) > 1, "multi ring input")
    bodies = [u["body"] for u in ins["duplicate_body"]["unions"]]
    require(all(type(b) is int for b in bodies) and len(set(bodies)) < len(bodies), "duplicate BodyId input")
    required = {
        "empty_pieces": ("cut/union", "invalid piece"),
        "internal_mismatch": ("cut/union", "invalid piece"),
        "nonzero_plane_body": ("cut/union", "invalid piece"),
        "outline_phi_mismatch": ("cut/union", "outline_phi size mismatch"),
        "short_ring": ("CUT_UNSUPPORTED", "region0: degenerate polygon"),
        "zero_area_ring": ("CUT_UNSUPPORTED", "region0: degenerate polygon"),
        "self_intersection": ("CUT_UNSUPPORTED", "region0: self-intersecting polygon"),
        "multiple_rings": ("CUT_UNSUPPORTED", "region0: multiple rings"),
        "duplicate_body": ("CUT_UNSUPPORTED", "body0: duplicate piece union"),
        "zero_normal": ("cut/plane", "nonfinite or zero normal"),
        "invalid_thresholds": ("cut/options", "invalid thresholds"),
        "nonpositive_box": ("cut/mesh", "positive affine box required"),
        "disconnected_cell": ("outline.unsupported", "outline: disconnected solid in one cell")}
    require({k: (r["path"], r["reason"]) for k, r in outs.items()} == required, "exact error paths/reasons")


def source_tables(text, generator):
    """Parse the small C++ aggregate grammar, keeping exact literal values/order."""
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    number = r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?"
    token = re.compile(number + r"|[{}(),]|[A-Za-z_]\w*")

    def read_aggregate(tokens, env, pos=0):
        require(pos < len(tokens), "truncated source table")
        t = tokens[pos]
        if t != "{":
            require(t in env or re.fullmatch(number, t), "unexpected source coordinate")
            return env[t] if t in env else F(t), pos+1
        values, pos = [], pos+1
        while pos < len(tokens) and tokens[pos] != "}":
            v, pos = read_aggregate(tokens, env, pos)
            values.append(v)
            if pos < len(tokens) and tokens[pos] == ",":
                pos += 1
            else:
                break
        require(pos < len(tokens) and tokens[pos] == "}", "invalid source aggregate")
        return values, pos+1

    for name in ("l_shape", "v_shape", "r_shape"):
        found = re.search(r"Shape\s+" + name + r"\s*\(([^)]*)\)\s*\{\s*return\s*(.*?);\s*\}", text, re.S)
        require(found is not None, "source missing " + name)
        params, aggregate = found.groups()
        env = {key: F(value) for key, value in re.findall(r"Real\s+(\w+)\s*=\s*(" + number + ")", params)}
        tokens = token.findall(aggregate)
        # Only parse outline and blocks; trailing scalar expressions include std::hypot.
        require(tokens[0] == "{", "source shape aggregate")
        outline, end = read_aggregate(tokens, env, 1)
        require(tokens[end] == ",", "source table separator")
        blocks, _ = read_aggregate(tokens, env, end+1)
        inp, _ = generator.build(name)
        expected_ring = [list(p) for p in polygon(inp["outline"])]
        expected_blocks = [[list(p) for p in polygon(b)] for b in inp["pieces"]]
        require(outline == expected_ring and blocks == expected_blocks, "ordered source drift: " + name)
    found = re.search(r"void\s+union_small_subs\s*\(\s*\)\s*\{\s*const\s+auto\s+s\s*=\s*l_shape\s*\(([^)]*)\)", text)
    require(found is not None, "missing L1 source")
    require([F(v.strip()) for v in found[1].split(",")] == [F(".012501")]*2, "L1 source parameter drift")


def branch_check(repo, ref, generator):
    if not shutil.which("git"):
        print("SKIP: git unavailable for optional source comparison")
        return
    def git(*args):
        return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=False)
    if git("rev-parse", "--verify", ref + "^{commit}").returncode:
        print("SKIP: optional source ref unavailable")
        return
    result = git("show", ref + ":" + SOURCE)
    require(result.returncode == 0, "existing source ref has no fixture source")
    source_tables(result.stdout, generator)
    print("optional source comparison: OK")


def load_json(path):
    require(path.is_file(), "missing required data: " + path.name)
    raw = path.read_bytes()
    require(b"\r" not in raw and raw.endswith(b"\n"), "expected UTF-8/LF text")
    def unique(items):
        out = {}
        for key, value in items:
            require(key not in out, "duplicate JSON key: " + key)
            out[key] = value
        return out
    return json.loads(raw.decode("utf-8"), object_pairs_hook=unique)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=HERE.parents[3])
    ap.add_argument("--branch-ref")
    args = ap.parse_args()
    if not args.repo.is_dir():
        print("repository directory unavailable", file=sys.stderr)
        return 2
    root = args.repo / "tests" / "contract" / "fixtures" / "cutgeom-nc-1.0"
    try:
        paths = ["expected.json"] + [case + "/" + filename for case in CASES
                                      for filename in ("input.json", "expected.json")]
        actual = {relative: load_json(root / relative) for relative in paths}
        independent_schema(actual)
        require(tuple(actual["expected.json"]["cases"]) == CASES, "manifest cases")
        for case in CASES:
            inp, exp = actual[case + "/input.json"], actual[case + "/expected.json"]
            if case == "errors":
                invalid_inputs(inp, exp)
            else:
                geometry(case, inp, exp)
        # Independent checks above validate read data; canonical comparison also
        # enforces provenance, exact source decimals and the closed data schema.
        spec = importlib.util.spec_from_file_location("cutgeom_fixture_gen", HERE / "gen.py")
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        templates = generator.payloads()
        for relative, template in templates.items():
            schema(actual[relative], template, relative)
            require(actual[relative] == template, "canonical value mismatch: " + relative)
            require((root / relative).read_bytes() == generator.dump(template).encode("utf-8"),
                    "serialization differs from generator: " + relative)
        allowed = set(templates) | {"gen.py", "check.py"}
        present = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()
                   and "__pycache__" not in p.parts and p.name != "test_acceptance.py"}
        require(present == allowed, "fixture relative file set differs")
        if args.branch_ref:
            branch_check(args.repo, args.branch_ref, generator)
    except (AssertionError, ValueError, KeyError, TypeError, UnicodeError) as exc:
        print("FAIL", exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print(exc, file=sys.stderr)
        return 2
    print("cutgeom-nc/1.0: OK (fixture geometry; C++ consumer assertions remain declarative)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
