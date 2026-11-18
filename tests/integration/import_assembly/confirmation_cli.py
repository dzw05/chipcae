"""Run the production CLI with only the STEP reader replaced by a test seam."""
import shutil
import subprocess
import sys
import tempfile
from assembly_test_support import Assembly, BUILD, check

with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    binary = a.root / 'package/bin/chipcae-import'
    binary.parent.mkdir(parents=True)
    shutil.copy2(sys.argv[1], binary)
    assets = a.root / 'package/share/chipcae'
    shutil.copytree(BUILD / 'import-assets', assets)
    protocol = assets / 'protocol/errors.yaml'
    protocol.write_text('\n'.join(line for line in protocol.read_text().splitlines()
                                  if 'name: IMPORT_CONFIRMATION_REQUIRED' not in line))
    a.document['parts_from_step'] = {
        'step': 'test.step', 'frame': 'step', 'origin': [0, 0, 0], 'bodies': {}}
    a.save()
    def run(*args):
        return subprocess.run([str(binary), '--assembly', str(a.root / 'assembly.json'),
                               '-o', str(a.output), *args], cwd=a.root,
                              capture_output=True, text=True)
    check(run(), 6)
    assert not a.output.exists()
    assert a.defects()['code'] == 'IMPORT_CONFIRMATION_REQUIRED'
    assert a.import_report()['defects'][0]['defect_id'] == 'unmapped/unmapped-body'
    check(run('--accept-b-tier'), 0)
    a.stack()
    a.output.unlink()
    check(run('--accept', str(a.accept(['unmapped/unmapped-body']))), 0)
    a.stack()

    # SP0 placeholder slicing is an empty result until SP2/SP3 land.
    a.output.unlink()
    a.document.pop('parts_from_step')
    a.part('lid', body='unmapped-body')
    check(run('--accept-b-tier'), 0)
    a.stack()
    assert a.defects()['code'] == 'OK'

    # Assembly alone translates report publication failure to INTERNAL.
    a.output.unlink()
    check(run('--accept-b-tier', '--report', str(a.root)), 1)
    assert not a.output.exists()
