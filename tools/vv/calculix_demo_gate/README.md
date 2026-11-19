<!-- SPDX-License-Identifier: Apache-2.0 -->
# DEMO-GATE CalculiX reference attempt

**V-INT-6 acceptance: FAIL — the governing brief's reference deliverable is
unfulfilled.** No qualified `REFERENCE.json`, archived L1 decks or formal
W_bfp/T_max/T_top_min reference values are published. The generator and validator
regressions do not complete the card or establish a passing chipcae comparison.

**Candidate status: 参考不合格 — prescribed warpage sequence is nonmonotone.**
The external qualification archive contains complete L1/L2/L3, L1 full-model
and L3 C3D20 cross-check results. Criteria (a) and (b) fail; criteria (c)–(f)
passed in that attempt. Candidate values must not populate
`demo_gate.reference_w_bfp_m` or judge chipcae. The brief requires a nonmonotone
result to be reported to V&V; its meshes, QoI definitions and thresholds remain
unchanged. Qualification requires a numerical resolution under (a)–(f), followed
by publication of the qualified artifact, its L1 decks and formal values.

Both CTests are registered unconditionally, with no acceptance opt-out:

- `vvccx_reference_json` requires the qualified artifact and archived L1 decks,
  independently recomputes (a)–(f), and runs synthetic validator regressions and
  negative checks using only the standard library. It never skips.
- `vvccx_regen_l1` requires L2/L3 recorded deck hashes and real L1 byte/QoI
  comparisons with the artifact, alongside all generator regressions. Missing
  artifacts fail before optional solver dependency checks.

Legacy test-mode environment variables cannot filter either suite. Only absent
ccx, absent numpy/scipy or a solver-version mismatch against the recorded
artifact may skip regeneration checks. All applicable regressions run first;
failures take precedence over skips. The final runner exits 77 only if tests
skipped and none failed, 1 on failure, and 0 when all tests passed. Dependency
skips do not defer artifact acceptance.

The model is derived from the original bytes of
`examples/demo_gate/{case,stack,materials,profile}.json`; each hash is recorded.
The generator checks geometry, regions, materials, loading, reference temperature,
linear mechanics, and run profile before solving. The outline is a 20 mm square
with four 2 mm chamfers. Layers are substrate 1 mm, bump equivalent 0.1 mm, and
die layer 0.5 mm. Silicon occupies the central 10 mm square in the die layer;
the rest is underfill. The quarter domain is `[0,10]² mm`, `x+y≤18 mm`, with
the die in `[0,5]² mm`. Straight-sided C3D20R bricks and C3D15 chamfer wedges
share every material-interface corner and midside node. Integer coordinate keys,
fixed traversal and sorted numbering make the decks deterministic. Exact
extrusion-volume integration checks positivity, total volume, die-layer volume,
node coverage, interface conformity and chamfer endpoints.

Generated meshes (all counts actually constructed):

| Model | h (mm) | Elements/layer in z | C3D20R | C3D15 | Total | Nodes | Maximum edge ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| L1 quarter | 1 | 2 | 582 | 12 | 594 | 3046 | 28.28427125 |
| L2 quarter | 0.5 | 4 | 4680 | 48 | 4728 | 21487 | 28.28427125 |
| L3 quarter | 0.25 | 8 | 37536 | 192 | 37728 | 161005 | 28.28427125 |
| L1 full | 1 | 2 | 2328 | 48 | 2376 | 11345 | 28.28427125 |

The aspect ratio is longest geometrical edge divided by shortest geometrical
edge, including the wedge diagonal; it is not a Jacobian condition number.
The quarter total volume is `98e-6 × 0.0016 m³`, and its die-layer volume is
`98e-6 × 0.0005 m³`.

`case_loader.cpp:interpolate_property` evaluates each material table at T_ref.
The generator likewise interpolates the 250/400 K tables at 298.15 K and uses
these constants throughout both analyses:

| Material | E (GPa) | ν | α (µK⁻¹) | k (W/(m K)) |
|---|---:|---:|---:|---:|
| organic_eq | 24.1975 | 0.30321 | 15.4815 | 3.1 |
| sac305_ball_eq | 43.5555 | 0.35321 | 22.7062 | 58 |
| underfill | 7.7432 | 0.32321 | 30.963 | 0.5 |
| silicon | 125.827 | 0.28321 | 2.68346 | 148 |

The thermal deck uses `*HEAT TRANSFER,STEADY STATE`, bottom temperature
298.15 K, top convection h=10 W/(m² K), sink temperature 298.15 K, and all
other faces insulated. The body flux is read from the input power divided by
the actual full die-layer volume: `10/(392e-6 × 0.0005)` W/m³. Integrating
this source over quarter elements gives 2.5 W and over full elements gives
10 W. The integrated source must satisfy the unchanged `1e-10` relative-error
limit on every mesh.

Two decks are used: `<level>_quarter_thermal.inp` followed by
`<level>_quarter_mech.inp`. `*TEMPERATURE,FILE=<thermal job>.frd,BSTEP=1`
was verified on the installed conda-forge `calculix 2.23` package (build
`pl5321h92dc11e_8`; generation records the package metadata). The external FRD
dependency and unchanged numbering were verified on all three levels, the L3
element cross-check, and the L1 full model. The mechanical deck starts at
298.15 K, specifies `*EXPANSION,ZERO=298.15`, and uses geometrically linear
loaded `*STATIC` steps. No single-deck fallback was required. See the official
[CalculiX 2.23 manual](https://www.dhondt.de/ccx_2.23.pdf), §7.132
`*TEMPERATURE`, §7.99 `*NODE PRINT`, and §7.115 `*SECTION PRINT`.

Quarter constraints set ux=0 on x=0, uy=0 on y=0, and uz=0 at (0,0,0).
The full model is generated in original board coordinates. Its bottom pins
are A=(2,0) mm (xyz), B=(18,0) mm (yz), C=(0,18) mm (z).
Both decks request `*NODE OUTPUT,DOUBLE` and output every node, including top
midsides. Native double FRD retains the fine-grid differences lost in E12.5
ASCII output. The strict reader supports double records and the original fixed
ASCII format; it rejects duplicate/unknown nodes, incomplete fields, nonfinite
values and truncated records. The binary layouts follow upstream
[frd.c](https://github.com/Dhondtguido/CalculiX/blob/master/src/frd.c) and
[frdselect.c](https://github.com/Dhondtguido/CalculiX/blob/master/src/frdselect.c).
Thermal and structural NDTEMP blocks are compared node by node against the
unchanged `1e-3 K` transfer limit.

QoIs use the shared `mirror_quarter`, `interp_grid`, `qoi_surface`, and
`best_fit_plane` implementations. Numpy and SciPy are required. The full
model is translated to the board center before processing. All top nodes
satisfy `|z−H|≤1e-12 H`. The 81×81 grid spans `[-10,10]² mm`; the chamfer
exterior stays NaN. W_bfp removes the best-fit plane from the interpolated
surface, while W_bfp_nodes separately removes the plane from the nodal field.
Before interpolation, coordinate columns in mm are rounded to nine decimal
places and points are sorted by y then x. This removes translation roundoff and
fixes traversal order for co-circular Delaunay nodes. Displacements retain their
full precision. Full and mirrored models consequently use the same triangulation,
including when their displacement fields differ by a rigid plane.
Stored temperatures are absolute; convergence, uncertainty, and cross-checks
use rises above 298.15 K.

L1/L2 mechanics uses the direct SPOOLES solver. L3 mechanics (both C3D20R and
C3D20) uses `SOLVER=ITERATIVE CHOLESKY` to avoid the direct factorization's memory
cost. Plain iterative solution was rejected during diagnostics because its
W_bfp differed from SPOOLES beyond the required tolerance.

The L3 mechanical deck first initializes at uniform T_ref with no load. Its
zero displacements and unchanged temperature are covered by a real-solver
regression. This initialization enables CalculiX's residual-correction loop.
Both subsequent loaded steps explicitly set `NLGEOM=NO`, use the same thermal
FRD, and set field residual/correction controls to 1e-10. Repeating the identical
temperature adds no load and corrects the remaining linear-solve residual;
CalculiX's absolute correction stopping rule makes field controls alone
insufficient. The final loaded solution uses the prescribed linear elastic,
small-strain equations. See the 2.23 manual, §7.24 `*CONTROLS`, §7.123 `*STATIC`
and §7.125 `*STEP`, and upstream
[steps.f](https://github.com/Dhondtguido/CalculiX/blob/master/src/steps.f) and
[checkconvergence.c](https://github.com/Dhondtguido/CalculiX/blob/master/src/checkconvergence.c).
The regression compares both W_bfp and the complete displacement field against
SPOOLES at unchanged 1e-6 relative tolerance. Additional L2 diagnostic results
remain in the external qualification archive. A failed iterative solver status
is fatal even if ccx exits zero and writes a fresh FRD.

The qualification attempt produced nonmonotone W_bfp: it decreases from L1 to
L2, then increases at L3. The generator's `assess_reference` records failed
criteria (a) and (b) and `status: "参考不合格"` in the work artifact.
The independent validator rejects nonmonotone data immediately, before its
status-consistency check; it does not accept an unqualified status.
The other criteria passed in that attempt; the full assessment remains subject
to V&V disposition. All original selection rules, acceptance thresholds,
mesh levels and QoI definitions are retained.

Unqualified QoI tables, selection/uncertainty results, comparison deltas and
numerical diagnostics are kept only in the external qualification archive and
a clearly marked non-authoritative qualification note alongside the task log.
They are omitted from this deliverable directory to prevent accidental use as
gate values. No candidate may populate `demo_gate.reference_w_bfp_m` or judge
chipcae. Only a future `status == "合格"` artifact may publish values labelled
“参考值（独立代码）”.

Heat balance is reported, not used as a qualification gate. Bottom outward
heat is minus the *NODE PRINT bottom RFL total. Top outward heat is the exact
integral of the quadratic surface temperature times h: quad corner/midside
weights −1/12 and 1/3, triangle weights 0 and 1/3. *SECTION PRINT also archives
conduction surface flux. Signed error is `(bottom + top − source)/source`.

Resources (**实测**, wall seconds per external solve; Linux peak RSS in KiB):

| Model | Thermal seconds | Thermal RSS | Mechanical seconds | Mechanical RSS |
|---|---:|---:|---:|---:|
| L1 quarter | 1.001397 | 79780 | 1.002425 | 100128 |
| L2 quarter | 4.003591 | 170844 | 15.009885 | 1267256 |
| L3 quarter | 90.219338 | 2344308 | 900.962972 | 1201888 |
| L1 full | 2.002308 | 79596 | 4.002485 | 507192 |
| L3 C3D20 cross | 149.241918 | 2504048 | 528.018193 | 1473764 |

Each solve runs in a fresh measuring process with at most two assembly/result
threads. The runner samples solver/descendant RSS once per second and terminates
and reaps the process group above 8 GiB. Nonzero ccx exits, `*ERROR`, iterative
nonconvergence, missing fresh FRD, malformed/duplicate/incomplete/nonfinite
fields and missing DISP fail. Operational failures do not write a final JSON.
The earlier direct L3 runs ended in SIGKILL; their measured peaks did not
establish a guard violation. The iterative path completes within the same guard.

To rebuild, set `CHIPCAE_CCX` to the installed executable (or use `ccx` on
PATH), and `DEMO_GATE_WORK` to an external archive directory. The repository's
default local `work/` is not ignored, so use an external directory. L2/L3 decks,
solver logs, FRD, per-model QoI JSON, native version string, resource records,
Richardson results and top CSV are archived locally under
`runs/<date>_<commit>/DEMO-GATE/reference/`; see the task log for the concrete
qualification archive. They stay outside Git. The work-directory JSON records the
input and deterministic deck SHA-256 digests and uses the distinct DEMO-GATE
schema `chipcae.vv.demo_gate.reference/0.1`; trilayer's contract is unchanged.

```sh
python3 tools/vv/calculix_demo_gate/generate.py --levels L1 --work "$DEMO_GATE_WORK"
python3 tools/vv/calculix_demo_gate/generate.py --work "$DEMO_GATE_WORK"
```

A complete run writes its assessment only to the work directory; publish only
`status == "合格"` and its L1 decks. Unqualified numerical outcomes remain in
the work archive. L1-only runs write neither a final JSON nor partial reference.
The recorded solver is `This is Version 2.23`; version-only exit 201 is accepted.

Use the shared build-slot shim on PATH, plain command names and two jobs:

```sh
cmake -S . -B build-vvccx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-vvccx -j2
ctest --test-dir build-vvccx -R '^vvccx_(reference_json|regen_l1)$' --output-on-failure -j2
ctest --test-dir build-vvccx --output-on-failure -j2
```

Both acceptance tests currently fail because the qualified artifact is absent.
This is the required failing result for the unfulfilled reference delivery;
passing validator fixtures cannot satisfy acceptance. Direct script execution
runs the same unconditional checks:

```sh
python3 tests/vv/calculix_demo_gate/test_reference_json.py
python3 tests/vv/calculix_demo_gate/test_regen_l1.py
```

Negative validation covers L3 W_bfp +1%, bad input hashes, forbidden provenance
paths and incorrect reference type, plus invalid fields and nonfinite values.
Deck-hash regressions reject arbitrary digests for every L2/L3 deck. The real L1
generator regression also applies the independent validator to emitted
heat-source, temperature-transfer, resource and signed heat-balance evidence.
This checks real solver evidence without establishing three-level qualification.
The separate mocked-solve artifact test checks producer/validator schema
compatibility using synthetic data; it provides no solver or qualification evidence.
The real qualified-artifact mutation/restore check remains blocked because there
is no qualified baseline to restore; synthetic mutations do not replace it.

The earlier complete generation command exited 0 and wrote the unqualified
assessment only to the work archive. The independent validator, pointed at that
archive and its real L1 deck bytes, rejected it specifically for nonmonotone
warpage. Operational completion does not qualify the candidate. The external
task log records verification commands, exit codes and the outstanding V&V
qualification failure.

This card does not change the night driver, chipcae comparison postprocessing,
or product code. Loading a qualified reference in the night driver and aligning
its W_bfp comparison with this 81×81 algorithm require a separate card.
