"""Generate disposable SP7 fixtures; dimensions in mm, placements in metres.

Requires cadquery-ocp. No vendor bytes are copied: bulk inputs are symlinked.
Use a directory made with mktemp -d outside the checkout, and remove it afterwards.
"""
import argparse
import json
import math
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
DERIVED_LABEL = '派生件（由真实盖板四角倒角，非原始设计）'


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2))


def configured_file(directory, value, variable):
    """Resolve a configured file without including private paths in diagnostics."""
    assert directory and value, variable
    path = Path(value)
    if not path.is_absolute():
        path = Path(directory).resolve() / path
    assert path.is_file(), variable
    return path


def remap_bodies(bodies, bodies_path):
    """Return mapped keys in template order, preserving the body values."""
    mapping = json.loads(bodies_path.read_text())
    assert isinstance(mapping, dict), 'CHIPCAE_DEMO_BULK_BODIES must be an object'
    missing = bodies.keys() - mapping.keys()
    extra = mapping.keys() - bodies.keys()
    # Only neutral vocabulary can appear in mapping diagnostics.
    neutral = {'example_parts/' + suffix for suffix in
               ('die', 'adhesive', 'tim', 'lid', 'substrate', 'carrier')}
    extra_names = sorted(key if key in neutral else '<non-neutral key>' for key in extra)
    assert not missing and not extra, f'missing: {sorted(missing)}; extra: {extra_names}'
    invalid = [key for key in bodies if not isinstance(mapping[key], str) or not mapping[key]]
    assert not invalid, f'invalid body targets: {invalid}'
    targets = list(mapping.values())
    duplicates = [key for key in bodies if targets.count(mapping[key]) > 1]
    assert not duplicates, f'duplicate body targets: {duplicates}'
    return {mapping[key]: value for key, value in bodies.items()}


def box(x, y, z, dx, dy, dz):
    from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox
    from OCP.gp import gp_Pnt
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Shape()


def write_step(path, shape):
    from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
    from OCP.IFSelect import IFSelect_RetDone
    writer = STEPControl_Writer()
    assert writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone
    assert writer.Write(str(path)) == IFSelect_RetDone


def prism(points, z, height):
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakePolygon, BRepBuilderAPI_MakeFace
    from OCP.BRepPrimAPI import BRepPrimAPI_MakePrism
    from OCP.gp import gp_Pnt, gp_Vec
    wire = BRepBuilderAPI_MakePolygon()
    for x, y in points:
        wire.Add(gp_Pnt(x, y, z))
    wire.Close()
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(), gp_Vec(0, 0, height)).Shape()


def prepare(directory, board_size=(56, 44)):
    # Original synthetic IPC, SI geometry below corresponds to its mm dimensions.
    width, height = board_size
    (directory / 'board.xml').write_text(f'''<IPC-2581 revision="C"><Ecad><CadData>
<Layer name="base" layerFunction="DIELECTRIC"/>
<Stackup><StackupGroup name="S"><StackupLayer layerOrGroupRef="base"
 thickness="0.5" sequence="0" matDes="Si"/></StackupGroup></Stackup>
<Step name="sp7"><Profile><Polygon><PolyBegin x="0" y="0"/>
<PolyStepSegment x="{width}" y="0"/><PolyStepSegment x="{width}" y="{height}"/>
<PolyStepSegment x="0" y="{height}"/></Polygon></Profile></Step>
</CadData></Ecad></IPC-2581>''')
    dump(directory / 'map.json', {'Si': 'silicon_eq'})
    library = json.loads((ROOT / 'schema/examples/fcbga_demo.materials.json').read_text())
    # Reuse published cards, freezing temperature tables as test_cut_pipeline does.
    for card in library['materials']:
        for section in ('elastic', 'thermal_expansion'):
            table = card[section]['table']
            table['temperatures'] = table['temperatures'][:1]
            table['columns'] = {k: v[:1] for k, v in table['columns'].items()}
            table.update(interpolation='constant', extrapolation='clamp')
    silicon = next(card for card in library['materials'] if card['id'] == 'silicon')
    library['materials'].append(dict(silicon, id='silicon_eq'))
    dump(directory / 'materials.json', library)
    dump(directory / 'profile.json', {'schema': 'chipcae.run_profile/1.0', 'ranks': 1, 'backend': 'cpu', 'gpus': 0})
    return {m['id'] for m in library['materials']}


def rounded_box(x, y, z, width, height, depth, radius):
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Fuse
    from OCP.BRepPrimAPI import BRepPrimAPI_MakeCylinder
    from OCP.gp import gp_Ax2, gp_Pnt, gp_Dir
    shape = box(x + radius, y, z, width - 2*radius, height, depth)
    shape = BRepAlgoAPI_Fuse(shape, box(x, y + radius, z, width, height - 2*radius, depth)).Shape()
    for cx in (x + radius, x + width - radius):
        for cy in (y + radius, y + height - radius):
            disk = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(cx, cy, z), gp_Dir(0, 0, 1)), radius, depth).Shape()
            shape = BRepAlgoAPI_Fuse(shape, disk).Shape()
    return shape


def box_parts(directory):
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut
    write_step(directory / 'lid.step', box(6, 6, 1, 44, 32, .5))
    ring = BRepAlgoAPI_Cut(box(6, 6, 0, 44, 32, 1), box(10, 10, 0, 36, 24, 1)).Shape()
    write_step(directory / 'stiffener.step', ring)


def rounded_parts(directory):
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
    from OCP.gp import gp_Trsf, gp_Ax1, gp_Pnt, gp_Dir
    walls = BRepAlgoAPI_Cut(rounded_box(6, 6, 0, 44, 32, 1, 2), box(8, 8, 0, 40, 28, 1)).Shape()
    hat = BRepAlgoAPI_Fuse(walls, rounded_box(6, 6, 1, 44, 32, .5, 2)).Shape()
    ring = BRepAlgoAPI_Cut(box(16, 16, 0, 24, 12, 1), box(18, 18, 0, 20, 8, 1)).Shape()
    rotation = gp_Trsf()
    rotation.SetRotation(gp_Ax1(gp_Pnt(28, 22, 0), gp_Dir(0, 0, 1)), math.radians(15))
    ring = BRepBuilderAPI_Transform(ring, rotation, True).Shape()
    write_step(directory / 'hat.step', hat)
    write_step(directory / 'ring.step', ring)


def cylinder(directory):
    from OCP.BRepPrimAPI import BRepPrimAPI_MakeCylinder
    from OCP.gp import gp_Ax2, gp_Pnt, gp_Dir
    shape = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(20, 20, 0), gp_Dir(0, 0, 1)), 10, 1).Shape()
    write_step(directory / 'cylinder.step', shape)


def ring_chamfer_parts(directory):
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut
    ring = BRepAlgoAPI_Cut(box(8, 2, 0, 40, 40, 1), box(13, 7, 0, 30, 30, 1)).Shape()
    write_step(directory / 'stiffener.step', ring)
    points = [(2, 0), (54, 0), (56, 2), (56, 42),
              (54, 44), (2, 44), (0, 42), (0, 2)]
    write_step(directory / 'lid.step', prism(points, 1, .5))


def chamfer_lid(directory, step_path):
    from OCP.STEPControl import STEPControl_Reader
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopAbs import TopAbs_SOLID
    from OCP.Bnd import Bnd_Box
    from OCP.BRepBndLib import BRepBndLib
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Common
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
    from OCP.gp import gp_Trsf, gp_Vec
    reader = STEPControl_Reader()
    assert reader.ReadFile(str(step_path)) == IFSelect_RetDone
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
    write_step(directory / 'lid_chamfer_derived.step', derived)


def bulk(directory, step_path, board_path, bodies_path):
    import xml.etree.ElementTree as ET
    model = json.loads((HERE / 'bulk_parts.assembly.json').read_text())
    step = model['parts_from_step']
    step['bodies'] = remap_bodies(step['bodies'], bodies_path)
    (directory / 'board.xml').unlink()
    (directory / 'board.xml').symlink_to(board_path)
    (directory / 'example_parts.step').symlink_to(step_path)
    mapping = {'copper': 'organic_eq'}
    for _, element in ET.iterparse(board_path, events=('end',)):
        if element.tag.split('}')[-1] == 'StackupLayer':
            mapping['layer:' + element.attrib['layerOrGroupRef']] = 'organic_eq'
        element.clear()
    dump(directory / 'map.json', mapping)
    # assembly/0.1 has no inline region: emit the equivalent temporary box.
    # STEP z .8..1.358 mm spans substrate top to HOST bottom after common origin.
    write_step(directory / 'interposer_approx.step', box(2, 2, .8, 52, 32, .558))
    return model


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--case', choices=('box', 'rounded', 'ring-chamfer', 'cylinder', 'chamfer', 'bulk', 'negative'),
                        default='box')
    parser.add_argument('--demo-step-dir', type=Path,
                        default=os.environ.get('CHIPCAE_DEMO_STEP_DIR') or None)
    for option, variable in (('--bulk-step', 'CHIPCAE_DEMO_BULK_STEP'),
                             ('--board-ipc', 'CHIPCAE_DEMO_BOARD_IPC'),
                             ('--bulk-bodies', 'CHIPCAE_DEMO_BULK_BODIES')):
        parser.add_argument(option, default=os.environ.get(variable))
    args = parser.parse_args()
    inputs = {}
    if args.case in ('bulk', 'chamfer'):
        required = [('demo_step_dir', 'CHIPCAE_DEMO_STEP_DIR', '--demo-step-dir'),
                    ('bulk_step', 'CHIPCAE_DEMO_BULK_STEP', '--bulk-step')]
        if args.case == 'bulk':
            required += [('board_ipc', 'CHIPCAE_DEMO_BOARD_IPC', '--board-ipc'),
                         ('bulk_bodies', 'CHIPCAE_DEMO_BULK_BODIES', '--bulk-bodies')]
        for attribute, variable, option in required:
            if not getattr(args, attribute):
                parser.error(f'set {variable} or {option}')
        for attribute, variable, _ in required[1:]:
            inputs[attribute] = configured_file(args.demo_step_dir, getattr(args, attribute), variable)
    destination = args.out.resolve()
    if destination == ROOT or ROOT in destination.parents:
        parser.error('--out must be outside the checkout')
    if destination.exists() and any(destination.iterdir()):
        parser.error('--out must be an empty directory')
    destination.mkdir(parents=True, exist_ok=True)
    if args.case != 'negative':
        try:
            import OCP
        except ImportError:
            parser.error('STEP generation requires cadquery-ocp')
    ids = prepare(destination, board_size=(40, 40) if args.case == 'cylinder' else (56, 44))
    model = None
    if args.case == 'box':
        box_parts(destination)
    elif args.case == 'rounded':
        rounded_parts(destination)
    elif args.case == 'ring-chamfer':
        ring_chamfer_parts(destination)
    elif args.case == 'cylinder':
        cylinder(destination)
    elif args.case == 'chamfer':
        chamfer_lid(destination, inputs['bulk_step'])
    elif args.case == 'bulk':
        model = bulk(destination, inputs['bulk_step'], inputs['board_ipc'], inputs['bulk_bodies'])
    filename = {'bulk': 'bulk_parts', 'box': 'box_parts', 'rounded': 'rounded_parts',
                'ring-chamfer': 'box_parts',
                'cylinder': 'cylinder', 'chamfer': 'derived_chamfer_lid',
                'negative': 'negative_material_layer'}[args.case]
    if model is None:
        model = json.loads((HERE / (filename + '.assembly.json')).read_text())
    # Use the published copper card automatically once it exists; never fabricate one.
    lid_material = 'copper' if 'copper' in ids else 'silicon'
    for part in model['parts']:
        if part['id'] == 'lid':
            part['material'] = lid_material
    for part in model.get('parts_from_step', {}).get('bodies', {}).values():
        if part['id'] == 'lid':
            part['material'] = lid_material
    dump(destination / 'assembly.json', model)
    for filename in ('box_parts_regions.stack.json', 'negative_material_layer.stack.json'):
        baseline = json.loads((HERE / filename).read_text())
        if filename.startswith('box'):
            baseline['layers'][-1]['regions'][0]['material'] = lid_material
        dump(destination / filename, baseline)
    case = json.loads((ROOT / 'examples/s2_solve/case.json').read_text())
    case['stack']['ref'] = 'stack.json'
    case['materials']['ref'] = 'materials.json'
    nx, ny, levels = {'cylinder': (8, 8, 0), 'ring-chamfer': (2, 2, 1),
                      'chamfer': (112, 88, 1)}.get(args.case, (28, 22, 1))
    case['mesh'].update(nx=nx, ny=ny, levels=levels, min_elems_per_layer=1, max_ratio=100)
    case['output']['fields'] = ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid']
    dump(destination / 'case.json', case)
    dump(destination / 'fixture-notes.json', {
        'case': args.case,
        'labels': {'bulk': ['近似材料', 'interposer_approx: 近似材料、近似几何'],
                   'chamfer': [DERIVED_LABEL]}.get(args.case, []),
        'lid_material': lid_material,
        'lid_caveat': '刚度近似，CTE 偏低，待 copper 入库后重跑' if lid_material == 'silicon' else '',
        'materials': 'Published cards frozen to their first temperature, as in acceptance tests',
        'negative': 'Import board-only assembly, then substitute negative_material_layer.stack.json; copper dependency required',
    })
    print(destination / 'assembly.json')


if __name__ == '__main__':
    main()
