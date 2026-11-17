import copy
import tempfile
from assembly_test_support import Assembly, check

with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    original = copy.deepcopy(a.document)
    invalid = [dict(original, schema='chipcae.assembly/9'),
               dict(original, unexpected=True),
               dict(original, board=dict(original['board'], path='../board.xml'))]
    for document in invalid:
        a.document = document
        a.save()
        result = a.run()
        check(result, 7)
        assert 'json_path' in result.stderr, result.stderr
        assert not a.output.exists()
    # Missing required fields, malformed JSON, and missing file are input errors.
    for text in ('{}', '{broken'):
        (a.root / 'assembly.json').write_text(text)
        result = a.run()
        check(result, 14)
        assert 'json_path' in result.stderr, result.stderr
        if text == '{broken':
            assert str(a.root / 'assembly.json') in a.defects()['message']
    (a.root / 'assembly.json').unlink()
    check(a.run(), 14)
    assert str(a.root / 'assembly.json') in a.defects()['message']

# /dev/full fails writes after opening; remove the partial output symlink and
# replace any previous success report with the final serialization error.
from pathlib import Path
from assembly_test_support import enabled
if enabled('ECAD') and Path('/dev/full').exists():
    with tempfile.TemporaryDirectory() as tmp:
        a = Assembly(tmp)
        check(a.run(), 0)
        a.output.unlink()
        a.output.symlink_to('/dev/full')
        check(a.run(), 14)
        assert not a.output.is_symlink()
        assert not a.output.exists()
        assert a.defects()['code'] == 'INPUT_INVALID'
        assert 'cannot write stack' in a.defects()['message']
