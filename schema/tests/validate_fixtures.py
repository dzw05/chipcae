import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
try:
    import jsonschema
except ImportError:
    print("jsonschema unavailable; checked fixture JSON syntax only")
    for path in (root / "schema/examples").glob("*.json"):
        json.loads(path.read_text(encoding="utf-8"))
    sys.exit(0)

examples = root / "schema/examples"
for schema_file, valid, invalid_prefix in [
    (root / "schema/case-1.0.schema.json", examples / "fcbga_demo.case.json", "negative-case-"),
    (root / "schema/run-profile-1.0.schema.json", examples / "local.run-profile.json", "negative-profile-"),
]:
    schema = json.loads(schema_file.read_text(encoding="utf-8"))
    resolver = jsonschema.RefResolver(base_uri=schema_file.resolve().as_uri(), referrer=schema)
    validator = jsonschema.Draft202012Validator(schema, resolver=resolver)
    instance = json.loads(valid.read_text(encoding="utf-8"))
    validator.validate(instance)
    for invalid in examples.glob(invalid_prefix + "*.json"):
        try:
            validator.validate(json.loads(invalid.read_text(encoding="utf-8")))
        except jsonschema.ValidationError:
            continue
        raise AssertionError(f"negative fixture unexpectedly validated: {invalid}")
print("case/run profile fixtures OK")
