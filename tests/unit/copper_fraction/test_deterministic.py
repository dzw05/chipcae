"""CLI byte reproducibility and frozen report envelope."""
from test_fraction import *

if __name__ == '__main__':
    from tools.stack.copper_fraction import main
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        stack = fixture(root)
        first = cli(root, stack, 'first')
        second = cli(root, stack, 'second')
        for a, b in zip(first, second, strict=True):
            assert a.read_bytes() == b.read_bytes()
        report = json.loads(first[1].read_text())
        assert set(report) == {'schema', 'method', 'layers'}
        assert report['schema'] == 'chipcae.stackprep.copper_fraction/0.1'
        assert report['method'] == 'layer-average'
        assert [item['layer'] for item in report['layers']] == ['F.Cu', 'Empty']
        assert first[1].read_bytes() == (json.dumps(report, indent=2, ensure_ascii=False) + '\n').encode()
