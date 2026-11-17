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
copper_entries = [item for item in library["materials"] if item["id"] == "copper"]
assert len(copper_entries) == 1, "materials library must contain exactly one copper entry"
copper = copper_entries[0]
assert copper["provenance"]["reference"].strip(), "copper must cite a nonempty reference"
assert copper["provenance"]["source_type"] == "literature", "copper source_type must be literature"
assert copper["provenance"]["confidence"] == "medium", "copper confidence must be medium"
assert (
    "近似材料：多晶纯铜室温值，用于 PACKAGE_BULK_PARTS 的 lid；未按实际盖板合金和工艺标定"
    in copper["provenance"]["notes"]
), "copper notes must contain the required approximation statement"
assert copper["model"] == "thermo_elastic"
assert copper["unit_system"] == "SI"
assert 8800 <= copper["density"] <= 9000, "copper density must be in kg/m³"
assert 350 <= copper["conductivity"] <= 420, "copper conductivity must be in W/(m·K)"
for property_name in ("elastic", "thermal_expansion"):
    assert copper[property_name]["symmetry"] == "isotropic"
    table = copper[property_name]["table"]
    assert table["temperatures"] == [250, 400]
    assert table["interpolation"] == "linear"
    assert table["extrapolation"] == "clamp"
for property_name, column, lower, upper in (
    ("elastic", "E", 100e9, 140e9),
    ("elastic", "nu", 0.30, 0.37),
    ("thermal_expansion", "alpha", 15e-6, 18e-6),
):
    values = copper[property_name]["table"]["columns"][column]
    assert len(values) == 2, f"copper {column} must have two table values"
    assert all(lower <= value <= upper for value in values), f"copper {column} outside SI bounds"
assert copper["thermal_expansion"]["cte_type"] == "tangent"
assert copper["thermal_expansion"]["stress_free_temperature"] == 298.15

assembly = json.loads((examples / "assembly/package_bulk_parts.assembly.json").read_text(encoding="utf-8"))
lid_parts = [part for part in assembly["parts"] if part["id"] == "lid"]
assert len(lid_parts) == 1, "package bulk parts assembly must contain exactly one lid"
lid_material = lid_parts[0]["material"]
assert lid_material == "copper", "package bulk parts lid material must be exactly copper"
assert lid_material in known_material_ids, "MATERIAL_MAPPING_MISSING: lid material is absent from library"

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
        validator.validate(json.loads((root / "examples/s2_solve/case.json").read_text()))
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
