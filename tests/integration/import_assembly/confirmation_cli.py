"""Run the production CLI with only the STEP reader replaced by a test seam."""
import shutil
import subprocess
import sys
import tempfile
from assembly_test_support import Assembly, BUILD, check, enabled

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
    assert a.import_report()['defects'][0]['tier'] == 'B'
    assert a.import_report()['defects'][0]['reason'] == 2  # PART_UNMAPPED
    check(run('--accept-b-tier'), 0)
    stack = a.stack()
    assert [layer['name'] for layer in stack['layers']] == ['F.Cu', 'Core', 'B.Cu']
    assert not any(layer.get('regions') for layer in stack['layers'])
    a.output.unlink()
    check(run('--accept', str(a.accept(['unmapped/unmapped-body']))), 0)
    a.stack()

    # The reader seam has no shape: real slicing rejects the mapped body,
    # and accepting B-tier defects cannot confirm this C-tier failure.
    a.output.unlink()
    a.document.pop('parts_from_step')
    a.part('lid', body='unmapped-body')
    check(run('--accept-b-tier'), 7)
    assert not a.output.exists()
    assert a.defects()['code'] == 'IMPORT_REJECTED'
    assert any(d['tier'] == 'C' and d['reason'] == (3 if enabled('OCCT') else 8)
               and d['defect_id'].startswith('lid/') for d in a.import_report()['defects'])

    # Assembly alone translates report publication failure to INTERNAL.
    check(run('--accept-b-tier', '--report', str(a.root)), 1)
    assert not a.output.exists()
