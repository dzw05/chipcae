"""D10 real CLI/JSONL fixture; test controls live in the input stack's mock object."""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('stack')
p.add_argument('--polyset')
p.add_argument('--nx', type=int)
p.add_argument('--ny', type=int)
p.add_argument('--levels', type=int, default=0)
p.add_argument('--max-ratio', type=float, default=10)
p.add_argument('-o', required=True)
p.add_argument('--surface')
a = p.parse_args()
if (a.nx is None) != (a.ny is None) or (a.nx is not None and min(a.nx, a.ny) <= 0):
    p.error('nx/ny must be paired positive integers')
if a.levels < 0 or a.max_ratio < 1:
    p.error('invalid levels/max-ratio')
c = json.loads(Path(a.stack).read_text()).get('mock', {})
mode = c.get('mode', 'success')
if a.polyset:
    json.loads(Path(a.polyset).read_text())
if c.get('argv'):
    Path(c['argv']).write_text(json.dumps(vars(a)))
if mode == 'cancel':
    time.sleep(30)
if mode == 'empty':
    sys.exit(0)
if mode == 'illegal':
    print('diagnostic illegally on stdout', flush=True)
if mode == 'bad_progress':
    print(json.dumps(dict(stage='mesh', progress=2, message='bad')), flush=True)
line = json.dumps(dict(stage='mesh', progress=0.5, message='meshing')) + '\n'
sys.stdout.write(line[:17]); sys.stdout.flush()
time.sleep(0.02)
sys.stdout.write(line[17:]); sys.stdout.flush()
print('stack warning on stderr', file=sys.stderr, flush=True)
if mode == 'empty_file':
    Path(a.o).write_bytes(b'')
else:
    shutil.copyfile(c['fixture'], a.o)
if a.surface:
    shutil.copyfile(c.get('surface_fixture', c['fixture']), a.surface)
effective = dict(grid=a.o, max_ratio=a.max_ratio, L=max(0, a.levels - 1),
                 n0=f'{a.nx or 4}x{a.ny or 5}')
effective.update(nx0=a.nx or 4, ny0=a.ny or 5, padded_cells=72, nz=2, mesh_sha256='a' * 64)
if mode == 'bad_digest':
    effective['nx0'] += 1
message = effective if mode == 'single_parse' else json.dumps(effective)
print(json.dumps(dict(stage='success', progress=1, message=message)), end='', flush=True)
sys.exit(3 if mode == 'nonzero' else 0)
