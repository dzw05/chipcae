"""Dense spectra, including the natural-BC quotient by the physical kernel."""
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'matrices.txt'
    subprocess.run([sys.argv[1], str(path)], check=True)
    estimates = {}
    failures = []
    with path.open() as file:
        for _ in range(2400):
            layered, aspect, elastic, position, gamma, variant, size = map(float, file.readline().split())
            size = int(size)
            matrix = np.array([list(map(float, file.readline().split())) for _ in range(size)])
            for scaled in (False, True):
                if scaled:
                    diag = np.sqrt(np.diag(matrix))
                    matrix = matrix / (diag[:, None] * diag[None, :])
                eigen = np.linalg.eigvalsh(matrix)
                kernel = (6 if elastic else 1) if variant == 0 else 0
                if kernel:
                    # Roundoff relative to the operator norm; also verify that
                    # the next eigenvalue is separated from this numerical kernel.
                    if np.max(np.abs(eigen[:kernel])) > 1e-10 * eigen[-1]:
                        failures.append(('kernel', layered, aspect, elastic, position, gamma, scaled))
                    if gamma and eigen[kernel] <= 1e-10 * eigen[-1]:
                        failures.append(('extra kernel', layered, aspect, elastic, position, gamma, scaled))
                condition = np.inf if eigen[kernel] <= 0 else eigen[-1] / eigen[kernel]
                key = (layered, aspect, elastic, gamma, variant, scaled)
                estimates.setdefault(key, []).append(condition)
    for key, values in estimates.items():
        ratio = max(values) / min(values)
        print('layered aspect elastic gamma variant scaled', key,
              'minimum', min(values), 'maximum', max(values), 'ratio', ratio, flush=True)
        if key[3]:
            if not np.isfinite(values).all() or ratio > 10:
                failures.append((key, ratio))
    assert not failures, failures
