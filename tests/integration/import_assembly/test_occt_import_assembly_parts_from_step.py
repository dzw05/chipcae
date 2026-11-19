import tempfile
import json
from assembly_test_support import Assembly, BUILD, b_ids, check, require

require('JSONCONS')
require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    a.boxes()
    a.document['parts'] = []
    a.document['parts_from_step'] = {'step': 'bulk.step', 'frame': 'step',
                                     'origin': [0, 0, .00167],
                                     'bodies': {'lid': {'id': 'alias', 'material': 'copper'}}}
    a.save()
    geometry_available = json.loads(
        (BUILD / 'import-assets/assembly-capabilities.json').read_text())['geometry'] == 'ON'
    if not geometry_available:
        check(a.run('--accept-b-tier'), 7)
        assert not a.output.exists()
        assert 'unavailable' in a.defects()['message']
        # Unmapped bodies still exercise confirmation without invoking geometry.
        a.document['parts_from_step']['bodies'] = {
            'LID': {'id': 'alias', 'material': 'copper'}}
        a.save()
        check(a.run(), 6)
        assert 'unmapped/lid' in b_ids(a.import_report())
        check(a.run('--accept-b-tier'), 0)
        a.stack()
        raise SystemExit(0)
    check(a.run(), 6)
    assert not a.output.exists()
    report = a.import_report()
    assert any(d['reason'] == 2 and d['tier'] == 'B' and
               d['defect_id'] == 'unmapped/unmapped' for d in report['defects']), report
    check(a.run('--accept-b-tier'), 0)
    # Body-name matching is case sensitive; a mismatched key cannot consume lid.
    a.document['parts_from_step']['bodies'] = {'LID': {'id': 'alias', 'material': 'copper'}}
    a.save()
    check(a.run(), 6)
    assert 'unmapped/lid' in b_ids(a.import_report())

    from assembly_test_support import require_assembly_geometry
    require_assembly_geometry()
    a.document['parts_from_step']['bodies'] = {'lid': {'id': 'alias', 'material': 'copper'}}
    a.save()
    check(a.run('--accept-b-tier'), 0)
    stack = a.stack()
    bodies = {r['body'] for l in stack['layers'] for r in l.get('regions', [])}
    assert 'lid' in bodies and 'unmapped' not in bodies, bodies
