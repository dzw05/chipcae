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
The current I14 1.0 library may reject embedded geometry with exit 13 and no
successful output or elastic diagnostic. A future supported library must
publish finite nonzero cut/EB U with matching summaries and EB coordinates/U
across output modes. This adapter passes the actual geometry; it never
substitutes an uncut mesh. The supported-cut reconstruction path cannot yet
be validated end to end against the current rejecting library.

All previous loader, schema, thermal, strict energy ledger, Wiener, MPI,
physical volume, copper diagnostics, negative input and FEM snapshot
assertions remain. In particular, the older thermal cut-success tests still
require success; they conflict with the current elastic cut limitation and
are not skipped or weakened. Frozen FEM snapshots predate upstream changes
to copper fields and conductivity parsing.

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

Known blockers outside the adapter's allowed scope:

- I14 section 6 prohibits parsing numbers from failure diagnostics, while
  `Expected<ElasticResult>` returns only `Error{path, reason}` on failure.
  Measured failure residual events require a producer/API ruling; the test
  stays in place without fabricated data or a frozen-header change.
- The results writer omits regular-hex `CellData/volume` supplied by the adapter.
- The imported board retains its 1% temperature-rise comparison gate; the
  earlier thermal baseline differed by 7.96%. Embedded-Robin energy and
  pre-upstream FEM snapshots also have known failures.
- I14 remains draft. Implementation is exempted, but merging requires freezing
  and green elastic logs from the specified nightly or Mac full-deps runner.
  Local MFEM/PETSc results do not establish that runner-origin merge gate.
