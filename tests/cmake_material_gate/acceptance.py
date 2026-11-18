"""Run real CMake gates; generate harnesses outside the checkout.

Configure this directory as a standalone Release project, then run CTest.
Whole-tree cases accept MAT_GATE_CMAKE_ARGS (a JSON argument array) and
MAT_GATE_KOKKOS_ROOT. They retain fresh build trees under MAT_GATE_BUILD_ROOT
(the system temporary directory by default) for inspection and later builds.
The five harness cases catch unconditional FATAL, unconditional return, an
incorrect REQUIRE condition, and changes to the existing registration arguments.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[2]
CHECK = unittest.TestCase()
PREFIXES = {"cte_contract": "ctecontract_", "temp_edges": "tempedge_"}
EXPECTED_NAMES = {
    "ctecontract_invalid",
    "ctecontract_nodes",
    "ctecontract_offknot",
    "tempedge_invalid",
    "tempedge_jump",
    "tempedge_range",
}
FILTER = "^(ctecontract_|tempedge_)"


def run(*command):
    print("$ " + shlex.join(str(arg) for arg in command), flush=True)
    result = subprocess.run(
        [str(arg) for arg in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    print(result.stdout, end="", flush=True)
    print(f"[exit {result.returncode}]", flush=True)
    return result


def skip_message(directory):
    return f"Skipping {directory}: chipcae::material is unavailable (requires Kokkos)"


def harness(directory, mode):
    # Only the registration function is replaced: the production gate is loaded verbatim.
    # Record its arguments so skipped/duplicate/changed registration cannot pass.
    with tempfile.TemporaryDirectory(prefix="material gate ") as scratch:
        root = Path(scratch)
        arguments = root / "calls.txt"
        project = [
            "cmake_minimum_required(VERSION 3.20)",
            "project(material_gate_harness LANGUAGES NONE)",
            'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/calls.txt" "")',
            "function(chipcae_auto_tests)",
            '    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/calls.txt" "${ARGV}\\n")',
            "endfunction()",
        ]
        if mode.startswith("target-"):
            project += [
                "add_library(material_stub INTERFACE)",
                "add_library(chipcae::material ALIAS material_stub)",
            ]
        if mode == "missing-undefined":
            project += [
                "unset(CHIPCAE_REQUIRE_KOKKOS CACHE)",
                "unset(CHIPCAE_REQUIRE_KOKKOS)",
            ]
        else:
            require = "ON" if mode.endswith("-on") else "OFF"
            project.append(f"set(CHIPCAE_REQUIRE_KOKKOS {require})")
        cmake_file = SOURCE / "tests" / "unit" / directory / "CMakeLists.txt"
        project.append(f'include("{cmake_file.as_posix()}")')
        (root / "CMakeLists.txt").write_text("\n".join(project) + "\n")
        result = run("cmake", "-S", root, "-B", root, "-DCMAKE_BUILD_TYPE=Release")
        calls = arguments.read_text().splitlines()
        message = skip_message(directory)
        if mode == "missing-on":
            CHECK.assertNotEqual(result.returncode, 0, "REQUIRE=ON must reject a missing target")
            CHECK.assertIn(f"{directory}: chipcae::material target missing", result.stdout)
            CHECK.assertNotIn(message, result.stdout)
            CHECK.assertEqual(calls, [])
        else:
            CHECK.assertEqual(
                result.returncode,
                0,
                f"{directory} {mode}: configuration must succeed;\n{result.stdout}",
            )
            if mode.startswith("missing-"):
                CHECK.assertIn("-- " + message, result.stdout.splitlines())
                CHECK.assertEqual(calls, [], "a skipped directory must not register tests")
            else:
                CHECK.assertEqual(
                    calls,
                    [f"LIBS;chipcae::material;LABELS;tierA;PREFIX;{PREFIXES[directory]}"],
                    "the existing target must register tests exactly once with unchanged arguments",
                )
                CHECK.assertNotIn("Skipping " + directory, result.stdout)


def tree(mode):
    common = json.loads(os.environ.get("MAT_GATE_CMAKE_ARGS", "[]"))
    CHECK.assertIsInstance(common, list)
    CHECK.assertTrue(all(isinstance(arg, str) for arg in common))
    # Keep dependency selection explicit: common arguments must not pre-enable Kokkos
    # or turn off the test tree. Fresh builds avoid stale-cache false positives.
    forbidden = (
        "KOKKOS", "Kokkos", "CMAKE_PREFIX_PATH", "CMAKE_BUILD_TYPE", "CHIPCAE_BUILD_TESTS"
    )
    CHECK.assertFalse(any(word in arg for arg in common for word in forbidden), common)
    build_root = os.environ.get("MAT_GATE_BUILD_ROOT")
    if build_root:
        Path(build_root).mkdir(parents=True, exist_ok=True)
    build = Path(tempfile.mkdtemp(prefix=mode + "-", dir=build_root))
    command = [
        "cmake", "-S", SOURCE, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        *common,
    ]
    if mode == "tree-kokkos":
        kokkos_root = os.environ.get("MAT_GATE_KOKKOS_ROOT")
        CHECK.assertTrue(kokkos_root, "MAT_GATE_KOKKOS_ROOT must name an existing installation")
        command += [
            f"-DKokkos_ROOT={kokkos_root}",
            "-DCHIPCAE_REQUIRE_KOKKOS=ON",
            "-DCHIPCAE_ENABLE_KOKKOS=ON",
        ]
    else:
        CHECK.assertFalse(os.environ.get("Kokkos_ROOT"), "skip case must not set Kokkos_ROOT")
        CHECK.assertFalse(os.environ.get("CMAKE_PREFIX_PATH"), "skip case needs a clean prefix path")
    result = run(*command)
    (build / "acceptance-configure.log").write_text(result.stdout)
    CHECK.assertEqual(result.returncode, 0, f"{mode}: configuration must succeed;\n{result.stdout}")
    if mode == "tree-skip":
        CHECK.assertIn(
            "chipcae material: Kokkos not found; skipping the material library", result.stdout
        )
        for directory in PREFIXES:
            CHECK.assertIn("-- " + skip_message(directory), result.stdout.splitlines())
            CHECK.assertNotRegex(
                result.stdout, rf"CMake Error at [^\n]*tests/unit/{directory}/CMakeLists\.txt"
            )
    else:
        targets = run("cmake", "--build", build, "--target", "help")
        CHECK.assertEqual(targets.returncode, 0)
        CHECK.assertRegex(targets.stdout, r"(?m)^chipcae_material:")
        # Successful generation of these link interfaces also verifies the material ALIAS.
    listing = run("ctest", "--test-dir", build, "-N", "-R", FILTER)
    CHECK.assertEqual(listing.returncode, 0)
    names = re.findall(r"(?m)^\s*Test\s+#\d+:\s+(\S+)\s*$", listing.stdout)
    expected = EXPECTED_NAMES if mode == "tree-kokkos" else set()
    CHECK.assertEqual(set(names), expected, "filtered CTest names must match exactly")
    CHECK.assertEqual(len(names), len(expected), "no missing or duplicate registrations")
    CHECK.assertRegex(listing.stdout, rf"(?m)^Total Tests: {len(expected)}\s*$")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=(
        "missing-undefined", "missing-off", "missing-on", "target-off", "target-on",
        "tree-skip", "tree-kokkos",
    ))
    parser.add_argument("--directory", choices=tuple(PREFIXES))
    args = parser.parse_args()
    if args.mode.startswith("tree-"):
        test = unittest.FunctionTestCase(lambda: tree(args.mode), description=args.mode)
    else:
        if not args.directory:
            parser.error("--directory is required for a harness case")
        test = unittest.FunctionTestCase(
            lambda: harness(args.directory, args.mode),
            description=f"{args.directory} {args.mode}",
        )
    outcome = unittest.TextTestRunner(verbosity=2).run(test)
    raise SystemExit(0 if outcome.wasSuccessful() else 1)
