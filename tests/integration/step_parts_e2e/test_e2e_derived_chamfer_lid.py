"""派生件（由真实盖板四角倒角，非原始设计）; exact cut volume within 1e-9."""
from parts_support import *


def acceptance(d):
    source = os.environ['CHIPCAE_DEMO_STEP_DIR']
    # Use the full generator: assembly/0.1 carries no provenance label.
    command = [sys.executable, str(ROOT / 'tools/samples/step_parts/make_parts.py'),
               '--case', 'chamfer', '--out', str(d), '--demo-step-dir', source,
               '--bulk-step', os.environ['CHIPCAE_DEMO_BULK_STEP']]
    result = subprocess.run(command, capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, (command, result.returncode, result.stderr)
    notes = json.loads((d / 'fixture-notes.json').read_text())
    assert '派生件（由真实盖板四角倒角，非原始设计）' in notes['labels'], notes
    c = 2.
    imported(d, json.loads((d / 'assembly.json').read_text()))
    stack = json.loads((d / 'stack.json').read_text())
    assert any(r.get('body') == 'lid_chamfer_derived' for layer in stack['layers'] for r in layer.get('regions', []))
    # C >= 4 h_xy; h=.5mm, and the box outline permits coincident edges (§6c).
    _, path = solve(d, 'chamfer', n=112, materials=['underfill'])
    exact = (56*44 - 2*c**2) * .5 * 1e-9
    measured = volume(path, 'underfill')
    error = abs(measured - exact) / exact
    print('MEASURED 派生件（由真实盖板四角倒角，非原始设计）', measured, exact, error)
    assert error <= 1e-9, error


if __name__ == '__main__':
    for variable in ('CHIPCAE_DEMO_STEP_DIR', 'CHIPCAE_DEMO_BULK_STEP'):
        if not os.environ.get(variable):
            print(f'SKIP: {variable} is unset')
            sys.exit(77)
    main(acceptance, dependency='SP6-B')
