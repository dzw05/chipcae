"""Extract measured test output from CTest JUnit, retaining additional metrics."""
import collections
import json
import re
import sys
import xml.etree.ElementTree as ET
junit, output, status = sys.argv[1:]
notes = {
    'T1': '100 groups per model/kinematics; FD h per D16; samples avoid Tg and ramp endpoints',
    'T2': 'Tangent symmetry and Jacobi eigenvalue positivity',
    'T3': '200 Mandel convention/rotation groups',
    'T4': '100 large rotations; Green strain, PK2, internals, Cauchy push-forward',
    'T5': 'Isotropic, transverse-isotropic and orthotropic rotations',
    'T6': 'Step relaxation: one, ten and unequal steps',
    'T7': 'Arbitrary-step constant strain-rate closed form',
    'T8': 'Three timestep halvings; error reports maximum absolute deviation of order from 2; 8192-step reference',
    'T9': 'Arrhenius isothermal time-temperature superposition',
    'T10': 'Checkpoint/replay/discard bitwise, s_applied, parameter mismatch rejection',
    'T11': 'CTE conversion and actual tangent/secant kernels; free thermal expansion and zero point; rejection fixtures in T16',
    'T12': 'Instantaneous/equilibrium limits, g switch continuity, finite large x',
    'T13': '1000 fixed-seed strain/temperature steps; nonnegative dissipation',
    'T14': 'Fixed-tau SciPy Lawson-Hanson NNLS, synthetic DMA and recovered log shift',
    'T15': 'All eight factory instances: batch/single-point and independent 1/4-thread bitwise results',
    'T16': 'One JSON negative per semantic condition, readable field paths',
    'T17': 'Affine 3D least-squares transfer; nontransferable fields; material-id rejection',
    'T18': 'Both volume modes: random shear, analytic volumetric relaxation, approximation note',
    'T19': 'Exact splice, one-sided limits, reference above/below Tg',
    'T20': 'E1 closed forms; both ramp directions and endpoints exactly Tg',
    'T21': 'Free/constrained/uniaxial shrinkage; one/50-step elastic and viscoelastic; table/step binding',
    'T22': '1 K factor-10 ramp Lipschitz/halving; ImportOptions{true,1} duplicate-knot import with WarningCode::ModulusRampApplied, Lipschitz/halving and T1-style FD tangent; default duplicate rejection in T16',
    'T23': 'Activation thermal zero, incremental stress, excluded/soft frozen state and bitwise event discard',
    'T24-pt': 'Biaxial constrained cooling and force/moment release; Timoshenko curvature',
}
results = {}
for tc in ET.parse(junit).iter('testcase'):
    name = tc.get('name') or ''
    if name.startswith('material_'):
        name = name[len('material_'):]
    tid = {'t03_mandel': 'T3', 'T15_threads': 'T15', 'T24pt': 'T24-pt'}.get(name, name)
    if tid not in notes:
        continue
    failed = any(child.tag in ('failure', 'error') for child in tc)
    metrics = []
    for text in tc.itertext():
        for match in re.finditer(r'METRIC (\{[^\n]+\})', text):
            metrics.append(json.loads(match[1]))
    entry = results.setdefault(tid, dict(id=tid, status='pass', measured_error=None,
                                         tolerance=None, seed=20261116, note=notes[tid], checks=[]))
    if failed or not metrics:
        entry['status'] = 'fail'
        if not metrics:
            entry['note'] += '; missing measured test output'
    entry['checks'].extend(dict(test=name, **m) for m in metrics)
    if metrics:
        # Preserve the pair from the tightest utilized criterion, not unrelated units.
        m = max(metrics, key=lambda x: x['measured_error']/x['tolerance'] if x['tolerance'] else 0)
        if entry['measured_error'] is None or (m['tolerance'] and m['measured_error']/m['tolerance'] > (entry['measured_error']/(entry['tolerance'] or 1))):
            entry.update({k:m[k] for k in ('measured_error','tolerance','seed')})
for i in range(1,24):
    tid=f'T{i}'
    if tid not in results:
        results[tid]=dict(id=tid,status='fail',measured_error=None,tolerance=None,seed=None,note='Missing CPU test result')
for tid,state,note,tol in [('T24','skip','Later K2/P1 FEM structural convergence work',None),('T25','pending','Awaiting a self-hosted GPU runner and architect ruling',1e-12),('CPU/GPU-100-step','pending','GPU consistency awaits the self-hosted runner',1e-11)]:
    results[tid]=dict(id=tid,status=state,measured_error=None,tolerance=tol,seed=None,note=note)
counts=dict(collections.Counter(r['status'] for r in results.values()))
report=dict(library='chipcae_material',chunk=3,ctest_status=int(status),counts=counts,tests=list(results.values()))
with open(output,'w') as f:
    json.dump(report,f,indent=2);f.write('\n')
print(f'Wrote {output}: {counts}')
if counts.get('fail',0):
    sys.exit(1)
