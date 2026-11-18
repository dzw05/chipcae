import tempfile
from assembly_test_support import Assembly, b_ids, check, require

require('JSONCONS')
require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    a.boxes()
    a.document['parts'] = []
    a.document['parts_from_step'] = {'step': 'bulk.step', 'frame': 'step',
                                     'origin': [0, 0, .00167],
                                     'bodies': {'lid': {'id': 'lid', 'material': 'copper'}}}
    a.save()
    check(a.run(), 6)
    assert not a.output.exists()
    report = a.import_report()
    assert any(d['reason'] == 2 and d['tier'] == 'B' and
               d['defect_id'] == 'unmapped/unmapped' for d in report['defects']), report
    check(a.run('--accept-b-tier'), 0)
    stack = a.stack()
    bodies = {r['body'] for l in stack['layers'] for r in l.get('regions', [])}
    assert 'lid' in bodies and 'unmapped' not in bodies, bodies
    # Body-name matching is case sensitive; a mismatched key cannot consume lid.
    a.document['parts_from_step']['bodies'] = {'LID': {'id': 'lid', 'material': 'copper'}}
    a.save()
    check(a.run(), 6)
    assert 'unmapped/lid' in b_ids(a.import_report())
