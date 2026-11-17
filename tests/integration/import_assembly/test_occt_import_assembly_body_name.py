import tempfile
from assembly_test_support import Assembly, check, require

require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    a.boxes()
    # Explicit selection must match an imported name exactly, before slicing.
    a.document['parts'][0]['body'] = 'no_such_body'
    a.save()
    check(a.run('--accept-b-tier'), 7)
    assert 'unknown body name no_such_body' in a.defects()['message'], a.defects()
    assert not a.output.exists()

    del a.document['parts'][0]['body']
    a.document['parts'][0]['step'] = 'bulk.step'
    a.save()
    check(a.run('--accept-b-tier'), 7)
    assert 'body is required for multi-body STEP' in a.defects()['message'], a.defects()
    assert not a.output.exists()
