"""Read-only documentation acceptance; never starts a numerical solver."""

import json
import math
from pathlib import Path
import re
import shlex
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def validate_input(root, path, schema_path):
    # Keep full schema checks; report missing/incompatible dependencies explicitly.
    try:
        from importlib.metadata import version
        import jsonschema
        from referencing import Registry, Resource
        for package, expected in (("jsonschema", "4.26.0"), ("referencing", "0.37.0")):
            require(version(package) == expected,
                    f"{package}=={expected} required; install with "
                    "python3 -m pip install -r tests/unit/sample_docs/requirements.txt")
    except ImportError as error:
        raise AssertionError(
            "schema validation dependencies unavailable; install with "
            "python3 -m pip install -r tests/unit/sample_docs/requirements.txt"
        ) from error

    require(path.is_file(), f"missing command input or reference: {path}")
    document = json.loads(path.read_text(encoding="utf-8"))
    registry = Registry()
    for source in list((root / "schema").glob("*.schema.json")) + list(
            (root / "material/schema").glob("*.schema.json")):
        schema = json.loads(source.read_text(encoding="utf-8"))
        registry = registry.with_resource(schema["$id"], Resource.from_contents(schema))
    schema = json.loads((root / schema_path).read_text(encoding="utf-8"))
    validator = jsonschema.validators.validator_for(schema)(schema, registry=registry)
    errors = list(validator.iter_errors(document))
    require(not errors, f"{path}: " + "; ".join(
        f"{error.json_path}: {error.message}" for error in errors))
    return document


def check_case(root, case_file, case):
    inputs = {}
    for kind, schema in (
        ("stack", "schema/stack-0.2.schema.json"),
        ("materials", "material/schema/material-library.v1.schema.json"),
    ):
        target = case_file.parent / case[kind]["ref"]
        inputs[kind] = validate_input(root, target, schema)
    materials = inputs["materials"]["materials"]
    label = f"{case_file}: {case['materials']['ref']}"
    require(isinstance(materials, list) and materials, f"{label}: empty materials")
    ids = [material["id"] for material in materials]
    require(len(ids) == len(set(ids)), f"{label}: duplicate material IDs")
    for layer in inputs["stack"]["layers"]:
        for entity in [layer] + layer.get("regions", []):
            require(entity["material"] in ids,
                    f"{case_file}: {case['stack']['ref']}: "
                    f"unknown material {entity['material']} in {entity['name']}")
    return inputs


def check(root):
    guide = root / "docs/examples/sample-readiness.md"
    require(guide.is_file(), "missing guide: docs/examples/sample-readiness.md")
    text = guide.read_text(encoding="utf-8")
    require(text.strip(), "empty guide: docs/examples/sample-readiness.md")

    blocks = re.findall(r"^```(?:sh|bash)\s*\n(.*?)^```", text, re.M | re.S)
    require(blocks, "guide must contain fenced sh/bash commands")
    commands = []
    for block in blocks:
        for line in block.replace("\\\n", " ").splitlines():
            argv = shlex.split(line, comments=True)
            if argv:
                commands.append(argv)
    s2 = ["chipcae-solve", "--case", "examples/s2_solve/case.json",
          "--profile", "examples/s2_solve/profile.json", "--out", "run-s2"]
    candidates = [argv for argv in commands if "examples/s2_solve/case.json" in argv]
    require(candidates and all(argv == s2 for argv in candidates),
            f"S2 command must equal {s2!r}; found {candidates!r}")
    o2 = ["chipcae-solve", "--case", "case.json",
          "--profile", "profile.json", "--out", "run_dir"]
    require(o2 in commands, f"missing O2 command template: {o2!r}")
    for argv in commands:
        if argv[0] == "chipcae-solve":
            require(len(argv) == 7 and argv[1::2] == ["--case", "--profile", "--out"],
                    f"solver command must use the public three-flag signature: {argv!r}")
            if argv == o2:
                continue
            validate_input(root, root / argv[4], "schema/run-profile-1.0.schema.json")
            case_file = root / argv[2]
            case = validate_input(root, case_file, "schema/case-1.0.schema.json")
            inputs = check_case(root, case_file, case)
            for material in inputs["materials"]["materials"]:
                require(material["model"] == "thermo_elastic", "S2 requires thermoelastic materials")
                for kind, columns in (("elastic", ("E", "nu")),
                                      ("thermal_expansion", ("alpha",))):
                    model = material[kind]
                    require(model["symmetry"] == "isotropic", f"S2 requires isotropic {kind}")
                    table = model["table"]
                    require(len(table["temperatures"]) == 1,
                            f"S2 requires single-knot {kind}")
                    for column in columns:
                        values = table["columns"].get(column)
                        require(isinstance(values, list) and len(values) == 1
                                and type(values[0]) in (int, float) and math.isfinite(values[0]),
                                f"S2 requires one finite {column} value")

    required_paths = {
        "apps/chipcae-solve/README.md",
        "schema/examples/fcbga_demo.case.json",
        "schema/examples/thermal.case.json",
        "schema/examples/fcbga_demo.materials.json",
        "examples/s2_solve/case.json", "examples/s2_solve/profile.json",
        "examples/s2_solve/stack.json", "examples/s2_solve/materials.json",
    }
    for case_path in ("schema/examples/fcbga_demo.case.json",
                      "schema/examples/thermal.case.json", "examples/s2_solve/case.json"):
        case_file = root / case_path
        require(case_file.is_file(), f"missing case: {case_path}")
        case = validate_input(root, case_file, "schema/case-1.0.schema.json")
        inputs = check_case(root, case_file, case)
        if case_path == "schema/examples/thermal.case.json":
            fcbga = root / "schema/examples/fcbga_demo.case.json"
            original = json.loads(fcbga.read_text(encoding="utf-8"))
            for kind in ("stack", "materials"):
                require(case[kind]["ref"] == original[kind]["ref"],
                        f"{case_file}: {kind} must reference the same FCBGA input")
            for material in inputs["materials"]["materials"]:
                require("conductivity" not in material,
                        f"{case_file}: {case['materials']['ref']}: "
                        f"{material['id']} now supplies conductivity; revisit thermal limitation")
        for kind in ("stack", "materials"):
            ref = case[kind]["ref"]
            target = case_file.parent / ref
            require(target.is_file(), f"missing {kind} reference in {case_path}: {ref}")
            required_paths.add(target.relative_to(root).as_posix())
    # Also check any other explicit repository-relative file references in prose.
    documented = set(re.findall(
        r"(?<![\w/])(?:apps|schema|examples|docs)/[\w./-]+\.(?:json|md)\b", text))
    require(required_paths <= documented,
            f"guide omits required paths: {sorted(required_paths - documented)}")
    for path in sorted(documented):
        require((root / path).is_file(), f"missing documented file: {path}")

    library = json.loads((root / "schema/examples/fcbga_demo.materials.json").read_text())
    materials = library.get("materials")
    require(isinstance(materials, list) and materials,
            "schema/examples/fcbga_demo.materials.json: materials must be a nonempty list")
    multi_knot = []
    for material in materials:
        for kind in ("elastic", "thermal_expansion"):
            temperatures = material[kind]["table"]["temperatures"]
            require(isinstance(temperatures, list), f"{material['id']}.{kind}: temperatures must be a list")
            require(temperatures, f"FCBGA {material['id']}.{kind}: empty temperatures")
            if len(temperatures) >= 2:
                multi_knot.append((material["id"], kind))
    require(multi_knot, "cited FCBGA library must contain at least one multi-knot table")

    # Concept checks allow alternative wording and do not prescribe whole sentences.
    prose = re.sub(r"\s+", " ", text.lower())
    concepts = {
        "schema validity": r"schema.{0,100}(?:valid|合法)",
        "O2 readiness": r"o2.{0,120}(?:ready|readiness|solv|可求解|就绪)",
        "S2 cut demonstration": r"s2.{0,100}cut.{0,100}(?:demo|演示)",
        "single-knot temperature requirement": r"(?:single[- ](?:knot|temperature)|单温|单节点)",
        "E/nu/alpha tables": r"\be\b.{0,80}\bnu\b.{0,80}\balpha\b",
        "adjacent-size max_ratio meaning": r"(?:adjacent|相邻).{0,100}(?:size|尺寸).{0,120}(?:limit|exceed|限制|超过|max_ratio)",
        "max_ratio": r"max_ratio",
        "positive conductivity": r"(?:positive.{0,50}conductivity|conductivity.{0,50}(?:positive|正))",
        "thermal sample lacks conductivity": r"(?:lack|missing|缺).{0,100}conductivity|conductivity.{0,100}(?:lack|missing|缺)",
        "steady conduction limitation": r"thermal(?:\.case\.json| sample).{0,240}(?:not ready|cannot solve|not solvable|不能求解|未就绪)",
        "checked-in FCBGA temperature-table limitation": r"(?:checked-in|original|as[- ]is|as checked in|原样).{0,220}(?:multi[- ]knot|two.{0,15}(?:temperature )?knots|多(?:温度)?节点)",
        "original sample input error": r"(?:multi[- ]knot|two.{0,15}(?:temperature )?knots).{0,180}input_invalid\s*[/ :]\s*14",
        "S2 dependencies": r"jsoncons.{0,160}(?:mpi|hdf5|mfem)",
        "local resource requirement": r"(?:local|本地).{0,100}(?:resource|资源)",
        "fresh exclusive output": r"(?:fresh|new|新).{0,100}(?:exclusive|独占)|(?:exclusive|独占).{0,100}(?:fresh|new|新)",
        "checks do not run numerical solve": r"(?:do not|does not|did not|not run|未|不).{0,80}(?:numerical|数值)",
        "illustrative materials": r"illustrative|示意",
    }
    for concept, pattern in concepts.items():
        require(re.search(pattern, prose), f"guide must explain {concept}")


if __name__ == "__main__":
    try:
        check(Path(sys.argv[1]).resolve())
    except (AssertionError, KeyError, ValueError, OSError, IndexError, TypeError) as error:
        sys.exit(f"sampledocs contract: {error}")
