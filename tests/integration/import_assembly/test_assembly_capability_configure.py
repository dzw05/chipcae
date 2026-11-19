"""Capability declarations must agree with linked implementations."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from assembly_test_support import SOURCE

text = (SOURCE / 'kernel/CMakeLists.txt').read_text()
start = text.index('function(chipcae_assembly_capability ')
function = text[start:text.index('endfunction()', start) + len('endfunction()')]

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    for name, declaration, override, expected in (
        ('default', '', '', 'OFF'),
        ('owner_enabled', 'ON', '', 'ON'),
        ('explicit_enabled', '', 'ON', 'ON'),
        ('explicit_disabled', 'ON', 'OFF', 'ON'),
    ):
        case = root / name
        case.mkdir()
        config = ('cmake_minimum_required(VERSION 3.24)\nproject(capability NONE)\n'
                  + function + '\nadd_library(implementation INTERFACE)\n')
        if declaration:
            config += ('set_property(TARGET implementation PROPERTY OWNER_AVAILABLE '
                       + declaration + ')\n')
        config += 'add_library(chipcae::geometry ALIAS implementation)\n'
        config += 'chipcae_assembly_capability(DETECTED chipcae::geometry OWNER_AVAILABLE)\n'
        config += ('if(NOT DETECTED STREQUAL "' + expected + '")\n'
                   'message(FATAL_ERROR "wrong detected capability")\nendif()\n')
        (case / 'CMakeLists.txt').write_text(config)
        args = ['cmake', '-G', 'Ninja' if shutil.which('ninja') else 'Unix Makefiles',
                '-S', str(case), '-B', str(case / 'build')]
        if override:
            args.append('-DDETECTED=' + override)
        result = subprocess.run(args, capture_output=True, text=True)
        assert result.returncode == 0, (name, result.stdout, result.stderr)

    # Reuse the same cache across an owner implementation becoming available.
    case = root / 'default'
    config = (case / 'CMakeLists.txt').read_text()
    config = config.replace('add_library(chipcae::geometry ALIAS implementation)',
                            'set_property(TARGET implementation PROPERTY OWNER_AVAILABLE ON)\n'
                            'add_library(chipcae::geometry ALIAS implementation)')
    config = config.replace('STREQUAL "OFF"', 'STREQUAL "ON"')
    (case / 'CMakeLists.txt').write_text(config)
    result = subprocess.run(['cmake', '-S', str(case), '-B', str(case / 'build')],
                            capture_output=True, text=True)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert 'cached OFF' in result.stderr, result.stderr
