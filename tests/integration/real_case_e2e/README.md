V-INT-12 runs a shared import → preview → FVM solve → readback pipeline for
`thermal`, `warpage` and `assembly`. Repository fixtures are generated or
synthetic; real inputs and all real output records must remain outside Git
worktrees. No real execution is claimed by the tests.

From the repository root, with the build queue wrappers first on PATH:

```sh
cmake -S . -B build-realcase -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-realcase -j2
ctest --test-dir build-realcase --output-on-failure -R '^realcase_real_case_unit$'
ctest --test-dir build-realcase --output-on-failure -R '^realcase_real_case_(thermal|warpage|assembly)$'
ctest --test-dir build-realcase --output-on-failure -L integration -R 'f1130|demo_gate|odbpp|assembly'
ctest --test-dir build-realcase --output-on-failure
```

The unit test needs numpy, but no compiled solver, MPI or CAD backend. HDF5
fixtures and reader checks import h5py when used and skip with `h5py not installed`
if it is unavailable. Parameter, stage-error, exit-status, envelope and data-gate
checks still run, including cell_budget transmission before preview rejection.
It generates valid temporary manifest/VTKHDF/CAELOG1 fixtures. Temporary external
programs exercise the driver's actual subprocess, resource and record handling;
their output provides no evidence of numerical accuracy. The E2Es use a 4 by 4,
level-zero mesh, two serial runs and one two-rank run when MPI is available.
Assembly generates a lid and ring, imports the board to discover its attachment
layer, and checks box regions and material names from the exported ID table.

The CLI accepts only these options:

```sh
python3 tools/vv/real_case_e2e.py thermal --params params.json --source-root . --out-root local-runs --np 1 2 --repeat 2
```

Select `warpage` or `assembly` in place of `thermal`. `--source-root` defaults to
the module's repository. CLI input paths are relative to params.json; library
calls resolve relative input paths from the current directory. Assembly's board,
map, material library and STEP references are relative to assembly.json and are
copied into the run's inputs dependency tree. Assembly import and solve use the
same parsed material content. A separate params library must match it exactly
by material ID, independently of list order. Approximate parts, including
parts_from_step bodies, mark the materials provenance illustrative.

Set `CHIPCAE_IMPORT_EXE`, `CHIPCAE_PREVIEW_EXE` and `CHIPCAE_SOLVE_EXE` to the
current build's executables. Thermal/warpage also resolve the corresponding
command names from PATH. Set `CHIPCAE_MPIEXEC` for a specific MPI launcher;
otherwise mpiexec/mpirun is discovered on PATH. CTest supplies current-build
executable paths under the brief's feature condition and supplies
`SP4_FIXTURE_GENERATOR` when that target exists.
Rank selections include the serial baseline: `--np 1` or `--np 1 2`.
The baseline supplies the reported QoIs and repeat check; the pair also runs C8.

Required params are case_id, data_class (synthetic/real), input, material_map,
materials (path or null) and mesh (nx, ny, levels, max_ratio,
min_elems_per_layer). Assembly keeps material_map in its document, so the params
entry may be null. Optional cell_budget must be an integer >= 1 and passes
through unchanged; omission keeps the solver's default. Loads, constraints,
linear settings and T_ref are preserved when supplied. Defaults are the
F-1130 thermal loads, Rigid321, Jacobi with rtol=1e-12, T_ref=298.15 and a
3600-second process timeout. Copper polygon sidecars remain intact for FVM.

| Checks | Applicability |
|---|---|
| C1–C10 | All three kinds; every primary run is checked |
| C7 | Exact deterministic digest equality across serial repeats |
| C8 | Serial/two-rank mesh, partition and QoI agreement; null without MPI |
| W1/W2 | Warpage and assembly when W is ready |
| W3 | Separate synthetic warpage run with exactly zero fixed-bottom U |
| W4 | Separate synthetic warpage/assembly run with exactly zero U at uniform T_ref |
| W5 | Synthetic noncut warpage FEM comparison, relative difference <= 0.05 |
| A1/A2/A3 | Assembly bodies/weld count/defects, material membership and check policy |

Thresholds are unchanged: 1e-8 K temperature tolerance, 1e-8 parallel relative
error, 1e-9 energy imbalance, exact repeat hashes and exact W3/W4 zeros.
The W5 5% bound comes from the fvm-thermoelastic-cutcell brief's comparison
criteria for synthetic noncut cases (comparison section, line 93).
Vertex temperature overshoot and recomputed vertex warpage are diagnostics.
C6 always checks the summary temperature upper bound; it also checks the cell
minimum when the optional T_cell field is available.
Digests use mesh SHA, the ordered three-QoI whitelist as float.hex, final
per-field iterations/residuals, and little-endian contiguous array values from
geometry and field datasets selected by the V-INT-10 digest rules.
Assembly links, reserved fields, metadata,
timing, logs, paths and file bytes are excluded. Raw file SHA values are recorded
separately. Exported material IDs are one-based indices into material_names;
material-library ordering never defines them.

| Exit | Meaning |
|---|---|
| 0 | All applicable evaluated checks pass |
| 1 | An evaluated check fails or a pipeline/driver error occurs |
| 2 | Invalid CLI/params or forbidden real output destination; no pipeline starts |
| 77 | Dependency unavailable, with nonempty item-specific reasons |

Evaluated failures take precedence over dependency status. Only assembly solve
13 is a dependency interruption. Missing MPI leaves C8 null without failing.
Each invocation allocates a new directory, writes run_record.json/md, result.json,
cmd.txt and individual stdout/stderr logs, and returns the finalized record.
Commands carry per-process monotonic wall time and wait4 RSS. A watchdog kills
the process group on timeout. A real solve timeout/signal retries once with
halved nx/ny in separate inputs/output directories; the invocation still fails.
Synthetic inputs never receive that retry.

Readiness requires the upstream fvm_pipeline.cpp for T, elastic.hpp and removal
of the thermal-only marker for W, and T plus geometry=ON for A. Assembly checks
only the nearest capability file ascending from the resolved import executable;
it never borrows another build's capabilities. Assembly evaluates W separately.
All cases retain the required thermal and mech physics declarations. Before W
is ready, the thermal-only solver stage supplies temperature results; the
driver omits mechanical checks, auxiliary runs and mechanical result QoIs.
An unavailable E2E prints `SKIP: <kind> upstream not merged: <reason>
(NEEDS-ATTENTION)` and returns 77 before generating inputs. A missing actual
welded_interfaces count fails A1; it is never synthesized or converted to 77.

`result.json` uses the frozen `chipcae.vv_result/0.2` contract and the existing
data-class checker. Dependency interruptions use its `skip` verdict; the
`needs_attention` verdict, item-specific reasons and exit code 77 remain in
`run_record.json`. The result notes also retain those reasons. The v0.2
reference categories cannot describe `无解析解（物理合理性检查）`, so reference is
null and that description is retained in notes. No new category is assigned
to a physical check. Real results still receive the exact D03 local-only
diagnostic. These metadata values are also accepted by the v0.2 exporter.

The following conservative specification deviations are explicit:

- Omitted --out-root allocates a persistent temporary root. Rejected real
  destinations receive no files; their usage record goes to a temporary root
  outside Git. The CLI prints the output directory; that runtime location is
  kept outside the returned and persisted run record.
- Unknown source commits remain unknown; C10 cannot be claimed for that schema
  conflict, and a completed pipeline fails rather than inventing a SHA.
- C9 excludes normal null status_detail, inapplicable sections, absent optional
  T_cell extrema and explicitly explained unavailable displacement diagnostics.
  Dependency-interrupted C1/C9 stay null. Other missing required data fails C9.
- C6 is null with a pending-bound note for custom loads or a body source. C8
  accepts identical zeros; an unequal zero-denominator comparison stays null
  with a pending-bound note. No new numerical tolerance is assigned.
- The decoder retains the validated event prefix at a damaged tail, including
  bad tail magic, without scanning ahead. Invalid initial magic is an error.
- Noncut DoFs are derived as 1 thermal / 3 mechanical unknowns per active volume; void cells are excluded.
  Current cut exporter polyhedron counts do not establish independent merged
  FVM unknown counts; cut-case DoFs remain missing until that contract exists.
- The local digest implements V-INT-10 while its module is absent; reuse the
  shared module when it becomes available. Real executions remain pending until
  the required upstream features merge.
