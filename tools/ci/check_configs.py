"""Validate CI configs, the protocol error table and workflow build limits."""

from __future__ import annotations

import re
import sys
from pathlib import Path

import yaml

REQUIRED_REPETITIONS = {
    "microbenchmark": {"warmup": 1, "n": 7, "aggregate": "median"},
    "end_to_end_cpu": {"warmup": 0, "n": 3, "aggregate": "median"},
    "end_to_end_gpu": {"warmup": 1, "n": 3, "aggregate": "median"},
    "scaling": {"warmup": 0, "n": 2, "aggregate": "best"},
}


def _load(path: Path):
    with path.open(encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def check_perf(path: Path) -> list[str]:
    errors = []
    data = _load(path)
    if not isinstance(data, dict):
        return [f"{path}: expected a mapping"]
    repetitions = data.get("repetitions") or {}
    for name, expected in REQUIRED_REPETITIONS.items():
        if repetitions.get(name) != expected:
            errors.append(f"{path}: repetitions.{name} is {repetitions.get(name)!r}, expected {expected!r}")
    noise = data.get("noise") or {}
    if noise.get("cv_max") != 0.05 or noise.get("reruns") != 1:
        errors.append(f"{path}: noise policy must stay cv_max=0.05 and reruns=1")
    baseline = data.get("baseline") or {}
    if baseline.get("auto_update") is not False:
        errors.append(f"{path}: baseline.auto_update must be false")
    if baseline.get("update_label") != "perf-baseline-update":
        errors.append(f"{path}: baseline update label drifted")
    if baseline.get("per_commit_k") != 20 or baseline.get("nightly_k") != 10:
        errors.append(f"{path}: baseline window must stay K=20 per-commit and K=10 nightly")
    thresholds = data.get("thresholds") or {}
    per_commit = thresholds.get("per_commit") or {}
    if per_commit.get("wall_time", {}).get("blocks") is not False:
        errors.append(f"{path}: per-commit wall time must not block")
    if per_commit.get("iterations", {}).get("warn") != 0.02 or per_commit.get("iterations", {}).get("block") != 0.10:
        errors.append(f"{path}: per-commit iteration thresholds drifted")
    if per_commit.get("peak_memory", {}).get("warn") != 0.05 or per_commit.get("peak_memory", {}).get("block") != 0.10:
        errors.append(f"{path}: per-commit memory thresholds drifted")
    nightly = thresholds.get("nightly") or {}
    if nightly.get("wall_time", {}).get("block_next_day") != 0.15:
        errors.append(f"{path}: nightly wall-time next-day hold must stay 15%")
    if not (data.get("tiers") or {}).get("a", {}).get("timing_informational"):
        errors.append(f"{path}: tier A timing must stay informational")
    return errors


def check_errors(path: Path) -> list[str]:
    data = _load(path)
    if not isinstance(data, dict) or not isinstance(data.get("errors"), list):
        return [f"{path}: expected a mapping with an errors list"]
    seen = set()
    errors = []
    for item in data["errors"]:
        if not isinstance(item, dict) or "code" not in item or "name" not in item:
            errors.append(f"{path}: each error needs code and name")
            continue
        if item["code"] in seen:
            errors.append(f"{path}: duplicate code {item['code']}")
        seen.add(item["code"])
    return errors


# This is a deliberately narrow proof, not a general shell interpreter. Keep the
# recognized block in sync with the workflow scripts and their Bash matrix tests.
_NPROC_CAP = "\n".join(
    r"[ \t]*" + re.escape(line) + r"[ \t]*"
    for line in (
        'if ! jobs="$(nproc)"; then',
        "jobs=2",
        "fi",
        'case "$jobs" in',
        "''|*[!0-9]*|0) jobs=2 ;;",
        "[1-4]) ;;",
        "*) jobs=4 ;;",
        "esac",
    )
)
_SHELL_PARTS = re.compile(
    r"(?P<cap>^" + _NPROC_CAP + r"(?=\n|$))"
    r"|(?P<space>[ \t\r]+)|(?P<comment>\#[^\n]*)"
    # Keep quotes on shell words so only double-quoted variables are accepted.
    r'''|(?P<word>(?:[^\s;&|()<>{}'"\\]+|\\[^\n]|"(?:\\.|[^"\\])*"|'[^']*')+)'''
    r"|(?P<separator>&&|\|\||[;\n&|()<>{}])|(?P<unknown>.)",
    re.MULTILINE,
)
_CMAKE_BUILD = re.compile(r"cmake[ \t]+--build")
_PARALLEL = re.compile(r"--parallel(?:=(.*))?")
_BOUNDED_CONSTANT = re.compile(r'''([A-Za-z_]\w*)=(?:[1-4]|"[1-4]"|'[1-4]')''')
_QUOTED_VARIABLE = re.compile(r'"\$(?:([A-Za-z_]\w*)|\{([A-Za-z_]\w*)\})"')
_ASSIGNMENT = re.compile(r"\b([A-Za-z_]\w*)\+?=")
_CONTROL_WORDS = {
    "if", "then", "elif", "else", "fi", "case", "esac", "for", "while",
    "until", "do", "done", "function", "!", "eval", "source", ".", "alias",
}


def _parallel_violations(words: list[str], bounded: set[str]) -> list[str]:
    """Return offending commands, retaining launcher support and word boundaries."""
    builds = [i for i in range(len(words) - 1)
              if _CMAKE_BUILD.fullmatch(" ".join(words[i:i + 2]))]
    violations = []
    for start, end in zip(builds, builds[1:] + [len(words)]):
        command = words[start:end]
        for index, word in enumerate(command):
            flag = _PARALLEL.fullmatch(word)
            if flag is None:
                continue
            argument = flag[1]
            if argument is None:
                argument = command[index + 1] if index + 1 < len(command) else ""
            variable = _QUOTED_VARIABLE.fullmatch(argument)
            if argument in {"1", "2", "3", "4"} or (
                variable and (variable[1] or variable[2]) in bounded
            ):
                continue
            violations.append(" ".join(command))
            break  # One actionable diagnostic per offending build command.
    return violations


def _join_shell_lines(script: str) -> str:
    """Join escaped newlines outside single quotes, respecting shell comments."""
    result = []
    quote = ""
    word_start = True
    index = 0
    while index < len(script):
        char = script[index]
        if char == "\\" and quote != "'":
            if script.startswith("\\\r\n", index):
                index += 3
                continue
            if script.startswith("\\\n", index):
                index += 2
                continue
            result.append(script[index:index + 2])
            index += 2
            word_start = False
            continue
        if not quote and char == "#" and word_start:
            # A backslash inside a comment cannot escape its terminating newline.
            index = script.find("\n", index)
            if index == -1:
                break
            continue
        if char in {"'", '"'}:
            if quote == char:
                quote = ""
            elif not quote:
                quote = char
            word_start = False
        elif not quote:
            word_start = char.isspace() or char in ";&|()<>{}"
        result.append(char)
        index += 1
    return "".join(result)


def _check_step_parallel(script: str) -> list[str]:
    # Ordinary newlines must never provide a missing --parallel argument.
    script = _join_shell_lines(script)
    bounded: set[str] = set()
    proofs_allowed = True
    words: list[str] = []
    violations = []

    def finish_command():
        nonlocal proofs_allowed
        if not words:
            return
        constant = _BOUNDED_CONSTANT.fullmatch(words[0]) if len(words) == 1 else None
        if constant and proofs_allowed:
            bounded.add(constant[1])
        else:
            # A conditional, function or sourced script needs shell analysis we do
            # not attempt. Never prove assignments inside/after such constructs.
            if words[0] in _CONTROL_WORDS:
                proofs_allowed = False
            for name in _ASSIGNMENT.findall(" ".join(words)):
                bounded.discard(name)
            # Arithmetic/parameter expansions can change the parent shell's vars.
            expansions = any(re.search(r"\$\(|`|\$\{(?![A-Za-z_]\w*\})", word) for word in words)
            build = any(_CMAKE_BUILD.fullmatch(" ".join(words[i:i + 2]))
                        for i in range(len(words) - 1))
            harmless = words[0] == "echo" or words == ["set", "-euo", "pipefail"] or build
            if not proofs_allowed or expansions or not harmless:
                bounded.clear()
            violations.extend(_parallel_violations(words, bounded))
        words.clear()

    for part in _SHELL_PARTS.finditer(script):
        kind = part.lastgroup
        if kind in {"space", "comment"}:
            continue
        if kind == "cap":
            if not words and proofs_allowed:
                bounded.add("jobs")
            else:
                bounded.clear()
                proofs_allowed = False
        elif kind == "word":
            words.append(part[0])
        else:
            finish_command()
            if kind == "unknown" or part[0] not in {"\n", ";"}:
                bounded.clear()
                proofs_allowed = False
    finish_command()
    return violations


def check_build_parallel(text: str, source: str = "<workflow>") -> list[str]:
    """Check literal cmake --build/--parallel commands in each YAML run step.

    Accept explicit 1-4 limits, or quoted variables proven by preceding simple
    constants/the exact guarded nproc block in the same step. Unsupported shell
    constructs cannot establish a variable bound; numeric limits still work.
    """
    try:
        workflow = yaml.safe_load(text)
    except yaml.YAMLError as error:
        return [f"{source}: invalid workflow YAML: {error}"]
    if not isinstance(workflow, dict) or not isinstance(workflow.get("jobs"), dict):
        return [f"{source}: expected a workflow with a jobs mapping"]
    errors = []
    for job_name, job in workflow["jobs"].items():
        if not isinstance(job, dict):
            continue
        for index, step in enumerate(job.get("steps", []), 1):
            if not isinstance(step, dict) or not isinstance(step.get("run"), str):
                continue
            name = step.get("name", step.get("id", f"step {index}"))
            for command in _check_step_parallel(step["run"]):
                errors.append(
                    f"{source}: job {job_name}, step {name}: {command}: "
                    "--parallel requires an explicit limit 1-4 or a quoted variable "
                    "with a proven preceding bound in this step"
                )
    return errors


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    problems = []
    problems.extend(check_perf(root / "bench" / "perf_config.yaml"))
    problems.extend(check_errors(root / "protocol" / "errors.yaml"))
    for name in ("ci.yml", "mesh.yml"):
        path = root / ".github" / "workflows" / name
        problems.extend(check_build_parallel(path.read_text(encoding="utf-8"), str(path)))
    if problems:
        print("config check failed:")
        for problem in problems:
            print(f"  {problem}")
        return 1
    print("config check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
