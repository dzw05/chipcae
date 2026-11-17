# Sample readiness: schema, O2, and S2

Schema validity, O2 solve readiness, and the S2 cut demonstration are separate
checks. A schema-valid input can still use material models or mesh settings that
the numerical backend cannot accept. A successful uncut check does not replace
cut-cell acceptance. The solver's capabilities and dependencies are documented in
[the solver README](../../apps/chipcae-solve/README.md)
(`apps/chipcae-solve/README.md`).

## Schema validity and O2 solve readiness

The shipped `schema/examples/fcbga_demo.case.json` is schema-valid but is not
O2-ready as checked in. It references `schema/examples/fcbga_demo.stack.json`
and `schema/examples/fcbga_demo.materials.json`; references resolve relative to
the containing case file, rather than the shell's working directory.

O2 requires constant isotropic thermoelastic materials with single-knot E, nu,
and alpha tables. For `elastic.table` and `thermal_expansion.table`, single-knot
means that the `temperatures` array has exactly one entry. The checked-in FCBGA
library has at least one table with two or more temperature knots. These
multi-knot temperature tables caused the original sample to fail with
`INPUT_INVALID/14` because they do not meet the single-knot requirement. Schema
validity alone does not establish support by O2.

After the temperature tables were corrected, a separate `max_ratio` failure
was observed: adjacent cell sizes exceeded the configured limit.
Inspect the mesh and layer discretization to understand that constraint. This
mesh constraint is separate from the temperature-table requirement. This guide
neither recommends a general `max_ratio` value nor validates any threshold;
loosening a limit requires evidence for the intended mesh.

Steady conduction requires positive `conductivity` in W/(m K).
The thermal sample `schema/examples/thermal.case.json` references the same FCBGA
stack and material library. At least one referenced material lacks conductivity,
so it is not ready for a steady conduction solve, in addition to the single-knot
limitation.

For O2 guidance, the public command form is
```sh
chipcae-solve --case case.json --profile profile.json --out run_dir
```

with case and profile replaced by suitable inputs. This is an interface description,
not a claim that the shipped FCBGA case is ready to run.

## S2 cut demonstration

The packaged S2 cut demonstration uses a two-material bilayer with cut corners:
`examples/s2_solve/case.json`, `examples/s2_solve/profile.json`,
`examples/s2_solve/stack.json`, and `examples/s2_solve/materials.json`.
The case references its stack and materials relative to its own directory.
The material data are illustrative, not measured or vendor data.

The numerical backend requires JSONcons, yaml-cpp, MPI, HDF5 and PETSc-enabled
MFEM. Follow the solver README's build instructions and enable
`CHIPCAE_ENABLE_JSONCONS`, `CHIPCAE_ENABLE_MPI`, `CHIPCAE_ENABLE_HDF5` and
`CHIPCAE_ENABLE_MFEM`. A bare build provides validation and help but reports an
unavailable numerical backend. Material-point comparison tests additionally
require Kokkos; those tests are distinct from this demonstration.

From the repository root, use the existing public command:

```sh
chipcae-solve --case examples/s2_solve/case.json --profile examples/s2_solve/profile.json --out run-s2
```

Use a fresh, exclusive output directory for `run-s2`; repeated runs need a new
exclusive directory. Exercise these inputs where the required local resources
and numerical dependencies are available. Each MPI rank solves the complete
global problem, so memory grows with rank count; rank agreement does not show
distributed solver scalability.

This task's contract checks do not run a numerical solve. They check documented
command tokens, file references, material temperature-array structure and the
readiness explanations. This task did not run the S2 numerical demonstration
and does not establish its numerical accuracy or performance.
