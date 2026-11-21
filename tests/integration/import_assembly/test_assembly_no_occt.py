import tempfile
from assembly_test_support import Assembly, enabled, check, require

if enabled('OCCT'):
    print('SKIP: OCCT-on build')
    raise SystemExit(77)
require('JSONCONS')
require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    (a.root / 'lid.step').write_text('file exists; OCCT must be checked before import')
    a.part()
    check(a.run(), 7)
    assert a.defects()['code'] == 'IMPORT_REJECTED'
    defects = a.import_report()['defects']
    assert len(defects) == 1, defects
    d = defects[0]
    assert (d['tier'], d['reason'], d['defect_id']) == ('C', 8, 'lid/occt_unavailable'), d
    check(a.run('--accept-b-tier'), 7)
    assert not a.output.exists()

    # Independent parts retain assembly order, including newly ready dependents.
    a.document['parts'] = []
    a.part('z_child', placement={'attach': 'part:z_parent@top'})
    a.part('z_parent')
    a.part('a_independent')
    a.part('m_independent')
    check(a.run(), 7)
    assert [d['defect_id'] for d in a.import_report()['defects']] == [
        'z_parent/occt_unavailable', 'z_child/occt_unavailable',
        'a_independent/occt_unavailable', 'm_independent/occt_unavailable']

    # Mapped STEP bodies each expand into a part even without OCCT.
    a.document['parts'] = []
    a.document['parts_from_step'] = {
        'step': 'lid.step', 'frame': 'step', 'origin': [0, 0, 0],
        'bodies': {'BodyA': {'id': 'mapped_a', 'material': 'copper'},
                   'BodyB': {'id': 'mapped_b', 'material': 'copper'}}}
    a.save()
    for args in ((), ('--accept-b-tier',)):
        check(a.run(*args), 7)
        assert a.defects()['code'] == 'IMPORT_REJECTED'
        defects = a.import_report()['defects']
        assert len(defects) == 2, defects
        assert {(d['tier'], d['reason'], d['defect_id']) for d in defects} == {
            ('C', 8, 'mapped_a/occt_unavailable'),
            ('C', 8, 'mapped_b/occt_unavailable')}, defects
        assert not a.output.exists()

    a.document['parts_from_step']['bodies']['BodyA']['material_approx'] = True
    a.save()
    check(a.run(), 7)
    assert a.import_report()['parts']['material_approx'] == [
        {'id': 'mapped_a', 'material_approx': True, 'label': '近似材料'}]
    # Mapped IDs determine reserved IDs and collisions (I2 §2.1).
    for body, mapped_id, expected, message in (
            ('void', 'safe', 7, None),
            ('BodyA', 'void', 14, 'INPUT_INVALID: part id void is reserved'),
            ('Core', 'safe', 7, None),
            ('BodyA', 'Core', 14, 'INPUT_INVALID: part id collides with Core')):
        a.document['parts_from_step']['bodies'] = {
            body: {'id': mapped_id, 'material': 'copper'}}
        a.save()
        check(a.run(), expected)
        if message is None:
            defects = a.import_report()['defects']
            assert len(defects) == 1, defects
            d = defects[0]
            assert (d['tier'], d['reason'], d['defect_id']) == (
                'C', 8, 'safe/occt_unavailable'), d
        else:
            assert a.defects()['message'] == message
        assert not a.output.exists()

    # Expanded IDs share the uniqueness namespace with explicit and mapped parts.
    for explicit in (True, False):
        a.document['parts'] = []
        if explicit:
            a.part('lid')
        a.document['parts_from_step']['bodies'] = {
            'BodyA': {'id': 'lid', 'material': 'copper'}}
        if not explicit:
            a.document['parts_from_step']['bodies']['BodyB'] = {
                'id': 'lid', 'material': 'copper'}
        a.save()
        check(a.run(), 7)
        assert a.defects()['code'] == 'IMPORT_REJECTED'
        assert a.defects()['message'] == 'IMPORT_REJECTED: duplicate part id lid'
        assert not a.output.exists()
