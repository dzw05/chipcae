# O2 input and mesh groundwork (incomplete solver)

`chipcae-solve --case case.json --profile run_profile.json --out run_dir`

This target currently validates JSON inputs when `CHIPCAE_ENABLE_JSONCONS=ON`,
then exits with `NOT_IMPLEMENTED` (2). It does not run finite elements, write
results, create the output directory, or claim completion. `--help` works in a
bare build. Without jsoncons, a run also fails explicitly with code 2.

The input library resolves case references relative to the case, checks SHA-256,
uses the existing stack schema loader, checks temperature-source exclusivity,
IDs, targets, profile rank count, mesh and linear parameters, and output fields.
`build_mesh` uses the uncut tensor builder and checks column ownership. It is
available to callers but is deliberately not invoked by the incomplete CLI.

Build and exercise the available front end:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_ENABLE_JSONCONS=ON -Djsoncons_DIR=/path/to/jsoncons/share/cmake/jsoncons
cmake --build build
ctest --test-dir build --output-on-failure
```

## Deviations and outstanding work

- This is groundwork, not completion of the O2 brief. FEM thermal mechanics,
  conduction, PETSc LU selection/refinement, GMG fallback, field sampling,
  warpage/summary computation, event orchestration, C3 STOP, and VTKHDF output
  integration remain unimplemented. The existing FEM stub is unchanged.
- The required-capability configuration fails at MPI discovery in the current
  environment. No dependencies were installed or rebuilt. Numerical acceptance,
  solve timings and np=1/2 differences were not measured.
- C1/C2/C3 are absent. Validation here is a semantic subset of the brief, not a
  replacement JSON schema validator. In particular unknown properties are not
  comprehensively rejected. YAML has no existing parser and is unsupported.
- Material JSON is read for syntactic validity only. `material.v1` in this tree
  describes a single card; a library container, layer mapping, conductivity
  source, and the full anisotropic constitutive adapter need an agreed contract.
  No material property defaults or new library conventions were introduced.
- Copper polygon references and all inclusions are conservatively rejected as
  `CUT_UNSUPPORTED`, including potentially aligned inclusions. Uniform copper
  mixture expansion is not implemented or tested here.
- New append-only error codes are 10 INPUT_HASH_MISMATCH, 11 INPUT_INVALID,
  12 INPUT_REFERENCE_MISSING, and 13 CUT_UNSUPPORTED. Codes 11–13 require
  architecture review; the brief did not prescribe their numeric values.
- No 3-2-1 coordinate/DOF convention was frozen without solver review. Top-surface
  Q2 fit weighting and conductivity source remain review questions.
- Front-end tests are gated on jsoncons, not the full solver dependency set, so
  input errors and mesh invariants can be checked independently. Numerical tests
  must be gated on the full dependency set when their backend exists. Proposed
  conduction errors should mean maximum absolute nodal error in kelvin.
