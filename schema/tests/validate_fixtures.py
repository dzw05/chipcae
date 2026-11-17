import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
try:
    import jsonschema
    from referencing import Registry, Resource
except ImportError:
    print("SKIP: jsonschema and referencing are required for schema fixture validation")
    sys.exit(77)

examples = root / "schema/examples"
parameters = json.loads((root / "schema/parameters.schema.json").read_text(encoding="utf-8"))
registry = Registry().with_resource(parameters["$id"], Resource.from_contents(parameters))
material_schema = json.loads((root / "material/schema/material.v1.schema.json").read_text(encoding="utf-8"))
library_schema = json.loads((root / "material/schema/material-library.v1.schema.json").read_text(encoding="utf-8"))
registry = registry.with_resource(material_schema["$id"], Resource.from_contents(material_schema))
registry = registry.with_resource(library_schema["$id"], Resource.from_contents(library_schema))
library_validator = jsonschema.Draft202012Validator(library_schema, registry=registry)
library = json.loads((root / "schema/examples/fcbga_demo.materials.json").read_text(encoding="utf-8"))
library_validator.validate(library)
material_ids = [item["id"] for item in library["materials"]]
assert len(material_ids) == len(set(material_ids)), "MATERIAL_ID_DUPLICATE: library ids must be unique"
stack = json.loads((root / "schema/examples/fcbga_demo.stack.json").read_text(encoding="utf-8"))
known_material_ids = set(material_ids)
for layer in stack.get("layers", []):
    referenced = {layer.get("material")}
    referenced.update(region.get("material") for region in layer.get("regions", []))
    assert referenced <= known_material_ids, "MATERIAL_MAPPING_MISSING: stack material is absent from library"
for invalid_library in (root / "material/schema/examples/negative-library-missing-schema.json",
                        root / "material/schema/examples/negative-library-invalid-material.json"):
    try:
        library_validator.validate(json.loads(invalid_library.read_text(encoding="utf-8")))
    except jsonschema.ValidationError:
        continue
    raise AssertionError(f"negative material library unexpectedly validated: {invalid_library}")

for schema_file, valid, invalid_prefix in [
    (root / "schema/case-1.0.schema.json", examples / "fcbga_demo.case.json", "negative-case-"),
    (root / "schema/run-profile-1.0.schema.json", examples / "local.run-profile.json", "negative-profile-"),
]:
    schema = json.loads(schema_file.read_text(encoding="utf-8"))
    registry = registry.with_resource(schema["$id"], Resource.from_contents(schema))
    validator = jsonschema.Draft202012Validator(schema, registry=registry)
    instance = json.loads(valid.read_text(encoding="utf-8"))
    validator.validate(instance)
    if schema_file.name == "case-1.0.schema.json":
        validator.validate(json.loads((examples / "thermal.case.json").read_text(encoding="utf-8")))
        def semantic_check(case):
            stack_path = (examples / case["stack"]["ref"]).resolve()
            stack = json.loads(stack_path.read_text(encoding="utf-8"))
            layer_names = {layer["name"] for layer in stack.get("layers", [])}
            for item in case["loads"]:
                if item.get("target", "").startswith("layer:"):
                    assert item["target"][6:] in layer_names, f"unknown layer target: {item['target']}"
            for item in case["constraints"].get("fixed", []):
                if item.get("target", "").startswith("layer:"):
                    assert item["target"][6:] in layer_names, f"unknown layer target: {item['target']}"
            ids = [x["id"] for x in case["loads"] if "id" in x]
            ids += [x["id"] for x in case["constraints"].get("fixed", []) if "id" in x]
            if "id" in case["constraints"]:
                ids.append(case["constraints"]["id"])
            assert len(ids) == len(set(ids)), "case IDs must be unique"
        semantic_check(instance)
    for invalid in examples.glob(invalid_prefix + "*.json"):
        try:
            validator.validate(json.loads(invalid.read_text(encoding="utf-8")))
        except jsonschema.ValidationError:
            continue
        raise AssertionError(f"negative fixture unexpectedly validated: {invalid}")
print("case/run profile fixtures OK")
