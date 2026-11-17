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
    (a.root / 'assembly.json').unlink()
    check(a.run(), 14)
