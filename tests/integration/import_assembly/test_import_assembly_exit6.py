import tempfile
from assembly_test_support import Assembly, b_ids, check, require

require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    a.boxes(thin=True)
    check(a.run(), 6)
    assert not a.output.exists()
    defects = a.import_report()['defects']
    thin = [d for d in defects if d['tier'] == 'B' and d['reason'] == 1]
    assert thin, defects
    assert all(d['defect_id'].split('/')[0] in ('lid', 'second') for d in thin)
    assert a.defects()['defects'], 'defects.json must contain the B-tier defect'
    check(a.run('--accept-b-tier'), 0)
    a.stack()
    a.output.unlink()
    result = a.run('--accept', a.accept(b_ids(a.import_report())))
    check(result, 0)
    a.stack()
