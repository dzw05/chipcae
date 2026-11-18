import tempfile
from assembly_test_support import Assembly, check, require

require("JSONCONS")

with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    (a.root / 'lid.step').write_text('reserved identifiers must be rejected before STEP import')
    a.part('void')
    check(a.run(), 14)
    d = a.defects()
    assert d['code'] == 'INPUT_INVALID', d
    assert d['message'] == 'INPUT_INVALID: part id void is reserved', d
    assert not a.output.exists()

# The collision case needs actual slicing; keep the reserved-id assertion above
# active even in OCCT-off builds.
from assembly_test_support import enabled
if enabled('OCCT'):
    with tempfile.TemporaryDirectory() as tmp:
        a = Assembly(tmp)
        a.boxes()
        a.document['parts'][0]['id'] = 'Core'
        a.save()
        check(a.run(), 14)
        d = a.defects()
        assert d['code'] == 'INPUT_INVALID', d
        assert d['message'] == 'INPUT_INVALID: part id collides with Core', d
