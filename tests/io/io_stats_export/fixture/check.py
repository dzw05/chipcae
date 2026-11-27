#!/usr/bin/env python3
"""m5-io-stats-export/1.0 跨模块 fixture 校验：python3 check.py [io_stats.json ...]；无参则自检。"""
import json, math, sys, copy, pathlib
KEYS = {"schema": str, "ranks": int, "write_step_seconds": float, "finalize_seconds": float,
        "last_write_step_seconds": float, "write_step_count": int, "bytes_written": int, "peak_rss_bytes": int}
def check(d):
    if not isinstance(d, dict): return "not an object"
    if set(d) != set(KEYS): return f"keys mismatch: {sorted(set(d) ^ set(KEYS))}"
    if d["schema"] != "chipcae.io_stats/1.0": return "bad schema"
    for k, t in KEYS.items():
        v = d[k]
        if k == "schema": continue
        if isinstance(v, bool): return f"{k} is bool"
        if t is int and not isinstance(v, int): return f"{k} not integer"
        if t is float and not isinstance(v, (int, float)): return f"{k} not number"
        if not math.isfinite(v) or v < 0: return f"{k} not finite non-negative"
    if d["ranks"] < 1: return "ranks < 1"
    if d["last_write_step_seconds"] > d["write_step_seconds"] + 1e-12 and d["write_step_count"] > 0:
        return "last_write_step_seconds > write_step_seconds"
    return None
def selftest():
    base = json.loads((pathlib.Path(__file__).parent / "example_np2.json").read_text())
    assert check(base) is None, check(base)
    bad = []
    for mut in [lambda d: d.pop("ranks"), lambda d: d.update(extra=1), lambda d: d.update(schema="x"),
                lambda d: d.update(bytes_written=1.5), lambda d: d.update(ranks=0),
                lambda d: d.update(finalize_seconds=float("nan")), lambda d: d.update(peak_rss_bytes=-1),
                lambda d: d.update(write_step_count=True)]:
        d = copy.deepcopy(base); mut(d); bad.append(check(d) is not None)
    assert all(bad), bad
    print("OK")
if __name__ == "__main__":
    if len(sys.argv) == 1: selftest()
    else:
        rc = 0
        for p in sys.argv[1:]:
            try: e = check(json.loads(pathlib.Path(p).read_text(encoding="utf-8")))
            except Exception as x: e = f"unreadable: {x}"
            print(p, "OK" if e is None else "FAIL " + e); rc |= e is not None
        sys.exit(rc)
