#!/usr/bin/env python3
"""Generate synthetic IPC examples, independently of any real export."""
from pathlib import Path
from xml.sax.saxutils import quoteattr as q
ROOT = Path(__file__).resolve().parent
PROFILE = '<Step name="board"><Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="5"/><PolyStepSegment x="0" y="5"/></Polygon></Profile>{}</Step>'
def emit(name, layers, repeat=(), feature='', late=False, materials=False):
    defs = ''.join(f'<Layer name={q(n)} layerFunction={q(f)}/>' for n, f, t, m in layers)
    stack = '<Stackup><StackupGroup name="S">' + ''.join(f'<StackupLayer layerOrGroupRef={q(n)} thickness={q(t)}><SpecRef id={q("spec-"+n)}/></StackupLayer>' for n, f, t, m in layers + list(repeat)) + '</StackupGroup></Stackup>'
    specs = ''.join(f'<Spec name={q("spec-"+n)}><General type="MATERIAL"><Property text=""/><Property text={q(m)}/>{'<Property text="ignored"/>' if m else ''}</General></Spec>' for n, f, t, m in layers)
    if not materials:
        import re
        stack = re.sub(r'<SpecRef[^>]*/>', '', stack)
        specs = ''
    text = '<IPC-2581><Ecad><CadData>' + (stack+defs if late else defs+stack) + specs + PROFILE.format(feature) + '</CadData></Ecad></IPC-2581>\n'
    (ROOT / (name+'.xml')).write_text(text)
cu = [(n, 'CONDUCTOR', t, 'COPPER') for n,t in [('F.Cu','0.035'),('In1.Cu','0.018'),('In2.Cu','0.020'),('B.Cu','0.030')]]
core = [('DIELECTRIC_1','DIELPREG','0.1','SiO2')]
emit('conductor', cu)
emit('conductor_late', [(n,f.lower(),t,m) for n,f,t,m in cu], late=True)
silk = [('F.Silkscreen','SILKSCREEN','0',''),('F.Paste','SOLDERPASTE','0','')]
feature = '<LayerFeature layerRef="F.Silkscreen"><Set><Polygon><PolyBegin x="1" y="1"/><PolyStepSegment x="2" y="1"/><PolyStepSegment x="2" y="2"/></Polygon></Set></LayerFeature>'
emit('skip_nonstructural', silk+core+[('B.Silkscreen','SILKSCREEN','0','')], repeat=[silk[0]], feature=feature)
emit('unknown_function', [('F.Cu','SIGNAL','0.035','COPPER'),('Mystery','FOO','0','')]+core)
emit('soldermask_b', core+[('B.Mask','SOLDERMASK','0.010','')], repeat=[('B.Mask','SOLDERMASK','0.010','')])
emit('dielectric_by_material', core, materials=True)
emit('dielectric_by_layer', [('DIELECTRIC_1','DIELECTRIC','0.1','')])
emit('all_missing', core+[('DIELECTRIC_2','DIELCORE','0.2','Resin'),('DIELECTRIC_3','DIELBASE','0.3','')], materials=True)
emit('unsupported_order', silk+[('B.Mask','SOLDERMASK','0.01',''),('Mystery','FOO','0','')]+core, repeat=silk, feature='<VendorExtension/><VendorExtension/><OtherExtension/>')
# Classification precedence and literal boundary cases.
emit('function_matrix', [(n,f,'0.01','') for n,f in [
    ('c1','conductor'),('c2','mixed'),('c3','preSIGNALpost'),('c4','PLANE'),
    ('d1','dielectric_topcoat'),('d2','DIEL_CORE'),('d3','DIELBASE'),
    ('mask','soldermask'),('silk','legend'),('paste','pastemask'),
    ('document','DOCUMENT'),('assembly','ASSEMBLY'),('court','COURTYARD'),
    ('glue','GLUE'),('outline','BOARD_OUTLINE'),('drill','DRILL_CONDUCTOR'),
    ('unknown','SOLDERMASK_TOP')]])
# matDes overrides the Spec source. Missing Spec and non-MATERIAL General do not supply names.
emit('material_precedence', [('DIELECTRIC_1','DIELECTRIC','0.1','SiO2')], late=True, materials=True)
p = ROOT / 'material_precedence.xml'
p.write_text(p.read_text().replace('thickness="0.1"','thickness="0.1" matDes="preferred"'))
emit('nonmaterial_spec', [('DIELECTRIC_1','DIELECTRIC','0.1','wrong')], materials=True)
p = ROOT / 'nonmaterial_spec.xml'
p.write_text(p.read_text().replace('type="MATERIAL"','type="OTHER"'))
emit('missing_spec', [('DIELECTRIC_1','DIELECTRIC','0.1','wrong')], materials=True)
p = ROOT / 'missing_spec.xml'
p.write_text(p.read_text().replace('id="spec-DIELECTRIC_1"','id="absent"'))

# I-IPC-GEOM: isolated traces and missing/mixed plating; old outputs stay unchanged.
def geometry_case(name, body):
    emit(name, [cu[0]], feature='<LayerFeature layerRef="F.Cu"><Set>'+body+'</Set></LayerFeature>')

def line(y, desc):
    return f'<Line startX="1" startY="{y}" endX="11" endY="{y}">{desc}</Line>'

geometry_case('inline_linedesc_with_id', line(1, '<LineDesc id="W1" lineWidth="0.4"/>') + line(3, '<LineDescRef id="W1"/>'))
for end in ['SQUARE', 'FLAT', 'FOO', 'round']:
    geometry_case('line_end_'+end.lower(), line(1, f'<LineDesc id="W1" lineWidth="0.4" lineEnd="{end}"/>'))
for end in ['ROUND', 'SQUARE']:
    geometry_case('arc_'+end.lower(), f'<Arc startX="0" startY="100" endX="200" endY="100" centerX="100" centerY="100" clockwise="false"><LineDesc lineWidth="2" lineEnd="{end}"/></Arc>')
plated = '<Layer name="drill-plated" layerFunction="DRILL"><Span fromLayer="F.Cu" toLayer="B.Cu"/><SpecRef id="plating"/></Layer><Spec name="plating"><General type="MATERIAL"><Property name="PLATING_THICKNESS" value="0.02" unit="MILLIMETER"/></General></Spec>'
missing = '<Layer name="drill-missing" layerFunction="DRILL"><Span fromLayer="F.Cu" toLayer="B.Cu"/></Layer>'
features = '<PadStackDef name="via"><PadstackHoleDef name="hole" diameter="0.5" platingStatus="VIA"/></PadStackDef>'
for name, count in [('plated',3), ('missing',2)]:
    features += f'<LayerFeature layerRef="drill-{name}"><Set>' + ''.join(f'<Pad padstackDefRef="via"><Location x="{i*2}" y="{1 if name=="plated" else 4}"/></Pad>' for i in range(count)) + '</Set></LayerFeature>'
emit('via_plating_mixed', [cu[0]]+core+[cu[-1]], feature=features)
p = ROOT / 'via_plating_mixed.xml'
p.write_text(p.read_text().replace('<Step name="board">', plated+missing+'<Step name="board">'))
