"""Convert polygon copper maps to whole-layer average fractions (整层平均)."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.stack.polyset import StackPrepError, read_polyset, layer_area_m2, ring_area_m2


def invalid(message):
    return StackPrepError(14, 'INPUT_INVALID', message)


def homogenize_copper(stack: dict, stack_dir: Path) -> tuple[dict, list[dict]]:
    try:
        return _homogenize(stack, stack_dir)
    except (KeyError, TypeError, ValueError, OverflowError, IndexError) as exc:
        raise invalid(str(exc)) from exc


def _homogenize(stack, stack_dir):
    if not isinstance(stack, dict):
        raise invalid('stack must be an object')
    if stack['unit'] != 'm':
        raise invalid('unit must be m')
    if not isinstance(stack.get('layers'), list):
        raise invalid('layers must be an array')
    if any(not isinstance(layer, dict) or
           ('copper' in layer and not isinstance(layer['copper'], dict))
           for layer in stack['layers']):
        raise invalid('invalid stack layer or copper object')
    result = copy.deepcopy(stack)
    result.pop('external_refs', None)
    converted = [layer for layer in result['layers']
                 if 'polygons' in layer.get('copper', {})]
    if not converted:
        return result, []
    refs = {}
    for ref in stack.get('external_refs', []):
        path, digest = ref['path'], ref['sha256']
        if path in refs and refs[path] != digest:
            raise StackPrepError(10, 'INPUT_HASH_MISMATCH', 'conflicting reference hashes')
        refs[path] = digest
    polysets = {}
    for layer in converted:
        path = layer['copper']['polygons']['file']
        if path not in refs:
            raise StackPrepError(12, 'INPUT_REFERENCE_MISSING', 'sidecar absent from external_refs')
        if path not in polysets:
            source = stack_dir / path
            try:
                data = source.read_bytes()
            except OSError as exc:
                raise StackPrepError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc
            if hashlib.sha256(data).hexdigest() != refs[path]:
                raise StackPrepError(10, 'INPUT_HASH_MISMATCH', 'sidecar SHA-256 mismatch')
            polysets[path] = read_polyset(source)
    outline_ring = None
    for polyset in polysets.values():
        for layer in polyset.layers:
            if layer.name == 'outline':
                if len(layer.rings) != 1:
                    raise StackPrepError(13, 'CUT_UNSUPPORTED', 'outline must contain exactly one ring')
                if outline_ring is None:
                    outline_ring = (layer.rings[0], polyset.dbu_m)
    outline = result['outline']
    if 'polygon' in outline:
        ring = tuple(tuple(point) for point in outline['polygon'])
        if len(ring) < 3 or any(len(p) != 2 or not all(math.isfinite(v) for v in p) for p in ring):
            raise invalid('invalid outline polygon')
        area = abs(ring_area_m2(ring, 1.0))
    elif outline_ring is not None:
        ring, dbu = outline_ring
        area = abs(ring_area_m2(ring, dbu))
        outline['polygon'] = [[x * dbu, y * dbu] for x, y in ring]
    else:
        x0, y0, x1, y1 = outline['box']
        if x1 <= x0 or y1 <= y0:
            raise invalid('nonpositive outline box')
        area = (x1 - x0) * (y1 - y0)
    if not math.isfinite(area) or area <= 0:
        raise invalid('nonpositive outline area')
    report = []
    for layer in converted:
        copper = layer['copper']
        polygons = copper['polygons']
        polyset = polysets[polygons['file']]
        found = next((item for item in polyset.layers if item.name == polygons['layer']), None)
        if found is None:
            raise invalid('copper layer not found')
        copper_area = layer_area_m2(found, polyset.dbu_m)
        fraction = copper_area / area
        if not math.isfinite(fraction) or not -1e-12 <= fraction <= 1 + 1e-12:
            raise invalid('copper fraction outside [0, 1]')
        fraction = min(1.0, max(0.0, fraction))
        layer['copper'] = {'metal': copper['metal'], 'fraction': fraction}
        report.append(dict(layer=layer['name'], copper_area_m2=copper_area,
                           outline_area_m2=area, fraction=fraction))
    return result, report


def main(argv: list[str] | None = None) -> int:
    class Parser(argparse.ArgumentParser):
        def error(self, message):
            raise invalid(message)

    parser = Parser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('-o', required=True, type=Path)
    parser.add_argument('--report', type=Path)
    try:
        args = parser.parse_args(argv)
        paths = [args.input.resolve(), args.o.resolve()]
        if args.report is not None:
            paths.append(args.report.resolve())
        if len(set(paths)) != len(paths) or any(
                a.exists() and b.exists() and a.samefile(b)
                for i, a in enumerate(paths) for b in paths[i + 1:]):
            raise invalid('input, output and report paths must differ')
        try:
            with args.input.open(encoding='utf-8') as source:
                stack = json.load(source)
        except OSError as exc:
            raise StackPrepError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc
        except (ValueError, UnicodeError) as exc:
            raise invalid(str(exc)) from exc
        output, entries = homogenize_copper(stack, args.input.parent)
        report = dict(schema='chipcae.stackprep.copper_fraction/0.1',
                      method='layer-average', layers=entries)
        payload = json.dumps(output, indent=2, ensure_ascii=False) + '\n'
        report_payload = json.dumps(report, indent=2, ensure_ascii=False) + '\n'
        # Stage both complete payloads before replacing either destination.
        staged = []
        try:
            for destination, content in [(args.o, payload)] + ([(args.report, report_payload)] if args.report else []):
                with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=destination.parent, delete=False) as target:
                    staged.append((Path(target.name), destination))
                    target.write(content)
            for temporary, destination in staged:
                temporary.replace(destination)
        finally:
            for temporary, _ in staged:
                temporary.unlink(missing_ok=True)
        return 0
    except SystemExit as exc:
        return 0 if exc.code == 0 else 14
    except OSError as exc:
        error = StackPrepError(12, 'INPUT_REFERENCE_MISSING', str(exc))
    except StackPrepError as exc:
        error = exc
    message = ' '.join(str(error).splitlines())
    print(f'copper_fraction: {error.name}: {message}', file=sys.stderr)
    return error.code


if __name__ == '__main__':
    raise SystemExit(main())
