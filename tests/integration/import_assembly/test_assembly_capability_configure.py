"""Capability declarations must agree with linked implementations."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
from assembly_test_support import SOURCE

text = (SOURCE / 'kernel/CMakeLists.txt').read_text()
start = text.index('function(chipcae_assembly_capability ')
function = text[start:text.index('endfunction()', start) + len('endfunction()')]

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    stub = 'int implementation() { return 0; }\n'
    digest = hashlib.sha256(stub.encode()).hexdigest()
    for name, source, declaration, override, linked, expected in (
        ('known_stub', stub, '', '', True, 0),
        ('stub_declared_available', stub, 'ON', '', True, 1),
        ('cache_contradiction', stub, '', 'ON', True, 1),
        ('unknown_empty_stub', 'int implementation() {}\n', '', '', True, 1),
        ('implemented_comment', '// not implemented is harmless commentary\nint implementation() { return 1; }\n',
         'ON', '', True, 0),
        ('missing_link', stub, 'ON', '', False, 1),
    ):
        case = root / name
        case.mkdir()
        (case / 'implementation.cpp').write_text(source)
        (case / 'other.cpp').write_text('int other();\n')
        config = ('cmake_minimum_required(VERSION 3.24)\nproject(capability CXX)\n'
                  + function + '\nadd_library(implementation STATIC '
                  + ('implementation.cpp' if linked else 'other.cpp') + ')\n')
        if declaration:
            config += ('set_property(TARGET implementation PROPERTY OWNER_AVAILABLE '
                       + declaration + ')\n')
        config += ('chipcae_assembly_capability(DETECTED implementation OWNER_AVAILABLE '
                   'implementation.cpp=' + digest + ')\n')
        config += ('if(NOT DETECTED STREQUAL "' + (declaration or 'OFF') + '")\n'
                   'message(FATAL_ERROR "wrong detected capability")\nendif()\n')
        (case / 'CMakeLists.txt').write_text(config)
        args = ['cmake', '-G', 'Ninja', '-S', str(case), '-B', str(case / 'build')]
        if override:
            args.append('-DDETECTED=' + override)
        result = subprocess.run(args, capture_output=True, text=True)
        assert (result.returncode == 0) == (expected == 0), (name, result.stdout, result.stderr)

        if expected:
            reason = {
                'stub_declared_available': 'contradicts the linked SP0 stub',
                'cache_contradiction': 'contradicts capability of linked target',
                'unknown_empty_stub': 'must declare target property',
                'missing_link': 'does not link capability source',
            }[name]
            assert reason in result.stderr, (name, result.stderr)
