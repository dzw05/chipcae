"""Private real bulk STEP acceptance; the only data gate is the documented env."""
from parts_support import *


def acceptance(d):
    source = Path(os.environ['CHIPCAE_DEMO_STEP_DIR']).resolve()
    assert (source / 'PACKAGE_BULK_PARTS.step').is_file(), source
    boards = [source / 'package_substrate.ipc2581.xml',
              source.parent / 'eda/substrate/package_substrate.ipc2581.xml']
    board = next((p for p in boards if p.is_file()), None)
    assert board is not None, f'package_substrate.ipc2581.xml missing near {source}'
    ids = prepare(d)
    # Symlink private sources: no vendor bytes copied to repo or CI artifacts.
    (d / 'board.xml').unlink()
    (d / 'board.xml').symlink_to(board)
    (d / 'PACKAGE_BULK_PARTS.step').symlink_to(source / 'PACKAGE_BULK_PARTS.step')
    import xml.etree.ElementTree as ET
    mapping = {'copper': 'organic_eq'}
    for element in ET.iterparse(board, events=('end',)):
        e = element[1]
        if e.tag.split('}')[-1] == 'StackupLayer':
            mapping['layer:' + e.attrib['layerOrGroupRef']] = 'organic_eq'
        e.clear()
    dump(d / 'map.json', mapping)
    lid = 'copper' if 'copper' in ids else 'silicon'
    if lid == 'silicon':
        print('近似材料: lid 刚度近似，CTE 偏低，待 copper 入库后重跑')
    model = assembly([])
    model['parts_from_step'] = {
        'step': 'PACKAGE_BULK_PARTS.step', 'frame': 'step', 'origin': [0, -.044, .000020002],
        'bodies': {f'PACKAGE_BULK_PARTS/{name}': {'id': ident, 'material': material, 'material_approx': True}
                   for name, ident, material in [('HOST_SILICON_BULK', 'host_si', 'silicon'),
                                                ('IHP_adhesive', 'ihp_adh', 'underfill'),
                                                ('TIM', 'tim', 'underfill'), ('lid', 'lid', lid)]}}
    # With real inputs configured, missing OCCT bindings are a setup failure.
    import OCP
    # Explicit temporary box equivalent to the requested manual box region.
    # STEP coordinates, lower face at substrate top after the shared origin.
    write_step(d / 'interposer_approx.step', box(2, 2, .8, 52, 32, .558))
    interposer = part('interposer_approx', 'underfill')
    interposer['placement']['origin'] = [0, -.044, .000020002]
    interposer['material_approx'] = True
    model['parts'].append(interposer)
    imported(d, model)
    # Diagnostic tokens and mapped IDs: docs/geometry/assembly-report-0.1.md.
    report = json.loads((d / 'report.json').read_text())
    text = json.dumps(report, ensure_ascii=False)
    assert '近似材料' in text, report
    for ident in ('host_si', 'ihp_adh', 'tim', 'lid', 'interposer_approx'):
        assert ident in text, ident
    solve(d, 'real-bulk', materials=[v['material'] for v in model['parts_from_step']['bodies'].values()])


if __name__ == '__main__':
    if not os.environ.get('CHIPCAE_DEMO_STEP_DIR'):
        print('SKIP: CHIPCAE_DEMO_STEP_DIR is unset')
        sys.exit(77)
    main(acceptance, needs_occt=False)
