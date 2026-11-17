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
