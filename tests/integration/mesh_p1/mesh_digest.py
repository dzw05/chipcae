"""Independent schema/README.md byte-contract oracle; no MPI needed by default."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def golden(nx):
    # Explicit global-node order (Morton columns, then z) and hex connectivity.
    columns = [(0, 0), (1, 0), (0, 1), (1, 1)]
    cells = [(0, 2, 6, 4, 1, 3, 7, 5)]
    if nx == 2:
        columns += [(2, 0), (2, 1)]
        cells += [(2, 8, 10, 6, 3, 9, 11, 7)]
    data = struct.pack('<6q', nx, 1, 0, 1, len(columns) * 2, nx)
    for x, y in columns:
        for z in (0, 1):
            data += struct.pack('<3d', float(x), float(y), float(z))
    for cell in cells:
        data += struct.pack('<8q', *cell)
    return hashlib.sha256(data).hexdigest()


def run(preview, stack, output, nx, launcher):
    result = subprocess.run(
        [*launcher, preview, str(stack), '--nx', str(nx), '--ny', '1',
         '--levels', '1', '-o', str(output)],
        check=False, capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, (result.args, result.stdout, result.stderr)
    events = [json.loads(line) for line in result.stdout.splitlines()]
    success = [event for event in events if event['stage'] == 'success']
    assert len(success) == 1, events
    summary = success[0]
    assert json.loads(summary['message'])['mesh_sha256'] == summary['mesh_sha256']
    for key, value in dict(nx0=nx, ny0=1, L=0, nz=1, padded_cells=0,
                           node_count=(nx + 1) * 4, cell_count=nx).items():
        assert type(summary[key]) is int and summary[key] == value, summary
    return summary['mesh_sha256']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('preview')
    parser.add_argument('--mpiexec')
    parser.add_argument('--numproc-flag', default='-n')
    parser.add_argument('--preflag', action='append', default=[])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mesh-digest-') as tmp:
        root = Path(tmp)
        for nx in (1, 2) if not args.mpiexec else (6,):
            stack = root / 'tiny.stack.json'
            stack.write_text(json.dumps(dict(
                id='tiny', unit='m', outline=dict(box=[0, 0, nx, 1]),
                symmetry='none', package_type='FCBGA', layers=[dict(
                    name='solid', thickness=1, kind='Homogenized', material='solid_eq',
                    n_elem_min=1, grading=1)])))
            baseline = run(args.preview, stack, root / f'grid-{nx}-1', nx, [])
            if args.mpiexec:
                for ranks in (2, 3):
                    actual = run(args.preview, stack, root / f'grid-{nx}-{ranks}', nx,
                                 [args.mpiexec, args.numproc_flag, str(ranks), *args.preflag])
                    assert actual == baseline, (ranks, baseline, actual)
                    print(f'{ranks} ranks == 1 rank: {actual}', flush=True)
            else:
                expected = golden(nx)
                assert baseline == expected, (nx, expected, baseline)
                print(f'{nx}x1x1 golden: {expected}', flush=True)


if __name__ == '__main__':
    main()
