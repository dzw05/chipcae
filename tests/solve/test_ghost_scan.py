"""Dense symmetric eigenvalue estimates for translated Q2 cut interfaces."""
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)/'matrices.txt'
    subprocess.run([sys.argv[1],str(path)],check=True)
    estimates = {}
    raw_estimates = {}
    with path.open() as file:
        for _ in range(800):
            layered, aspect, elastic, position, gamma, size = map(float, file.readline().split())
            size = int(size)
            matrix = np.array([list(map(float,file.readline().split())) for _ in range(size)])
            # Symmetric Jacobi scaling gives a dimensionless estimate, including
            # the 1:10 normal aspect-ratio case. Keep the zero-penalty comparison.
            raw = np.linalg.eigvalsh(matrix)
            raw_condition = np.inf if raw[0] <= 0 else raw[-1]/raw[0]
            raw_estimates.setdefault((layered,aspect,elastic,gamma),[]).append(raw_condition)
            diag = np.sqrt(np.diag(matrix))
            matrix /= diag[:,None]*diag[None,:]
            eigen = np.linalg.eigvalsh(matrix)
            condition = np.inf if eigen[0] <= 0 else eigen[-1]/eigen[0]
            estimates.setdefault((layered,aspect,elastic,gamma),[]).append(condition)
    for (layered,aspect,elastic,gamma),values in estimates.items():
        ratio = max(values)/min(values)
        print('layered',layered,'aspect',aspect,'elastic',elastic,'gamma',gamma,'minimum',min(values),'maximum',max(values),'ratio',ratio,flush=True)
        if gamma:
            assert np.isfinite(values).all()
            assert ratio <= 10, ratio

    failures = []
    for (layered,aspect,elastic,gamma),values in raw_estimates.items():
        ratio = max(values)/min(values)
        print('unscaled layered',layered,'aspect',aspect,'elastic',elastic,'gamma',gamma,'minimum',min(values),'maximum',max(values),'ratio',ratio,flush=True)
        if gamma:
            assert np.isfinite(values).all()
            if ratio > 10:
                failures.append((layered,aspect,elastic,ratio))

    assert not failures, failures
