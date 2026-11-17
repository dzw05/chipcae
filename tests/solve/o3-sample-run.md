# O3 shipped sample checkpoint

The solver retains raw temperature tables for E, nu, and alpha and evaluates each
at the mechanical reference temperature T_ref before constitutive assembly.
The supported uniform-temperature thermoelastic step is one-way, with fixed
stiffness during the mechanical solve; no spatial temperature variation requires
quadrature-dependent properties. The existing thermal-to-mechanical path also
uses this fixed reference-temperature convention. Over the 100 K step, alpha rises
about 10%, so alpha(T_ref) times delta-T differs from the integrated CTE strain
by a few percent; this solve does not integrate alpha. The stress-free-temperature
field is unused; the sample aligns it with T_ref. Tables use piecewise linear
interpolation; one-point tables remain constant at every temperature. Outside a
multi-point table's range, the solver clamps and emits one event-log warning per
material/property per run when extrapolation is `clamp`; `error` rejects
out-of-range requests.
The numerical solver does not implement the material library's other interpolation
modes and rejects them (except one-point `constant`). Invalid table sizes,
nonfinite points, and non-increasing temperatures
produce INPUT_INVALID; diagnostics identify the material/property. The material
JSON schema does not enforce temperature ordering, so loader validation supplies
the targeted diagnostic (including for CTE jumps, unsupported by this solver).

The sample's adjacent z-cell sizes are 0.5 mm and 0.05 mm. The mesh rule correctly
checks adjacent size changes, so only the sample max_ratio changes, from 2.5 to 10.
This knob also loosens in-plane grading limits; those limits are inactive for
the ungraded sample.
Four distinct illustrative SI material cards preserve their provenance and carry
the explicit note “illustrative, not measured or vendor data.”

## Measurements

Measured 2026-11-17 on a shared Linux x86_64 VM, kernel 6.12.113+, eight virtual
Intel Xeon cores, 15 GiB RAM, no swap. Other users' builds/tests were active;
these are single observations, not isolated performance comparisons.
Release build (`-O3 -DNDEBUG`), GCC 14.4.0 via Open MPI 5.0.11, MFEM 4.10.0 with
PETSc direct LU, MPI HDF5, jsoncons; METIS and Kokkos disabled. CPU backend,
OMP_NUM_THREADS=1, OPENBLAS_NUM_THREADS=1, OMP_PROC_BIND=false; direct singleton
launch (np=1). Materials, stack, and case are the shipped fixtures, copied to a
scratch directory with only mesh nx/ny changed. The stack refines the requested
mesh, so nx=4 and nx=8 become 17x17x6 and 33x33x6 hex grids respectively.

| Requested nx=ny | np | Exit | Elapsed seconds | Peak RSS (KiB) | Warpage PV (m) |
| --- | --- | --- | --- | --- | --- |
| 4 | 1 | 0 | 31.871861 | 1,106,496 | 3.209170699280293e-5 |
| 8 | 1 | 0 | 267.769760 | 5,323,772 | 3.174393481797087e-05 |

Method: each resolution uses a fresh Python measurement process, calls the CLI
with `subprocess.run`, measures wall time around that call with
`time.perf_counter()`, and then reads
`resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss` (KiB on Linux). The CLI is
the sole child process of each measurement process. Elapsed time includes process
startup, solve, output, and shutdown; peak RSS is the child process high-water
mark, not a sum of sampled RSS values. Raw measurements and outputs are retained
at `/workspace/tasks/logs/o3-bench/nx{4,8}/` on the measurement host.

CLI invocation for each copied fixture:

```sh
OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 OMP_PROC_BIND=false \
  chipcae-solve --case fcbga_demo.case.json --profile profile.json --out results
```

The profile is `chipcae.run_profile/1.0`, ranks=1, backend=cpu, gpus=0.

The prior checkpoint `/workspace/tasks/ckpt1130/REPORT.md` reported nx=8, np=2
killed by the OOM killer at approximately 4.6 GB RSS per rank, with redundant
global LU assembly on each rank. That was a prior run with identical placeholder
materials, not a measurement repeated here. Memory optimization is out of scope.

## Validation and deviations

The interpolation unit test covers interior values, endpoints, clamp flags,
one-point constants, invalid ordering, nonfinite points, and table size errors.
The CLI input test checks ordering diagnostics for elastic and expansion tables.
The demo integration copies the shipped fixtures, changes only nx/ny, verifies
manifest-referenced nonempty VTKHDF points/connectivity, compares the result with
collapsed one-point tables as a consistency check, and checks each clamp warning
appears exactly once. The existing trilayer acceptance tolerance and pipeline timeout
remain unchanged.

Deviation from the plan: besides the requested solve_inputs CTest timeout of
600 seconds, its individual subprocess timeout increases from 30 to 120 seconds.
The prior checkpoint documented a failure of that separate 30-second limit under
load; changing only the outer CTest timeout would leave that failure possible.
The demo uses a focused new integration script. No memory optimization or np=2
OOM reproduction was attempted.

Release CTest: 100% pass, 51 tests registered, 49 executed successfully and two
GPU-only tests skipped; total wall time 238.04 seconds. solve_inputs took
195.54 seconds, solve_pipeline 34.51 seconds, and solve_demo 28.70 seconds.
clang-format 23.1.5 `-i` followed by `--dry-run --Werror` passed for all changed
C++ files; `git diff --check` passed.

The existing trilayer result remains 0.004269532769110056 m, with relative error
2.5777e-11 against 4.269532769e-3 m (required tolerance 1e-6). The small shipped
demo has warpage PV 3.2515124133460844e-5 m.

## Review fixes

The loader now honors table modes: linear interpolation and one-point constant
cards are accepted; pchip and multi-point constant cards return INPUT_INVALID.
Multi-point extrapolation `error` rejects out-of-range T_ref, while `clamp`
retains the endpoint value and one warning per material/property. Evaluated
properties and clamp flags are stored during loading and consumed by the
pipeline without evaluating the tables again. Nonfinite interpolated properties
and isotropic constitutive coefficients return INPUT_INVALID.

The demo cards explicitly request clamp extrapolation and align their unused
stress-free temperature with T_ref. The sibling YAML sample uses the same mesh
settings and uniform-temperature mechanics as the JSON sample. The integration
test now changes nx/ny from 2 to 3.

Review validation: Release build passed; all 51 registered CTests completed with
49 passing and two GPU-only skips in 209.89 seconds. The interpolation test also
passed after removing a duplicate assertion. clang-format 23.1.5 `-i` followed by
`--dry-run --Werror` and `git diff --check` passed.
