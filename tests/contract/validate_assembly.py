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
    for index, part in enumerate(document['parts']):
        if part['id'] in seen:
            result.append((f'/parts/{index}/id', 'duplicate part ID'))
        seen.add(part['id'])
        placement = part['placement']
        if 'bond' in placement and 'gap' in placement:
            if placement['gap'] != placement['bond']['thickness']:
                result.append((f'/parts/{index}/placement/gap', 'gap differs from bond thickness'))
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
        except (ValueError, OSError) as error:
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
    check('package_bulk_parts.assembly.json')
    for suffix, expected in {
        'bad_attach': '/parts/0/placement/attach',
        'dup_id': '/parts/1/id',
        'bond_gap_mismatch': '/parts/0/placement/gap',
        'abs_path': '/parts/0/step',
        'tilt_field': '/parts/0/placement',
        'frame_with_attach': '/parts/0/placement',
    }.items():
        check(f'negative-assembly-{suffix}.json', expected)
    if positive is not None:
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
