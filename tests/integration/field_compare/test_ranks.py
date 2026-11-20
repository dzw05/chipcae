# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Owned-cell and field equivalence for a synthetic stack at np=1 and np=2."""
from pathlib import Path
import sys
import tempfile

from fixtures import measured_comparison, mpi_launcher, owned_ids, solve, solver_environment


def main():
    exe, source = solver_environment(sys.argv[1])
    launcher = mpi_launcher()
    with tempfile.TemporaryDirectory(prefix="field-ranks-") as tmp:
        root = Path(tmp)
        ref, _ = solve(root, "np1", exe, source, ranks=1)
        other, _ = solve(root, "np2", exe, source, ranks=2, launcher=launcher)
        owned_ids(ref)
        owned_ids(other)
        measured_comparison(ref, other, "ranks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
