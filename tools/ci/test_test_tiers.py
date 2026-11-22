"""Exercise classification and validation through real, configure-only CTest projects."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TestTiers(unittest.TestCase):
    def configure(self, files):
        temporary = tempfile.TemporaryDirectory(prefix="test-tiers-")
        self.addCleanup(temporary.cleanup)
        source = Path(temporary.name) / "source"
        build = Path(temporary.name) / "build"
        files = dict(files)
        files["CMakeLists.txt"] = f'''cmake_minimum_required(VERSION 3.28)
project(tier_fixture NONE)
enable_testing()
include("{ROOT / 'cmake/ChipcaeTestTiers.cmake'}")
{files.get("CMakeLists.txt", "")}
chipcae_apply_test_tiers()
'''
        for name, contents in files.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        result = subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"],
                                capture_output=True, text=True)
        return result, source, build

    def test_source_prefixes_directory_fallback_and_tier_precedence(self):
        result, _, build = self.configure({
            "CMakeLists.txt": "add_subdirectory(tests)\nadd_subdirectory(tests/integration/import_cli)\nadd_subdirectory(tests/unit)",
            "tests/unit/CMakeLists.txt": f'''
include("{ROOT / 'cmake/ChipcaeAutoTests.cmake'}")
chipcae_auto_tests(LABELS unit PREFIX autotier_ TIMEOUT 30)
''',
            "tests/unit/test_material_probe.py": "pass\n",
            "tests/CMakeLists.txt": '''
add_test(NAME core_types COMMAND cmake -E true)
add_test(NAME contract_schemas COMMAND cmake -E true)
add_test(NAME source_override COMMAND cmake -E true)
set_property(GLOBAL PROPERTY CHIPCAE_TEST_SOURCE_source_override "${PROJECT_SOURCE_DIR}/kernel/src/import_board.cpp")
set_tests_properties(source_override PROPERTIES TIMEOUT 119 LABELS custom)
add_test(NAME slow COMMAND cmake -E true)
set_property(GLOBAL PROPERTY CHIPCAE_TEST_SOURCE_slow "${PROJECT_SOURCE_DIR}/core/slow.cpp")
set_tests_properties(slow PROPERTIES TIMEOUT 120)
add_test(NAME measured COMMAND cmake -E true)
set_tests_properties(measured PROPERTIES LABELS "highmem;performance;owner:M3" TIMEOUT 900)
add_test(NAME explicit COMMAND cmake -E true)
set_tests_properties(explicit PROPERTIES LABELS "unit;nightly;owner:M7" TIMEOUT 900)
add_test(NAME headers_no_mpi_check COMMAND cmake -E true)
set_tests_properties(headers_no_mpi_check PROPERTIES LABELS "owner:M6")
''',
            "tests/integration/import_cli/CMakeLists.txt": '''
add_test(NAME import_cli COMMAND cmake -E true)
add_test(NAME end_e2e COMMAND cmake -E true)
add_test(NAME throughput COMMAND cmake -E true)
add_test(NAME fast_benchmark COMMAND cmake -E true)
set_tests_properties(fast_benchmark PROPERTIES LABELS "performance;nightly")
''',
        })
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        listing = subprocess.run(["ctest", "--test-dir", str(build), "-N", "--show-only=json-v1"],
                                 check=True, capture_output=True, text=True)
        tests = {test["name"]: {prop["name"]: prop["value"] for prop in test["properties"]}
                 for test in json.loads(listing.stdout)["tests"]}
        expected = {
            "core_types": ("unit", "M6"), "contract_schemas": ("unit", "ARCH"),
            "source_override": ("unit", "M1"), "slow": ("module", "M6"),
            "measured": ("highmem", "M3"), "explicit": ("unit", "M7"),
            "import_cli": ("module", "M1"), "end_e2e": ("acceptance", "M1"),
            "throughput": ("perf", "M1"), "fast_benchmark": ("perf", "M1"),
            "autotier_material_probe": ("unit", "M4"),
            "headers_no_mpi_check": ("unit", "M6"),
        }
        tiers = {"unit", "module", "highmem", "perf", "acceptance"}
        for name, (tier, owner) in expected.items():
            with self.subTest(name=name):
                labels = tests[name]["LABELS"]
                self.assertEqual(set(labels) & tiers, {tier})
                self.assertEqual([label for label in labels if label.startswith("owner:")],
                                 [f"owner:{owner}"])
        self.assertEqual(tests["source_override"]["TIMEOUT"], 119)
        self.assertIn("custom", tests["source_override"]["LABELS"])
        check = subprocess.run(["python3", str(ROOT / "tests/tier_labels/check_tier_labels.py"),
                                str(build)], capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)

    def test_invalid_or_missing_labels_fail_configuration(self):
        for labels in ("unit;module;owner:M6", "unit;owner:M1;owner:M2", "unit;owner:UNKNOWN", "unit"):
            with self.subTest(labels=labels):
                result, _, _ = self.configure({"CMakeLists.txt": f'''
add_test(NAME unowned COMMAND cmake -E true)
set_tests_properties(unowned PROPERTIES LABELS "{labels}")
'''})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Invalid CTest tier/owner labels", result.stderr)
                self.assertIn("unowned", result.stderr)

    def test_ctest_check_rejects_missing_and_conflicting_labels(self):
        result, _, build = self.configure({"CMakeLists.txt": '''
add_test(NAME missing COMMAND cmake -E true)
set_tests_properties(missing PROPERTIES LABELS "unit;owner:M6")
'''})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        testfile = build / "CTestTestfile.cmake"
        original = testfile.read_text()
        for labels in ("tierA", "unit;module;owner:M6", "unit;owner:M1;owner:M2", "unit;owner:UNKNOWN"):
            with self.subTest(labels=labels):
                testfile.write_text(original.replace("unit;owner:M6", labels))
                check = subprocess.run(["python3", str(ROOT / "tests/tier_labels/check_tier_labels.py"),
                                        str(build)], capture_output=True, text=True)
                self.assertNotEqual(check.returncode, 0)
                self.assertIn("missing: tiers=", check.stderr)
