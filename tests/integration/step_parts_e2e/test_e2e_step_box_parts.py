"""Box STEP assembly must match an independently authored regions baseline."""
from parts_support import *


def acceptance(d):
    ids = prepare(d)
    lid_material = 'copper' if 'copper' in ids else 'silicon'
    box_parts(d)
    imported(d, assembly([part('lid', lid_material), part('stiffener', 'underfill')]))
    baseline = json.loads((ROOT / 'tools/samples/step_parts/box_parts_regions.stack.json').read_text())
    baseline['layers'][-1]['regions'][0]['material'] = lid_material
    dump(d / 'regions.json', baseline)
    pv_step, _ = solve(d, 'step', materials=[lid_material, 'underfill'])
    pv_regions, _ = solve(d, 'regions', stack='regions.json', materials=[lid_material, 'underfill'])
    assert abs(pv_regions) > 0
    error = abs(pv_step - pv_regions) / abs(pv_regions)
    print('MEASURED box relative PV error', error)
    assert error <= 1e-10, error


if __name__ == '__main__':
    main(acceptance)
