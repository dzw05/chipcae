"""Validate the actual transformed fixture against the repository schema."""
from test_fraction import *

if __name__ == '__main__':
    try:
        import jsonschema
    except ImportError:
        raise SystemExit(77)
    with tempfile.TemporaryDirectory() as directory:
        output = check(Path(directory))
        jsonschema.Draft202012Validator(json.loads((SOURCE / 'schema/stack-0.2.schema.json').read_text())).validate(output)
        stack = fixture(Path(directory))
        _, report = cli(Path(directory), stack)
        report_schema = json.loads((SOURCE / 'tools/stack/copper_fraction-report-0.1.schema.json').read_text())
        jsonschema.Draft202012Validator.check_schema(report_schema)
        validator = jsonschema.Draft202012Validator(report_schema)
        emitted = json.loads(report.read_text())
        validator.validate(emitted)
        # Ensure the version and method are enforced by the repository contract.
        for field in ('schema', 'method'):
            changed = dict(emitted, **{field: 'unsupported'})
            assert not validator.is_valid(changed)
        changed = copy.deepcopy(emitted)
        changed['layers'][0]['fraction'] = 1.01
        assert not validator.is_valid(changed)
