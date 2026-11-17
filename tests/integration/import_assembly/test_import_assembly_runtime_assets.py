"""A relocated importer uses its packaged schemas and error protocol."""
import json
import shutil
import subprocess
import tempfile
from assembly_test_support import Assembly, BUILD, check, require

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

    check(run(), 0)
    report = a.import_report()
    assert {'parts', 'L', 'tier_counts', 'defects'} <= report.keys(), report
    assert (a.root / 'import_report.csv').is_file()
    assert not (a.root / 'assembly-report.tmp').exists()
    # Changing only the packaged schema proves it is the runtime schema owner.
    schema_path = assets / 'schema/assembly-0.1.schema.json'
    schema = json.loads(schema_path.read_text())
    schema['properties']['schema']['const'] = 'packaged-schema-sentinel'
    schema_path.write_text(json.dumps(schema))
    check(run(), 7)
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
