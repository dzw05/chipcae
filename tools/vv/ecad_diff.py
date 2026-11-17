"""Compare imported ECAD stacks using the frozen IMP-X tolerances."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import tempfile

SCHEMA = 'chipcae.vv.ecad_diff/0.1'
DEFAULT_DELTA_M = 1e-9


class EcadDiffError(Exception):
    def __init__(self, code: int, name: str, message: str):
        super().__init__(message)
        self.code = code
        self.name = name


def _invalid(message):
    raise EcadDiffError(14, 'INPUT_INVALID', message)


def _bytes(path):
    try:
        return Path(path).read_bytes()
    except OSError as exc:
        raise EcadDiffError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc


def _number(value, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _invalid('expected a finite number')
    if not math.isfinite(value) or (positive and value <= 0):
        _invalid('expected a finite positive number' if positive else 'nonfinite number')
    return value


def _string(value):
    if not isinstance(value, str) or not value:
        _invalid('expected a nonempty string')
    return value


def _read_polyset(data):
    position = 0

    def take(size):
        nonlocal position
        if size < 0 or size > len(data) - position:
            _invalid('truncated polyset')
        value = data[position:position + size]
        position += size
        return value

    def unpack(fmt):
        return struct.unpack(fmt, take(struct.calcsize(fmt)))

    if take(8) != b'CCPOLY01':
        _invalid('invalid polyset magic')
    dbu, count = unpack('<dI')
    _number(dbu, True)
    layers = {}
    for _ in range(count):
        name = take(unpack('<I')[0]).decode('utf-8')
        if name in layers:
            _invalid('duplicate polyset layer name')
        rings = unpack('<I')[0]
        offsets = unpack('<' + str(rings + 1) + 'q')
        if offsets[0] != 0 or any(b - a < 3 for a, b in zip(offsets, offsets[1:])):
            _invalid('invalid ring offsets')
        coordinates = take(offsets[-1] * 16)
        points = [(_number(x * dbu), _number(y * dbu))
                  for x, y in struct.iter_unpack('<qq', coordinates)]
        layers[name] = [points[a:b] for a, b in zip(offsets, offsets[1:])]
    if position != len(data):
        _invalid('trailing polyset bytes')
    return layers


def _has_outline(data):
    """Inspect polyset layer headers without decoding unrelated geometry."""
    if not data.startswith(b'CCPOLY01'):
        return False
    try:
        count = struct.unpack_from('<I', data, 16)[0]
        position = 20
        for _ in range(count):
            size = struct.unpack_from('<I', data, position)[0]
            position += 4
            name = data[position:position + size]
            position += size
            if name == b'outline':
                return True
            rings = struct.unpack_from('<I', data, position)[0]
            position += 4
            points = struct.unpack_from('<q', data, position + rings * 8)[0]
            if points < 0:
                return False
            position += (rings + 1) * 8 + points * 16
    except (struct.error, OverflowError):
        return False
    return False


def _geometry(points):
    if not isinstance(points, list) or len(points) < 3:
        _invalid('polygon requires at least three points')
    for point in points:
        if not isinstance(point, (list, tuple)) or len(point) != 2:
            _invalid('invalid polygon coordinate')
        for coordinate in point:
            _number(coordinate)
    area = abs(math.fsum(a[0] * b[1] - b[0] * a[1]
                         for a, b in zip(points, points[1:] + points[:1]))) / 2
    perimeter = math.fsum(math.hypot(a[0] - b[0], a[1] - b[1])
                          for a, b in zip(points, points[1:] + points[:1]))
    box = [min(p[0] for p in points), min(p[1] for p in points),
           max(p[0] for p in points), max(p[1] for p in points)]
    _number(area)
    _number(perimeter)
    return dict(area=area, perimeter=perimeter, box=box, vertices=len(points))


def _load(path):
    stack = json.loads(_bytes(path))
    if stack['unit'] != 'm' or not isinstance(stack['layers'], list):
        _invalid('expected meter units and a layer array')
    outline = stack['outline']
    if not isinstance(outline, dict):
        _invalid('invalid outline')
    polysets = {}

    def load_polyset(file, outline_only=False):
        _string(file)
        if Path(file).is_absolute() or '..' in Path(file).parts or '\\' in file or '://' in file:
            _invalid('polyset path must be relative')
        if file not in polysets:
            refs = stack.get('external_refs', [])
            if not isinstance(refs, list):
                _invalid('invalid external_refs')
            ref = next((r for r in refs if r['path'] == file), None)
            if ref is None:
                raise EcadDiffError(12, 'INPUT_REFERENCE_MISSING', 'polyset absent from external_refs')
            data = _bytes(Path(path).parent / file)
            digest = _string(ref['sha256'])
            if len(digest) != 64 or any(c not in '0123456789abcdefABCDEF' for c in digest):
                _invalid('invalid SHA-256 digest')
            if hashlib.sha256(data).hexdigest() != digest.lower():
                raise EcadDiffError(10, 'INPUT_HASH_MISMATCH', 'polyset SHA-256 differs')
            if outline_only and not _has_outline(data):
                return
            polysets[file] = _read_polyset(data)

    for layer in stack['layers']:
        _string(layer['name'])
        _number(layer['thickness'], True)
        _string(layer['material'])
        if 'copper' not in layer:
            continue
        copper = layer['copper']
        _string(copper['metal'])
        if ('fraction' in copper) == ('polygons' in copper):
            _invalid('expected exactly one copper representation')
        if 'fraction' in copper:
            if not 0 <= _number(copper['fraction']) <= 1:
                _invalid('invalid copper fraction')
            continue
        polygons = copper['polygons']
        file = _string(polygons['file'])
        _string(polygons['layer'])
        load_polyset(file)
        if polygons['layer'] not in polysets[file]:
            _invalid('copper polygon layer missing')
    refs = stack.get('external_refs', [])
    if not isinstance(refs, list):
        _invalid('invalid external_refs')
    for ref in refs:
        file = _string(ref['path'])
        load_polyset(file, outline_only=True)
    rings = next((p['outline'] for p in polysets.values() if 'outline' in p), None)
    if rings is not None:
        if len(rings) != 1:
            _invalid('polyset outline must have exactly one ring')
        geometry = _geometry(rings[0])
    elif 'polygon' in outline:
        geometry = _geometry(outline['polygon'])
    else:
        box = outline['box']
        if not isinstance(box, list) or len(box) != 4:
            _invalid('invalid outline box')
        x, y, xx, yy = [_number(v) for v in box]
        if xx <= x or yy <= y:
            _invalid('invalid outline box bounds')
        geometry = _geometry([[x, y], [xx, y], [xx, yy], [x, yy]])
    copper_values = []
    for layer in stack['layers']:
        copper = layer.get('copper')
        if copper is None:
            copper_values.append(None)
        elif 'fraction' in copper:
            copper_values.append((copper['fraction'] * geometry['area'],
                                  geometry['perimeter'] * math.sqrt(copper['fraction'])))
        else:
            p = copper['polygons']
            gs = [_geometry(r) for r in polysets[p['file']][p['layer']]]
            copper_values.append((math.fsum(g['area'] for g in gs),
                                  math.fsum(g['perimeter'] for g in gs)))
    return stack['layers'], geometry, copper_values


def diff_stacks(odb_stack: Path, ipc_stack: Path, delta_m: float = DEFAULT_DELTA_M) -> dict:
    try:
        return _diff(odb_stack, ipc_stack, _number(delta_m, True))
    except EcadDiffError:
        raise
    except (ValueError, TypeError, KeyError, IndexError, OverflowError, struct.error) as exc:
        _invalid(str(exc))


def _diff(odb, ipc, delta):
    a, ga, ca = _load(odb)
    b, gb, cb = _load(ipc)
    entries = []

    def add(item, key, left, right, difference, tol, reason, grade):
        entries.append(dict(item=item, key=key, odb=left, ipc=right, delta=difference,
                            tol=tol, tol_derivation=reason, grade=grade, source_loc=None))

    reason = f'δ = {delta}; absolute thickness difference'

    def thickness(item, key, x, y):
        d = abs(x - y)
        # Compare endpoints so subtraction cancellation cannot move equality above δ.
        add(item, key, x, y, d, delta, reason, 'hard' if max(x, y) > min(x, y) + delta else 'info')

    sa = ['copper' in layer for layer in a]
    sb = ['copper' in layer for layer in b]
    names_a = [l['name'] for l in a]
    names_b = [l['name'] for l in b]
    same_structure = len(a) == len(b) and sa == sb
    add('layers', 'stack', names_a, names_b, abs(len(a) - len(b)),
        None, 'identical' if names_a == names_b else 'layer names differ',
        'info' if same_structure else 'hard')
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        key = f"{i}:{x['name'] if x else ''}|{y['name'] if y else ''}"
        if x is None or y is None:
            add('thickness', key, x['thickness'] if x else None,
                y['thickness'] if y else None, None, None, f'δ = {delta}; missing layer', 'hard')
            continue
        thickness('thickness', key, x['thickness'], y['thickness'])
        mx = dict(material=x['material'], metal=x.get('copper', {}).get('metal'))
        my = dict(material=y['material'], metal=y.get('copper', {}).get('metal'))
        add('material', key, mx, my, 0 if mx == my else 1, None,
            f'δ = {delta}; exact material and copper metal identity', 'info' if mx == my else 'hard')
        ax, px = ca[i] or (0, 0)
        ay, py = cb[i] or (0, 0)
        d, p = abs(ax - ay), max(px, py)
        relative = d / max(ax, ay) if max(ax, ay) else 0
        grade = ('hard' if sa[i] != sb[i] or relative > 0.01 else
                 'discuss' if d > delta * p else 'info')
        add('copper_area', key, ax, ay, d, delta * p,
            f'δ = {delta}; P_layer = {p} (total copper ring perimeter; '
            'fractions use scaled outline); copper/dielectric mismatch is hard; '
            'absolute ring areas summed; relative difference uses larger area, zero versus nonzero is 100%', grade)
    thickness('total_thickness', 'stack', math.fsum(l['thickness'] for l in a),
              math.fsum(l['thickness'] for l in b))
    p = max(ga['perimeter'], gb['perimeter'])
    d = abs(ga['area'] - gb['area'])
    hard = d > delta * p or any(abs(x - y) > delta for x, y in zip(ga['box'], gb['box']))
    add('outline', 'outline', ga, gb, d, delta * p,
        f'δ = {delta}; P = {p}; area tolerance δ × P; box coordinate tolerance δ; '
        'arcs already discretized by readers; e_chord not included',
        'hard' if hard else 'discuss' if ga['vertices'] != gb['vertices'] else 'info')
    for item in ('vias', 'components'):
        add(item, 'stack', None, None, 0, None,
            'not compared: not present in stack/0.2', 'info')
    return dict(schema=SCHEMA, delta_m=delta, inputs=dict(odb=str(odb), ipc=str(ipc)),
                entries=entries, summary={g: sum(e['grade'] == g for e in entries)
                                          for g in ('hard', 'discuss', 'info')})


def write_report(result: dict, out_dir: Path) -> None:
    out = Path(out_dir)
    lines = [f"odb: {result['inputs']['odb']}; ipc: {result['inputs']['ipc']}; δ = {result['delta_m']}",
             '', '| item | hard | discuss | info |', '| --- | --- | --- | --- |']
    for item in dict.fromkeys(e['item'] for e in result['entries']):
        counts = [sum(e['item'] == item and e['grade'] == g for e in result['entries'])
                  for g in ('hard', 'discuss', 'info')]
        lines.append(f'| {item} | ' + ' | '.join(map(str, counts)) + ' |')
    lines.append('| 合计 | ' + ' | '.join(str(result['summary'][g]) for g in ('hard', 'discuss', 'info')) + ' |')
    lines.extend(['', '| item | key | odb | ipc | delta | tol | grade |',
                  '| --- | --- | --- | --- | --- | --- | --- |'])
    details = [e for e in result['entries'] if e['grade'] != 'info']
    for e in details:
        lines.append('| ' + ' | '.join(str(e[k]).replace('\n', ' ').replace('|', r'\|') for k in
                                      ('item', 'key', 'odb', 'ipc', 'delta', 'tol', 'grade')) + ' |')
    if not details:
        lines.append('| 无 | | | | | | |')
    payload = json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False)
    try:
        out.mkdir(parents=True, exist_ok=True)
        names = ('diff.json', 'diff.md')
        previous = {name: (out / name).read_bytes() if (out / name).exists() else None
                    for name in names}
        with tempfile.TemporaryDirectory(prefix='.ecad-diff-', dir=out) as staging:
            staged = Path(staging)
            (staged / names[0]).write_text(payload, encoding='utf-8')
            (staged / names[1]).write_text('\n'.join(lines) + '\n', encoding='utf-8')
            published = []
            try:
                for name in names:
                    (staged / name).replace(out / name)
                    published.append(name)
            except OSError:
                for name in reversed(published):
                    if previous[name] is None:
                        (out / name).unlink()
                    else:
                        (staged / name).write_bytes(previous[name])
                        (staged / name).replace(out / name)
                raise
    except OSError as exc:
        _invalid(str(exc))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--odb', required=True)
    parser.add_argument('--ipc', required=True)
    parser.add_argument('-o', required=True)
    parser.add_argument('--delta-m', default=DEFAULT_DELTA_M)
    args = parser.parse_args(argv)
    try:
        try:
            delta = float(args.delta_m)
        except ValueError:
            _invalid('delta must be a finite positive number')
        write_report(diff_stacks(args.odb, args.ipc, delta), args.o)
    except EcadDiffError as exc:
        print(f'ecad_diff: {exc.name}: ' + ' '.join(str(exc).splitlines()), file=sys.stderr)
        return exc.code
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
