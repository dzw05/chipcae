import tempfile
from assembly_test_support import Assembly, enabled, check, require

if enabled('OCCT'):
    print('SKIP: OCCT-on build')
    raise SystemExit(77)
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
