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
import math
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
    completed = nonzero = threshold = timeouts = invalid_reports = 0
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
                # Popen time is part of the fixed startup-through-exit budget.
                remaining = max(0.0, 10.0 - (time.perf_counter() - begin))
                status = child.wait(timeout=remaining)
                elapsed = time.perf_counter() - begin
            except subprocess.TimeoutExpired:
                timed_out = True
                # Record the observed deadline overrun before cleanup. Never
                # clamp a scheduling delay to 10 s or charge the kill grace
                # period to startup. Timeout is also an independent failure.
                elapsed = time.perf_counter() - begin
                terminate_group(child)
                status = child.returncode
            except BaseException:
                terminate_group(child)
                raise
            if not timed_out and (elapsed > 10.0 or status != 0):
                # A failed parent can exit normally while descendants keep
                # running and retaining the regular-file capture descriptors.
                terminate_group(child)
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
            valid_report = True
            if not timed_out and status == 0:
                reports = re.findall(r"^probe init/finalize=([^ ]+) s$", diagnostic, re.M)
                try:
                    valid_report = len(reports) == 1 and math.isfinite(float(reports[0])) and float(reports[0]) >= 0
                except ValueError:
                    valid_report = False
                if not valid_report:
                    invalid_reports += 1
                    print(f"iteration={index}: invalid probe init/finalize report", flush=True)
            if timed_out or elapsed > 10.0 or status != 0 or not valid_report:
                break
    print(f"summary requested={iterations} completed={completed} maximum={maximum:.6f} s "
          f"nonzero={nonzero} threshold_failures={threshold} timeouts={timeouts} "
          f"invalid_reports={invalid_reports}", flush=True)
    return int(completed != iterations or nonzero != 0 or threshold != 0 or timeouts != 0 or invalid_reports != 0)


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
            # Force the parent's TERM grace period too, so cleanup would add
            # at least 0.5 s to an incorrectly measured startup duration.
            body += ("import signal\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                     "time.sleep(30)\n" if case == "sleep" else "raise SystemExit(7)\n")
        elif case == "exit7":
            body += "raise SystemExit(7)\n"
        elif case == "timeout_timing":
            body += "time.sleep(30)\n"
        elif case == "environment":
            body += ("assert os.environ['CHIPCAE_SINGLETON_SENTINEL'] == 'child inherits spaces=ok'\n"
                     "print('inherited sentinel verified')\n")
        if case not in ("missing_report", "invalid_report"):
            body += "print('probe init/finalize=0.000001 s')\n"
        elif case == "invalid_report":
            body += "print('probe init/finalize=nan s')\n"
        probe.write_text(body)
        probe.chmod(0o700)
        if case == "missing":
            probe.unlink()
        if case in ("timing", "timeout_timing"):
            # Completed and timed-out processes must retain the observed
            # overrun. The completed case tests the threshold independently.
            import contextlib
            import io
            from unittest.mock import patch
            output = io.StringIO()
            with patch.object(time, "perf_counter", side_effect=[0.0, 0.0, 10.25]), \
                    contextlib.redirect_stdout(output):
                status = launches(str(probe), 200)
            print(output.getvalue(), end="", flush=True)
            assert status == 1 and marker.read_text().splitlines() == ["launch"]
            if case == "timing":
                assert "elapsed=10.250000 s exit=0 timeout=0" in output.getvalue()
                assert "maximum=10.250000 s nonzero=0 threshold_failures=1 timeouts=0" in output.getvalue()
            else:
                assert "elapsed=10.250000 s" in output.getvalue() and "timeout=1" in output.getvalue()
                assert "maximum=10.250000 s nonzero=1 threshold_failures=1 timeouts=1" in output.getvalue()
            return
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
                expected = {"sleep": "timeouts=1", "exit7": "exit=7",
                            "descendant_exit": "exit=7", "missing": "exit=launch-error",
                            "iterations": "integer >= 200",
                            "missing_report": "invalid probe init/finalize report",
                            "invalid_report": "invalid probe init/finalize report"}[case]
                assert expected in result.stdout, f"wrong failure for {case}: expected {expected!r}"
                launched = case in ("sleep", "exit7", "descendant_exit", "missing_report", "invalid_report")
                assert actual == ["launch"] * int(launched), "driver retried/launched invalid input"
                if launched:
                    assert len(records) == 1 and "completed=1 " in result.stdout
                    if case == "sleep":
                        assert float(records[0][1]) >= 10.0 and records[0][3] == '1', \
                            "timeout must report the observed deadline overrun"
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


def singleton_environment(props):
    values = mca_environment(props)
    # OpenPMIx 5 uses ordinary environment variables for its directory roots.
    # Audit these too, retaining all MCA assignment/duplication assertions above.
    for prop in ("ENVIRONMENT", "ENVIRONMENT_MODIFICATION"):
        for entry in props.get(prop, []):
            name, value = entry.split("=", 1)
            if name in ("PMIX_SERVER_TMPDIR", "PMIX_SYSTEM_TMPDIR"):
                assert name not in values, f"{name}: duplicate singleton directory assignment"
                if prop == "ENVIRONMENT_MODIFICATION":
                    assert value.startswith("set:"), f"{name}: directory must not concatenate values"
                    value = value[4:]
                values[name] = value
    return values


DIRECTORY_KEYS = {"PRTE_MCA_prte_tmpdir_base", "PMIX_SERVER_TMPDIR", "PMIX_SYSTEM_TMPDIR"}


def check_protection(name, settings, protection):
    assert protection.keys() <= settings.keys(), f"{name}: direct singleton path lacks probe protection"
    for key, value in protection.items():
        if key in DIRECTORY_KEYS:
            assert Path(settings[key]).is_absolute(), f"{name}: {key} lacks an absolute session directory"
        else:
            assert settings[key] == value, f"{name}: {key} disagrees with probe protection"


def coverage_check(build, entries):
    # CTest's full registration inventory is independent of the module's
    # directory traversal. Runtime dispatch cannot be enumerated statically:
    # every entry must have protection, including opaque/generated wrappers.
    acceptance = next(test for test in entries if test["name"] == "infra_ompi_singleton_startup")
    protection = singleton_environment(test_properties(acceptance))
    assert DIRECTORY_KEYS <= protection.keys(), "acceptance lacks singleton directory protection"
    missing = []
    directories = {}
    for test in entries:
        try:
            settings = singleton_environment(test_properties(test))
            check_protection(test["name"], settings, protection)
            for key in DIRECTORY_KEYS:
                path = Path(settings[key]).resolve()
                assert path.is_dir(), f"{test['name']}: {key} directory does not exist"
                token = (key, path)
                assert token not in directories, f"{test['name']}: shares {key} with {directories.get(token)}"
                directories[token] = test["name"]
        except AssertionError as error:
            missing.append(str(error))
    print(f"independent MPI coverage: {len(entries)} registrations audited, "
          f"{len(entries) - len(missing)} protected (including dynamic dispatch)", flush=True)
    assert not missing, "independent MPI coverage gaps:\n" + "\n".join(missing)


def dynamic_coverage_check(build):
    # A wrapper may resolve the executable from arbitrary runtime input. None
    # of these registrations contains an executable name or a Python import.
    import copy
    entries = registered_tests(build)
    coverage_check(build, entries)
    for command in (["sh", "-c", 'exec "$RUNTIME_PROGRAM"'],
                    [sys.executable, "-c", "import os; os.system(os.environ['RUNTIME_PROGRAM'])"],
                    ["cmake", "-P", "generated-at-runtime.cmake"]):
        opaque = copy.deepcopy(next(test for test in entries
                                   if test["name"] == "infra_ompi_singleton_startup"))
        opaque["name"] = "opaque_runtime_dispatch"
        opaque["command"] = command
        opaque["properties"] = [prop for prop in opaque["properties"]
                                if prop["name"] not in ("ENVIRONMENT", "ENVIRONMENT_MODIFICATION")]
        try:
            coverage_check(build, [*entries, opaque])
        except AssertionError as error:
            assert opaque["name"] in str(error), str(error)
        else:
            raise AssertionError(f"independent audit missed dynamic invocation: {command}")
    print("independent audit rejects unprotected shell, Python and generated CMake dispatch")


def build_cmake_requirements(text):
    # Tokenize just enough CMake to find commands, ignoring comments and keeping
    # quoted/bracket arguments atomic so example declarations cannot count.
    lexemes = re.finditer(
        r'(?P<comment>\#\[(?P<equals>=*)\[.*?\](?P=equals)\]|\#[^\n]*)'
        r'|"(?:\\.|[^"\\])*"|\[(?P<bracket>=*)\[.*?\](?P=bracket)\]'
        r'|[^\s()"#\[\]]+|[^\s]', text, re.S)
    tokens = [match[0] for match in lexemes if match.lastgroup != "comment"]
    commands = []
    index = 0
    while index + 1 < len(tokens):
        name = tokens[index].lower()
        assert tokens[index + 1] == "(", (
            f"CMakeLists.txt: expected command after {tokens[index]!r}")
        end, depth = index + 2, 1
        while end < len(tokens) and depth:
            depth += (tokens[end] == "(") - (tokens[end] == ")")
            end += 1
        assert depth == 0, f"CMakeLists.txt: unterminated {name} command"
        commands.append((name, tokens[index + 2:end - 1]))
        index = end
        if name == "project":
            break
    minimum = next((i for i, command in enumerate(commands)
                    if command[0] == "cmake_minimum_required"), None)
    assert minimum is not None, "CMakeLists.txt: expected cmake_minimum_required VERSION 3.28; missing"
    arguments = commands[minimum][1]
    assert len(arguments) >= 2 and arguments[:2] == ["VERSION", "3.28"], (
        f"CMakeLists.txt: expected exact minimum VERSION 3.28; actual arguments={arguments!r}")
    following = commands[minimum + 1] if minimum + 1 < len(commands) else None
    assert following == ("cmake_policy", ["VERSION", "3.20"]), (
        f"CMakeLists.txt: expected next command cmake_policy(VERSION 3.20); actual={following!r}")
    project = next((i for i, command in enumerate(commands) if command[0] == "project"), None)
    assert project is not None and minimum + 1 < project, (
        "CMakeLists.txt: expected minimum and policy before project(); project missing or precedes policy")


def build_readme_requirements(text):
    section = re.search(r"^## 构建\s*\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    assert section and "CMake 3.28 或更高版本" in section[1], (
        "README.md: expected 构建 requirement 'CMake 3.28 或更高版本'; missing")
    assert "CMake 3.20 或更高版本" not in section[1], (
        "README.md: expected current minimum 3.28; found 'CMake 3.20 或更高版本'")


def build_ci_requirements(text):
    assert not re.search(r"CMake\s*<\s*3\.28[^.]*automatic assignment warns and is skipped", text), (
        "docs/ci.md: expected no supported CMake < 3.28 warning/skip path; found obsolete compatibility text")
    assert re.search(r"(?:minimum[^.\n]*CMake\s+3\.28|CMake\s+3\.28[^.\n]*minimum|"
                     r"CMake\s*(?:>=|≥)\s*3\.28|CMake\s+3\.28\s+or (?:newer|higher|later)|"
                     r"CMake\s+3\.28\s*或更高版本)", text, re.I), (
        "docs/ci.md: expected explicit minimum CMake 3.28 requirement; missing")


def build_requirements_check(script):
    root = Path(script).resolve().parent.parent
    failures = []
    for filename, check in (("CMakeLists.txt", build_cmake_requirements),
                            ("README.md", build_readme_requirements),
                            ("docs/ci.md", build_ci_requirements)):
        try:
            check((root / filename).read_text(encoding="utf-8"))
        except AssertionError as error:
            failures.append(str(error))
    assert not failures, "build requirements:\n" + "\n".join(failures)
    print("build requirements: exact CMake 3.28 minimum, 3.20 policy and documentation verified")


def configuration_check(build, script):
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

    if "mesh_digest_golden" in tests:
        golden = tests["mesh_digest_golden"]
        props = properties(golden)
        assert props["TIMEOUT"] == 120, "mesh_digest_golden timeout changed"
        assert set(props["LABELS"]) == {"mesh", "tierA", "preview", "module", "owner:M2"}
        assert len(golden["command"]) == 3
        assert Path(golden["command"][1]).name == "mesh_digest.py"
        assert Path(golden["command"][2]).name == "chipcae-preview"
    core_environment = properties(tests["core_types"])["ENVIRONMENT"]
    assert [entry for entry in core_environment if entry.split("=", 1)[0] not in DIRECTORY_KEYS] == [
        "OMP_NUM_THREADS=2", "OMP_PROC_BIND=false"]
    # Do not prescribe which candidate wins. Require the chosen MCA protection
    # to reach the existing direct paths as well as the new acceptance probe.
    protection = mca_environment(acceptance)
    assert protection, "missing singleton startup protection: acceptance has no test-local MCA environment"
    protection.update(singleton_environment(acceptance))
    coverage_check(build, entries)
    # Remove protection from real direct callers and an entry with no known
    # MPI dependency. Opaque runtime dispatch must not evade the audit.
    import copy
    for omitted in ("k1_results_validation", "mesh_digest_golden", "core_types"):
        if omitted not in tests:
            continue
        mutated = copy.deepcopy(entries)
        test = next(test for test in mutated if test["name"] == omitted)
        test["properties"] = [prop for prop in test["properties"]
                              if prop["name"] not in ("ENVIRONMENT", "ENVIRONMENT_MODIFICATION")]
        try:
            coverage_check(build, mutated)
        except AssertionError as error:
            assert omitted in str(error), "independent audit failed for an unrelated reason"
        else:
            raise AssertionError(f"independent audit missed unprotected {omitted}")
    dynamic_coverage_check(build)
    build_requirements_check(script)
    print("singleton registration, independent caller coverage and existing test properties verified")


def module_check(case, script, python, real_build):
    # Exercise the real module with the linked installation, not fabricated
    # Open MPI detection/cache results. Controlled callers avoid product fixtures.
    cache = Path(real_build, "CMakeCache.txt").read_text()
    options = []
    for key in ("MPI_C_COMPILER", "MPI_CXX_COMPILER", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"):
        match = re.search(rf"^{key}:[^=]+=(.+)$", cache, re.M)
        assert match, f"configured installation lacks {key}"
        if case != "c_only" or "CXX" not in key:
            options.append(f"-D{key}={match[1]}")
    languages = "C" if case == "c_only" else "C CXX"
    module = Path(script).with_name("ChipcaeMpiSingleton.cmake").as_posix()
    groups = {
        "tests/integration/mesh_p1": ("mesh_digest_golden", "mesh_digest_mpi", "k1_preview",
                                      "k1_preview_mpi_surface", "k1_vtkhdf", "k1_results_validation"),
        "tests/solve": ("solve_inputs", "solve_profile_cuda_unavailable",
                        "solve_profile_cuda_gpus_zero", "solve_profile_cpu_gpus_nonzero",
                        "solve_pipeline", "solve_cut_manufactured", "solve_cut_pipeline"),
        "tests/preview": ("preview_zalign", "mesh_hash_preview_np", "preview_copper_area",
                          "preview_copper_phi", "preview_lid_nonconvex", "preview_mesh_params",
                          "preview_outline_nonconvex", "preview_region_planes"),
        "tests/install": ("install_smoke", "install_relocate", "cpack_tgz"),
        "tests/results": ("vtkhdf_results_serial", "cut_results_serial"),
        "tests/contract": ("contract_output_solver",),
        "tests/opaque": ("opaque_runtime_dispatch",),
        "tests/integration/callers": ("controlled_integration_cli", "import_assembly_cf_chain",
                                       "stack_writer_outline_polyset", "e2e_step_box_parts",
                                       "e2e_real_bulk_parts", "e2e_step_ring_chamfer_lid",
                                       "e2e_cylinder_volume", "e2e_derived_chamfer_lid",
                                       "e2e_cut_unsupported_material_layer"),
        "solvers": ("solve_column_gmg_prolongation", "solve_column_gmg_unsupported",
                    "solve_fvm_thermal", "solve_fvm_thermal_cut", "solve_fvm_elastic",
                    "solve_fvm_elastic_concave", "solve_fvm_elastic_benchmark", "solve_fvm_bimetal",
                    "tracemap_fvm_slab_harmonic", "tracemap_fvm_slab_series_xfail",
                    "tracemap_cut_inside_normalized", "tracemap_cut_outside_xfail",
                    "tracemap_cut_conservation_xfail", "tracemap_merge_conservation",
                    "tracemap_merge_directional_xfail"),
    }
    selected = tuple(name for names in groups.values() for name in names)
    other_tests = ("core_types", "k1_background", "k1_mesh_digest", "k1_cut", "solve_mesh",
                 "unrelated_mpi_label")
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
        (root / "dispatch_helper.py").write_text('''import json, os, subprocess
from pathlib import Path
def run():
    Path(os.environ["OBSERVATION"]).write_text(json.dumps(dict(os.environ)))
    subprocess.run(json.loads(Path(os.environ["DISPATCH_FILE"]).read_text()), check=True)
    print("controlled wrapper completed")
''')
        expected_commands = {}
        for relative, names in {**groups, ".": other_tests}.items():
            source = root / relative
            source.mkdir(parents=True, exist_ok=True)
            definitions = []
            for name in names:
                command = [python, str(recorder), "--wrapper", str(root / (name + ".json")),
                           name, "space = token"]
                if name == "opaque_runtime_dispatch":
                    command = [python, "-c", "import importlib,os; "
                               "importlib.import_module(os.environ['DISPATCH_MODULE']).run()"]
                expected_commands[name] = command
                definitions.append(f"add_test(NAME {name} COMMAND " +
                                   " ".join(map(literal, command)) + ")")
                labels = ("mesh;tierA;preview;module;owner:M2" if name == "mesh_digest_golden"
                          else "mpi" if name == "unrelated_mpi_label" else "fixture")
                definitions.append(f'''set_tests_properties({name} PROPERTIES
    TIMEOUT 120 PROCESSORS 2 LABELS "{labels}"
    PASS_REGULAR_EXPRESSION "controlled wrapper completed"
    FAIL_REGULAR_EXPRESSION "controlled wrapper failure"
    ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false"
    ENVIRONMENT_MODIFICATION "CHIPCAE_PRESERVED=set:kept")''')
                if name == "opaque_runtime_dispatch":
                    definitions.append(f'''set_property(TEST {name} APPEND PROPERTY ENVIRONMENT
    "PYTHONPATH={root}" "DISPATCH_MODULE=dispatch_helper"
    "DISPATCH_FILE=${{PROJECT_BINARY_DIR}}/dispatch.json" "OBSERVATION={root}/{name}.json")''')
                if name == "controlled_integration_cli":
                    definitions.append('set_property(TEST controlled_integration_cli APPEND '
                                       'PROPERTY ENVIRONMENT "CHIPCAE_SOLVE_EXE=controlled")')
                if name == "mesh_digest_golden":
                    definitions.append('''if(DEFINED FIXTURE_ENV)
    set_property(TEST mesh_digest_golden APPEND PROPERTY ENVIRONMENT "${FIXTURE_ENV}" "${FIXTURE_ENV}")
endif()
if(DEFINED FIXTURE_MOD)
    set_property(TEST mesh_digest_golden APPEND PROPERTY ENVIRONMENT_MODIFICATION "${FIXTURE_MOD}")
endif()''')
            (source / "CMakeLists.txt").write_text("\n".join(definitions) + "\n")
        (root / "tests/CMakeLists.txt").write_text(
            "add_subdirectory(integration)\nadd_subdirectory(solve)\nadd_subdirectory(preview)\n"
            "add_subdirectory(install)\nadd_subdirectory(contract)\nadd_subdirectory(results)\n"
            "add_subdirectory(opaque)\n")
        (root / "tests/integration/CMakeLists.txt").write_text(
            "add_subdirectory(mesh_p1)\nadd_subdirectory(callers)\n")
        root_definitions = (root / "CMakeLists.txt").read_text()
        (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(singleton_module_acceptance LANGUAGES {languages})
enable_testing()
set(CHIPCAE_BUILD_TESTS ON)
set(CHIPCAE_ENABLE_MPI ON)
find_package(MPI REQUIRED COMPONENTS {languages})
if(FIXTURE_MPI_LIBRARY)
    set_property(TARGET MPI::MPI_C PROPERTY INTERFACE_LINK_LIBRARIES "${{FIXTURE_MPI_LIBRARY}}")
endif()
function(chipcae_set_warnings)
endfunction()
{root_definitions}
add_subdirectory(tests)
add_subdirectory(solvers)
if(DEFINED FIXTURE_CMAKE_VERSION)
    set(CMAKE_VERSION "${{FIXTURE_CMAKE_VERSION}}")
endif()
include("{module}")
include("{module}")
file(GENERATE OUTPUT "${{PROJECT_BINARY_DIR}}/dispatch.json"
    CONTENT [===[["$<TARGET_FILE:chipcae_ompi_singleton_probe>"]]===])
''')
        build = root / "build"

        def configure(destination, extra=(), environment=None):
            result = subprocess.run(["cmake", "-G", "Ninja", "-S", str(root), "-B", str(destination),
                                     "-DCMAKE_BUILD_TYPE=Release", f"-DPython3_EXECUTABLE={python}",
                                     *options, *extra], env=environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(result.stdout, end="", flush=True)
            return result

        if case in ("cross_compile", "non_runnable"):
            extra = (["-DCMAKE_SYSTEM_NAME=Linux"] if case == "cross_compile" else
                     ["-DCHIPCAE_SINGLETON_RUNTIME_RUNNABLE=OFF"])
            configured = configure(build, extra)
            assert configured.returncode == 0, "non-runnable MPI target must configure"
            entries = registered_tests(build)
            acceptance = next(test for test in entries if test["name"] == "infra_ompi_singleton_startup")
            props = test_properties(acceptance)
            assert "not runnable" in str(props.get("SKIP_REGULAR_EXPRESSION", "")), \
                "non-runnable target must have an explicit CTest skip"
            subprocess.run(["cmake", "--build", str(build), "--target",
                            "chipcae_ompi_singleton_probe", "-j2"], check=True)
            result = subprocess.run(["ctest", "--test-dir", str(build), "-R",
                                     "^infra_ompi_singleton_startup$", "-V"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(result.stdout, end="", flush=True)
            assert result.returncode == 0 and "Skipped" in result.stdout
            assert "iteration=" not in result.stdout, "non-runnable target was executed"
            for test in entries:
                assert DIRECTORY_KEYS <= singleton_environment(test_properties(test)).keys(), \
                    f"{test['name']}: non-runnable configuration lost protection"
            return

        configured = configure(build)
        assert configured.returncode == 0, "real Open MPI fixture failed to configure"
        if case == "runtime_mismatch":
            stub = root / "stub"
            stub.mkdir()
            (stub / "version.c").write_text('''#include <string.h>
int MPI_Get_library_version(char *version, int *length) {
    const char *value = "MPICH controlled mismatched runtime";
    strcpy(version, value);
    *length = (int)strlen(value);
    return 0;
}
''')
            (stub / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(mismatched_runtime LANGUAGES C)
add_library(mpi_mismatch STATIC version.c)
''')
            subprocess.run(["cmake", "-G", "Ninja", "-S", str(stub), "-B", str(stub / "build")], check=True)
            subprocess.run(["cmake", "--build", str(stub / "build"), "-j2"], check=True)
            library = next((stub / "build").glob("*mpi_mismatch*.a"))
            result = configure(build, [f"-DFIXTURE_MPI_LIBRARY={library}"])
            assert result.returncode != 0, "Open MPI headers concealed a mismatched linked runtime"
            assert "runtime" in result.stdout.lower() and "MPICH" in result.stdout
            return
        if case == "compatibility":
            # Exercise our version guard with the installed interpreter. This
            # is not a claim of running a second/older CMake executable.
            result = configure(build, ["-DFIXTURE_CMAKE_VERSION=3.27.9"])
            assert result.returncode != 0, "older CMake silently left Open MPI tests unprotected"
            assert "3.28" in result.stdout and "Open MPI" in result.stdout
            assert re.search(r"requires|needs|upgrade", result.stdout, re.I)
            print("compatibility: older-version guard explicitly rejects unprotected configuration")
            return
        entries = registered_tests(build)
        acceptance = [test for test in entries if test["name"] == "infra_ompi_singleton_startup"]
        assert len(acceptance) == 1, "including module twice duplicated acceptance registration"
        assert "SKIP_REGULAR_EXPRESSION" not in test_properties(acceptance[0]), \
            "configured Open MPI component was misclassified and acceptance skipped"
        tests = {test["name"]: test for test in entries}
        for name in (*selected, *other_tests):
            test = tests[name]
            props = test_properties(test)
            assert test["command"] == expected_commands[name], f"{name}: existing command/arguments changed"
            assert props["TIMEOUT"] == 120 and props["PROCESSORS"] == 2
            assert props["PASS_REGULAR_EXPRESSION"] == ["controlled wrapper completed"]
            assert props["FAIL_REGULAR_EXPRESSION"] == ["controlled wrapper failure"]
            expected_labels = ({"mesh", "tierA", "preview", "module", "owner:M2"}
                               if name == "mesh_digest_golden" else
                               {"mpi"} if name == "unrelated_mpi_label" else {"fixture"})
            assert set(props["LABELS"]) == expected_labels, f"{name}: labels changed"
            assert "OMP_NUM_THREADS=2" in props["ENVIRONMENT"] and "OMP_PROC_BIND=false" in props["ENVIRONMENT"]
            assert "CHIPCAE_PRESERVED=set:kept" in props["ENVIRONMENT_MODIFICATION"]
        protection = mca_environment(test_properties(acceptance[0]))
        assert protection, "missing singleton startup protection: cannot validate module environment contract"
        protection.update(singleton_environment(test_properties(acceptance[0])))

        if case.startswith("conflict_") or case in ("matching", "inherited_baseline"):
            # Discover the independently selected candidate rather than locking
            # the tests to hash or another unvalidated switch. Component values
            # and per-test session paths are both supported.
            settings = singleton_environment(test_properties(tests["mesh_digest_golden"]))
            assert protection.keys() <= settings.keys(), "golden lacks acceptance protection"
            for key in protection:
                value = settings[key]
                conflict = "chipcae-controlled-conflicting-value"
                extra = []
                environment = dict(os.environ)
                if case == "inherited_baseline":
                    environment[key] = str(root)
                elif case == "matching":
                    extra = [f"-DFIXTURE_ENV={key}={value}", f"-DFIXTURE_MOD={key}=set:{value}"]
                    # A global session path cannot match deliberately distinct
                    # per-test paths. Matching inheritance applies to shared values.
                    if key not in DIRECTORY_KEYS:
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
                if case not in ("matching", "inherited_baseline"):
                    assert result.returncode != 0, f"silently accepted {case}: {key}"
                    assert key in result.stdout, "conflict diagnostic must identify the MCA name"
                    assert re.search(r"conflict|incompatible|冲突", result.stdout, re.I), "no explicit conflict diagnostic"
                    if case in ("conflict_environment", "conflict_modification", "conflict_unset"):
                        assert "mesh_digest_golden" in result.stdout, "diagnostic must identify the affected test"
                    if case != "conflict_unset":
                        assert conflict in result.stdout, "diagnostic must preserve the conflicting value"
                    print(f"{case}: explicit MCA conflict diagnostic verified")
                    continue
                assert result.returncode == 0, "matching assignments and inherited directory baselines must configure"
                if case == "inherited_baseline":
                    assert any("overrid" in line.lower() and f"{key}={root}" in line
                               for line in result.stdout.splitlines()), "inherited override must identify its key and baseline"
                tests = {test["name"]: test for test in registered_tests(build)}
                # The combined reader rejects duplicate ENVIRONMENT entries and
                # repeated assignments across ENVIRONMENT / MODIFICATION.
                assert singleton_environment(test_properties(tests["mesh_digest_golden"]))[key] == value
            if case != "matching":
                return

        session_paths = set()
        for name in (*selected, *other_tests, "infra_ompi_singleton_startup"):
            props = test_properties(tests[name])
            settings = singleton_environment(props)
            assert protection.keys() <= settings.keys(), f"{name}: direct singleton path lacks probe protection"
            assert all(any(entry == f"{key}={value}" for entry in props.get("ENVIRONMENT", []))
                       for key, value in settings.items()), f"{name}: protection must be test-local ENVIRONMENT"
            for key, value in protection.items():
                if key in DIRECTORY_KEYS:
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
            absolute_keys = [key for key in protection if key in DIRECTORY_KEYS]
            for key in absolute_keys:
                paths = {singleton_environment(test_properties(tests[name]))[key]
                         for name in (*selected, *other_tests, "infra_ompi_singleton_startup")}
                assert len(paths) == len(selected) + len(other_tests) + 1, f"{key}: test session directories are shared"
            second = root / "another-build"
            assert configure(second).returncode == 0
            other = {test["name"]: test for test in registered_tests(second)}
            other_paths = {Path(singleton_environment(test_properties(other[name]))[key]).resolve()
                           for name in (*selected, *other_tests, "infra_ompi_singleton_startup") for key in absolute_keys}
            assert session_paths.isdisjoint(other_paths), "concurrent builds share session directories"
        if case == "dynamic_execution":
            subprocess.run(["cmake", "--build", str(build), "--target",
                            "chipcae_ompi_singleton_probe", "-j2"], check=True)
            result = subprocess.run(["ctest", "--test-dir", str(build), "-R",
                                     "^opaque_runtime_dispatch$", "-V"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(result.stdout, end="", flush=True)
            assert result.returncode == 0 and "probe init/finalize=" in result.stdout
            observed = json.loads((root / "opaque_runtime_dispatch.json").read_text())
            for key, value in singleton_environment(test_properties(tests["opaque_runtime_dispatch"])).items():
                assert observed.get(key) == value, f"dynamic child did not inherit {key}"
            coverage_check(build, entries)
        if case == "propagation":
            result = subprocess.run(["ctest", "--test-dir", str(build), "-R",
                                     "^(mesh_digest_golden|mesh_digest_mpi|solve_inputs)$", "-V"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(result.stdout, end="", flush=True)
            assert result.returncode == 0, "controlled direct/MPI wrapper failed"
            for name in ("mesh_digest_golden", "mesh_digest_mpi", "solve_inputs"):
                observed = json.loads((root / (name + ".json")).read_text())
                for key, value in singleton_environment(test_properties(tests[name])).items():
                    assert observed.get(key) == value, f"{name}: child did not inherit {key}"
                assert observed["OMP_NUM_THREADS"] == "2" and observed["CHIPCAE_PRESERVED"] == "kept"
        if case == "c_only":
            subprocess.run(["cmake", "--build", str(build), "--target",
                            "chipcae_ompi_singleton_probe", "-j2"], check=True)
            environment = dict(os.environ, **singleton_environment(test_properties(acceptance[0])))
            probe = next(arg[len("-DPROBE="):] for arg in acceptance[0]["command"]
                         if arg.startswith("-DPROBE="))
            result = subprocess.run([probe], env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=10)
            print(result.stdout, end="", flush=True)
            assert result.returncode == 0 and "probe init/finalize=" in result.stdout
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
if mode == "component_paths":
    try:
        check_protection("component_paths", {"PMIX_MCA_psec": "/different/component"},
                         {"PMIX_MCA_psec": "/original/component"})
    except AssertionError as error:
        assert "PMIX_MCA_psec" in str(error)
    else:
        raise AssertionError("absolute component values bypassed equality check")
    check_protection("directories", {key: "/different/session" for key in DIRECTORY_KEYS},
                     {key: "/original/session" for key in DIRECTORY_KEYS})
    print("directory names permit isolation; absolute component values require equality")
elif mode in ("sleep", "timing", "timeout_timing", "exit7", "missing", "iterations", "success", "environment", "descendant_exit", "missing_report", "invalid_report"):
    driver_check(mode, script, python)
elif mode in ("dynamic_execution", "cross_compile", "non_runnable", "c_only", "selection", "propagation", "matching", "inherited_baseline", "runtime_mismatch", "compatibility", "conflict_inherited", "conflict_environment",
              "conflict_modification", "conflict_unset"):
    module_check(mode, script, python, build)
elif mode == "coverage_dynamic":
    dynamic_coverage_check(build)
elif mode == "build_requirements":
    build_requirements_check(script)
elif mode == "configuration":
    configuration_check(build, script)
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
