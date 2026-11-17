import tempfile
from assembly_test_support import Assembly, check, require

require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    default = a.root / 'import.accept.json'
    default.write_text('{malformed')
    check(a.run('--accept-b-tier'), 0)
    a.stack()
    a.output.unlink()
    result = a.run()
    assert result.returncode != 0, 'default accept file was ignored'
    assert 'accept' in result.stderr.lower(), result.stderr
    assert not a.output.exists()
    default.write_text('{"accept":[]}')
    check(a.run(), 0)
    a.stack()
