"""Synthetic acceptance fixtures; no checked-in binary sidecars."""
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

SOURCE = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE))
RECT = ((0, 0), (2000, 0), (2000, 2000), (0, 2000))
HOLE = ((500, 500), (500, 1500), (1500, 1500), (1500, 500))


def polyset(layers, dbu=1e-6):
    data = b'CCPOLY01' + struct.pack('<dI', dbu, len(layers))
    for name, rings in layers:
        name = name.encode('utf-8')
        offsets = [0]
        for ring in rings:
            offsets.append(offsets[-1] + len(ring))
        data += struct.pack('<I', len(name)) + name + struct.pack('<I', len(rings))
        data += struct.pack('<' + 'q' * len(offsets), *offsets)
        for ring in rings:
            for x, y in ring:
                data += struct.pack('<qq', x, y)
    return data


def fixture(root, data=None):
    data = data if data is not None else polyset([('F.Cu', (RECT, HOLE)), ('Empty', ())])
    (root / 'copper.polyset').write_bytes(data)
    layers = []
    for name, copper in [('F.Cu', {'metal': 'mat.cu', 'polygons': {'file': 'copper.polyset', 'layer': 'F.Cu'}}),
                         ('Empty', {'metal': 'mat.cu', 'polygons': {'file': 'copper.polyset', 'layer': 'Empty'}}),
                         ('Already', {'metal': 'mat.cu', 'fraction': 0.5}), ('Core', None)]:
        layer = dict(name=name, thickness=0.001, kind='Homogenized', material='mat.fr4', n_elem_min=1, grading=1.0)
        if copper is not None:
            layer['copper'] = copper
        layers.append(layer)
    return dict(id='合成板', unit='m', outline={'box': [0, 0, 0.01, 0.005]}, layers=layers,
                symmetry='none', package_type='pcb', external_refs=[{'path': 'copper.polyset', 'sha256': hashlib.sha256(data).hexdigest()}])


def cli(root, stack, suffix='one'):
    source = root / 'input.json'
    source.write_text(json.dumps(stack, ensure_ascii=False), encoding='utf-8')
    output, report = root / (suffix + '.json'), root / (suffix + '.report.json')
    result = subprocess.run([sys.executable, str(SOURCE / 'tools/stack/copper_fraction.py'), str(source), '-o', str(output), '--report', str(report)], capture_output=True, text=True, cwd=root, timeout=45)
    assert result.returncode == 0, (result.returncode, result.stderr)
    return output, report


def assert_key_order(actual, expected):
    """Compare every nested object order as well as values."""
    if isinstance(expected, dict):
        assert list(actual) == list(expected)
        for key in expected:
            assert_key_order(actual[key], expected[key])
    elif isinstance(expected, list):
        assert len(actual) == len(expected)
        for new, old in zip(actual, expected, strict=True):
            assert_key_order(new, old)
    else:
        assert actual == expected


def check(root):
    from tools.stack.copper_fraction import homogenize_copper
    from tools.stack.polyset import read_polyset, ring_area_m2, layer_area_m2
    stack = fixture(root)
    before = copy.deepcopy(stack)
    parsed = read_polyset(root / 'copper.polyset')
    assert parsed.dbu_m == 1e-6
    assert parsed.layers[0].rings == (RECT, HOLE)
    assert math.isclose(ring_area_m2(HOLE, parsed.dbu_m), -1e-6, rel_tol=1e-12)
    assert math.isclose(layer_area_m2(parsed.layers[0], parsed.dbu_m), 3e-6, rel_tol=1e-12)
    output, report = homogenize_copper(stack, root)
    assert stack == before
    assert json.dumps(stack, ensure_ascii=False) == json.dumps(before, ensure_ascii=False)
    extended = copy.deepcopy(stack)
    extended['x-note'] = {'values': [{'z-last': 1, 'a-first': 2.25}], 'label': '整层平均'}
    extension_output, _ = homogenize_copper(extended, root)
    assert_key_order(extension_output['x-note'], extended['x-note'])
    assert list(extension_output) == [key for key in extended if key != 'external_refs']
    assert list(output) == [key for key in stack if key != 'external_refs']
    assert 'external_refs' not in output
    for key in stack:
        if key not in ('external_refs', 'layers'):
            assert_key_order(output[key], stack[key])
    for index, (old, new) in enumerate(zip(stack['layers'], output['layers'], strict=True)):
        assert list(old) == list(new)
        if index < 2:
            assert_key_order({k: v for k, v in new.items() if k != 'copper'},
                             {k: v for k, v in old.items() if k != 'copper'})
            assert list(new['copper']) == ['metal', 'fraction']
            assert new['copper']['metal'] == old['copper']['metal']
            assert math.isclose(new['copper']['fraction'], [0.06, 0][index], rel_tol=1e-12)
        else:
            assert_key_order(new, old)
    assert len(report) == 2
    for item, name, area, fraction in zip(report, ['F.Cu', 'Empty'], [3e-6, 0], [0.06, 0], strict=True):
        assert set(item) == {'layer', 'copper_area_m2', 'outline_area_m2', 'fraction'}
        assert item['layer'] == name
        for key, expected in [('copper_area_m2', area), ('outline_area_m2', 5e-5), ('fraction', fraction)]:
            assert math.isclose(item[key], expected, rel_tol=1e-12)
    # Equal duplicate references and repeated sidecar use are supported.
    duplicate = copy.deepcopy(stack)
    duplicate['external_refs'] *= 2
    assert homogenize_copper(duplicate, root) == (output, report)
    for layers in ([], [stack['layers'][2], stack['layers'][3]]):
        plain = dict(stack, layers=layers)
        converted, entries = homogenize_copper(plain, root)
        assert_key_order(converted, {k: v for k, v in plain.items() if k != 'external_refs'})
        assert entries == []
    # Tolerance is applied to signed fractions before clamping.
    for width, expected in [(1.0 + 5e-13, 1.0), (-5e-13, 0.0)]:
        boundary = fixture(root, polyset([('F.Cu', (((0, 0), (1, 0), (1, 1), (0, 1)),))], 1.0))
        boundary['layers'] = boundary['layers'][:1]
        boundary['outline'] = {'box': [0, 0, 1.0 / abs(width), 1]}
        if width < 0:
            fixture(root, polyset([('F.Cu', (((0, 0), (0, 1), (1, 1), (1, 0)),))], 1.0))
            boundary['external_refs'][0]['sha256'] = hashlib.sha256((root / 'copper.polyset').read_bytes()).hexdigest()
        bounded, _ = homogenize_copper(boundary, root)
        assert bounded['layers'][0]['copper']['fraction'] == expected
    from tools.stack.polyset import StackPrepError
    boundary['outline'] = {'box': [0, 0, 1.0 / 2e-12, 1]}
    try:
        homogenize_copper(boundary, root)
    except StackPrepError as exc:
        assert (exc.code, exc.name) == (14, 'INPUT_INVALID')
    else:
        raise AssertionError('fraction below -1e-12 must be rejected')
    fixture(root)  # Restore the original sidecar before exercising the CLI.
    path, rp = cli(root, stack)
    assert json.loads(path.read_text()) == output
    assert json.loads(rp.read_text())['layers'] == report
    assert path.read_bytes() == (json.dumps(output, indent=2, ensure_ascii=False) + '\n').encode()
    return output


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        check(Path(directory))
