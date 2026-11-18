import tempfile
from assembly_test_support import Assembly, check, require

require('JSONCONS')
require('ECAD')
with tempfile.TemporaryDirectory() as tmp:
    a = Assembly(tmp)
    a.boxes()
    board = a.root / 'board.xml'
    board.write_text(board.read_text().replace('Core', 'L'))
    # Begin at the Core bottom and end inside it: exactly two Core slices.
    a.document['parts'][0]['placement'] = {'frame': 'step', 'origin': [0, 0, .000035]}
    a.document['parts'][0]['material'] = 'fr4_eq'
    a.save()
    check(a.run('--accept-b-tier'), 0)
    parts = a.import_report()['parts']
    assert parts['split_layers'] == {'L': ['L#0', 'L#1']}, parts
    assert {'from': 'layer:L', 'to': ['L#0', 'L#1']} in parts['rewrite_targets']
