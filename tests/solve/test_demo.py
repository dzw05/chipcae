"""Exercise the shipped demo and its references, changing only mesh resolution."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import h5py

exe, source = sys.argv[1:]
with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    examples = Path(source) / 'schema/examples'
    for name in ('case', 'stack', 'materials'):
        shutil.copyfile(examples / f'fcbga_demo.{name}.json', root / f'fcbga_demo.{name}.json')
    case_path = root / 'fcbga_demo.case.json'
    case = json.loads(case_path.read_text())
    case['mesh'].update(nx=2, ny=2)
    case_path.write_text(json.dumps(case))
    profile = root / 'profile.json'
    profile.write_text(json.dumps({'schema': 'chipcae.run_profile/1.0',
                                  'ranks': 1, 'backend': 'cpu', 'gpus': 0}))
    out = root / 'results'
    result = subprocess.run([exe, '--case', str(case_path), '--profile', str(profile),
                             '--out', str(out)], capture_output=True, text=True, timeout=540)
    assert result.returncode == 0, result.stderr
    manifest = json.loads((out / 'manifest.json').read_text())
    with h5py.File(out / manifest['steps'][0]['file']) as f:
        assert f['VTKHDF/hex/Points'].size > 0
        assert f['VTKHDF/hex/Connectivity'].size > 0
    summary = {s['name']: s['value'] for s in manifest['summary']}
    assert summary['warpage_pv'] > 1e-9, summary
    print('demo warpage_pv=', summary['warpage_pv'], flush=True)

    # Independent reference: collapse each table to its linear value at T_ref.
    material_path = root / 'fcbga_demo.materials.json'
    library = json.loads(material_path.read_text())
    reference = case['loads'][0]['T_ref']
    for material in library['materials']:
        for section in ('elastic', 'thermal_expansion'):
            table = material[section]['table']
            lo, hi = table['temperatures']
            for name, values in table['columns'].items():
                table['columns'][name] = [values[0] + (reference-lo)*(values[1]-values[0])/(hi-lo)]
            table['temperatures'] = [reference]
    material_path.write_text(json.dumps(library))
    constant_out = root / 'constant'
    result = subprocess.run([exe, '--case', str(case_path), '--profile', str(profile),
                             '--out', str(constant_out)], capture_output=True, text=True, timeout=540)
    assert result.returncode == 0, result.stderr
    constant = json.loads((constant_out / 'manifest.json').read_text())
    constant_summary = {s['name']: s['value'] for s in constant['summary']}
    assert abs(constant_summary['warpage_pv']/summary['warpage_pv']-1) <= 1e-10

    # All multi-point properties clamp; each material/property logs exactly once.
    shutil.copyfile(examples / 'fcbga_demo.materials.json', material_path)
    case['loads'][0]['T_ref'] = 200
    case_path.write_text(json.dumps(case))
    clamped_out = root / 'clamped'
    result = subprocess.run([exe, '--case', str(case_path), '--profile', str(profile),
                             '--out', str(clamped_out)], capture_output=True, text=True, timeout=540)
    assert result.returncode == 0, result.stderr
    log = (clamped_out / 'events.bin').read_bytes()
    library = json.loads(material_path.read_text())
    for material in library['materials']:
        for prop in ('E', 'nu', 'alpha'):
            message = f"material {material['id']}/{prop}: T_ref outside table range; clamped to nearest endpoint"
            assert log.count(message.encode()) == 1, message
