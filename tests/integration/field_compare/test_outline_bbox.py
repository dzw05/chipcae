# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Always test plain-stack GMG/direct; conditionally test the bbox outline."""
from pathlib import Path
import sys
import tempfile

from fixtures import measured_comparison, solve, solver_environment


def assert_gmg(logs):
    assert sum(b"U column_gmg L=" in line for line in logs) == 1, logs
    assert not any(b"not yet available" in line for line in logs), logs


def main():
    exe, source = solver_environment(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="field-outline-") as tmp:
        root = Path(tmp)
        direct, _ = solve(root, "plain-direct", exe, source, levels=2)
        plain, logs = solve(root, "plain-gmg", exe, source, levels=2, preconditioner="column_gmg")
        assert_gmg(logs)
        measured_comparison(direct, plain, "same-mesh-solver")
        blocker = "has_cut || !m.pending_cut.empty()"
        if blocker in (source / "solvers/fem/src/column_gmg.cpp").read_text():
            print("SKIP: BLOCKED-BY #205 (column_gmg rejects outline without cut cells)")
            return 77
        bbox, logs = solve(root, "bbox-gmg", exe, source, levels=2,
                           preconditioner="column_gmg", bbox=True)
        assert_gmg(logs)
        measured_comparison(plain, bbox, "same-mesh-solver")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
