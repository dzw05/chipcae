import tempfile
import subprocess
from assembly_test_support import BUILD
from assembly_test_support import Assembly, b_ids, check, require

with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    empty = a.accept([])
    check(a.run('--accept', empty, '--accept-b-tier'), 2)
    check(a.run('--accept-b-tier', '--accept', empty), 2)
    require('ECAD')
    result = subprocess.run([str(BUILD / 'kernel/chipcae-import'), '--accept', str(empty),
                             '--assembly', str(a.root / 'assembly.json'), '-o', str(a.output)],
                            text=True, capture_output=True, timeout=25)
    check(result, 0)
    a.stack()
    a.output.unlink()
    a.boxes(thin=True)
    check(a.run(), 6)
    ids = b_ids(a.import_report())
    assert ids, a.import_report()
    check(a.run('--accept', a.accept(ids[:-1])), 6)
    assert not a.output.exists()
    check(a.run('--accept', a.accept(ids)), 0)
    a.stack()
    result = a.run('--accept', a.accept(ids + ['nonexistent/defect']))
    check(result, 0)
    assert 'warning' in result.stderr.lower(), result.stderr
    # Recognized options are independent of their position relative to the mode.
    result = subprocess.run([str(BUILD / 'kernel/chipcae-import'), '--accept',
                             str(a.accept(ids)), '--assembly', str(a.root / 'assembly.json'),
                             '-o', str(a.output)], text=True, capture_output=True, timeout=25)
    check(result, 0)
    a.stack()
