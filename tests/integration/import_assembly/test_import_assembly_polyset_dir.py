import tempfile
from assembly_test_support import Assembly, check, require

require('ECAD')
with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as other:
    a = Assembly(tmp)
    check(a.run('--polyset', other + '/wrong.polyset'), 14)
    assert not a.output.exists()
    check(a.run(), 0)
    stack = a.stack()
    expected = a.output.with_suffix('.polyset')
    assert expected.is_file(), expected
    assert all((a.root / r['path']).resolve().parent == a.root.resolve()
               for r in stack['external_refs'])
    assert all(layer['copper']['polygons']['file'] == expected.name
               for layer in stack['layers'] if 'copper' in layer)
