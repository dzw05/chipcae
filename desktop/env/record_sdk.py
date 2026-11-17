"""Record installed conda artifacts and key ABI inputs; run from repository root."""
import hashlib
import json
from pathlib import Path

prefix = Path('/workspace/tools/chipcae-desktop')
jsoncons = Path('/workspace/deps/jsoncons')
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
packages = []
for p in sorted((prefix / 'conda-meta').glob('*.json')):
    meta = json.loads(p.read_text())
    packages.append({k: meta.get(k) for k in ('name', 'version', 'build', 'url', 'sha256')})
paths = ['bin/protoc', 'bin/x86_64-conda-linux-gnu-c++',
         'lib/libQt6Core.so.6', 'lib/libQt6Gui.so.6', 'lib/libQt6Widgets.so.6',
         'lib/libQt6OpenGL.so.6', 'lib/libQt6OpenGLWidgets.so.6',
         'lib/libvtkGUISupportQt-9.7.so', 'lib/libvtkIOHDF-9.7.so',
         'lib/libvtkChartsCore-9.7.so', 'lib/libvtkViewsContext2D-9.7.so',
         'lib/libvtkRenderingContextOpenGL2-9.7.so', 'lib/libvtkRenderingOpenGL2-9.7.so',
         'lib/libprotobuf.so', 'lib/libstdc++.so.6',
         'lib/cmake/vtk-9.7/vtk-config.cmake', 'lib/cmake/protobuf/protobuf-config.cmake']
hashes = {str(prefix / p): sha(prefix / p) for p in paths}
# Lock all header-only schema implementation inputs, not just its version macro.
for p in sorted(jsoncons.rglob('*')):
    if p.is_file():
        hashes[str(p)] = sha(p)
lock = {'format': 1, 'prefix': str(prefix), 'packages': packages, 'sha256': hashes,
        'probe': {'qt': '6.12.0', 'vtk': '9.7.2', 'vtk_qt_major': 6,
                  'compiler': 'GNU 15.3.0, x86_64-conda-linux-gnu, C++11 libstdc++ ABI',
                  'protobuf_package': '7.35.1', 'protobuf_cmake': '35.1.0',
                  'protoc': 'libprotoc 35.1', 'jsoncons': '1.4.0 (existing header-only prefix)',
                  'modules': 'GUISupportQt IOHDF ChartsCore ViewsContext2D RenderingContextOpenGL2',
                  'actual_tests': '2026-11-17: CTest 5/5; EGL PNG 320x240 range 13:234; Xvfb embedded show exit 0',
                  'abi_macros': '_GLIBCXX_USE_CXX11_ABI=1; __GXX_ABI_VERSION=1020',
                  'schema_test': 'draft 2020-12 prefixItems/items and unevaluatedProperties/allOf'}}
# JSON is valid YAML 1.2; no YAML library needed.
Path('desktop/env/desktop-linux.lock.yml').write_text(json.dumps(lock, indent=2) + '\n')
cmake = ['# Generated from desktop-linux.lock.yml; fail closed on SDK drift.',
         'function(chipcae_verify_sdk)']
for path, digest in hashes.items():
    cmake += [f'    file(SHA256 "{path}" actual)',
              f'    if(NOT actual STREQUAL "{digest}")',
              f'        message(FATAL_ERROR "Locked SDK changed: {path}")', '    endif()']
cmake += ['endfunction()', 'chipcae_verify_sdk()']
Path('desktop/cmake/SDKLock.cmake').write_text('\n'.join(cmake) + '\n')
