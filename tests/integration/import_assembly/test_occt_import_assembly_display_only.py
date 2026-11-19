import subprocess
import tempfile
from assembly_test_support import Assembly, check, require, step_generator

require('JSONCONS')
require('ECAD')
require('OCCT')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    check(subprocess.run([step_generator(), str(a.root), 'display'],
                         capture_output=True, text=True), 0)
    # The filename is ordinary; only the imported product name has the marker.
    a.part(step='bulk.step', body='unmapped')
    check(a.run('--accept-b-tier'), 7)
    assert a.defects()['code'] == 'IMPORT_REJECTED'
    assert 'display-only geometry' in a.defects()['message']
    assert not a.output.exists()
    a.document['parts'] = []
    a.document['parts_from_step'] = {
        'step': 'bulk.step', 'frame': 'step', 'origin': [0, 0, 0],
        'bodies': {'unmapped': {'id': 'unmapped', 'material': 'copper'}}}
    a.save()
    check(a.run('--accept-b-tier'), 7)
    assert 'display-only geometry' in a.defects()['message']
    assert not a.output.exists()
