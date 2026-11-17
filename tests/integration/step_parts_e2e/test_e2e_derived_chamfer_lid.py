"""派生件（由真实盖板四角倒角，非原始设计）; exact cut volume within 1e-9."""
from parts_support import *


def acceptance(d):
    source = os.environ.get('CHIPCAE_DEMO_STEP_DIR')
    if not source:
        print('SKIP: deriving the real lid requires CHIPCAE_DEMO_STEP_DIR')
        sys.exit(77)
    prepare(d)
    chamfer_lid(d, source)
    c = 2.
    imported(d, assembly([part('lid_chamfer_derived', 'underfill')]))
    stack = json.loads((d / 'stack.json').read_text())
    assert any(r.get('body') == 'lid_chamfer_derived' for layer in stack['layers'] for r in layer.get('regions', []))
    # C >= 4 h_xy; h=.5mm, and the box outline permits coincident edges (§6c).
    _, path = solve(d, 'chamfer', n=112, materials=['underfill'])
    exact = (56*44 - 2*c**2) * .5 * 1e-9
    measured = volume(path, 'underfill')
    error = abs(measured - exact) / exact
    print('MEASURED 派生件（由真实盖板四角倒角，非原始设计）', measured, exact, error)
    assert error <= 1e-9, error
    # Provenance token: docs/geometry/assembly-report-0.1.md; decode JSON escapes.
    report = json.loads((d / 'report.json').read_text())
    assert '派生件（由真实盖板四角倒角，非原始设计）' in json.dumps(report, ensure_ascii=False)


if __name__ == '__main__':
    main(acceptance, dependency='SP6-B')
