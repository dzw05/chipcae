"""A relocated importer uses its packaged schemas and error protocol."""
import json
import shutil
import subprocess
import tempfile
from assembly_test_support import Assembly, BUILD, check, require

require('JSONCONS')
require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    package = a.root / 'package'
    binary = package / 'bin/chipcae-import'
    binary.parent.mkdir(parents=True)
    shutil.copy2(BUILD / 'kernel/chipcae-import', binary)
    assets = package / 'share/chipcae'
    shutil.copytree(BUILD / 'import-assets', assets)

    def run():
        return subprocess.run([str(binary), '--assembly', str(a.root / 'assembly.json'),
                               '-o', str(a.output)], cwd=package, text=True,
                              capture_output=True, timeout=25)

    # An interrupted older importer may have left its staging directory behind.
    staging = a.root / 'assembly-report.tmp'
    staging.mkdir()
    (staging / 'partial').write_text('interrupted report')
    check(run(), 0)
    report = a.import_report()
    assert {'parts', 'L', 'tier_counts', 'defects'} <= report.keys(), report
    assert (a.root / 'import_report.csv').is_file()
    assert (staging / 'partial').read_text() == 'interrupted report'
    # The writer emits resolved body names; board to_stack omits them. Reject
    # that final-only field to prove the serialized output is schema validated.
    stack_schema_path = assets / 'schema/stack-0.2.schema.json'
    saved_stack_schema = stack_schema_path.read_text()
    stack_schema = json.loads(saved_stack_schema)
    stack_schema['properties']['layers']['items']['properties']['body'] = {'not': {}}
    stack_schema_path.write_text(json.dumps(stack_schema))
    previous = a.output.read_bytes()
    check(run(), 14)
    assert a.defects()['code'] == 'INPUT_INVALID'
    assert a.defects()['message'].startswith('INPUT_INVALID: ')
    assert a.output.read_bytes() == previous
    assert not list(a.root.glob("out.stack.json.tmp.*"))
    a.output.unlink()
    check(run(), 14)
    assert not a.output.exists()
    assert not list(a.root.glob("out.stack.json.tmp.*"))
    stack_schema_path.write_text(saved_stack_schema)
    # Report publication failure must also preserve a previous stack.
    check(run(), 0)
    previous = a.output.read_bytes()
    csv = a.root / 'import_report.csv'
    csv.unlink()
    csv.mkdir()
    check(run(), 1)
    assert a.output.read_bytes() == previous
    assert not list(a.root.glob("out.stack.json.tmp.*"))
    csv.rmdir()
    a.output.unlink()
    # Changing only the packaged schema proves it is the runtime schema owner.
    schema_path = assets / 'schema/assembly-0.1.schema.json'
    schema = json.loads(schema_path.read_text())
    schema['properties']['schema']['const'] = 'packaged-schema-sentinel'
    schema_path.write_text(json.dumps(schema))
    check(run(), 14)
    assert 'json_path' in a.defects()['message']
    schema['properties']['schema']['const'] = 'chipcae.assembly/0.1'
    schema_path.write_text(json.dumps(schema))
    a.part('Core')
    check(run(), 14)
    protocol = assets / 'protocol/errors.yaml'
    protocol.write_text(protocol.read_text().replace('code: 14, name: INPUT_INVALID',
                                                    'code: 41, name: INPUT_INVALID'))
    check(run(), 41)
    assert a.defects()['code'] == 'INPUT_INVALID'

    # Normal block-style YAML must use the packaged mapping too.
    protocol.write_text('errors:\n  - code: 1\n    name: INTERNAL\n'
                        '  - name: INPUT_INVALID\n    code: 42\n')
    check(run(), 42)
    assert a.defects()['code'] == 'INPUT_INVALID'
    a.document['parts'] = []
    a.save()
    a.output.unlink(missing_ok=True)
    for asset in (schema_path, assets / 'schema/stack-0.2.schema.json', protocol):
        saved = asset.read_text()
        for corrupt in (False, True):
            if corrupt:
                asset.write_text('{corrupt runtime asset')
            else:
                asset.unlink()
            check(run(), 7)
            assert a.defects()['code'] == 'IMPORT_REJECTED'
            assert 'runtime assets' in a.defects()['message']
            assert not a.output.exists()
            asset.write_text(saved)
