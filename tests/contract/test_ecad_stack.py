"""IPC-2581 -> stack schema -> documented polyset -> mesh consumer."""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import jsonschema

source, executable = Path(sys.argv[1]), sys.argv[2]
with tempfile.TemporaryDirectory(prefix='c1-ecad-') as tmp:
    directory = Path(tmp)
    # Explicit illustrative mapping into the shipped demo library.
    (directory / 'materials.map.json').write_text(json.dumps({'copper': 'sac305_ball_eq',
                                                            'FR4': 'organic_eq'}))
    def run(mode):
        result = subprocess.run([executable, str(source), str(directory), mode],
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
    run('export')
    stack = json.loads((directory / 'board.stack.json').read_text())
    schema = json.loads((source / 'schema/stack-0.2.schema.json').read_text())
    jsonschema.Draft202012Validator(schema).validate(stack)
    library = json.loads((source / 'schema/examples/fcbga_demo.materials.json').read_text())
    ids = {material['id'] for material in library['materials']}
    for layer in stack['layers']:
        assert layer['material'] in ids
        if 'copper' in layer:
            assert layer['copper']['metal'] in ids
    names = set()
    for ref in stack['external_refs']:
        path = directory / ref['path']
        raw = path.read_bytes()
        assert hashlib.sha256(raw).hexdigest() == ref['sha256']
        assert raw[:8] == b'CCPOLY01'
        offset = 8
        def read(fmt):
            global offset
            values = struct.unpack_from('<' + fmt, raw, offset)
            offset += struct.calcsize('<' + fmt)
            return values
        dbu, count = read('dI')
        assert math.isfinite(dbu) and dbu > 0
        for _ in range(count):
            length, = read('I')
            name = raw[offset:offset+length].decode('utf-8')
            offset += length
            assert name not in names
            names.add(name)
            rings, = read('I')
            offsets = read(f'{rings+1}q')
            assert offsets[0] == 0
            assert all(b-a >= 3 for a,b in zip(offsets, offsets[1:]))
            coords = read(f'{2*offsets[-1]}q')
            for a,b in zip(offsets, offsets[1:]):
                points = list(zip(coords[2*a:2*b:2], coords[2*a+1:2*b:2]))
                area = sum(x*v-u*y for (x,y),(u,v) in zip(points, points[1:]+points[:1]))
                assert area > 0  # This minimal fixture has exterior rings, no holes.
        assert offset == len(raw), 'polyset has trailing bytes'
    for layer in stack['layers']:
        if 'copper' in layer:
            polygons = layer['copper']['polygons']
            assert polygons['layer'] in names
            assert polygons['file'] in {ref['path'] for ref in stack['external_refs']}
    run('consume')
print('ECAD stack/polyset/mesh contract OK')
