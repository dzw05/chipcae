"""Configure-only, offline regressions for the real miniz resolver and ECAD consumer."""

import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(os.environ.get("CHIPCAE_SOURCE_DIR", Path(__file__).resolve().parents[3]))


def cmake_quote(value):
    return "[=[" + str(value) + "]=]"


def run(command):
    # Keep PATH and plain command names: shared build slots may queue indefinitely.
    return subprocess.run(command, capture_output=True, text=True)


def outer_generator():
    binary = os.environ.get("CHIPCAE_BINARY_DIR")
    if binary:
        try:
            cache = (Path(binary) / "CMakeCache.txt").read_text()
        except OSError:
            return None
        match = re.search(r"^CMAKE_GENERATOR:[^=]+=(.+)$", cache, re.MULTILINE)
        if match:
            return match[1]
    return None


class MinizImportVisibility(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake"):
            raise AssertionError("cmake >= 3.24 is required; missing cmake must fail")
        result = run(["cmake", "--version"])
        match = re.search(r"cmake version (\d+)\.(\d+)", result.stdout)
        if result.returncode or not match or tuple(map(int, match.groups())) < (3, 24):
            raise AssertionError("cmake >= 3.24 is required\n" + result.stdout + result.stderr)

    def configure_case(self, form, cycle, safe_entries):
        # Follow MinizResolutionAcceptance.configure_case's source-boundary stubs,
        # without its forced Ninja generator or build/command inspection steps.
        temporary = tempfile.TemporaryDirectory(prefix="miniz-visibility-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source, prefix, fallback, binary = (
            root / name for name in ("source", "prefix", "fallback", "build")
        )
        for directory in (source, prefix, fallback):
            directory.mkdir()
        archive = prefix / "synthetic.a"
        archive.write_bytes(b"!<arch>\n")
        tool = prefix / "synthetic-tool"
        tool.touch()
        names = ("miniz",) if form == "plain" else ("miniz::miniz",)
        if form == "independent":
            names += ("miniz",)
        package = [
            "add_library(miniz_dep::a INTERFACE IMPORTED)",
            "add_library(miniz_dep::b UNKNOWN IMPORTED)",
            f"set_property(TARGET miniz_dep::b PROPERTY IMPORTED_LOCATION {cmake_quote(archive)})",
            "set_property(TARGET miniz_dep::a PROPERTY INTERFACE_LINK_LIBRARIES "
            '"$<LINK_ONLY:miniz_dep::b>")',
            "add_library(pkg_extra::unrelated STATIC IMPORTED)",
            f"set_property(TARGET pkg_extra::unrelated PROPERTY IMPORTED_LOCATION {cmake_quote(archive)})",
            "add_executable(pkg_extra::tool IMPORTED)",
            f"set_property(TARGET pkg_extra::tool PROPERTY IMPORTED_LOCATION {cmake_quote(tool)})",
        ]
        dependency = "miniz_dep::a"
        if safe_entries:
            package += [
                "add_library(miniz_dep::alias ALIAS miniz_dep::a)",
                "add_library(miniz_dep::global INTERFACE IMPORTED GLOBAL)",
            ]
            # The local dependency alias stays in its defining scope. The backing
            # import must be promoted and its retained link reference must generate.
            dependency = ("miniz_dep::alias;miniz_dep::global;caller_dep::local;" +
                          str(archive) + ";-lz;$<$<BOOL:0>:pkg_extra::unrelated>")
        for name in names:
            package += [
                f"add_library({name} STATIC IMPORTED)",
                f"set_property(TARGET {name} PROPERTY IMPORTED_LOCATION {cmake_quote(archive)})",
                f"set_property(TARGET {name} PROPERTY INTERFACE_LINK_LIBRARIES {cmake_quote(dependency)})",
            ]
        if form == "alias":
            package.append("add_library(miniz ALIAS miniz::miniz)")
        if cycle:
            package.append("set_property(TARGET miniz_dep::b PROPERTY INTERFACE_LINK_LIBRARIES miniz_dep::a)")
        (prefix / "minizConfig.cmake").write_text("\n".join(package) + "\n")

        (fallback / "miniz.c").write_text("int synthetic_miniz(void) { return 0; }\n")
        for name in ("miniz_zip.c", "miniz_tinfl.c", "miniz_tdef.c"):
            (fallback / name).write_text("/* Synthetic pinned source boundary. */\n")
        (fallback / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(synthetic_miniz C)\n"
            "add_library(miniz STATIC miniz.c)\n")

        observed = ["miniz::miniz", "miniz", "miniz_dep::a", "miniz_dep::b",
                    "pkg_extra::unrelated", "pkg_extra::tool"]
        if safe_entries:
            observed += ["miniz_dep::global", "caller_dep::local"]
        (source / "observe.cmake").write_text("""
file(WRITE "${CMAKE_BINARY_DIR}/${scope}.txt" "")
foreach(name IN LISTS observed)
    if(TARGET ${name})
        get_target_property(global ${name} IMPORTED_GLOBAL)
        get_target_property(alias ${name} ALIASED_TARGET)
        file(APPEND "${CMAKE_BINARY_DIR}/${scope}.txt" "${name}=TRUE,${global},${alias}\n")
    else()
        file(APPEND "${CMAKE_BINARY_DIR}/${scope}.txt" "${name}=FALSE\n")
    endif()
endforeach()
if(TARGET miniz::miniz)
    set(selected miniz::miniz)
elseif(TARGET miniz)
    set(selected miniz)
else()
    set(selected missing)
endif()
if(TARGET ${selected})
    get_target_property(kind ${selected} TYPE)
endif()
get_target_property(links chipcae_geometry_ecad LINK_LIBRARIES)
get_target_property(interface chipcae_geometry_ecad INTERFACE_LINK_LIBRARIES)
file(APPEND "${CMAKE_BINARY_DIR}/${scope}.txt"
    "selected=${selected}\nkind=${kind}\nlinks=${links}\ninterface=${interface}\n")
# Real consumers force CMake generation to resolve the retained link closure in
# both scopes, including package-local dependency aliases and LINK_ONLY entries.
add_executable(${scope}_consumer "${CMAKE_SOURCE_DIR}/main.cpp")
target_link_libraries(${scope}_consumer PRIVATE chipcae_geometry_ecad)
""")
        (source / "main.cpp").write_text("int main() { return 0; }\n")
        (source / "consumer").mkdir()
        (source / "consumer/CMakeLists.txt").write_text(
            'set(scope sibling)\ninclude("${CMAKE_SOURCE_DIR}/observe.cmake")\n')
        (source / "CMakeLists.txt").write_text("\n".join([
            "cmake_minimum_required(VERSION 3.24)",
            "project(miniz_import_visibility LANGUAGES C CXX)",
            "set(CHIPCAE_ENABLE_ECAD ON)",
            "set(BUILD_SHARED_LIBS OFF)",
            f"set(CMAKE_PREFIX_PATH {cmake_quote(prefix)})",
            *[f"set(CMAKE_FIND_USE_{name} OFF)" for name in (
                "PACKAGE_REGISTRY", "SYSTEM_PACKAGE_REGISTRY", "SYSTEM_ENVIRONMENT_PATH",
                "CMAKE_ENVIRONMENT_PATH", "CMAKE_SYSTEM_PATH", "PACKAGE_ROOT_PATH")],
            "set(CMAKE_DISABLE_FIND_PACKAGE_Clipper2 ON)",
            "set(CMAKE_DISABLE_FIND_PACKAGE_EXPAT ON)",
            "add_library(Clipper2 INTERFACE)",
            "add_library(EXPAT::EXPAT INTERFACE IMPORTED)",
            "add_library(geometry INTERFACE)",
            "add_library(chipcae::geometry ALIAS geometry)",
            "add_library(caller_dep::local INTERFACE IMPORTED)",
            "function(chipcae_set_warnings)", "endfunction()",
            f"set(FETCHCONTENT_SOURCE_DIR_MINIZ {cmake_quote(fallback)})",
            "set(FETCHCONTENT_FULLY_DISCONNECTED ON)",
            f"include({cmake_quote(SOURCE / 'cmake/ChipcaeDependencies.cmake')})",
            f"add_subdirectory({cmake_quote(SOURCE / 'geometry/ecad')} ecad)",
            "set(observed " + " ".join(observed) + ")",
            "set(scope parent)",
            'include("${CMAKE_SOURCE_DIR}/observe.cmake")',
            "add_subdirectory(consumer)",
        ]) + "\n")
        command = ["cmake", "-S", str(source), "-B", str(binary),
                   "-DCMAKE_BUILD_TYPE=Release"]
        generator = outer_generator()
        if generator:
            command += ["-G", generator]
        snapshots = []
        for invocation in (1, 2):
            result = run(command)
            self.assertEqual(result.returncode, 0,
                             f"configure {invocation}: {shlex.join(command)}\n"
                             + result.stdout + result.stderr)
            snapshots.append({scope: (binary / (scope + ".txt")).read_text()
                              for scope in ("parent", "sibling")})
        self.assertEqual(snapshots[0], snapshots[1], "reconfigure changed visibility observations")
        self.assertEqual(snapshots[0]["parent"], snapshots[0]["sibling"],
                         "parent and sibling visibility must agree")
        return snapshots[0]

    def test_only_miniz_link_closure_escapes_discovery(self):
        for form in ("namespaced", "plain", "independent", "alias"):
            for cycle in (False, True):
                with self.subTest(form=form, cycle=cycle):
                    self.assert_visibility(form, cycle, safe_entries=False)

    def test_dependency_aliases_global_imports_and_non_target_entries(self):
        self.assert_visibility("namespaced", False, safe_entries=True)

    def assert_visibility(self, form, cycle, safe_entries):
        observations = self.configure_case(form, cycle, safe_entries)
        selected = "miniz" if form == "plain" else "miniz::miniz"
        visible = {selected, "miniz_dep::a", "miniz_dep::b"}
        if form in ("alias", "independent"):
            visible.add("miniz")
        if safe_entries:
            visible.add("miniz_dep::global")
        for scope, text in observations.items():
            values = dict(line.split("=", 1) for line in text.splitlines())
            self.assertEqual(values["selected"], selected, text)
            self.assertEqual(values["kind"], "STATIC_LIBRARY", text)
            self.assertIn(selected, values["links"].split(";"), text)
            self.assertIn(f"$<LINK_ONLY:{selected}>", values["interface"].split(";"), text)
            for target in sorted(visible):
                self.assertEqual(values[target].split(",")[:2], ["TRUE", "TRUE"],
                                 f"{scope}: closure target {target} must be global\n{text}")
            if form == "alias":
                self.assertEqual(values["miniz"].split(",")[2], "miniz::miniz", text)
            if safe_entries:
                self.assertEqual(values["caller_dep::local"].split(",")[:2], ["TRUE", "FALSE"],
                                 "a dependency owned by the caller must remain local\n" + text)
            for target in ("miniz::miniz", "miniz"):
                if target not in visible:
                    self.assertEqual(values[target], "FALSE", text)
            # Keep these independent so every leaked target/scope is reported.
            for target in ("pkg_extra::unrelated", "pkg_extra::tool"):
                with self.subTest(scope=scope, unrelated=target):
                    self.assertEqual(values[target], "FALSE",
                                     f"{scope}: unrelated import {target} escaped discovery\n{text}")


if __name__ == "__main__":
    # chipcae_auto_tests passes the source root as its sole positional argument.
    unittest.main(argv=[sys.argv[0]], verbosity=2)
