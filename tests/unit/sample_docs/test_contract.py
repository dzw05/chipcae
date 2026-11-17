"""Read-only documentation acceptance; never starts a numerical solver."""

import json
from pathlib import Path
import re
import shlex
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


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
    for argv in commands:
        if argv[0] == "chipcae-solve":
            require(len(argv) == 7 and argv[1::2] == ["--case", "--profile", "--out"],
                    f"solver command must use the public three-flag signature: {argv!r}")
            for path in (argv[2], argv[4]):
                require((root / path).is_file(), f"missing command input: {path}")

    required_paths = {
        "apps/chipcae-solve/README.md",
        "schema/examples/fcbga_demo.case.json",
        "schema/examples/thermal.case.json",
        "schema/examples/fcbga_demo.materials.json",
        "examples/s2_solve/case.json", "examples/s2_solve/profile.json",
        "examples/s2_solve/stack.json", "examples/s2_solve/materials.json",
    }
    for case_path in ("schema/examples/fcbga_demo.case.json", "examples/s2_solve/case.json"):
        case_file = root / case_path
        require(case_file.is_file(), f"missing case: {case_path}")
        case = json.loads(case_file.read_text(encoding="utf-8"))
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
    require(isinstance(materials, list) and materials, "FCBGA materials must be a nonempty list")
    multi_knot = []
    for material in materials:
        for kind in ("elastic", "thermal_expansion"):
            temperatures = material[kind]["table"]["temperatures"]
            require(isinstance(temperatures, list), f"{material['id']}.{kind}: temperatures must be a list")
            single_knot = len(temperatures) == 1
            if not single_knot and len(temperatures) >= 2:
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
        "steady conduction limitation": r"(?:steady|稳态).{0,180}(?:not ready|cannot|not solv|缺|不能|未就绪|requires)",
        "original FCBGA temperature-table failure": r"(?:original|as[- ]is|as checked in|原样).{0,220}(?:multi[- ]knot|two.{0,15}knot|temperature tables|温度表)",
        "historical INPUT_INVALID/14": r"input_invalid.{0,30}\b14\b",
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
    except (AssertionError, KeyError, ValueError, OSError, IndexError) as error:
        sys.exit(f"sampledocs contract: {error}")
