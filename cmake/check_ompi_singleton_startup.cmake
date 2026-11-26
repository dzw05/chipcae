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
        descendant = root / "descendant.pid"
        body = (f"#!{python}\nfrom pathlib import Path\nimport os, subprocess, time\n"
                f"with Path({str(marker)!r}).open('a') as log: log.write('launch\\n')\n")
        if case in ("sleep", "descendant_exit"):
            # Own the same process group and output descriptors as the probe.
            # Ignore TERM so the check also exercises the driver's KILL sweep.
            worker = ("import os, signal, time\nfrom pathlib import Path\n"
                      "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                      f"Path({str(descendant)!r}).write_text(str(os.getpid()))\n"
                      "time.sleep(60)\n")
            body += (f"subprocess.Popen([{python!r}, '-c', {worker!r}])\n"
                     f"while not Path({str(descendant)!r}).exists(): time.sleep(0.01)\n")
            body += "time.sleep(30)\n" if case == "sleep" else "raise SystemExit(7)\n"
        elif case == "exit7":
            body += "raise SystemExit(7)\n"
        elif case == "environment":
            body += ("assert os.environ['CHIPCAE_SINGLETON_SENTINEL'] == 'child inherits spaces=ok'\n"
                     "print('inherited sentinel verified')\n")
        probe.write_text(body)
        probe.chmod(0o700)
        if case == "missing":
            probe.unlink()
        count = 199 if case == "iterations" else 200
        command = ["cmake", f"-DPROBE={probe}", f"-DPYTHON={python}"]
        # The success case must exercise the default, not a supplied N=200.
        if case != "success":
            command.append(f"-DITERATIONS={count}")
        command += ["-P", script]
        environment = dict(os.environ, CHIPCAE_SINGLETON_SENTINEL="child inherits spaces=ok")
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=30, env=environment)
            print(result.stdout, end="", flush=True)
            actual = marker.read_text().splitlines() if marker.exists() else []
            records = re.findall(r"^iteration=(\d+) elapsed=([0-9]+\.[0-9]{2,}) s "
                                 r"exit=(-?\d+) timeout=(\d+)$", result.stdout, re.M)
            if case in ("success", "environment"):
                assert result.returncode == 0, "driver rejected 200 successful fresh processes"
                assert actual == ["launch"] * 200, "default must launch exactly 200 fresh processes"
                assert [int(row[0]) for row in records] == list(range(1, 201)), "missing timing records"
                assert all(float(row[1]) <= 10.0 and row[2:] == ('0', '0') for row in records)
                summary = re.search(r"summary requested=200 completed=200 maximum=([0-9.]+) s "
                                    r"nonzero=0 threshold_failures=0 timeouts=0", result.stdout)
                assert summary, "success must report exact counts and a maximum"
                assert abs(float(summary[1]) - max(float(row[1]) for row in records)) <= 1e-6
                if case == "environment":
                    assert result.stdout.count("inherited sentinel verified") == 200
            else:
                assert result.returncode != 0, f"driver accepted controlled {case} failure"
                expected = {"sleep": "threshold_failures=1 timeouts=1", "exit7": "exit=7",
                            "descendant_exit": "exit=7", "missing": "exit=launch-error",
                            "iterations": "integer >= 200"}[case]
                assert expected in result.stdout, f"wrong failure for {case}: expected {expected!r}"
                launched = case in ("sleep", "exit7", "descendant_exit")
                assert actual == ["launch"] * int(launched), "driver retried/launched invalid input"
                if launched:
                    assert len(records) == 1 and "completed=1 " in result.stdout
                elif case == "missing":
                    assert "completed=0 " in result.stdout
            if case in ("sleep", "descendant_exit"):
                assert descendant.exists(), "controlled descendant never started"
                pid = int(descendant.read_text())
                # Allow kernel reaping time; zombies are terminated, not survivors.
                deadline = time.perf_counter() + 1.0
                while process_alive(pid) and time.perf_counter() < deadline:
                    time.sleep(0.02)
                assert not process_alive(pid), f"driver left controlled descendant {pid} alive"
        finally:
            # A red cleanup assertion must not itself leak the controlled process.
            if descendant.exists():
                try:
                    os.kill(int(descendant.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        print(f"controlled {case}: launch counts, timings and process cleanup verified")


def process_alive(pid):
    result = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)],
                            stdout=subprocess.PIPE, text=True, check=False)
    return result.returncode == 0 and bool(result.stdout.strip()) and not result.stdout.lstrip().startswith("Z")


def test_properties(test):
    return {item["name"]: item["value"] for item in test["properties"]}


def registered_tests(build):
    result = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                            check=True, stdout=subprocess.PIPE, text=True)
    return json.loads(result.stdout)["tests"]


def mca_environment(props):
    values = {}
    for entry in props.get("ENVIRONMENT", []):
        name, value = entry.split("=", 1)
        if re.match(r"(?:PMIX|PRTE|OMPI)_MCA_", name):
            assert name not in values, f"{name}: duplicate MCA environment assignment"
            values[name] = value
    for entry in props.get("ENVIRONMENT_MODIFICATION", []):
        name, operation = entry.split("=", 1)
        if re.match(r"(?:PMIX|PRTE|OMPI)_MCA_", name):
            assert name not in values, f"{name}: duplicate MCA environment modification"
            assert operation.startswith("set:"), f"{name}: component selection must not concatenate values"
            values[name] = operation[4:]
    return values


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
                "solve_pipeline", "solve_preview_mesh_digest", "solve_ghost_scan",
                # main.cpp constructs MpiSession before CLI validation, even
                # without the numerical backend. These have no MPI label.
                "solve_inputs", "solve_profile_cuda_unavailable",
                "solve_profile_cuda_gpus_zero", "solve_profile_cpu_gpus_nonzero",
                "solve_cut_pipeline", "solve_demo", "solve_copper_trace_map_paths",
                "solve_fvm_cut_vertex_recon", "solve_lu_refine_floor", "solve_fvm_trace_map",
                "preview_outline_chamfer_differs", "preview_outline_volume",
                "preview_solve_outline_digest", "preview_outline_nonconvex_rejected",
                "preview_outline_external_polyset_rejected")
    # The parts harness initializes MPI, including the direct np=1 path. The
    # auto-registered integration wrappers obtain the CLI through ENVIRONMENT.
    selected += tuple(name for name, test in tests.items()
                      if name.startswith("solve_parts_") or
                      any(entry.startswith(("CHIPCAE_SOLVE_EXE=", "CHIPCAE_PREVIEW_EXE="))
                          for entry in properties(test).get("ENVIRONMENT", [])))
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
    protection = mca_environment(acceptance)
    assert protection, "missing singleton startup protection: acceptance has no test-local MCA environment"
    for name in selected:
        if name in tests:
            settings = mca_environment(properties(tests[name]))
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
            assert not mca_environment(properties(tests[name])), f"{name}: unrelated test received MCA injection"
    print("singleton registration, direct-path protection and existing test properties verified")


def module_check(case, script, python, real_build):
    # Exercise the real module with the linked installation, not fabricated
    # Open MPI detection/cache results. Controlled callers avoid product fixtures.
    cache = Path(real_build, "CMakeCache.txt").read_text()
    options = []
    for key in ("MPI_C_COMPILER", "MPI_CXX_COMPILER", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"):
        match = re.search(rf"^{key}:[^=]+=(.+)$", cache, re.M)
        assert match, f"configured installation lacks {key}"
        options.append(f"-D{key}={match[1]}")
    module = Path(script).with_name("ChipcaeMpiSingleton.cmake").as_posix()
    groups = {
        "tests/integration/mesh_p1": ("mesh_digest_golden", "mesh_digest_mpi", "k1_preview",
                                      "k1_preview_mpi_surface", "k1_vtkhdf", "k1_results_validation"),
        "tests/solve": ("solve_inputs", "solve_profile_cuda_unavailable",
                        "solve_profile_cuda_gpus_zero", "solve_profile_cpu_gpus_nonzero",
                        "solve_pipeline", "solve_cut_manufactured", "solve_cut_pipeline"),
        "tests/preview": ("preview_zalign",),
    }
    selected = tuple(name for names in groups.values() for name in names)
    unrelated = ("core_types", "k1_background", "k1_mesh_digest", "k1_cut", "solve_mesh")
    literal = lambda value: "[=[" + str(value) + "]=]"
    with tempfile.TemporaryDirectory(prefix="chipcae-singleton-module-") as directory:
        root = Path(directory)
        recorder = root / "record.py"
        recorder.write_text('''import json, os, subprocess, sys
from pathlib import Path
if sys.argv[1] == '--wrapper':
    subprocess.run([sys.executable, __file__, *sys.argv[2:]], check=True)
    print('controlled wrapper completed')
else:
    Path(sys.argv[1]).write_text(json.dumps(dict(os.environ)))
''')
        expected_commands = {}
        for relative, names in {**groups, ".": unrelated}.items():
            source = root / relative
            source.mkdir(parents=True, exist_ok=True)
            definitions = []
            for name in names:
                command = [python, str(recorder), "--wrapper", str(root / (name + ".json")),
                           name, "space = token"]
                expected_commands[name] = command
                definitions.append(f"add_test(NAME {name} COMMAND " +
                                   " ".join(map(literal, command)) + ")")
                labels = "mesh;tierA;preview;module;owner:M2" if name == "mesh_digest_golden" else "fixture"
                definitions.append(f'''set_tests_properties({name} PROPERTIES
    TIMEOUT 120 PROCESSORS 2 LABELS "{labels}"
    PASS_REGULAR_EXPRESSION "controlled wrapper completed"
    FAIL_REGULAR_EXPRESSION "controlled wrapper failure"
    ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false"
    ENVIRONMENT_MODIFICATION "CHIPCAE_PRESERVED=set:kept")''')
                if name == "mesh_digest_golden":
                    definitions.append('''if(DEFINED FIXTURE_ENV)
    set_property(TEST mesh_digest_golden APPEND PROPERTY ENVIRONMENT "${FIXTURE_ENV}" "${FIXTURE_ENV}")
endif()
if(DEFINED FIXTURE_MOD)
    set_property(TEST mesh_digest_golden APPEND PROPERTY ENVIRONMENT_MODIFICATION "${FIXTURE_MOD}")
endif()''')
            (source / "CMakeLists.txt").write_text("\n".join(definitions) + "\n")
        (root / "tests/CMakeLists.txt").write_text(
            "add_subdirectory(integration)\nadd_subdirectory(solve)\nadd_subdirectory(preview)\n")
        (root / "tests/integration/CMakeLists.txt").write_text("add_subdirectory(mesh_p1)\n")
        root_definitions = (root / "CMakeLists.txt").read_text()
        (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(singleton_module_acceptance LANGUAGES C CXX)
enable_testing()
set(CHIPCAE_BUILD_TESTS ON)
set(CHIPCAE_ENABLE_MPI ON)
find_package(MPI REQUIRED COMPONENTS C CXX)
function(chipcae_set_warnings)
endfunction()
{root_definitions}
add_subdirectory(tests)
include("{module}")
include("{module}")
''')
        build = root / "build"

        def configure(destination, extra=(), environment=None):
            result = subprocess.run(["cmake", "-G", "Ninja", "-S", str(root), "-B", str(destination),
                                     "-DCMAKE_BUILD_TYPE=Release", f"-DPython3_EXECUTABLE={python}",
                                     *options, *extra], env=environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(result.stdout, end="", flush=True)
            return result

        configured = configure(build)
        assert configured.returncode == 0, "real Open MPI fixture failed to configure"
        entries = registered_tests(build)
        acceptance = [test for test in entries if test["name"] == "infra_ompi_singleton_startup"]
        assert len(acceptance) == 1, "including module twice duplicated acceptance registration"
        tests = {test["name"]: test for test in entries}
        for name in (*selected, *unrelated):
            test = tests[name]
            props = test_properties(test)
            assert test["command"] == expected_commands[name], f"{name}: existing command/arguments changed"
            assert props["TIMEOUT"] == 120 and props["PROCESSORS"] == 2
            assert props["PASS_REGULAR_EXPRESSION"] == ["controlled wrapper completed"]
            assert props["FAIL_REGULAR_EXPRESSION"] == ["controlled wrapper failure"]
            expected_labels = {"mesh", "tierA", "preview", "module", "owner:M2"} if name == "mesh_digest_golden" else {"fixture"}
            assert set(props["LABELS"]) == expected_labels, f"{name}: labels changed"
            assert "OMP_NUM_THREADS=2" in props["ENVIRONMENT"] and "OMP_PROC_BIND=false" in props["ENVIRONMENT"]
            assert "CHIPCAE_PRESERVED=set:kept" in props["ENVIRONMENT_MODIFICATION"]
            if name in unrelated:
                assert not mca_environment(props), f"{name}: unrelated test received protection"
        protection = mca_environment(test_properties(acceptance[0]))
        assert protection, "missing singleton startup protection: cannot validate module environment contract"

        if case.startswith("conflict_") or case == "matching":
            # Discover the independently selected candidate rather than locking
            # the tests to hash or another unvalidated switch. Component values
            # and per-test session paths are both supported.
            settings = mca_environment(test_properties(tests["mesh_digest_golden"]))
            assert protection.keys() <= settings.keys(), "golden lacks acceptance protection"
            key = next(iter(protection))
            value = settings[key]
            conflict = "chipcae-controlled-conflicting-value"
            extra = []
            environment = dict(os.environ)
            if case == "matching":
                extra = [f"-DFIXTURE_ENV={key}={value}", f"-DFIXTURE_MOD={key}=set:{value}"]
                # A global session path cannot match deliberately distinct
                # per-test paths. Matching inheritance applies to shared values.
                if not Path(value).is_absolute():
                    environment[key] = value
            elif case == "conflict_inherited":
                environment[key] = conflict
            elif case == "conflict_environment":
                extra = [f"-DFIXTURE_ENV={key}={conflict}"]
            elif case == "conflict_modification":
                extra = [f"-DFIXTURE_MOD={key}=set:{conflict}"]
            else:
                extra = [f"-DFIXTURE_MOD={key}=unset:"]
            result = configure(build, extra, environment)
            if case != "matching":
                assert result.returncode != 0, f"silently accepted {case}: {key}"
                assert key in result.stdout, "conflict diagnostic must identify the MCA name"
                assert re.search(r"conflict|incompatible|冲突", result.stdout, re.I), "no explicit conflict diagnostic"
                if case in ("conflict_environment", "conflict_modification", "conflict_unset"):
                    assert "mesh_digest_golden" in result.stdout, "diagnostic must identify the affected test"
                if case != "conflict_unset":
                    assert conflict in result.stdout, "diagnostic must preserve the conflicting value"
                print(f"{case}: explicit MCA conflict diagnostic verified")
                return
            assert result.returncode == 0, "matching MCA assignments must not conflict"
            tests = {test["name"]: test for test in registered_tests(build)}
            # The combined reader rejects duplicate ENVIRONMENT entries and
            # repeated assignments across ENVIRONMENT / MODIFICATION.
            assert mca_environment(test_properties(tests["mesh_digest_golden"]))[key] == value

        session_paths = set()
        for name in (*selected, "infra_ompi_singleton_startup"):
            props = test_properties(tests[name])
            settings = mca_environment(props)
            assert protection.keys() <= settings.keys(), f"{name}: direct singleton path lacks probe protection"
            assert all(any(entry == f"{key}={value}" for entry in props.get("ENVIRONMENT", []))
                       for key, value in settings.items()), f"{name}: protection must be test-local ENVIRONMENT"
            for key, value in protection.items():
                if Path(value).is_absolute():
                    path = Path(settings[key])
                    assert path.is_absolute() and build.resolve() in path.resolve().parents
                    assert path.is_dir() and os.access(path, os.R_OK | os.W_OK | os.X_OK)
                    assert path.stat().st_mode & 0o700 == 0o700, "session directory lacks owner permissions"
                    token = path / "controlled-write-check"
                    token.write_text("writable")
                    token.unlink()
                    session_paths.add(path.resolve())
                else:
                    assert settings[key] == value, f"{name}: candidate disagrees with acceptance"
            assert name in configured.stdout, f"configure must report affected test {name}"
        for key, value in protection.items():
            assert key in configured.stdout and value in configured.stdout, "configure must report selected protection"
        if session_paths:
            absolute_keys = [key for key, value in protection.items() if Path(value).is_absolute()]
            for key in absolute_keys:
                paths = {mca_environment(test_properties(tests[name]))[key]
                         for name in (*selected, "infra_ompi_singleton_startup")}
                assert len(paths) == len(selected) + 1, f"{key}: test session directories are shared"
            second = root / "another-build"
            assert configure(second).returncode == 0
            other = {test["name"]: test for test in registered_tests(second)}
            other_paths = {Path(mca_environment(test_properties(other[name]))[key]).resolve()
                           for name in (*selected, "infra_ompi_singleton_startup") for key in absolute_keys}
            assert session_paths.isdisjoint(other_paths), "concurrent builds share session directories"
        if case == "propagation":
            result = subprocess.run(["ctest", "--test-dir", str(build), "-R",
                                     "^(mesh_digest_golden|mesh_digest_mpi|solve_inputs)$", "-V"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(result.stdout, end="", flush=True)
            assert result.returncode == 0, "controlled direct/MPI wrapper failed"
            for name in ("mesh_digest_golden", "mesh_digest_mpi", "solve_inputs"):
                observed = json.loads((root / (name + ".json")).read_text())
                for key, value in mca_environment(test_properties(tests[name])).items():
                    assert observed.get(key) == value, f"{name}: child did not inherit {key}"
                assert observed["OMP_NUM_THREADS"] == "2" and observed["CHIPCAE_PRESERVED"] == "kept"
        print(f"{case}: recursive selection, duplicate include and environment contract verified")
    assert not root.exists(), "controlled build/session directories were not cleaned up"


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
if mode in ("sleep", "exit7", "missing", "iterations", "success", "environment", "descendant_exit"):
    driver_check(mode, script, python)
elif mode in ("selection", "propagation", "matching", "conflict_inherited", "conflict_environment",
              "conflict_modification", "conflict_unset"):
    module_check(mode, script, python, build)
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
