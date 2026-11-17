"""Validate real solver and desktop fixtures against the same published schema."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from output_contract import validate_manifest, validate_output

try:
    import h5py
    import numpy
except ImportError as error:
    print(f'SKIP: {error}')
    sys.exit(77)

mode, source, *args = sys.argv[1:]
source = Path(source)
with tempfile.TemporaryDirectory(prefix='c1-output-') as tmp:
    directory = Path(tmp)
    if mode == 'desktop':
        sys.path.insert(0, str(source / 'desktop/tools'))
        from make_samples import generate_run
        generate_run(directory)
        validate_output(source, directory)
        golden = source / 'desktop/tests/fixtures/s1b/manifest.json'
        validate_manifest(source, json.loads(golden.read_text()))
    else:
        if not args or args[0] == 'disabled':
            print('SKIP: MPI/numerical backend unavailable')
            sys.exit(77)
        for kind in ('case', 'stack', 'materials'):
            name = f'fcbga_demo.{kind}.json'
            shutil.copyfile(source / 'schema/examples' / name, directory / name)
        case_path = directory / 'fcbga_demo.case.json'
        case = json.loads(case_path.read_text())
        case['mesh'].update(nx=3, ny=3)
        case['output']['fields'].append('T')
        case_path.write_text(json.dumps(case))
        profile = directory / 'profile.json'
        profile.write_text(json.dumps({'schema': 'chipcae.run_profile/1.0',
                                      'ranks': 1, 'backend': 'cpu', 'gpus': 0}))
        result = subprocess.run([args[0], '--case', str(case_path), '--profile', str(profile),
                                 '--out', str(directory / 'results')],
                                capture_output=True, text=True, timeout=540)
        assert result.returncode == 0, result.stderr
        manifest = validate_output(source, directory / 'results', uncut=True)
        assert manifest['mesh'] and manifest['steps']
        # Exercise the actual early manifest, published before the first output.
        early = directory / 'stopped'
        early.mkdir()
        (early / 'control.jsonl').write_text('{"cmd_seq":"1","action":"STOP"}\n')
        result = subprocess.run([args[0], '--case', str(case_path), '--profile', str(profile),
                                 '--out', str(early)],
                                capture_output=True, text=True, timeout=540)
        assert result.returncode == 0, result.stderr
        manifest = json.loads((early / 'manifest.json').read_text())
        assert manifest['steps'] == [] and manifest['mesh']
        validate_manifest(source, manifest)
print(f'contract output {mode} OK')
