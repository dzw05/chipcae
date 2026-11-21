from assembly_test_support import require_assembly_geometry
import math
import tempfile
import subprocess
from assembly_test_support import Assembly, b_ids, check, require, step_generator

require('JSONCONS')
require('ECAD')
require_assembly_geometry()
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    check(subprocess.run([step_generator(), str(a.root), 'draft'],
                         text=True, capture_output=True), 0)
    a.part(placement={'frame': 'step', 'origin': [.001, .001, .00167]})
    check(a.run(), 6)
    assert not a.output.exists()
    ids = b_ids(a.import_report())
    assert ids and all(id_.startswith('lid/') for id_ in ids)
    report = a.import_report()
    summary = a.defects()
    deviations = [d for d in report['defects']
                  if d['tier'] == 'B' and d['reason'] == 3
                  and d['defect_id'].startswith('lid/')]
    assert deviations
    for d in deviations:
        assert d['dimension'] == 'delta'
        assert d['size_unit'] == 'm'
        assert type(d['size']) in (int, float)
        assert math.isfinite(d['size'])
        assert 0 < d['size'] <= report['tau_B']
        matches = [entry for entry in summary['defects']
                   if entry['defect_id'] == d['defect_id']]
        assert len(matches) == 1
        assert matches[0]['dimension'] == d['dimension']
        assert matches[0]['size_unit'] == d['size_unit']
    check(a.run('--accept', a.accept(ids)), 0)
    a.stack()
