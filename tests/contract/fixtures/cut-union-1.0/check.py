#!/usr/bin/env python3
"""cut-union/1.0 (I10) fixture checker. Standard library only.

  python3 check.py                 self-check: files == gen.py output, plus independent invariants
                                   (assertion constants of #202 vs exact Fraction values, CCW/convex
                                   pieces, sum of piece areas == outline area, internal flags)
  python3 check.py --actual FILE   compare an implementation dump (format of dump_actual.cpp,
                                   e.g. actual-pr202-0d97b59.json) against expected.json
Tolerances (contract section 6): per-cell sub_volfrac abs 1e-12; solid volume and embedded area
rel 1e-10 (same as the #202 assertions); counts, error path/reason and bitwise flags exact.
Exit code 0 = pass, 1 = any failure.
"""
import argparse, importlib.util, json, math, os, sys
sys.dont_write_bytecode = True
from fractions import Fraction as F

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gen", os.path.join(HERE, "gen.py"))
gen = importlib.util.module_from_spec(spec); spec.loader.exec_module(gen)
FAILS = []

def fail(msg):
    FAILS.append(msg); print("FAIL", msg)

def rel_ok(a, e, rel):
    return math.isfinite(a) and abs(a / e - 1) <= rel

def load(case, fn):
    with open(os.path.join(HERE, case, fn)) as f:
        return json.load(f)

def selfcheck():
    cases = gen.build()
    for name, (inp, exp) in cases.items():
        for fn, obj in (("input.json", inp), ("expected.json", exp)):
            try:
                disk = load(name, fn)
            except OSError as e:
                fail(f"{name}/{fn}: {e}"); continue
            if json.dumps(disk, sort_keys=True) != json.dumps(json.loads(json.dumps(obj)), sort_keys=True):
                fail(f"{name}/{fn}: differs from gen.py output (edited by hand?)")
        if not exp["ok"]:
            if exp["path"] not in ("cut/union", "cut/options", "cut/plane", "outline.unsupported"):
                fail(f"{name}: unknown error path {exp['path']}")
            continue
        # independent invariants from disk input
        disk_in, disk_exp = load(name, "input.json"), load(name, "expected.json")
        outline = [(F(x), F(y)) for x, y in disk_in["outline"]]
        blocks = [[(F(x), F(y)) for x, y in b] for b in disk_in["blocks"]]
        ua = gen.area(outline)
        if sum(gen.area(b) for b in blocks) != ua:
            fail(f"{name}: sum of piece areas != outline area")
        for k, b in enumerate(blocks):
            if gen.area(b) <= 0:
                fail(f"{name}: piece {k} not CCW")
            for i in range(len(b)):
                a, c, d = b[i], b[(i + 1) % len(b)], b[(i + 2) % len(b)]
                if (c[0] - a[0]) * (d[1] - c[1]) - (c[1] - a[1]) * (d[0] - c[0]) < 0:
                    fail(f"{name}: piece {k} not convex")
            for i, pl in enumerate(disk_in["pieces"][k]):
                a, c = b[i], b[(i + 1) % len(b)]
                if pl["internal"] == gen.original_edge(a, c, outline):
                    fail(f"{name}: piece {k} plane {i} internal flag wrong")
                if abs(math.hypot(pl["n"][0], pl["n"][1]) - 1) > 1e-15 or pl["n"][2] != 0:
                    fail(f"{name}: piece {k} plane {i} normal not unit/xy")
        if F(disk_exp["solid_volume_exact"]) != ua * gen.THICK:
            fail(f"{name}: solid_volume_exact != area*thickness")
        fr = sum(F(s) for s in disk_exp["cells_exact"]) + (disk_exp["n_cells"] - disk_exp["n_cut"])
        if fr * gen.H * gen.H * gen.HZ != ua * gen.THICK:
            fail(f"{name}: per-cell fractions do not sum to the solid volume")
        if not rel_ok(disk_exp["solid_volume"], float(F(disk_exp["solid_volume_exact"])), 1e-15):
            fail(f"{name}: solid_volume float mismatch")
        a = disk_exp.get("assert", {})
        if "volume" in a:
            if F(a["volume"]) != ua * gen.THICK:
                fail(f"{name}: #202 asserted volume {a['volume']} != exact {ua * gen.THICK}")
            ea = 2 * math.hypot(.0039, .0087) * .001 if a["embedded_area"].startswith("2*hypot") else float(a["embedded_area"])
            if not rel_ok(disk_exp["embedded_area"], ea, 1e-12):
                fail(f"{name}: #202 asserted embedded area {a['embedded_area']} != exterior perimeter")
        if "corner_fraction" in a:
            corner = [c for c in disk_exp["cells"] if c[0] == 5 and c[1] == 5]
            strip = [c for c in disk_exp["cells"] if (c[0] >= 6 and c[1] == 5) or (c[0] == 5 and c[1] >= 6)]
            if len(corner) != a["corners"] or len(strip) != a["strips"]:
                fail(f"{name}: L1 cell selection {len(corner)}/{len(strip)} != 2/8")
            if any(not rel_ok(c[3], float(a["corner_fraction"]), a["rel"]) for c in corner):
                fail(f"{name}: L1 corner fraction != 7.9984e-4")
            if any(not rel_ok(c[3], float(a["strip_fraction"]), a["rel"]) for c in strip):
                fail(f"{name}: L1 strip fraction != 4e-4")
            th = float(disk_in["options"]["small_sub_vf"])
            want = sum(1 for c in disk_exp["cells"] if c[3] < th)
            if want != disk_exp["n_small"]:
                fail(f"{name}: n_small {disk_exp['n_small']} != count(fraction < {th}) = {want}")
    return len(cases)

def compare(actual_path):
    with open(actual_path) as f:
        act = json.load(f)
    n = 0
    for name in gen.build():
        exp = load(name, "expected.json"); n += 1
        a = act.get(name)
        if a is None:
            fail(f"{name}: missing in actual"); continue
        if a.get("ok") != exp["ok"]:
            fail(f"{name}: ok={a.get('ok')} expected {exp['ok']} ({a.get('path')}: {a.get('reason')})"); continue
        if not exp["ok"]:
            for k in ("path", "reason", "mesh_unchanged"):
                if a.get(k) != exp[k]:
                    fail(f"{name}: {k}={a.get(k)!r} expected {exp[k]!r}")
            continue
        for k in ("solid_volume", "embedded_area"):
            if not rel_ok(a[k], exp[k], 1e-10):
                fail(f"{name}: {k}={a[k]!r} expected {exp[k]!r} (rel 1e-10)")
        for k in ("n_cells", "n_cut") + (("n_small",) if "n_small" in exp else ()):
            if a.get(k) != exp[k]:
                fail(f"{name}: {k}={a.get(k)} expected {exp[k]}")
        ec = {tuple(c[:3]): c[3] for c in exp["cells"]}
        ac = {tuple(c[:3]): c[3] for c in a.get("cells", [])}
        if set(ec) != set(ac):
            fail(f"{name}: cut cell set differs: missing {sorted(set(ec) - set(ac))[:5]} extra {sorted(set(ac) - set(ec))[:5]}")
        for key in set(ec) & set(ac):
            if not abs(ac[key] - ec[key]) <= 1e-12:
                fail(f"{name}: cell {key} sub_volfrac={ac[key]!r} expected {ec[key]!r} (abs 1e-12)")
        if "bitwise_vs_build_cut_layer" in exp:
            b = act.get(name.split("_")[0] + "_bitwise", {})
            for k in ("cut_equal", "mesh_equal"):
                if b.get(k) is not True:
                    fail(f"{name}: convex input not bitwise identical to build_cut_layer ({k}={b.get(k)})")
            if b.get("cut_fnv") != b.get("legacy_fnv"):
                fail(f"{name}: bitwise hash differs")
        if "pieces" in a:  # helper planes must equal the fixture planes (same float recipe as piece())
            ep = load(name, "input.json")["pieces"]
            for k, (pa, pe) in enumerate(zip(a["pieces"], ep)):
                for i, (x, y) in enumerate(zip(pa, pe)):
                    if x["internal"] != y["internal"] or any(abs(u - v) > 1e-15 for u, v in zip(x["n"] + [x["d"]], y["n"] + [y["d"]])):
                        fail(f"{name}: piece {k} plane {i} differs from input.json")
    return n

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--actual")
    args = ap.parse_args()
    n = selfcheck()
    print(f"selfcheck: {n} cases, {len(FAILS)} failures")
    if args.actual:
        before = len(FAILS)
        m = compare(args.actual)
        print(f"actual {os.path.basename(args.actual)}: {m} cases, {len(FAILS) - before} failures")
    print("PASS" if not FAILS else f"FAILED ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)

if __name__ == "__main__":
    main()
