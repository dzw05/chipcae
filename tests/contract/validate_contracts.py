"""Read-only acceptance checks for published schemas and process error codes."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys


def dependencies(schemas=False):
    try:
        import yaml
        if schemas:
            import jsonschema
            import referencing
        return yaml
    except ImportError as error:
        print(f"FAIL: contract validation dependency unavailable: {error}", file=sys.stderr)
        sys.exit(1)


def schemas(root):
    yaml = dependencies(schemas=True)
    import jsonschema
    from referencing import Registry, Resource
    sys.path.insert(0, str(root))
    from tools.stack.polyset import read_polyset

    registry = Registry()
    documents = {}
    for path in sorted(root.glob("schema/*.schema.json")) + sorted(root.glob("material/schema/*.schema.json")):
        try:
            schema = json.loads(path.read_text(encoding="utf-8"))
            jsonschema.validators.validator_for(schema).check_schema(schema)
        except (ValueError, jsonschema.SchemaError) as error:
            raise AssertionError(f"{path}: schema field {getattr(error, 'json_path', '$')}: {error}") from error
        documents[path.relative_to(root).as_posix()] = schema
        registry = registry.with_resource(schema["$id"], Resource.from_contents(schema))

    for path in sorted((root / "schema/examples").iterdir()):
        if path.suffix not in (".json", ".yaml", ".yml"):
            continue
        if path.name.endswith(".polyset.json"):
            # The polyset contract uses CCPOLY01, including for .json sidecars.
            polyset = read_polyset(path)
            stack_path = path.with_name(path.name.removesuffix(".polyset.json") + ".stack.json")
            stack = json.loads(stack_path.read_text(encoding="utf-8"))
            refs = [ref for ref in stack["external_refs"] if ref["path"] == path.name]
            assert len(refs) == 1, f"{path}: expected one stack reference"
            assert refs[0]["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest(), path
            names = {layer.name for layer in polyset.layers}
            polygons = [layer["copper"]["polygons"] for layer in stack["layers"]
                        if "polygons" in layer.get("copper", {})
                        and layer["copper"]["polygons"]["file"] == path.name]
            assert polygons, f"{path}: expected a copper polygons reference"
            for reference in polygons:
                assert reference["layer"] in names, f"{path}: unknown polyset layer"
            continue
        text = path.read_text(encoding="utf-8")
        instance = json.loads(text) if path.suffix == ".json" else yaml.safe_load(text)
        if ".stack." in path.name:
            # Unversioned stacks may conform to any published stack contract.
            targets = sorted(key for key in documents if key.startswith("schema/stack-") and key.endswith(".schema.json"))
            assert targets, f"{path}: $: no published stack schemas"
        elif ".case." in path.name or path.name.startswith("negative-case-"):
            targets = ["schema/case-1.0.schema.json"]
        elif ".run-profile." in path.name or path.name.startswith("negative-profile-"):
            targets = ["schema/run-profile-1.0.schema.json"]
        elif ".materials." in path.name:
            targets = ["material/schema/material-library.v1.schema.json"]
        else:
            raise AssertionError(f"{path}: $: no corresponding schema")
        failures = []
        accepted = False
        for target in targets:
            schema = documents[target]
            validator = jsonschema.validators.validator_for(schema)(schema, registry=registry)
            errors = list(validator.iter_errors(instance))
            if path.name.startswith("negative-"):
                assert errors, f"{path}: $: negative fixture unexpectedly accepted by {target}"
            else:
                accepted |= not errors
                failures.extend(
                    f"{path}: {error.json_path}: {error.message} ({target})" for error in errors
                )
        if not path.name.startswith("negative-"):
            assert accepted, "\n".join(failures)

    # Reuse library negatives, material mappings, and case semantic checks.
    result = subprocess.run([sys.executable, str(root / "schema/tests/validate_fixtures.py"), str(root)],
                            capture_output=True, text=True)
    assert result.returncode == 0, f"schema/tests/validate_fixtures.py:\n{result.stdout}{result.stderr}"

    fixture_selfchecks(root)


def fixture_selfchecks(root):
    for path in sorted(root.glob("tests/contract/fixtures/*/check.py")):
        result = subprocess.run([sys.executable, str(path), "--repo", str(root)],
                                capture_output=True, text=True)
        assert result.returncode == 0, (
            f"fixture {path.parent.name}:\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def error_codes(root):
    yaml = dependencies()
    path = root / "protocol/errors.yaml"
    entries = yaml.safe_load(path.read_text(encoding="utf-8"))["errors"]
    published = """INTERNAL NOT_IMPLEMENTED COLUMN_SPLIT_ACROSS_RANKS HIERARCHY_INVALID
    VERSION_UNSUPPORTED IMPORT_CONFIRMATION_REQUIRED IMPORT_REJECTED IMPORT_UNSUPPORTED_ELEMENT
    MATERIAL_MAPPING_MISSING INPUT_HASH_MISMATCH MATERIAL_ID_DUPLICATE INPUT_REFERENCE_MISSING
    CUT_UNSUPPORTED INPUT_INVALID""".split()
    header = (root / "core/include/chipcae/core/error_code.hpp").read_text(encoding="utf-8")
    constants = dict(re.findall(r"\b([A-Z][A-Z_]+)\s*=\s*(\d+)", header))
    seen = set()
    actual = {}
    for index, entry in enumerate(entries):
        code = entry.get("code")
        field = f"{path}: $.errors[{index}].code ({code!r})"
        assert type(code) is int, f"{field}: expected integer"
        assert code not in seen, f"{field}: duplicate code"
        seen.add(code)
        # main.cpp returns error.code directly: protocol code is the stable exit code.
        assert 1 <= code <= 255, f"{field}: not a portable nonzero process exit code"
        name = entry.get("name")
        assert constants.get(name) == str(code), f"{field}: {name} disagrees with ErrorCode exit mapping"
        assert name not in actual, f"{path}: $.errors[{index}].name: duplicate {name}"
        actual[name] = code
    for code, name in enumerate(published, 1):
        assert actual.get(name) == code, f"{path}: {name}.code: stable exit code must remain {code}"


if __name__ == "__main__":
    try:
        {"schemas": schemas, "error_codes": error_codes}[sys.argv[1]](pathlib.Path(sys.argv[2]).resolve())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
    print(f"contract_{sys.argv[1]} OK")
