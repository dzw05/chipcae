#!/usr/bin/env python3
"""Compute dense symmetric spectra of the dumped sweep matrices using NumPy."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('--results', type=Path, default=Path('results'))
r = parser.parse_args().results
with (r / 'gp_sweep.csv').open() as f:
    rows = list(csv.DictReader(f))
fields = ['physics', 'vf', 'gamma_N', 'gamma_g', 'ndof', 'lambda_min', 'lambda_max',
          'kappa', 'negative_eigenvalues', 'indefinite', 'jacobi_lambda_min',
          'jacobi_lambda_max', 'jacobi_kappa']
with (r / 'gp_spectrum.csv').open('w', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=fields)
    writer.writeheader()
    for row in rows:
        name = f"{row['physics']}_N{float(row['gamma_N']):g}_g{float(row['gamma_g']):g}_vf{float(row['vf']):g}.mtx"
        path = r / 'debug' / name
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        cache_path = r / 'debug' / (name + '.spectrum.json')
        if cache_path.exists():
            cache = json.loads(cache_path.read_text())
            if cache.get('sha256') == digest:
                writer.writerow(cache['result'])
                print(name + ' (cached)', flush=True)
                continue
        with path.open() as matrix:
            header = matrix.readline().strip()
            if header != '%%MatrixMarket matrix coordinate real general':
                raise ValueError(f'Unexpected matrix format: {header}')
            n, m, count = map(int, matrix.readline().split())
            data = np.loadtxt(matrix, ndmin=2)
        if n != m or len(data) != count or n != int(row['ndof']):
            raise ValueError(f'Invalid matrix dimensions: {name}')
        a = np.zeros((n, n))
        a[data[:, 0].astype(int) - 1, data[:, 1].astype(int) - 1] = data[:, 2]
        if not np.allclose(a, a.T, rtol=1e-12, atol=1e-14):
            raise ValueError(f'Non-symmetric matrix: {name}')
        ev = np.linalg.eigvalsh(a)
        diag = np.diag(a)
        if np.all(diag > 0):
            scale = 1 / np.sqrt(diag)
            jev = np.linalg.eigvalsh(a * scale[:, None] * scale[None, :])
            jmin, jmax = jev[0], jev[-1]
            jk = jmax / jmin if jmin > 0 else float('nan')
        else:
            jmin = jmax = jk = float('nan')
        result = dict(physics=row['physics'], vf=row['vf'], gamma_N=row['gamma_N'],
                             gamma_g=row['gamma_g'], ndof=n, lambda_min=ev[0], lambda_max=ev[-1],
                             kappa=ev[-1] / ev[0] if ev[0] > 0 else float('nan'),
                             negative_eigenvalues=np.count_nonzero(ev < 0), indefinite=int(ev[0] <= 0),
                             jacobi_lambda_min=float(jmin), jacobi_lambda_max=float(jmax), jacobi_kappa=float(jk))
        result = {key: str(value) for key, value in result.items()}
        writer.writerow(result)
        cache_path.write_text(json.dumps({'sha256': digest, 'result': result}))
        print(name, flush=True)
