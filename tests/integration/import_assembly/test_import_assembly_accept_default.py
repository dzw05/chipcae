import tempfile
from assembly_test_support import Assembly, b_ids, check, enabled, require

require('JSONCONS')
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
    # The empty-board fixture above has no B-tier defects. Exercise confirmation
    # with actual thin-layer defects as well.
    if enabled('OCCT'):
        a.output.unlink()
        a.boxes(thin=True)
        check(a.run(), 6)
        assert not a.output.exists()
        ids = b_ids(a.import_report())
        assert ids
        default.write_text(a.accept(ids).read_text())
        check(a.run(), 0)
        a.stack()
