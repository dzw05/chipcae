import tempfile
from assembly_test_support import Assembly, check, require

require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    (a.root / 'lid.step').write_text('exists')
    a.part(placement={'attach': 'layer:missing@top'})
    check(a.run('--accept-b-tier'), 7)
    assert a.defects()['code'] == 'IMPORT_REJECTED'
    assert a.import_report()['defects']
    assert not a.output.exists()
