"""Read-only documentation acceptance; never starts a numerical solver."""

import json
from pathlib import Path
import re
import shlex
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_object(path):
    require(path.is_file(), f"missing command input or reference: {path}")
    document = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(document, dict), f"{path}: expected a JSON object")
    return document


def check_case(case_file, case):
    inputs = {}
    for kind in ("stack", "materials"):
        reference = case.get(kind)
        require(isinstance(reference, dict), f"{case_file}: missing {kind} reference")
        ref = reference.get("ref")
        require(isinstance(ref, str) and ref, f"{case_file}: invalid {kind} ref")
        require(not Path(ref).is_absolute(), f"{case_file}: {kind} ref must be relative: {ref}")
        inputs[kind] = read_object(case_file.parent / ref)
    materials = inputs["materials"].get("materials")
    label = f"{case_file}: {case['materials']['ref']}"
    require(isinstance(materials, list) and materials, f"{label}: empty materials")
    for material in materials:
        require(isinstance(material, dict), f"{label}: material must be an object")
    layers = inputs["stack"].get("layers")
    require(isinstance(layers, list) and layers,
            f"{case_file}: {case['stack']['ref']}: layers must be a nonempty list")
    referenced = set()
    for layer in layers:
        require(isinstance(layer, dict), f"{case_file}: layer must be an object")
        regions = layer.get("regions", [])
        require(isinstance(regions, list), f"{case_file}: regions must be a list")
        for entity in [layer] + regions:
            require(isinstance(entity, dict), f"{case_file}: region must be an object")
            identifier = entity.get("material")
            if isinstance(identifier, str):
                referenced.add(identifier)
    inputs["referenced_materials"] = referenced
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
            profile_file = root / argv[4]
            profile = read_object(profile_file)
            require(profile.get("schema") == "chipcae.run_profile/1.0",
                    f"{profile_file}: expected run-profile schema")
            require(profile.get("backend") in ("cpu", "cuda"),
                    f"{profile_file}: invalid backend")
            for key, minimum in (("ranks", 1), ("gpus", 0)):
                require(type(profile.get(key)) is int and profile[key] >= minimum,
                        f"{profile_file}: {key} must be an integer >= {minimum}")
            case_file = root / argv[2]
            check_case(case_file, read_object(case_file))

    required_paths = {
        "apps/chipcae-solve/README.md",
        "schema/examples/fcbga_demo.case.json",
        "schema/examples/thermal.case.json",
        "examples/s2_solve/case.json", "examples/s2_solve/profile.json",
    }
    case_inputs = {}
    for case_path in ("schema/examples/fcbga_demo.case.json",
                      "schema/examples/thermal.case.json", "examples/s2_solve/case.json"):
        case_file = root / case_path
        require(case_file.is_file(), f"missing case: {case_path}")
        case = read_object(case_file)
        inputs = check_case(case_file, case)
        case_inputs[case_path] = inputs
        if case_path == "schema/examples/thermal.case.json":
            require(inputs["referenced_materials"],
                    f"{case_file}: no referenced thermal materials found")
            # One missing material suffices for the documented unreadiness;
            # adding conductivity to other materials is a legitimate improvement.
            require(any("conductivity" not in material
                        for material in inputs["materials"]["materials"]
                        if material.get("id") in inputs["referenced_materials"]),
                    f"{case_file}: all referenced materials now supply conductivity; "
                    "revisit the guide's thermal limitation")
    # Scan explicit prose references in the guide's current top-level directories.
    # Case-relative inputs are checked above; they need not be listed in prose.
    documented = set(re.findall(
        r"(?<![\w/])(?:apps|schema|examples|docs)/[\w./-]+\.(?:json|md)\b", text))
    require(required_paths <= documented,
            f"guide omits required paths: {sorted(required_paths - documented)}")
    for path in sorted(documented):
        require((root / path).is_file(), f"missing documented file: {path}")

    library = read_object(root / "schema/examples/fcbga_demo.materials.json")
    materials = library.get("materials")
    require(isinstance(materials, list) and materials,
            "schema/examples/fcbga_demo.materials.json: materials must be a nonempty list")
    multi_knot = []
    referenced = case_inputs["schema/examples/fcbga_demo.case.json"]["referenced_materials"]
    for material in materials:
        if not isinstance(material, dict) or material.get("id") not in referenced:
            continue
        for kind in ("elastic", "thermal_expansion"):
            model = material.get(kind)
            if not isinstance(model, dict):
                continue
            table = model.get("table")
            if not isinstance(table, dict):
                continue
            temperatures = table.get("temperatures")
            if isinstance(temperatures, list) and len(temperatures) >= 2:
                multi_knot.append((material.get("id"), kind))
    require(multi_knot,
            "cited FCBGA stack must reference at least one multi-knot table; "
            f"referenced material ids: {sorted(referenced)!r}")
    thermal = case_inputs["schema/examples/thermal.case.json"]
    if thermal["stack"] == case_inputs["schema/examples/fcbga_demo.case.json"]["stack"]:
        require(any(identifier in thermal["referenced_materials"]
                    for identifier, _ in multi_knot),
                "shared thermal stack must reference a multi-knot FCBGA table; "
                f"material ids: {sorted(thermal['referenced_materials'])!r}")

    # Concept checks allow alternative wording and do not prescribe whole sentences.
    prose = re.sub(r"\s+", " ", text.lower())
    concepts = {
        "schema validity": r"schema.{0,100}(?:valid|合法)",
        "O2 readiness": r"o2.{0,120}(?:ready|readiness|solv|可求解|就绪)",
        "S2 cut demonstration": r"s2.{0,100}cut.{0,100}(?:demo|演示)",
        "single-knot temperature requirement": r"(?:single[- ](?:knot|temperature)|单温|单节点)",
        "E/nu/alpha tables": r"\be\b.{0,80}\bnu\b.{0,80}\balpha\b",
        "adjacent-size max_ratio meaning": r"(?:adjacent|相邻).{0,100}(?:size|尺寸).{0,120}(?:limit|exceed|限制|超过|max_ratio)",
        "max_ratio after temperature correction": r"(?:after|corrected|修正).{0,120}max_ratio",
        "no general max_ratio recommendation": r"(?:neither|not|no|不).{0,60}(?:recommend|推荐).{0,60}(?:general|通用).{0,60}max_ratio",
        "mesh evidence for loosening limits": r"(?:loosen|放宽).{0,100}(?:evidence|依据)",
        "uncut does not replace cut acceptance": r"uncut.{0,100}(?:does not|cannot|不).{0,80}(?:replace|替代).{0,80}cut[- ]cell",
        "positive conductivity": r"(?:positive.{0,50}conductivity|conductivity.{0,50}(?:positive|正))",
        "thermal sample lacks conductivity": r"(?:lack|missing|缺).{0,100}conductivity|conductivity.{0,100}(?:lack|missing|缺)",
        "steady conduction limitation": r"thermal(?:\.case\.json| sample).{0,240}(?:not ready|cannot solve|not solvable|不能求解|未就绪)",
        "checked-in FCBGA temperature-table limitation": r"(?:checked-in|original|as[- ]is|as checked in|原样).{0,220}(?:multi[- ]knot|two.{0,15}(?:temperature )?knots|多(?:温度)?节点)",
        "original sample input error": r"(?:multi[- ]knot|two.{0,15}(?:temperature )?knots).{0,180}input_invalid\s*[/ :]\s*14",
        "S2 dependencies": r"jsoncons.{0,160}yaml-cpp.{0,160}(?:mpi|hdf5|mfem)",
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
