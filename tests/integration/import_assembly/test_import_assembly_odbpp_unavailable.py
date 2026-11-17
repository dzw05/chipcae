import tempfile
from assembly_test_support import Assembly, CACHE, check, require

require('ECAD')
if 'CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE:BOOL=ON' in CACHE:
    print('SKIP: implemented ODB++ reader is enabled')
    raise SystemExit(77)
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    a.document['board']['format'] = 'odbpp'
    a.save()
    check(a.run(), 7)
    assert a.defects()['code'] == 'IMPORT_REJECTED'
    assert 'odbpp reader not yet available' in a.defects()['message']
    assert not a.output.exists()
