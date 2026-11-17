import tempfile
import subprocess
from assembly_test_support import Assembly, b_ids, check, require, step_generator

require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    check(subprocess.run([step_generator(), str(a.root), 'draft'],
                         text=True, capture_output=True), 0)
    a.part(placement={'frame': 'step', 'origin': [.001, .001, .00167]})
    check(a.run(), 6)
    assert not a.output.exists()
    ids = b_ids(a.import_report())
    assert ids and all(id_.startswith('lid/') for id_ in ids)
    check(a.run('--accept', a.accept(ids)), 0)
    a.stack()
