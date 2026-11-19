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
    model = json.loads((ROOT / 'tools/samples/step_parts/bulk_parts.assembly.json').read_text())
    for body in model['parts_from_step']['bodies'].values():
        if body['id'] == 'lid':
            body['material'] = lid
    # With real inputs configured, missing OCCT bindings are a setup failure.
    import OCP
    # Explicit temporary box equivalent to the requested manual box region.
    # STEP coordinates, lower face at substrate top after the shared origin.
    write_step(d / 'interposer_approx.step', box(2, 2, .8, 52, 32, .558))
    interposer = next(p for p in model['parts'] if p['id'] == 'interposer_approx')
    stack = imported(d, model)
    # SP7 diagnostic checks: docs/demo-1214.md.
    report = json.loads((d / 'report.json').read_text())
    text = json.dumps(report, ensure_ascii=False)
    assert '近似材料' in text, report
    expected = {v['id']: v['material']
                for v in model['parts_from_step']['bodies'].values()}
    expected[interposer['id']] = interposer['material']
    # Assembly v1.0.2 §4: imported part IDs survive as region.body.
    # Diagnostic mentions and duplicate materials cannot prove body coverage.
    regions = [r for layer in stack['layers'] for r in layer.get('regions', [])]
    for ident, material in expected.items():
        assert ident in text, ident
        matches = [r for r in regions if r.get('body') == ident]
        assert matches, f'missing imported body {ident}'
        assert all(r['material'] == material for r in matches), (ident, matches)
    pv, _ = solve(d, 'real-bulk', materials=set(expected.values()), emit_summary=False)
    dump(d / 'result.json', {
        'data_class': 'real',
        'measurement': '实测',
        'labels': ['近似材料', 'interposer_approx: 近似材料、近似几何'],
        'copper_layers': '整层平均',
        'materials': expected,
        'lid_caveat': '刚度近似，CTE 偏低，待 copper 入库后重跑' if lid == 'silicon' else '',
        'warpage_pv': pv,
        'warpage_unit': 'm',
    })


if __name__ == '__main__':
    if not os.environ.get('CHIPCAE_DEMO_STEP_DIR'):
        print('SKIP: CHIPCAE_DEMO_STEP_DIR is unset')
        sys.exit(77)
    main(acceptance, needs_occt=False, local_runs=True)
