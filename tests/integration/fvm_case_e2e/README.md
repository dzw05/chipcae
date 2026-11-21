# FVM case CLI acceptance

`fvm_fvm_case_e2e` selects `fvm.conduction` / `fvm.solid` through
`physics[].solver`, without adding CLI arguments. It is registered with labels
`integration;numerical;fvm`, timeout 1200 seconds and skip return code 77.
It exercises the real loader, mesh, thermal and elastic solvers, results
writer and event stream using temporary synthetic fixtures. CMake supplies
the configured executables and MPI launcher. Missing executables or numpy/h5py
return 77; configured command failures fail acceptance. Schema validation
alone skips if jsonschema is missing.

The mechanics gates require solved, finite, nonzero U and positive warpage
for the board and heated bilayer. Summary methods must equal
`FVM top-face centroids; least-squares plane removal` and `FVM cell centers`.
The one rank-0 level-1 `FVM_ELASTIC ` record has exactly four unique keys:
`iterations`, `residual`, `warpage_pv`, `U_mag_max`. Iterations must be a
nonnegative integer and all other values finite and nonnegative. The log,
manifest and final `fvm.solid`/`U` residual agree to relative tolerance 1e-12.
The former displacement fallback is forbidden in adapter source and output.

Board and uniform bilayer FVM/FEM-direct PV must agree within 10%, with the
same signed top quadratic curvature after plane removal. Tests print both
PVs, both U maxima and their relative differences. Each process reports wall
time and measured peak child RSS where wait4 is available; MPI RSS is a
maximum, not a rank sum. These measurements are not performance thresholds.
Zero temperature rise retains normal methods and requires PV <= 1e-14 times
characteristic length and all displacement components <= 1e-14 m. MPI np=1/2
aligns by parent GID and vertex coordinates, using global relative U and PV
tolerances of 1e-8. Rigid321, fixed bottom and a nonfirst fixed layer exercise
support mapping and prescribed zero vertices, including fixed/free edges.

Nonconvergence must exit 1 with measured residuals and no successful output.
STOP fixtures cover prequeued commands and a FIFO that queues STOP after the
first control acknowledgment. The second checkpoint must acknowledge it once,
end with `user_stop`, and publish no output. The FIFO tests checkpoint ordering,
not cancellation inside the elastic library.

Cut mechanics tests exercise both polyhedron and `--cut-output hex` output.
Frozen I14 1.0 supports embedded faces and cut CVs. Both output modes must
succeed, publish finite nonzero cut/EB U, and agree on summaries and EB
coordinates/U. The earlier exit-13 checks remain for diagnostic coverage,
but cannot satisfy the supported-cut success gate. The adapter passes
`is_cut` in caller order, including merged CVs, and converts grid side 6 to
I14's embedded side -1. No uncut substitution is used.

All previous loader, schema, thermal, strict energy ledger, Wiener, MPI,
physical volume, copper diagnostics, negative input and FEM snapshot
assertions remain. The merge retains four additional upstream tests for
FVM-only volume selection, loader error precedence, fixed-temperature
roundoff and split-layer convection. Frozen FEM snapshots predate upstream
changes to copper fields and conductivity parsing; they are preserved.

The decoder's negative fixtures reject wrong levels/ranks, missing or repeated
records, duplicate/missing/extra keys, invalid numeric types, nonfinite values,
CRC errors, truncated frames and trailing payload bytes. Partition fixtures
exercise reordered ranks and deliberately perturbed U/T. Curvature fixtures
verify rigid translation/tilt invariance.

Use plain shared build-slot wrappers with at most two build jobs:

```sh
cmake --build "$build" -j2
ctest --test-dir "$build" -N -L fvm
ctest --test-dir "$build" -R '^fvm_fvm_case_e2e$' --output-on-failure
ctest --test-dir "$build" -R 'fvm_|elastic' --output-on-failure
ctest --test-dir "$build" -L integration -R 'f1130|demo_gate' --output-on-failure
ctest --test-dir "$build" -R solve_ --output-on-failure
ctest --test-dir "$build" --output-on-failure
```

Supply local dependency prefixes as needed and retain the shared shim's PATH
precedence. Full numerical builds require MFEM with MPI and PETSc, ECAD,
jsoncons and HDF5. Bare configuration keeps those dependencies optional.

Local Release measurements (实测, synthetic inputs, MFEM with MPI/PETSc):

| Fixture | FVM PV (m) | FEM/direct PV (m) | Relative PV difference | FVM U maximum (m) | FEM U maximum (m) | Relative U difference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Imported board | 1.3821524030478399e-6 | 7.6705217582044357e-6 | 0.81980985823168007 | 9.2238241640278916e-7 | 7.5643159714428214e-6 | 0.87806135810759189 |
| Uniform bilayer | 0.0017444475179898399 | 0.0019414505357316298 | 0.1014720767364541 | 0.0012533119660306468 | 0.0020458574104428601 | 0.3873903627724738 |

| Fixture / process | Wall seconds | Measured peak RSS (KiB) |
| --- | ---: | ---: |
| Board FVM np=1 | 1.756074 | 54592 |
| Board FVM np=2 | 1.782220 | 57248 |
| Board FEM/direct | 1.689694 | 64276 |
| Bilayer FVM | 1.598403 | 60916 |
| Bilayer FEM/direct | 1.963532 | 107824 |

RSS is the wait4 process/child maximum, including the MPI launcher, rather
than a sum across ranks. Timings are observations under shared load.
Supplemental fitted top curvatures have matching negative signs: board
FVM -4.4003741123158115e-6 versus FEM -2.9873172891963482e-5; bilayer
FVM -0.012207036266297606 versus FEM -0.014372943771064434.

The four I14 diagnostics are measured through a local probe using the same
adapter helpers and numerical libraries; the production event retains exactly
its required four keys:

| Fixture | min_dn_ratio | min_dn_gid | gradient_corrections | max_identity_defect |
| --- | ---: | ---: | ---: | ---: |
| Board (80 CVs) | 1 | 0 | 0 | 0 |
| Bilayer (192 CVs) | 1 | 0 | 0 | 0 |
| Cut bilayer (192 CVs, 12 cut) | 0.79262398910460019 | 8 | 0 | 4.4408920985006262e-16 |

Board maximum CV temperature is 321.92657327984955 K versus FEM
323.98160735460056 K, with reference 298.15 K. The temperature-rise relative
difference is 0.079555021355843233, above the unchanged 1% gate. A supplemental
thermal probe gives maximum face temperature 323.20710930981846 K and maximum
vertex temperature 323.13920209610802 K; sampling location explains part of
the gap, while the remaining thermal discretization difference is unresolved.
The adapter retains the specified maximum-CV `T_max` semantics.

The focused adapter/contract harness passes 5/5 checks, including all 14 I14
fixtures. It checks caller-order cut flags for regular/cut meshes, double-prefix
INTERNAL routing, malformed-result rejection and the library contract. These
supplemental checks are local artifacts; repository acceptance remains in the
two Python files. The registered e2e runs 42 unittest methods with 14 assertion
failures and zero errors. The FVM/library group passes 10/11 tests (only e2e
fails); the selected integration group passes 2/2. The `solve_` regression group passes 95 tests, fails 3 and retains 10 existing
disabled tests (108 registered / 98 enabled). The failures reproduce the prior
column-GMG killed-process cases and FEM thermal-bow cross-run tolerance gate.
The complete suite records 437 passed, 9 failed, 26 skipped and 10 existing
disabled tests (482 registered / 472 enabled), 1569.39 seconds wall. Its e2e
again reports 42 methods and 14 assertion failures. Additional failures include
three field-comparison fixtures rejected by FEM input validation, a fine-grid
cut-bimetal subprocess killed with signal 9, and the synthetic warpage driver's
W5 auxiliary assertion. None of those fixtures or their gates is modified.

Remaining gates and owners:

- Producer/API: I14 section 6 prohibits parsing failure diagnostic numbers,
  while `Expected<ElasticResult>` exposes only `Error{path, reason}` on failure.
  All four solid-only/coupled nonconvergence assertions (np=1/2) still require
  measured residuals. No norm is fabricated and no frozen header is changed.
- Solver/thermal investigation: the board and bilayer comparisons above exceed
  their original gates. Adapter array mapping passes the local checks; the
  numerical discrepancy is not corrected by rescaling output or changing case
  options. Its full numerical root cause remains unresolved.
- Elastic producer: demo geometry reaches the unchanged 500-iteration limit
  in both output modes and with embedded Robin heat transfer. It returns exit 1
  and no successful step. Supported synthetic cut reconstruction does pass.
- Loader/fixture owner: four FEM snapshots omit current default copper diagnostic
  fields; they remain unchanged under this brief's preservation requirement.
- I14 is frozen as of November 21. Green `ci-ok` and elastic logs from the
  specified nightly or Mac full-deps runner remain separate merge requirements.
  Local green library logs do not establish the runner-origin gate.
- Real-board runs and their commands, logs and result hashes remain local and
  outside commits. No real-board result is claimed here.
