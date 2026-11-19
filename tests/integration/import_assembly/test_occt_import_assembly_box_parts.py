from assembly_test_support import require_assembly_geometry
import tempfile
from assembly_test_support import Assembly, check, require

require('JSONCONS')
require('ECAD')
require_assembly_geometry()
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    a.boxes()
    a.part('stiffener', 'stiffener.step')
    a.document['parts'][0]['material_approx'] = True
    a.save()
    check(a.run('--accept-b-tier'), 0)
    stack = a.stack()
    bodies = {r['body'] for layer in stack['layers'] for r in layer.get('regions', [])}
    assert {'lid', 'stiffener'} <= bodies, bodies
    report = a.import_report()
    assert all(d['defect_id'].startswith(('board/', 'lid/', 'stiffener/'))
               for d in report['defects'])
    assert 'lid' in str(report['parts']) and '近似材料' in str(report['parts'])
