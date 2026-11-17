# Installing chipcae

Configure a Release build with the required backends, then run:

```sh
cmake --build build -j3
cmake --install build --prefix "$HOME/opt/chipcae"
cpack --config build/CPackConfig.cmake -G TGZ
```

The TGZ is named `chipcae-<version>-<os>-<arch>.tar.gz`, using the project
version and lowercased CMake system name and processor: for example,
`chipcae-0.1.0-linux-x86_64` or `chipcae-0.1.0-darwin-arm64`.
Extract with `tar -xzf chipcae-<version>-<os>-<arch>.tar.gz`, then move the
extracted directory wherever desired and add its `bin` directory to PATH.
No root access or system installation script is required.

GNUInstallDirs controls the installation layout. With default directories:

- `bin/`: chipcae-solve, chipcae-preview, chipcae-import.
- `share/chipcae/schema/`: stack and material schema JSON files, flattened into
  one directory.
- `share/chipcae/protocol/errors.yaml`: protocol error declarations.
- `share/chipcae/examples/`: fcbga_demo case (JSON and YAML), stack, complete
  material library, and local.run-profile.json. Disable with
  `CHIPCAE_INSTALL_EXAMPLES=OFF` (the acceptance suite requires examples ON).
- `share/doc/chipcae/`: LICENSE, THIRD_PARTY_NOTICES, and material/LICENSE-NOTES.md
  (installed as LICENSE-NOTES.md). The latter supplements the dependency policy
  and third-party declarations with material component licensing information.

chipcae-kernel and chipcae-jobmgr currently expose skeleton developer entry
points and are excluded. Libraries, headers, and third-party binaries are not
installed as part of this distribution.

Schema lookup first uses a nonempty `CHIPCAE_DATA_DIR`, interpreted as the data
root containing `schema/`. Relative overrides are relative to the working
directory. An invalid nonempty override is authoritative and produces the existing
schema loading error; an empty override behaves as unset. Otherwise lookup uses
the real executable directory plus `../share/chipcae`, then a compiled relative
path from the build executable directory to the source tree for development.
Linux uses `/proc/self/exe`; macOS uses `_NSGetExecutablePath` and resolves
symlinks. Custom GNUInstallDirs layouts that differ from `bin` and `share` should
set `CHIPCAE_DATA_DIR` explicitly.

MPI, HDF5, PETSc, and other enabled shared dependencies must be installed
separately with compatible ABI versions and visible to the platform loader.
The numerical solver requires the MPI/MFEM/PETSc/HDF5 backend; preview requires
JSONCONS and HDF5. Reduced builds can install the CLIs but cannot perform every
operation. Dependencies are never bundled. On macOS executables retain external
library references and use an install rpath of `@loader_path/../lib` plus external
link directories for dependencies using `@rpath`. TGZ packaging performs no
signing or notarization. macOS execution must be verified on a macOS host.

For the default layout, from the extracted package directory:

```sh
bin/chipcae-solve --version
bin/chipcae-import --help
bin/chipcae-preview share/chipcae/examples/fcbga_demo.stack.json -o /tmp/chipcae-preview
mpiexec -n 4 bin/chipcae-solve --case share/chipcae/examples/fcbga_demo.case.json \
  --profile share/chipcae/examples/local.run-profile.json --out /tmp/chipcae-solve
```

The local profile requires four MPI ranks. Serial acceptance runs reuse the
existing single-rank test profile. CTest maps only the specific unavailable
numerical backend diagnostic with NOT_IMPLEMENTED exit code to skip 77; input
and schema errors remain failures.

Run `ctest --test-dir build -L install --output-on-failure` to check install,
relocation, and TGZ extraction. Tests temporarily hide source schema directories,
restore them even on failure, and run serially to avoid interfering with other
CTest tests.
