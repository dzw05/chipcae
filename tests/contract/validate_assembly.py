"""SP0 schema acceptance: validate published fixtures and semantic invariants."""
import copy
import json
import pathlib
import sys

try:
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
except ImportError:
    print('SKIP: jsonschema and referencing are required')
    sys.exit(77)


def pointer(path):
    return ''.join('/' + str(item).replace('~', '~0').replace('/', '~1') for item in path)


def errors(document, validator):
    result = []

    def collect(error):
        result.append((pointer(error.absolute_path), error.message))
        for child in error.context:
            collect(child)

    for error in validator.iter_errors(document):
        collect(error)
    if result:
        return result
    seen = set()

    def check_id(part, path):
        if part['id'] in seen:
            result.append((pointer(path), 'duplicate part ID'))
        seen.add(part['id'])

    # Explicit parts precede the shorthand expansion, regardless of key order.
    for index, part in enumerate(document['parts']):
        check_id(part, ['parts', index, 'id'])
        placement = part['placement']
        if 'bond' in placement and 'gap' in placement:
            if placement['gap'] != placement['bond']['thickness']:
                result.append((f'/parts/{index}/placement/gap', 'gap differs from bond thickness'))
    for body, part in document.get('parts_from_step', {}).get('bodies', {}).items():
        check_id(part, ['parts_from_step', 'bodies', body, 'id'])
    return result


def main(root):
    schema_path = root / 'schema/assembly-0.1.schema.json'
    if not schema_path.is_file():
        print('FAIL: missing assembly/0.1 schema:', schema_path)
        return 1
    schema = json.loads(schema_path.read_text())
    Draft202012Validator.check_schema(schema)
    registry = Registry().with_resource(schema['$id'], Resource.from_contents(schema))
    validator = Draft202012Validator(schema, registry=registry)
    examples = root / 'schema/examples/assembly'
    failures = []

    def check(name, expected=None):
        path = examples / name
        if not path.is_file():
            failures.append(f'{name}: missing acceptance fixture')
            return None
        try:
            document = json.loads(path.read_text())
        except (json.JSONDecodeError, OSError) as error:
            failures.append(f'{name}: cannot read acceptance fixture: {error}')
            return None
        found = errors(document, validator)
        paths = {path for path, _ in found}
        if expected is None and found:
            failures.append(f'{name}: positive fixture rejected: {found}')
        elif expected is not None and (not found or expected not in paths):
            failures.append(f'{name}: expected {expected}; errors={found}')
        return document

    positive = check('fcbga_lid_stiffener.assembly.json')
    bulk = check('step_frame_parts.assembly.json')
    if bulk is not None:
        # Exercise duplicates within shorthand bodies and against explicit parts.
        for duplicate_explicit in (False, True):
            duplicate = copy.deepcopy(bulk)
            bodies = duplicate['parts_from_step']['bodies']
            body = 'example_parts~test/duplicate'
            bodies[body] = copy.deepcopy(next(iter(bodies.values())))
            if duplicate_explicit:
                bodies[body]['id'] = duplicate['parts'][0]['id']
            expected = '/parts_from_step/bodies/example_parts~0test~1duplicate/id'
            found = errors(duplicate, validator)
            if expected not in {location for location, _ in found}:
                failures.append(f'duplicate expanded ID (explicit={duplicate_explicit}): {found}')
        unique = copy.deepcopy(bulk)
        unique_bodies = unique['parts_from_step']['bodies']
        unique_bodies[body] = copy.deepcopy(next(iter(unique_bodies.values())))
        unique_bodies[body]['id'] = 'unique_body'
        if errors(unique, validator):
            failures.append('unique expanded ID must be accepted')
    for suffix, expected in {
        'empty_body': '/parts_from_step/bodies',
        'bad_attach': '/parts/0/placement/attach',
        'dup_id': '/parts/1/id',
        'bond_gap_mismatch': '/parts/0/placement/gap',
        'abs_path': '/parts/0/step',
        'tilt_field': '/parts/0/placement',
        'frame_with_attach': '/parts/0/placement',
    }.items():
        check(f'negative-assembly-{suffix}.json', expected)
    if positive is not None:
        # Layer names are preserved verbatim; part attachments accept both sides.
        for target in ('layer:C@top', 'layer:C@bottom', 'layer:@@top',
                       'layer:Cu@inner@top', 'layer:Cu@inner@bottom',
                       'layer:@top@bottom', 'part:lid@top', 'part:lid@bottom'):
            document = copy.deepcopy(positive)
            document['parts'][0]['placement']['attach'] = target
            if errors(document, validator):
                failures.append(f'valid attachment {target!r} rejected')
        for target in ('layer:', 'layer:@top', 'layer:@bottom',
                       'layer:Cu@inner', 'layer:Cu@inner@side',
                       'part:bad@id@top', 'part:lid', 'part:lid@side',
                       'part:1bad@top'):
            document = copy.deepcopy(positive)
            document['parts'][0]['placement']['attach'] = target
            found = errors(document, validator)
            if '/parts/0/placement/attach' not in {location for location, _ in found}:
                failures.append(f'invalid attachment {target!r} accepted: {found}')
        # Reject embedded and trailing line breaks, including regex end-anchor cases.
        for line_break in ('\n', '\r', '\r\n', '\u2028', '\u2029'):
            for target in (f'layer:Cu{line_break}inner@top',
                           f'layer:Cu@top{line_break}', f'part:lid@top{line_break}'):
                document = copy.deepcopy(positive)
                document['parts'][0]['placement']['attach'] = target
                found = errors(document, validator)
                if '/parts/0/placement/attach' not in {location for location, _ in found}:
                    failures.append(f'multiline attachment {target!r} accepted: {found}')
        # Exercise both published shapes without replacing existing shorthand data.
        for fixture in (positive, bulk):
            if fixture is None:
                continue
            for path in (('board', 'path'), ('board', 'material_map'),
                         ('materials',), ('parts', 0, 'step'), ('parts_from_step', 'step')):
                for unsafe in ('', ' ', '\t', '\n', ' \r\n\t', '\u2003',
                               'ok\n/../outside', 'ok\n\\outside', 'ok\nhttps://outside'):
                    document = copy.deepcopy(fixture)
                    document.setdefault('parts_from_step', {
                        'step': 'bulk.step', 'frame': 'step', 'origin': [0, 0, 0], 'bodies': {}
                    })
                    parent = document
                    for key in path[:-1]:
                        parent = parent[key]
                    parent[path[-1]] = unsafe
                    found = errors(document, validator)
                    if pointer(path) not in {location for location, _ in found}:
                        failures.append(f'unsafe path {unsafe!r} at {pointer(path)} accepted: {found}')
        empty = copy.deepcopy(positive)
        empty['parts'] = []
        if errors(empty, validator):
            failures.append('parts: [] must be accepted')
        for gap in (None, 0, 1e-4):
            no_bond = copy.deepcopy(positive)
            placement = no_bond['parts'][0]['placement']
            placement.pop('bond', None)
            placement.pop('gap', None)
            if gap is not None:
                placement['gap'] = gap
            if errors(no_bond, validator):
                failures.append(f'no bond with gap {gap!r} must be accepted')
        implicit_gap = copy.deepcopy(positive)
        placement = implicit_gap['parts'][0]['placement']
        placement['bond'] = {'material': 'TIM1', 'thickness': 5e-5}
        placement.pop('gap', None)
        if errors(implicit_gap, validator):
            failures.append('bond without optional gap must be accepted')
        placement['gap'] = 5e-5
        if errors(implicit_gap, validator):
            failures.append('bond with exactly equal numeric gap must be accepted')
    for failure in failures:
        print('FAIL:', failure)
    return int(bool(failures))


if __name__ == '__main__':
    sys.exit(main(pathlib.Path(sys.argv[1])))
