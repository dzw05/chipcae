"""Generate isolated D03 fixtures without touching checked-in samples."""
import json
from pathlib import Path
import shutil
import sys
import h5py
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import make_samples as S
root = Path(sys.argv[1])
sys.argv = [sys.argv[0], '--out', str(root)]
S.main()
for variant in ('empty', 'mid_step', 'hex_only', 'particles_only', 'legacy_summary', 'preferred_summary', 'unsupported', 'escape', 'bad_checksum', 'digest'):
    target = root / variant
    shutil.copytree(root / 'default', target, dirs_exist_ok=True)
    m = json.loads((target / 'manifest.json').read_text())
    if variant == 'empty': m['steps'] = []
    if variant == 'mid_step': m['steps'][0]['complete'] = False
    if variant in ('hex_only', 'particles_only'):
        keep = 'hex' if variant == 'hex_only' else 'particles'
        m['blocks'] = [b for b in m['blocks'] if b['name'] == keep]
        for step in m['steps']:
            with h5py.File(target / step['file'], 'r+') as f:
                g = f['VTKHDF']
                del g['Assembly']
                assembly = g.create_group('Assembly')
                assembly[keep] = h5py.SoftLink('/VTKHDF/' + keep)
                for name in ('hex', 'cut', 'eb_surface', 'particles'):
                    if name != keep: del g[name]
                g[keep].attrs['Index'] = 0
            step.pop('checksum')
    if variant == 'legacy_summary':
        for s in m['summary']:
            if s['name'] == 'U_mag_max': s['name'] = 'U_abs_max'
    if variant == 'preferred_summary': m['summary'].append(dict(name='U_abs_max', step=0, value=-1))
    if variant == 'bad_checksum': m['steps'][0]['checksum'] = '0' * 64
    if variant == 'digest': m['mesh'] = dict(nx0=4, ny0=5, L=2, padded_cells=20, nz=3, mesh_sha256='a' * 64)
    if variant == 'unsupported': m['event_log_format'] = 2
    if variant == 'escape': m['steps'][0]['file'] = '../default/out/step-0.vtkhdf'
    (target / 'manifest.json').write_text(json.dumps(m))

# Standalone temporal file with an explicit manifest; exercise real time offsets.
(root / 'temporal.json').write_text(json.dumps(dict(
    schema_version='chipcae.results/1.0', fields=[],
    steps=[dict(n=n, time=n * .5, file='temporal_ug.vtkhdf') for n in range(3)])))
for name, version in [('legacy', [2, 0]), ('future', [2, 10]), ('wrong_major', [3, 0])]:
    shutil.copyfile(root / 'temporal_ug.vtkhdf', root / (name + '.vtkhdf'))
    with h5py.File(root / (name + '.vtkhdf'), 'r+') as f:
        f['VTKHDF'].attrs['Version'] = version

import numpy as np
shutil.copyfile(root / 'temporal_ug.vtkhdf', root / 'type_multi.vtkhdf')
with h5py.File(root / 'type_multi.vtkhdf', 'r+') as f:
    del f['VTKHDF'].attrs['Type']
    f['VTKHDF'].attrs.create('Type', np.array([b'PartitionedDataSetCollection', b'extra'], dtype='S32'))
