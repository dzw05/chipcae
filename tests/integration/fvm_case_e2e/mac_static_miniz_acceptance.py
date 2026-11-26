"""Build-system regressions for ECAD miniz selection and private FVM probes.

All packages and generated projects are synthetic and live in temporary directories.
Run directly with unittest, or through the existing fvm_fvm_case_e2e entry point.
"""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import fvm_case_util as util


SOURCE = Path(__file__).resolve().parents[3]
TARGETS = ('adapter_probe', 'elastic_replay', 'pipeline_failure_probe', 'contract_probe')
PAIRS = (
    ('-arch', 'arm64'), ('-arch', 'x86_64'),
    ('-target', 'arm64-apple-macos13'), ('--target', 'aarch64-linux-gnu'),
    ('-framework', 'CoreFoundation'),
    ('-Xlinker', '-rpath'), ('-Xlinker', '/runtime with spaces'),
    ('-isysroot', '/SDK with spaces'), ('--sysroot', '/sysroot with spaces'),
    ('-F', '/frameworks with spaces'), ('-L', '/libraries with spaces'),
    ('-iframework', '/system frameworks with spaces'),
)


def cmake_quote(value):
    return '[=[' + str(value) + ']=]'


def run(args, **kwargs):
    # Plain command names preserve the caller's shared-build-slot wrappers.
    return subprocess.run(args, capture_output=True, text=True, **kwargs)


class MinizResolutionAcceptance(unittest.TestCase):
    """Exercise the real dependency resolver and unchanged ECAD CMake consumer."""

    def configure_case(self, targets=(), *, shared=None, policy=None, minimum=None,
                       disabled=False, promote=False, alias=False, global_import=False):
        temporary = tempfile.TemporaryDirectory(prefix='miniz-acceptance-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        prefix = root / 'prefix'
        prefix.mkdir()
        package = []
        for name, kind in targets:
            archive = prefix / (name.replace('::', '_') + ('.so' if kind in
                                ('SHARED', 'MODULE') else '.a'))
            archive.write_bytes(b'!<arch>\n')
            underlying = name.replace('::', '_') + '_impl' if alias else name
            if kind == 'INTERFACE':
                # A package-local wrapper and transitive imported archive must
                # remain usable from the parent after isolated discovery.
                backing = underlying.replace('::', '_') + '_archive'
                package += [f'add_library({backing} STATIC IMPORTED)',
                            f'set_property(TARGET {backing} PROPERTY IMPORTED_LOCATION '
                            f'{cmake_quote(archive)})',
                            f'add_library({underlying} INTERFACE IMPORTED)',
                            f'set_property(TARGET {underlying} PROPERTY '
                            f'INTERFACE_LINK_LIBRARIES {backing})']
            else:
                package += [f'add_library({underlying} {kind} IMPORTED)',
                            f'set_property(TARGET {underlying} PROPERTY IMPORTED_LOCATION '
                            f'{cmake_quote(archive)})']
            if alias:
                package.append(f'add_library({name} ALIAS {underlying})')
        if global_import:
            package = [line.replace(' IMPORTED)', ' IMPORTED GLOBAL)') for line in package]
        if targets:
            (prefix / 'minizConfig.cmake').write_text('\n'.join(package) + '\n')

        # Model the upstream target at FetchContent's source boundary. This
        # avoids downloads in the matrix; the real declaration's URL/hash are
        # checked below from CMake's expanded execution trace.
        fallback = root / 'fallback'
        fallback.mkdir()
        (fallback / 'miniz.c').write_text('int synthetic_miniz(void) { return 0; }\n')
        (fallback / 'CMakeLists.txt').write_text('''
cmake_minimum_required(VERSION 3.20)
project(synthetic_miniz C)
if(BUILD_SHARED_LIBS)
    message(FATAL_ERROR "FetchContent miniz must build with BUILD_SHARED_LIBS=OFF")
endif()
add_library(miniz miniz.c)
''')
        settings = dict(BUILD_SHARED_LIBS=shared, CMAKE_POLICY_DEFAULT_CMP0077=policy,
                        CMAKE_POLICY_VERSION_MINIMUM=minimum,
                        CMAKE_FIND_PACKAGE_TARGETS_GLOBAL='ON' if promote else 'OFF')
        before, after = [], []
        for key, value in settings.items():
            before.append(f'unset({key} CACHE)\nunset({key})' if value is None else
                          f'set({key} {cmake_quote(value)})')
            condition = f'DEFINED {key}' if value is None else (
                f'NOT DEFINED {key} OR NOT "${{{key}}}" STREQUAL "{value}"')
            after.append(f'if({condition})\nmessage(FATAL_ERROR '
                         f'"resolver did not restore {key}")\nendif()')
        (root / 'main.cpp').write_text('int main() { return 0; }\n')
        (root / 'CMakeLists.txt').write_text('\n'.join([
            'cmake_minimum_required(VERSION 3.24)',
            'project(miniz_resolution_acceptance LANGUAGES C CXX)',
            'set(CHIPCAE_ENABLE_ECAD ON)',
            f'set(CMAKE_PREFIX_PATH {cmake_quote(prefix)})',
            'set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)',
            'set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)',
            'set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)',
            'set(CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH OFF)',
            'set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)',
            'set(CMAKE_FIND_USE_PACKAGE_ROOT_PATH OFF)',
            # Keep unrelated optional dependencies out of this miniz fixture.
            'set(CMAKE_DISABLE_FIND_PACKAGE_Clipper2 ON)',
            'set(CMAKE_DISABLE_FIND_PACKAGE_EXPAT ON)',
            'add_library(Clipper2 INTERFACE)',
            'add_library(EXPAT::EXPAT INTERFACE IMPORTED)',
            'add_library(geometry INTERFACE)',
            'add_library(chipcae::geometry ALIAS geometry)',
            'function(chipcae_set_warnings)', 'endfunction()',
            f'set(FETCHCONTENT_SOURCE_DIR_MINIZ {cmake_quote(fallback)})',
            f'set(CMAKE_DISABLE_FIND_PACKAGE_miniz {"ON" if disabled else "OFF"})',
            *before,
            f'include({cmake_quote(SOURCE / "cmake/ChipcaeDependencies.cmake")})',
            *after,
            f'add_subdirectory({cmake_quote(SOURCE / "geometry/ecad")} ecad)',
            'if(TARGET miniz::miniz)', 'set(selected miniz::miniz)',
            'else()', 'set(selected miniz)', 'endif()',
            'get_target_property(kind ${selected} TYPE)',
            'file(WRITE "${CMAKE_BINARY_DIR}/selection.txt" "${selected}\\n${kind}\\n")',
            'add_executable(ecad_link main.cpp)',
            'target_link_libraries(ecad_link PRIVATE chipcae_geometry_ecad)',
        ]) + '\n')
        command = ['cmake', '-S', str(root), '-B', str(root / 'build'), '-G', 'Ninja',
                   '-DCMAKE_BUILD_TYPE=Release', '--trace-expand', '--trace-format=json-v1']
        # Repeat even failing configurations: a prior cache must not change the
        # verdict or leave a colliding public target on the next configure.
        results = [run(command), run(command)]
        for cycle, result in enumerate(results, 1):
            with self.subTest(cycle=cycle):
                self.assertEqual(result.returncode, 0,
                                 f'{shlex.join(command)}\n{result.stdout}\n' +
                                 '\n'.join(line for line in result.stderr.splitlines()
                                           if not line.startswith('{')))
        if any(result.returncode for result in results):
            return
        selected, kind = (root / 'build/selection.txt').read_text().splitlines()
        expected_name, expected_kind = next((item for item in targets if item[0] == 'miniz::miniz'),
                                            targets[0] if targets else ('miniz', 'STATIC'))
        rejected = expected_kind in ('SHARED', 'MODULE') or disabled or not targets
        self.assertEqual(kind, 'STATIC_LIBRARY' if rejected else expected_kind + '_LIBRARY')
        if not rejected:
            self.assertEqual(selected, expected_name)
        commands = run(['ninja', '-C', str(root / 'build'), '-t', 'commands', 'ecad_link'],
                       check=True).stdout
        # BUILD_SHARED_LIBS=ON legitimately makes ECAD itself shared. Inspect
        # the complete closure and reject only the synthetic package libraries.
        for name, package_kind in targets:
            if package_kind in ('SHARED', 'MODULE'):
                self.assertNotIn(str(prefix / (name.replace('::', '_') + '.so')), commands,
                                 'ECAD link closure contains synthetic shared miniz')
        if rejected:
            self.assertIn('libminiz.a', commands)
            declarations = []
            for line in results[-1].stderr.splitlines():
                if line.startswith('{'):
                    event = json.loads(line)
                    if event.get('cmd', '').lower() == 'fetchcontent_declare':
                        declarations.append(event['args'])
            miniz = next(item for item in declarations if item[0].lower() == 'miniz')
            self.assertEqual(miniz[miniz.index('URL') + 1],
                             'https://codeload.github.com/richgel999/miniz/tar.gz/refs/tags/3.0.2')
            self.assertEqual(miniz[miniz.index('URL_HASH') + 1],
                             'SHA256='
                             'c4b4c25a4eb81883448ff8924e6dba95c800094a198dc9ce66a292ac2ef8e018')
        else:
            self.assertIn(str(prefix / (expected_name.replace('::', '_') + '.a')), commands)

    def test_shared_and_module_packages_fall_back_without_name_collisions(self):
        for kind in ('SHARED', 'MODULE'):
            for names in (('miniz',), ('miniz::miniz',), ('miniz', 'miniz::miniz')):
                with self.subTest(kind=kind, names=names):
                    self.configure_case(tuple((name, kind) for name in names))

    def test_static_interface_and_unknown_packages_keep_their_archive(self):
        for kind in ('STATIC', 'INTERFACE', 'UNKNOWN'):
            for names in (('miniz',), ('miniz::miniz',), ('miniz', 'miniz::miniz')):
                with self.subTest(kind=kind, names=names):
                    self.configure_case(tuple((name, kind) for name in names))

    def test_aliases_and_transitive_imports_remain_visible_to_ecad(self):
        for kind in ('STATIC', 'INTERFACE', 'UNKNOWN'):
            with self.subTest(kind=kind):
                self.configure_case((('miniz::miniz', kind),), alias=True)

    def test_ecad_namespaced_target_precedence_controls_fallback(self):
        for plain, namespaced in (('STATIC', 'SHARED'), ('SHARED', 'STATIC')):
            with self.subTest(plain=plain, namespaced=namespaced):
                self.configure_case((('miniz', plain), ('miniz::miniz', namespaced)))

    def test_automatic_global_promotion_does_not_leak_rejected_targets(self):
        for kind in ('SHARED', 'INTERFACE'):
            with self.subTest(kind=kind):
                self.configure_case((('miniz::miniz', kind),), promote=True)

    def test_explicit_global_rejected_imports_do_not_occupy_fallback_names(self):
        for kind in ('SHARED', 'MODULE'):
            for names in (('miniz',), ('miniz::miniz',), ('miniz', 'miniz::miniz')):
                for alias in (False, True):
                    with self.subTest(kind=kind, names=names, alias=alias):
                        self.configure_case(tuple((name, kind) for name in names),
                                            global_import=True, alias=alias)

    def test_explicit_global_accepted_imports_remain_usable(self):
        for kind in ('STATIC', 'INTERFACE', 'UNKNOWN'):
            for alias in (False, True):
                with self.subTest(kind=kind, alias=alias):
                    self.configure_case((('miniz::miniz', kind),),
                                        global_import=True, alias=alias)

    def test_fallback_restores_shared_and_policy_settings(self):
        for package in ('missing', 'disabled', 'shared'):
            for shared, policy, minimum in ((None, None, None), ('OFF', 'OLD', '3.5'),
                                            ('ON', 'NEW', '3.24')):
                with self.subTest(package=package, shared=shared, policy=policy, minimum=minimum):
                    targets = () if package == 'missing' else (('miniz', 'SHARED'),)
                    self.configure_case(targets, disabled=package == 'disabled', shared=shared,
                                        policy=policy, minimum=minimum)


class AdapterFlagsAcceptance(unittest.TestCase):
    """Feed real Ninja/Make discovery, then inspect CMake's generated commands."""

    @contextmanager
    def probe(self, discovery, compile_flags, link_flags, *, build=False):
        with tempfile.TemporaryDirectory(prefix='adapter-flags-') as tmp:
            root = Path(tmp)
            binary = root / 'original build'
            binary.mkdir()
            for name in ('first.a', 'second.a'):
                (binary / name).write_bytes(b'!<arch>\n')
            compiler = shutil.which('c++')
            self.assertIsNotNone(compiler, 'a C++ compiler is required for the Release probe')
            compile_line = shlex.join([compiler, *compile_flags])
            link_line = shlex.join([compiler, *link_flags])
            if discovery == 'ninja':
                (binary / 'build.ninja').touch()
            else:
                directory = binary / 'apps/CMakeFiles/chipcae-solve.dir'
                directory.mkdir(parents=True)
                (directory / 'link.txt').write_text(link_line)
                (directory / 'flags.make').write_text(
                    'CXX_DEFINES = \nCXX_INCLUDES = \nCXX_FLAGS = ' +
                    shlex.join(compile_flags) + '\n')
            generated = root / 'probe'
            generated.mkdir()
            contract = root / 'contract'
            contract.mkdir()
            (contract / 'dump_actual.cpp').write_text('int main() { return 0; }\n')
            real_run = subprocess.run

            def execute(args, **kwargs):
                if args[0] == 'ninja' and str(binary) in args:
                    return subprocess.CompletedProcess(args, 0,
                        compile_line + '\n: && ' + link_line + ' && :\n', '')
                if args[0] == 'cmake' and '--build' in args and not build:
                    # Darwin driver flags cannot execute on a Linux compiler.
                    # Configuration and command generation still use real CMake.
                    return subprocess.CompletedProcess(args, 0, '', '')
                if args[0] == 'cmake' and '-S' in args:
                    with (generated / 'CMakeLists.txt').open('a') as stream:
                        for target in TARGETS:
                            stream.write('file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/' + target +
                                         '.options" CONTENT "' + '\\n'.join(
                                             '$<TARGET_PROPERTY:' + target + ',' + prop + '>'
                                             for prop in ('COMPILE_OPTIONS', 'LINK_OPTIONS',
                                                          'LINK_LIBRARIES')) + '\\n")\n')
                return real_run(args, **kwargs)

            environment = dict(CHIPCAE_BINARY_DIR=str(binary),
                               CHIPCAE_SOLVE_EXE=str(binary / 'apps/chipcae-solve'))
            # Keep the parser, schema copy, generated project, CMake configure,
            # and (for Linux flags) build real. Only replace unrelated large TUs.
            with patch.dict(os.environ, environment), \
                    patch.object(util, 'ADAPTER_PROBE', 'int main() { return 0; }\n'), \
                    patch.object(util, 'ELASTIC_REPLAY', 'int main() { return 0; }\n'), \
                    patch.object(util, 'PIPELINE_FAILURE_PROBE', 'int main() { return 0; }\n'), \
                    patch.object(util.subprocess, 'run', side_effect=execute):
                returned = util.build_adapter_probe(generated, SOURCE, contract)
            self.assertEqual(returned, (generated / 'build/adapter_probe',
                                        generated / 'build/contract_probe',
                                        generated / 'build/elastic_replay'))
            commands = run(['ninja', '-C', str(generated / 'build'), '-t', 'commands'],
                           check=True).stdout.splitlines()
            properties = {target: (generated / 'build' / (target + '.options')).read_text()
                          .splitlines() for target in TARGETS}
            yield SimpleNamespace(binary=binary, commands=commands, properties=properties)

    def assert_pairs(self, tokens, pairs):
        # Compare the entire sequence of occurrences, including repeated flags;
        # merely finding one adjacent pair misses CMake option deduplication.
        for flag in dict(pairs):
            actual = [tokens[index + 1] if index + 1 < len(tokens) else '<missing>'
                      for index, token in enumerate(tokens) if token == flag]
            self.assertEqual(actual, [value for option, value in pairs if option == flag],
                             f'{flag} lost its immediately following operand: {tokens}')

    def assert_shell_pairs(self, options):
        groups = [shlex.split(item[6:]) for item in options if item.startswith('SHELL:')]
        for pair in PAIRS:
            self.assertTrue(any(group[index:index + 2] == list(pair)
                                for group in groups for index in range(len(group))),
                            f'{pair} is not grouped in SHELL options: {options}')

    def test_paired_compile_options_survive_cmake_deduplication_for_every_probe(self):
        flags = [token for pair in PAIRS for token in pair]
        for discovery in ('ninja', 'make'):
            with self.subTest(discovery=discovery), self.probe(
                    discovery, [*flags, '-o', 'old.o', '-c', 'fvm_pipeline.cpp'],
                    ['old.o', '-o', 'chipcae-solve', '-lm']) as probe:
                for target in TARGETS:
                    with self.subTest(target=target):
                        command = next(line for line in probe.commands
                                       if target + '.dir/' in line and ' -c ' in line)
                        self.assert_pairs(shlex.split(command), PAIRS)
                        self.assert_shell_pairs(probe.properties[target][0].split(';'))

    def test_paired_link_options_remain_driver_options_for_every_probe(self):
        flags = [token for pair in PAIRS for token in pair]
        for discovery in ('ninja', 'make'):
            with self.subTest(discovery=discovery), self.probe(
                    discovery, ['-O2', '-c', 'fvm_pipeline.cpp', '-o', 'old.o'],
                    ['old.o', *flags, '-o', 'chipcae-solve', 'first.a', '-lm']) as probe:
                for target in TARGETS:
                    with self.subTest(target=target):
                        command = next(line for line in probe.commands if f'-o {target} ' in line)
                        tokens = shlex.split(command)
                        self.assert_pairs(tokens, PAIRS)
                        for _, value in PAIRS:
                            self.assertNotIn('-l' + value, tokens)
                        self.assert_shell_pairs(probe.properties[target][1].split(';'))
                        self.assertNotIn('old.o', tokens)
                        self.assertNotIn('chipcae-solve', tokens)

    def test_linux_flags_includes_relative_archives_and_linker_order(self):
        includes = ('-I', '-isystem', '-iquote')
        flags = ['-O2', '-DNDEBUG', '-pthread']
        for flag in includes:
            flags += [flag, '/include with spaces/' + flag[1:]]
        flags += ['-MMD', '-MT', 'old.o', '-MF', 'old.d', '-MQ', 'quoted.o',
                  '-o', 'old.o', '-c', 'fvm_pipeline.cpp']
        libraries = ['-Wl,--start-group', 'first.a', '-Wl,--whole-archive', 'second.a',
                     '-Wl,--no-whole-archive', '-Wl,--end-group', 'm']
        for discovery in ('ninja', 'make'):
            with self.subTest(discovery=discovery), self.probe(
                    discovery, flags, ['-O2', '-pthread', 'old.o', 'old.obj', '-o',
                                       'chipcae-solve', *libraries],
                    build=sys.platform.startswith('linux')) as probe:
                for target in TARGETS:
                    with self.subTest(target=target):
                        compile_line = next(line for line in probe.commands
                                            if target + '.dir/' in line and ' -c ' in line)
                        tokens = shlex.split(compile_line)
                        for flag in ('-O2', '-DNDEBUG', '-pthread'):
                            self.assertIn(flag, tokens)
                        for flag in includes:
                            path = '/include with spaces/' + flag[1:]
                            self.assertTrue(flag + path in tokens or any(
                                tokens[index:index + 2] == [flag, path]
                                for index in range(len(tokens))))
                        for removed in ('old.o', 'old.obj', 'old.d', 'quoted.o',
                                        'fvm_pipeline.cpp'):
                            self.assertNotIn(removed, tokens)
                        link = shlex.split(next(line for line in probe.commands
                                                if f'-o {target} ' in line))
                        expected = [str(probe.binary / item) if item.endswith('.a') else
                                    '-lm' if item == 'm' else item
                                    for item in libraries]
                        positions = [link.index(item) for item in expected]
                        self.assertEqual(positions, sorted(positions), link)
                        for removed in ('old.o', 'old.obj', 'chipcae-solve'):
                            self.assertNotIn(removed, link)

    def test_unpaired_driver_flags_use_link_options(self):
        flags = ['-O2', '-pthread', '-Wl,-rpath,/synthetic-runtime']
        for discovery in ('ninja', 'make'):
            with self.subTest(discovery=discovery), self.probe(
                    discovery, ['-c', 'fvm_pipeline.cpp', '-o', 'old.o'],
                    [*flags, 'old.o', '-o', 'chipcae-solve', '-lm']) as probe:
                for target in TARGETS:
                    with self.subTest(target=target):
                        options = probe.properties[target][1].split(';')
                        option_tokens = [token for item in options for token in (
                            shlex.split(item[6:]) if item.startswith('SHELL:') else [item])]
                        libraries = probe.properties[target][2].split(';')
                        for flag in flags:
                            self.assertIn(flag, option_tokens)
                            self.assertNotIn(flag, libraries)

    def test_missing_paired_operands_name_the_flag(self):
        for discovery in ('ninja', 'make'):
            for flag in dict(PAIRS):
                for stage in ('compile', 'link'):
                    with self.subTest(discovery=discovery, flag=flag, stage=stage):
                        compile_flags = ['-c', 'fvm_pipeline.cpp', '-o', 'old.o']
                        link_flags = ['old.o', '-o', 'chipcae-solve']
                        (compile_flags if stage == 'compile' else link_flags).append(flag)
                        with self.assertRaisesRegex((ValueError, AssertionError),
                                                    '.*' + flag + '.*'):
                            with self.probe(discovery, compile_flags, link_flags):
                                pass


class EntryPointEnvironmentAcceptance(unittest.TestCase):
    def test_missing_environment_skips_before_build_acceptance(self):
        import test_fvm_case_e2e as entry
        required = ('CHIPCAE_IMPORT_EXE', 'CHIPCAE_PREVIEW_EXE', 'CHIPCAE_SOLVE_EXE',
                    'CHIPCAE_FVM_LOADER_PROBE', 'CHIPCAE_MPIEXEC',
                    'CHIPCAE_MPIEXEC_NUMPROC_FLAG')
        with patch.dict(os.environ):
            for key in required:
                os.environ.pop(key, None)
            with patch.object(entry.mac_static_miniz_acceptance, 'suite',
                              side_effect=AssertionError('build acceptance ran before skip')):
                self.assertEqual(entry.main(), 77)


def suite():
    return unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(cls)
                              for cls in (MinizResolutionAcceptance, AdapterFlagsAcceptance,
                                          EntryPointEnvironmentAcceptance))


if __name__ == '__main__':
    unittest.main(verbosity=2)
