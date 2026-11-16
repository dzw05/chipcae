import os
import pathlib
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory() as tmp:
    blobs = []
    for threads in [1, 4]:
        path = pathlib.Path(tmp)/str(threads)
        env = dict(os.environ, OMP_NUM_THREADS=str(threads), OMP_PROC_BIND='false', K3_BATCH_DUMP=str(path))
        subprocess.run([sys.argv[1], 'T15'], env=env, check=True)
        blobs.append(path.read_bytes())
    assert blobs[0] == blobs[1], 'seed=20261116 batch state/stress differs between 1 and 4 threads'
print('METRIC {"id":"T15","measured_error":0,"tolerance":0,"seed":20261116}')
