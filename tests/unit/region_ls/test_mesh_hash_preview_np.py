"""Exercise the actual preview digest; MPI ranks require an MPI-enabled build."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    build = next(p for p in [Path.cwd(), *Path.cwd().parents]
                 if (p / 'CMakeCache.txt').exists())
    preview = build / 'preview/chipcae-preview'
    cache = (build / 'CMakeCache.txt').read_text()
    with tempfile.TemporaryDirectory(prefix='sp5-preview-') as tmp:
        root = Path(tmp)
        stack = root / 'lid.stack.json'
        stack.write_text(json.dumps(dict(
            id='lid', unit='m', outline=dict(box=[0, 0, 8, 8]),
            symmetry='none', package_type='FCBGA', layers=[dict(
                name='void_layer', thickness=1, kind='Explicit', n_elem_min=2,
                grading=1, regions=[dict(name='lid', body='lid', material='solid',
                                        footprint=dict(box=[2, 2, 6, 6]))])])))
        hashes = []
        for ranks in (1, 2, 3):
            launcher = []
            if ranks > 1:
                mpi = shutil.which('mpiexec') or shutil.which('mpirun')
                if not mpi or 'CHIPCAE_ENABLE_MPI:BOOL=ON' not in cache:
                    print('SKIP: np=2,3 require MPI launcher and MPI-enabled preview')
                    return 77
                launcher = [mpi, '-n', str(ranks)]
            env = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1',
                       OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1')
            result = subprocess.run(
                [*launcher, str(preview), str(stack), '--nx', '12', '--ny', '12',
                 '--levels', '1', '-o', str(root / f'out-{ranks}')],
                capture_output=True, text=True, env=env, timeout=40)
            assert result.returncode == 0, (result.stdout, result.stderr)
            events = [json.loads(line) for line in result.stdout.splitlines()]
            summary, = [e for e in events if e.get('stage') == 'success']
            candidate = summary['nx0'] * summary['ny0'] * summary['nz']
            assert summary['cell_count'] < candidate, summary
            assert summary['void_cells'] == candidate - summary['cell_count'], summary
            hashes.append(summary['mesh_sha256'])
        assert len(set(hashes)) == 1, hashes
        print('np=1,2,3 mesh_sha256 identical:', hashes[0])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
