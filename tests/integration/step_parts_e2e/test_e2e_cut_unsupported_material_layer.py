"""Import succeeds; only preview/solve reject polygon in a material layer."""
from parts_support import *


def acceptance(d):
    import yaml
    prepare(d)
    # Board-only assembly still exercises the required import CLI before meshing
    # the hand-authored negative stack; import must not reject mesh preconditions.
    imported(d, assembly([]))
    source = ROOT / 'tools/samples/step_parts/negative_material_layer.stack.json'
    dump(d / 'stack.json', json.loads(source.read_text()))
    spec = yaml.safe_load((ROOT / 'protocol/errors.yaml').read_text())
    error = next(e for e in spec['errors'] if e['name'] == 'CUT_UNSUPPORTED')
    code, name = error['code'], error['name']
    result = run('preview', [d / 'stack.json', '-o', d / 'negative.vtkhdf'], expected=code)
    assert name in result.stderr, result.stderr
    assert 'polygon in material layer' in result.stderr, result.stderr
    case = json.loads((ROOT / 'examples/s2_solve/case.json').read_text())
    case['stack']['ref'] = 'stack.json'
    case['materials']['ref'] = 'materials.json'
    dump(d / 'case.json', case)
    result = run('solve', ['--case', d / 'case.json', '--profile', d / 'profile.json', '--out', d / 'negative'], expected=code)
    assert name in result.stderr, result.stderr
    assert 'polygon in material layer' in result.stderr, result.stderr


if __name__ == '__main__':
    main(acceptance, dependency='SP6-B', needs_occt=False)
