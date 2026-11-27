"""A wired FVM elastic solve must not publish a failed one-iteration result."""

import argparse
from pathlib import Path
import re
import sys
import tempfile

from test_fvm_solver_convergence import (check, configure, no_success, output_text)
from fvm_case_util import bimaterial_strip, solve


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe')
    parser.add_argument('mpiexec')
    args = parser.parse_args()
    configure(args.exe, args.mpiexec)
    checks = []
    with tempfile.TemporaryDirectory(prefix='fvm-elastic-convergence-') as tmp:
        root = Path(tmp)
        case = bimaterial_strip(root)
        case['solver']['linear'].update(preconditioner='jacobi', max_iter=1, rtol=1e-14)
        out, process = solve(root, case, 'elastic-failure', expected=None)
        text = output_text(out, process)
        print(f'elastic-failure actual output:\n{text}', flush=True)
        # Only an explicit unavailable-stage diagnostic permits the soft-dependency skip.
        if 'not solved: fvm thermal-only stage' in text:
            print('FVM elasticity is not wired: return 77', flush=True)
            return 77
        check(checks, 'elastic-failure nonzero exit', process.returncode != 0,
              process.returncode)
        no_success(out, checks, 'elastic-failure')
        check(checks, 'elastic-failure nonconvergence prefix',
              any(line.startswith('chipcae-solve: fvm.elastic: FVM elastic nonconvergence: ')
                  for line in process.stderr.splitlines()),
              process.stderr)
        check(checks, 'elastic-failure PETSc reason retained',
              'KSP failure reason=-' in process.stderr and
              re.search(r'KSP failure reason=-[0-9]+', process.stderr) is not None,
              process.stderr)
    failures = [name for name, passed in checks if not passed]
    assert not failures, 'Failed acceptance assertions: ' + ', '.join(failures)
    return 0


if __name__ == '__main__':
    sys.exit(main())
