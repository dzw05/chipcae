import json
import math
import subprocess
import tempfile
from assembly_test_support import Assembly, SOURCE, check, require, enabled, solve_stack

script = SOURCE / 'tools/stack/copper_fraction.py'
if not script.is_file():
    print('SKIP: CKPT-CF copper_fraction.py absent')
    raise SystemExit(77)
require('JSONCONS')
require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    # The ECAD fixture is a rectangular box board and needs no STEP/OCCT.
    check(a.run('--accept-b-tier'), 0)
    a.stack()
    output = a.root / 'solve.stack.json'
    report = a.root / 'cf.json'
    check(subprocess.run(['python3', str(script), str(a.output), '-o', str(output),
                          '--report', str(report)], capture_output=True, text=True), 0)
    fractions = json.loads(report.read_text())
    # Report stores layer records under layers (CKPT-CF public report format).
    records = fractions['layers']
    values = {r['layer']: r['fraction'] for r in records}
    assert math.isclose(values['F.Cu'], .12, rel_tol=1e-12)
    assert math.isclose(values['B.Cu'], .24, rel_tol=1e-12)
    if not all(enabled(name) for name in ('MFEM', 'MPI', 'HDF5')):
        print('ECAD/copper-fraction chain passed; solve backend unavailable')
    else:
        solve_stack(a.root, output, 'cf-solve')
        import h5py
        with h5py.File(a.root / 'cf-solve/out/step-0.vtkhdf') as file:
            assert sum(file['VTKHDF/cut/NumberOfCells'][:]) == 0
