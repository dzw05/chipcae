# O2 uncut solver

`chipcae-solve --case case.json --profile run_profile.json --out run_dir`

The numerical build requires JSONcons, yaml-cpp, MPI, HDF5 and MFEM built with
PETSc. Enable `CHIPCAE_ENABLE_JSONCONS`, `CHIPCAE_ENABLE_MPI`,
`CHIPCAE_ENABLE_HDF5` and `CHIPCAE_ENABLE_MFEM`, and set the corresponding package
paths. yaml-cpp 0.8.0 (MIT) is fetched if no installed package is found. With
CMake 4, configure with `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for yaml-cpp.
Bare builds retain `--help` and report an unavailable numerical backend.

JSON and YAML cases resolve relative file references against their containing
file. Optional SHA-256 checks precede parsing. Materials use the B1 container
`{schema: "chipcae.material_library/1", materials: [material.v1 cards...]}`;
IDs must be unique, and all layer, region and copper material names must map.
O2 supports constant, isotropic thermoelastic cards with single-knot E, nu and
alpha tables. Unsupported constitutive models are rejected explicitly. Steady
conduction also requires positive scalar `conductivity` in W/(m K) on each card.
The material schema defines this optional scalar property; the conduction
loader requires it. Copper fractions use M1's orthotropic laminate
rules and arithmetic in-plane / harmonic normal conductivity. Polygon copper
maps and geometries requiring cuts report CUT_UNSUPPORTED; aligned boxes work.

The executable builds the uncut tensor grid, assembles FP64 H1 Q2 conduction
and thermal elasticity, and solves with PETSc's built-in sequential sparse LU
plus residual-checked iterative refinement. Each MPI rank solves a redundant
global system, appropriate for demonstration meshes; this does not provide
scalable distributed assembly. Result cells follow the unsplit Morton column
ownership. `column_gmg` emits a warning and falls back to this direct solver.
3-2-1 fixes xyz at the bottom xmin/ymin corner, yz at xmax/ymin and z at
xmin/ymax. Fixed face constraints are also supported.

Results use O1's VTKHDF writer, sampled at exact Q2 vertex nodes. Selected fields
are honored; summaries use all Q2 nodes. `warpage_pv` removes the least-squares
plane from top-surface Q2 displacement samples. The manifest includes T_max,
U_mag_max, warpage_pv and the global mesh hash, independent of MPI size.
`events.bin` contains CAELOG1 state, progress, residual, warning, output,
control acknowledgement and exit records. Complete `control.jsonl` lines are
checked after each physics field; STOP acknowledges the last completed stage
and exits with reason `user_stop`. Partial lines wait, malformed lines warn,
old sequences are ignored and unsupported actions are rejected.

`solve_inputs` checks CLI, JSON/YAML, references, material schemas/mappings,
integer bounds and profiles. `solve_mesh` checks the 128-cell/two-owner grid.
With the numerical backend, `solve_pipeline` exercises Timoshenko curvature,
the three-layer prototype reference, fixed-temperature/convection/source
conduction, np=1/2 consistency, field selection and STOP, using independent
h5py/numpy readback. Select a Python interpreter containing these packages via
`Python3_EXECUTABLE`. Existing O1 tests provide additional VTK reader checks.
