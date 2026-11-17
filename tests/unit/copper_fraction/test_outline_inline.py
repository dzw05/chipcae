"""Referenced octagonal outline and inline-outline precedence."""
from test_fraction import *


def main():
    from tools.stack.copper_fraction import homogenize_copper
    points = ((2000, 0), (18000, 0), (20000, 2000), (20000, 18000), (18000, 20000), (2000, 20000), (0, 18000), (0, 2000))
    copper = ((0, 0), (4000, 0), (4000, 4000), (0, 4000))
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        stack = fixture(root, polyset([('outline', (points,)), ('F.Cu', (copper,)), ('Empty', ())]))
        stack['outline']['x-note'] = {'z-last': 1, 'a-first': 2}
        result, report = homogenize_copper(stack, root)
        assert list(result['outline']) == [*stack['outline'], 'polygon']
        for key in stack['outline']:
            assert_key_order(result['outline'][key], stack['outline'][key])
        assert result['outline']['polygon'] == [[x * 1e-6, y * 1e-6] for x, y in points]
        assert math.isclose(report[0]['outline_area_m2'], 3.92e-4, rel_tol=1e-12)
        assert math.isclose(report[0]['fraction'], 1.6e-5 / 3.92e-4, rel_tol=1e-12)
        stack['outline']['box'] = [0, 0, .02, .02]
        stack['outline']['polygon'] = [[0, 0], [0, .02], [.02, .02], [.02, 0]]
        result, report = homogenize_copper(stack, root)
        assert_key_order(result['outline'], stack['outline'])
        assert math.isclose(report[0]['fraction'], .04, rel_tol=1e-12)


if __name__ == '__main__':
    main()
