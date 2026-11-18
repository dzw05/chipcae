"""Optional backend check called by the C++ outline roundtrip test."""
import sys
from pathlib import Path
from assembly_test_support import enabled, solve_stack

if all(enabled(flag) for flag in ('MFEM', 'MPI', 'HDF5')):
    root = Path(sys.argv[1])
    first = solve_stack(root, root / 'original.json', 'original-solve')
    second = solve_stack(root, root / 'rewritten.json', 'rewritten-solve')
    assert first['mesh']['mesh_sha256'] == second['mesh']['mesh_sha256']
    x = {v['name']: v['value'] for v in first['summary']}['warpage_pv']
    y = {v['name']: v['value'] for v in second['summary']}['warpage_pv']
    assert abs(x - y) <= abs(x) * 1e-12 + 1e-15
else:
    print('Optional solve comparison omitted: numerical backend disabled')
