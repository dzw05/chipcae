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
    if schema_file.name == "case-1.0.schema.json":
        def semantic_check(case):
            physics = case["physics"]
            loads = case["loads"]
            thermal = any(p.get("id") == "thermal" for p in physics)
            mech = next((p for p in physics if p.get("id") == "mech"), None)
            uniform = [x for x in loads if x.get("type") == "uniform_temperature"]
            if uniform:
                assert len(uniform) == 1 and not thermal and mech and "temp_from" not in mech
                assert not any(x.get("type") in {"fixed_temperature", "convection", "heat_source"} for x in loads)
            if thermal:
                assert mech and mech.get("temp_from") == "thermal" and "T_ref" in mech
                assert not uniform and any(x.get("type") in {"fixed_temperature", "convection"} for x in loads)
            ids = [x["id"] for x in loads if "id" in x]
            ids += [x["id"] for x in case["constraints"].get("fixed", []) if "id" in x]
            if "id" in case["constraints"]: ids.append(case["constraints"]["id"])
            assert len(ids) == len(set(ids)), "case IDs must be unique"
        semantic_check(instance)
    for invalid in examples.glob(invalid_prefix + "*.json"):
        try:
            validator.validate(json.loads(invalid.read_text(encoding="utf-8")))
        except jsonschema.ValidationError:
            continue
        raise AssertionError(f"negative fixture unexpectedly validated: {invalid}")
print("case/run profile fixtures OK")
