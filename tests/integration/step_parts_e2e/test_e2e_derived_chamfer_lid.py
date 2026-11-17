"""派生件（由真实盖板四角倒角，非原始设计）; exact cut volume within 1e-9."""
from parts_support import *


def acceptance(d):
    from OCP.STEPControl import STEPControl_Reader
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopAbs import TopAbs_SOLID
    from OCP.Bnd import Bnd_Box
    from OCP.BRepBndLib import BRepBndLib
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Common
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
    from OCP.gp import gp_Trsf, gp_Vec
    source = os.environ.get('CHIPCAE_DEMO_STEP_DIR')
    if not source:
        print('SKIP: deriving the real lid requires CHIPCAE_DEMO_STEP_DIR')
        sys.exit(77)
    prepare(d)
    reader = STEPControl_Reader()
    assert reader.ReadFile(str(Path(source) / 'PACKAGE_BULK_PARTS.step')) == IFSelect_RetDone
    assert reader.TransferRoots() > 0
    solids = TopExp_Explorer(reader.OneShape(), TopAbs_SOLID)
    lids = []
    while solids.More():
        shape = solids.Current()
        bounds = Bnd_Box()
        BRepBndLib.AddOptimal_s(shape, bounds, False, False)
        lo, hi = bounds.CornerMin(), bounds.CornerMax()
        x0, y0, z0 = lo.X(), lo.Y(), lo.Z()
        x1, y1, z1 = hi.X(), hi.Y(), hi.Z()
        if all(abs(a-b) < 1e-6 for a, b in zip((x1-x0, y1-y0, z1-z0), (56, 44, .5))):
            lids.append((shape, x0, y0, z0))
        solids.Next()
    assert len(lids) == 1, f'Expected one real 56 x 44 x .5 mm lid, found {len(lids)}'
    lid, x, y, z = lids[0]
    c = 2.
    footprint = [(x+c,y),(x+56-c,y),(x+56,y+c),(x+56,y+44-c),
                 (x+56-c,y+44),(x+c,y+44),(x,y+44-c),(x,y+c)]
    derived = BRepAlgoAPI_Common(lid, prism(footprint, z, .5)).Shape()
    transform = gp_Trsf()
    transform.SetTranslation(gp_Vec(-x, -y, -z))
    derived = BRepBuilderAPI_Transform(derived, transform, True).Shape()
    write_step(d / 'lid_chamfer_derived.step', derived)
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
    assert '派生件（由真实盖板四角倒角，非原始设计）' in (d / 'report.json').read_text()


if __name__ == '__main__':
    main(acceptance, dependency='SP6-B')
