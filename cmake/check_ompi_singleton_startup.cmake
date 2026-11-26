# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

if(NOT CHECK)
    if(NOT DEFINED ITERATIONS)
        set(ITERATIONS 200)
    endif()
    if(NOT ITERATIONS MATCHES "^[0-9]+$" OR ITERATIONS LESS 200)
        message(FATAL_ERROR "ITERATIONS must be an integer >= 200")
    endif()
    if(NOT DEFINED PROBE OR PROBE STREQUAL "")
        message(FATAL_ERROR "PROBE must name an executable")
    endif()
endif()
if(NOT PYTHON)
    message(FATAL_ERROR "PYTHON must name the interpreter located by the test module")
endif()

# Pass the runner via -c: no generated source in the repository or shared file
# name. Probe output goes to files, since PRRTE descendants may inherit pipes.
set(_runner [==[
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


def terminate_group(child):
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        child.wait(timeout=0.5)
    except subprocess.TimeoutExpired:
        pass
    # The parent may have exited while a PRRTE descendant still owns output.
    try:
        os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    child.wait()


def launches(probe, iterations):
    maximum = 0.0
    completed = nonzero = threshold = timeouts = 0
    for index in range(1, iterations + 1):
        with tempfile.TemporaryFile() as output:
            begin = time.perf_counter()
            try:
                child = subprocess.Popen([probe], stdout=output, stderr=output,
                                         start_new_session=True)
            except OSError as error:
                elapsed = time.perf_counter() - begin
                maximum = max(maximum, elapsed)
                print(f"iteration={index} elapsed={elapsed:.6f} "
                      f"s exit=launch-error: {error}", flush=True)
                nonzero += 1
                break
            timed_out = False
            try:
                status = child.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                timed_out = True
                terminate_group(child)
                status = child.returncode
            except BaseException:
                terminate_group(child)
                raise
            elapsed = time.perf_counter() - begin
            maximum = max(maximum, elapsed)
            completed += 1
            threshold += elapsed > 10.0
            timeouts += timed_out
            nonzero += status != 0
            print(f"iteration={index} elapsed={elapsed:.6f} s exit={status} "
                  f"timeout={int(timed_out)}", flush=True)
            # Read the regular file's current contents; never wait for EOF on
            # a descendant-owned capture pipe. Bound diagnostics to 64 KiB.
            output.seek(0)
            diagnostic = output.read(65536).decode(errors="replace")
            if diagnostic:
                print(diagnostic, end="" if diagnostic.endswith("\n") else "\n", flush=True)
            if timed_out or elapsed > 10.0 or status != 0:
                break
    print(f"summary requested={iterations} completed={completed} maximum={maximum:.6f} s "
          f"nonzero={nonzero} threshold_failures={threshold} timeouts={timeouts}", flush=True)
    return int(completed != iterations or nonzero != 0 or threshold != 0 or timeouts != 0)


def driver_check(case, script, python):
    with tempfile.TemporaryDirectory(prefix="chipcae-singleton-check-") as directory:
        root = Path(directory)
        probe = root / "controlled-probe"
        marker = root / "launches"
        body = (f"#!{python}\nfrom pathlib import Path\nimport time\n"
                f"with Path({str(marker)!r}).open('a') as log: log.write('launch\\n')\n")
        if case == "sleep":
            body += "time.sleep(30)\n"
        elif case == "exit7":
            body += "raise SystemExit(7)\n"
        probe.write_text(body)
        probe.chmod(0o700)
        if case == "missing":
            probe.unlink()
        count = 199 if case == "iterations" else 200
        command = ["cmake", f"-DPROBE={probe}", f"-DPYTHON={python}",
                   f"-DITERATIONS={count}", "-P", script]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=25)
        print(result.stdout, end="", flush=True)
        assert result.returncode != 0, f"driver accepted controlled {case} failure"
        expected = {"sleep": "threshold_failures=1 timeouts=1", "exit7": "exit=7",
                    "missing": "exit=launch-error", "iterations": "integer >= 200"}[case]
        assert expected in result.stdout, f"wrong failure for {case}: expected {expected!r}"
        actual = marker.read_text().splitlines() if marker.exists() else []
        assert len(actual) == (1 if case in ("sleep", "exit7") else 0), "driver retried/launched invalid input"
        if case in ("sleep", "exit7"):
            assert "completed=1 " in result.stdout, "driver did not fail immediately"
        print(f"controlled {case}: expected rejection verified")


def configuration_check(build):
    result = subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1"],
                            check=True, stdout=subprocess.PIPE, text=True)
    entries = json.loads(result.stdout)["tests"]
    matches = [test for test in entries if test["name"] == "infra_ompi_singleton_startup"]
    assert len(matches) == 1, "acceptance must appear exactly once"
    tests = {test["name"]: test for test in entries}
    properties = lambda test: {item["name"]: item["value"] for item in test["properties"]}
    acceptance = properties(matches[0])
    assert acceptance["TIMEOUT"] == 2400, "acceptance timeout must be 2400"
    assert acceptance["RUN_SERIAL"], "acceptance must run serially"
    assert set(acceptance["LABELS"]) == {"infra", "acceptance", "owner:INFRA"}
    assert "SKIP_REGULAR_EXPRESSION" not in acceptance, "Open MPI startup failures must fail"
    command = matches[0]["command"]
    assert any(arg.startswith("-DPROBE=") for arg in command), "acceptance must directly exec the probe"

    # Hand-audited direct callers. mesh_digest_mpi and mpi_surface also exec a
    # baseline preview without a launcher. Label-only selection misses golden.
    selected = ("mesh_digest_golden", "mesh_digest_mpi", "k1_preview",
                "k1_preview_mpi_surface", "preview_zalign", "k1_results_validation",
                "k1_vtkhdf", "k1_patch_gpu", "k1_cut_mfem_gpu",
                "solve_cut_manufactured", "solve_cut_output_eb_layer",
                "solve_ghost_penalty_rank_guard", "solve_cut_bilayer",
                "solve_cut_bilayer_gamma", "solve_cut_convection",
                "solve_pipeline", "solve_preview_mesh_digest", "solve_ghost_scan")
    if "mesh_digest_golden" in tests:
        golden = tests["mesh_digest_golden"]
        props = properties(golden)
        assert props["TIMEOUT"] == 120, "mesh_digest_golden timeout changed"
        assert set(props["LABELS"]) == {"mesh", "tierA", "preview", "module", "owner:M2"}
        assert len(golden["command"]) == 3
        assert Path(golden["command"][1]).name == "mesh_digest.py"
        assert Path(golden["command"][2]).name == "chipcae-preview"
    assert properties(tests["core_types"])["ENVIRONMENT"] == ["OMP_NUM_THREADS=2", "OMP_PROC_BIND=false"]
    # Do not prescribe which candidate wins. Require the chosen MCA protection
    # to reach the existing direct paths as well as the new acceptance probe.
    def mca(props):
        values = {}
        for entry in props.get("ENVIRONMENT", []):
            name, value = entry.split("=", 1)
            if re.match(r"(?:PMIX|PRTE|OMPI)_MCA_", name):
                values[name] = value
        for entry in props.get("ENVIRONMENT_MODIFICATION", []):
            name, operation = entry.split("=", 1)
            if re.match(r"(?:PMIX|PRTE|OMPI)_MCA_", name):
                assert operation.startswith("set:"), f"{name}: component selection must not concatenate values"
                values[name] = operation[4:]
        return values

    protection = mca(acceptance)
    assert protection, "missing singleton startup protection: acceptance has no test-local MCA environment"
    for name in selected:
        if name in tests:
            settings = mca(properties(tests[name]))
            assert protection.keys() <= settings.keys(), f"{name}: direct singleton path lacks probe protection"
            for key, value in protection.items():
                if Path(value).is_absolute():
                    # Session-directory isolation may deliberately use a
                    # different absolute directory for each test.
                    assert Path(settings[key]).is_absolute(), f"{name}: {key} lacks an absolute session directory"
                else:
                    assert settings[key] == value, f"{name}: {key} disagrees with probe protection"
    for name in ("core_types", "k1_background", "k1_mesh_digest", "k1_cut"):
        if name in tests:
            assert not mca(properties(tests[name])), f"{name}: unrelated test received MCA injection"
    print("singleton registration, direct-path protection and existing test properties verified")


def gating_check(case, script):
    module = Path(script).with_name("ChipcaeMpiSingleton.cmake").as_posix()
    with tempfile.TemporaryDirectory(prefix="chipcae-singleton-gating-") as directory:
        root = Path(directory)
        # A configured non-Open-MPI header proves detection follows the linked
        # component even when ompi_info/mpiexec on PATH belong to Open MPI.
        (root / "mpi.h").write_text("#define MPI_VERSION 4\n")
        enabled = "OFF" if case == "disabled" else "ON"
        testing = "OFF" if case == "tests_off" else "ON"
        (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(singleton_gating LANGUAGES CXX)
enable_testing()
set(CHIPCAE_BUILD_TESTS {testing})
set(CHIPCAE_ENABLE_MPI {enabled})
add_library(MPI::MPI_CXX INTERFACE IMPORTED)
set_property(TARGET MPI::MPI_CXX PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${{CMAKE_CURRENT_SOURCE_DIR}}")
include("{module}")
include("{module}")
if(TARGET chipcae_ompi_singleton_probe OR TARGET MPI::MPI_C)
    message(FATAL_ERROR "disabled/non-Open-MPI configuration created a probe")
endif()
''')
        build = root / "build"
        configured = subprocess.run(["cmake", "-G", "Ninja", "-S", str(root), "-B", str(build)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(configured.stdout, end="", flush=True)
        assert configured.returncode == 0, "gating fixture failed to configure"
        result = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                                check=True, stdout=subprocess.PIPE, text=True)
        tests = json.loads(result.stdout)["tests"]
        if case == "tests_off":
            assert tests == [], "tests-disabled build registered singleton tests"
        else:
            assert len(tests) == 1, "skip diagnostic must appear exactly once"
            props = {item["name"]: item["value"] for item in tests[0]["properties"]}
            assert not props.get("ENVIRONMENT"), "non-Open-MPI build injected environment"
            assert not props.get("ENVIRONMENT_MODIFICATION"), "non-Open-MPI build modified environment"
            assert all("-DPROBE=" not in arg for arg in tests[0]["command"]), "skip would launch probe"
            skipped = subprocess.run(["ctest", "--test-dir", str(build),
                                      "-R", "^infra_ompi_singleton_startup$", "-V"],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(skipped.stdout, end="", flush=True)
            assert skipped.returncode == 0
            assert "本构建不是 Open MPI" in skipped.stdout and "***Skipped" in skipped.stdout
        print(f"{case}: zero probe launches and correct registration verified")


mode, script, python, probe, iterations, build = sys.argv[1:]
if mode in ("sleep", "exit7", "missing", "iterations"):
    driver_check(mode, script, python)
elif mode == "configuration":
    configuration_check(build)
elif mode in ("disabled", "non_open_mpi", "tests_off"):
    gating_check(mode, script)
elif mode:
    raise SystemExit(f"unknown check: {mode}")
else:
    raise SystemExit(launches(probe, int(iterations)))
]==])

execute_process(COMMAND "${PYTHON}" -c "${_runner}"
    "${CHECK}" "${CMAKE_CURRENT_LIST_FILE}" "${PYTHON}" "${PROBE}" "${ITERATIONS}" "${BUILD_DIR}"
    RESULT_VARIABLE _result)
if(NOT _result STREQUAL "0")
    message(FATAL_ERROR "Open MPI singleton startup check failed (exit=${_result})")
endif()
