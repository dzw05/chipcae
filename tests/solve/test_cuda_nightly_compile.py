"""Offline CUDA nightly acceptance; real hosted CUDA builds remain mandatory.

The compile-fault cases build the real main.cpp object in Release mode using a
test-only MFEM Device header. They test macro wiring, not CUDA compatibility.
No test executes a solver or claims GPU validation.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


FAULT = "injected CUDA-branch compile fault"
CPU_DIAGNOSTIC = "built without CUDA-enabled MFEM"


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    print("$ " + " ".join(map(str, command)), flush=True)
    print((result.stdout + result.stderr).replace("\0", "\n"), end="", flush=True)
    return result


def registration_contract(source, compiler, generator):
    expected = {
        "solve_cuda_nightly_workflow", "solve_cuda_nightly_dependencies",
        "solve_cuda_nightly_cuda_default", "solve_cuda_nightly_cuda_injected",
        "solve_cuda_nightly_cpu_injected", "solve_cuda_nightly_no_mfem_injected",
    }
    with tempfile.TemporaryDirectory(prefix="solve-cuda-registration-") as temporary:
        cpu_tests = None
        for name, definitions, enabled in (
            ("cpu", [], False),
            ("cpu_off", ["-DMFEM_USE_CUDA=OFF", "-DCHIPCAE_CI_INJECT_CUDA_FAULT=OFF"], False),
            ("cuda", ["-DMFEM_USE_CUDA=ON"], True),
            ("injected", ["-DCHIPCAE_CI_INJECT_CUDA_FAULT=ON"], True),
        ):
            build = Path(temporary) / name
            command = [
                "cmake", "-S", str(source), "-B", str(build), "-G", generator,
                "-DCMAKE_BUILD_TYPE=Release", "-DCHIPCAE_BUILD_TESTS=ON",
                f"-DPython3_EXECUTABLE={sys.executable}", *definitions,
            ]
            if compiler:
                command.append(f"-DCMAKE_CXX_COMPILER={compiler}")
            configured = run(command)
            assert configured.returncode == 0, f"{name}: registration must configure"
            inventory = run([
                "ctest", "--test-dir", str(build), "--show-only=json-v1", "-R", "^solve_",
            ])
            assert inventory.returncode == 0, f"{name}: cannot list solver tests"
            tests = {test["name"] for test in json.loads(inventory.stdout)["tests"]}
            nightly = {test for test in tests if test.startswith("solve_cuda_nightly_")}
            assert nightly == (expected if enabled else set()), (
                f"{name}: unexpected CUDA nightly registrations: {sorted(nightly)}"
            )
            if cpu_tests is None:
                cpu_tests = tests
            assert tests - nightly == cpu_tests, f"{name}: existing solver tests changed"


def workflow_contract(source, compiler, generator):
    # PyYAML is installed by ci.yml for the existing contract tests as well.
    import yaml

    workflow = yaml.safe_load((source / ".github/workflows/nightly-mpi.yml").read_text())
    jobs = workflow["jobs"]
    assert "cuda-solve-compile" in jobs, (
        "nightly-mpi.yml has no cuda-solve-compile job; solver CUDA sources "
        "are not covered by the nightly entry point"
    )
    job = jobs["cuda-solve-compile"]
    assert "needs" not in job, "CUDA solver compilation must be independent"
    assert job["runs-on"] == "ubuntu-24.04", job["runs-on"]
    assert job["timeout-minutes"] == 150, job["timeout-minutes"]
    # PyYAML's YAML 1.1 resolver may decode the GitHub Actions key 'on' as True.
    triggers = workflow.get("on", workflow.get(True))
    assert set(triggers) == {"schedule", "workflow_dispatch"}, triggers
    assert triggers["schedule"] == [{"cron": "0 18 * * 0-4"}], triggers
    dispatch = triggers["workflow_dispatch"] or {}
    inputs = dispatch.get("inputs", {})
    assert "inject_cuda_fault" in inputs, "dispatch must expose fault injection"
    fault = inputs["inject_cuda_fault"]
    assert fault.get("type") == "boolean", fault
    assert fault.get("default") is False, fault
    assert fault.get("required", False) is False, fault
    assert job.get("continue-on-error", False) is False, job
    steps = job.get("steps", [])
    assert steps, "cuda-solve-compile must have executable steps"
    for step in steps:
        assert step.get("continue-on-error", False) is False, step
        if "run" in step:
            checked = run(["bash", "-n"], input=step["run"])
            assert checked.returncode == 0, "invalid CUDA job shell syntax"
    print("CUDA solver nightly entry point and default-false dispatch accepted")
    registration_contract(source, compiler, generator)


def lock_values(path):
    assert path.is_file(), f"missing CUDA dependency lock: {path}"
    keys = [f"{dependency}_{field}"
            for dependency in ("METIS", "HYPRE", "HDF5", "JSONCONS", "MFEM", "PETSC")
            for field in ("VERSION", "URL")]
    keys += ["HDF5_TAG", "PETSC_SHA256"]
    # Source the real shell interface, accepting either copied or sourced CPU pins.
    # Emit only the named fields, never the inherited process environment.
    checked = run([
        "bash", "-c",
        'set -euo pipefail; lock=$1; shift; '
        'for key in "$@"; do unset "$key"; done; source "$lock"; '
        'for key in "$@"; do printf "%s=%s\\0" "$key" "${!key-}"; done',
        "cuda-lock-test", str(path), *keys,
    ])
    assert checked.returncode == 0, f"cannot source {path}"
    return dict(item.split("=", 1) for item in checked.stdout.split("\0") if "=" in item)


def dependency_contract(source):
    ci = source / "apps/chipcae-solve/ci"
    cpu = lock_values(source / "tests/integration/mesh_p1/ci/deps.lock")
    cuda = lock_values(ci / "deps_cuda.lock")
    for dependency in ("METIS", "HYPRE", "HDF5", "JSONCONS", "MFEM"):
        for field in ("VERSION", "URL"):
            key = f"{dependency}_{field}"
            assert cpu.get(key), f"CPU lock has an empty {key}"
            assert cuda.get(key), f"CUDA lock has an empty {key}"
            assert cuda[key] == cpu[key], f"{key}: CUDA={cuda[key]!r}, CPU={cpu[key]!r}"
    assert cuda.get("HDF5_TAG") == cpu["HDF5_TAG"], "HDF5 tag drift"
    assert cuda.get("PETSC_VERSION") == "3.26.1", "PETSc must stay at 3.26.1"
    assert cuda.get("PETSC_URL") == (
        "https://web.cels.anl.gov/projects/petsc/download/release-snapshots/"
        "petsc-3.26.1.tar.gz"
    ), "PETSc release URL drift"
    assert re.fullmatch(r"[0-9a-fA-F]{64}", cuda.get("PETSC_SHA256", "")), (
        "PETSC_SHA256 must pin the downloaded archive"
    )
    script = ci / "build_deps_cuda.sh"
    assert script.is_file(), f"missing CUDA dependency builder: {script}"
    checked = run(["bash", "-n", str(script)])
    assert checked.returncode == 0, "invalid CUDA dependency builder shell syntax"


def compile_fault_contract(source, compiler, generator, scenario):
    cuda, mfem, inject = {
        "cuda_default": (True, True, False),
        "cuda_injected": (True, True, True),
        "cpu_injected": (False, True, True),
        "no_mfem_injected": (True, False, True),
    }[scenario]
    with tempfile.TemporaryDirectory(prefix="solve-cuda-fault-") as temporary:
        build = Path(temporary) / "build"
        definitions = []
        if mfem:
            definitions.append("CHIPCAE_SOLVE_MFEM=1")
        if cuda:
            definitions.append("TEST_MFEM_CUDA=1" if mfem else "MFEM_USE_CUDA=1")
        if inject:
            definitions.append("CHIPCAE_CI_INJECT_CUDA_FAULT=1")
        configured = run([
            "cmake", "-S", str(source / "tests/solve/cuda_fault"),
            "-B", str(build), "-G", generator, "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_CXX_COMPILER={compiler}", f"-DCHIPCAE_SOURCE_DIR={source}",
            "-DFAULT_DEFINITIONS=" + ";".join(definitions),
        ])
        assert configured.returncode == 0, "fault acceptance harness must configure"
        built = run([
            "cmake", "--build", str(build), "--parallel", "2",
            "--target", "solve_cuda_fault",
        ])
        if cuda and mfem and inject:
            assert built.returncode != 0, (
                "CUDA main.cpp compiled successfully with CHIPCAE_CI_INJECT_CUDA_FAULT; "
                "the CUDA-branch compile fault is missing"
            )
            assert FAULT in built.stdout + built.stderr, (
                "injected build failed for an unrelated reason; expected " + FAULT
            )
        else:
            assert built.returncode == 0, f"{scenario} must compile without an injected fault"
            objects = list(build.rglob("main.cpp.o"))
            assert len(objects) == 1 and objects[0].stat().st_size > 0, objects
            strings = run(["strings", str(objects[0])])
            assert strings.returncode == 0, "could not inspect the real main.cpp object"
            assert (CPU_DIAGNOSTIC in strings.stdout) == (not (cuda and mfem)), (
                f"{scenario}: unexpected non-CUDA diagnostic in compiled object"
            )
        print(f"accepted compile-fault scenario: {scenario}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("case", choices=(
        "workflow", "dependencies", "cuda_default", "cuda_injected",
        "cpu_injected", "no_mfem_injected",
    ))
    parser.add_argument("--compiler")
    parser.add_argument("--generator", default="Ninja")
    args = parser.parse_args()
    if args.case == "workflow":
        workflow_contract(args.source, args.compiler, args.generator)
    elif args.case == "dependencies":
        dependency_contract(args.source)
    else:
        assert args.compiler, "the parent CMake compiler is required"
        compile_fault_contract(args.source, args.compiler, args.generator, args.case)


if __name__ == "__main__":
    main()
