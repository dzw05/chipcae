import tempfile
from assembly_test_support import Assembly, check, require

require('JSONCONS')
require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    (a.root / 'NOT_CAE_GEOMETRY.step').write_text('display-only input')
    a.part(step='NOT_CAE_GEOMETRY.step')
    check(a.run('--accept-b-tier'), 7)
    assert a.defects()['code'] == 'IMPORT_REJECTED'
    assert 'display-only geometry' in a.defects()['message']
    assert not a.output.exists()
