import copy
import tempfile
from assembly_test_support import Assembly, check, require

require("JSONCONS")

with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    original = copy.deepcopy(a.document)
    invalid = [dict(original, schema='chipcae.assembly/9'),
               dict(original, unexpected=True),
               {},
               *[{k: v for k, v in original.items() if k != missing}
                 for missing in ('schema', 'board', 'parts')],
               dict(original, board=dict(original['board'], format='unknown')),
               dict(original, board=dict(original['board'], path='../board.xml'))]
    for document in invalid:
        a.document = document
        a.save()
        result = a.run()
        check(result, 14)
        assert 'json_path' in result.stderr, result.stderr
        assert not a.output.exists()
    # Malformed JSON and missing files are input errors.
    for text in ('{broken',):
        (a.root / 'assembly.json').write_text(text)
        result = a.run()
        check(result, 14)
        assert 'json_path' in result.stderr, result.stderr
        assert a.defects()['message'].startswith('INPUT_INVALID:'), a.defects()
        if text == '{broken':
            assert str(a.root / 'assembly.json') in a.defects()['message']
    (a.root / 'assembly.json').unlink()
    check(a.run(), 14)
    assert str(a.root / 'assembly.json') in a.defects()['message']

# Publishing replaces the output symlink itself; it must not open its target.
# Low-level /dev/full write failures are covered by stack_writer_roundtrip.
from pathlib import Path
from assembly_test_support import enabled
if enabled('ECAD') and Path('/dev/full').exists():
    with tempfile.TemporaryDirectory() as tmp:
        a = Assembly(tmp)
        check(a.run(), 0)
        a.output.unlink()
        a.output.symlink_to('/dev/full')
        check(a.run(), 0)
        assert not a.output.is_symlink()
        assert a.output.is_file()
        a.stack()
        assert a.defects()['code'] == 'OK'

# Board input validation precedes material-map resolution and reader dispatch.
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    a.document['board']['path'] = 'missing.xml'
    a.document['board']['material_map'] = 'missing-map.json'
    a.save()
    check(a.run(), 14)
    assert 'json_path=$/board/path' in a.defects()['message']
    (a.root / 'board.xml').unlink()
    (a.root / 'board.xml').mkdir()
    a.document['board']['path'] = 'board.xml'
    a.save()
    check(a.run(), 14)
    assert 'json_path=$/board/path' in a.defects()['message']

# The frozen schema applies the same lexical path rule to every input.
for field in ('path', 'material_map', 'step'):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        nested = root / 'assemblies'
        nested.mkdir()
        a = Assembly(nested)
        shared = root / 'shared-input'
        shared.write_bytes((nested / 'board.xml').read_bytes())
        for path in ('../shared-input', str(shared)):
            if field == 'step':
                a.document['parts'] = []
                a.part(step=path)
            else:
                a.document['board'][field] = path
                a.save()
            check(a.run(), 14)
            assert 'json_path' in a.defects()['message']
            assert not a.output.exists()

# Schema-valid symlinks can resolve to shared inputs outside the assembly directory.
if enabled('ECAD'):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        nested = root / 'assemblies'
        nested.mkdir()
        a = Assembly(nested)
        for name in ('board.xml', 'map.json'):
            (nested / name).rename(root / name)
            (nested / name).symlink_to(Path('..') / name)
        check(a.run(), 0)
        a.stack()
        assert a.defects()['code'] == 'OK'
