<!-- SPDX-License-Identifier: Apache-2.0 -->
# DEMO-GATE CalculiX independent reference generator

**Qualification blocked; no reference artifact is published.** The generator and
failure regressions are implemented, but the prescribed run did not produce a
qualified independent reference. Artifact acceptance checks skip while
`REFERENCE.json` is unpublished; generator regressions still run when their
dependencies are available. Present artifacts must pass every existing assertion;
mesh levels and numerical thresholds are unchanged.
The numbers below are **参考值（独立代码）候选，未合格**; they must
not be used to judge chipcae results.

The attempted archive is `runs/20261119_7a00f37/DEMO-GATE/reference/` under the
local task archive root. It contains the generated decks, solver logs, FRD,
version, available QoI JSON/CSV, and generator snapshot. L2/L3 decks and solver
outputs stay outside Git. In accordance with the brief, neither an unqualified
JSON nor its L1 decks is added to this directory.

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
10 W. Maximum relative source error across all three constructed meshes and
the full model is `5.3291e-16`, below `1e-10`.

Two decks are used: `<level>_quarter_thermal.inp` followed by
`<level>_quarter_mech.inp`. `*TEMPERATURE,FILE=<thermal job>.frd,BSTEP=1`
was verified on the installed conda-forge `calculix 2.23` package (build
`pl5321h92dc11e_8`; generation records the package metadata). The external FRD dependency and unchanged numbering were
verified on L1, L2, and the separate L1 full model. The mechanical deck starts
at 298.15 K, specifies `*EXPANSION,ZERO=298.15`, and uses a geometrically linear
`*STATIC` step. No single-deck fallback was required. See the official
[CalculiX 2.23 manual](https://www.dhondt.de/ccx_2.23.pdf), §7.132
`*TEMPERATURE`, §7.99 `*NODE PRINT`, and §7.115 `*SECTION PRINT`.

Quarter constraints set ux=0 on x=0, uy=0 on y=0, and uz=0 at (0,0,0).
The full model is generated in original board coordinates. Its bottom pins
are A=(2,0) mm (xyz), B=(18,0) mm (yz), C=(0,18) mm (z).
Both decks output every node, including top midsides. Thermal and structural
NDTEMP blocks are compared node by node; maximum transfer difference is 0 K
for L1 (3046 nodes), L2 (21487), and L1 full (11345).

QoIs use the shared `mirror_quarter`, `interp_grid`, `qoi_surface`, and
`best_fit_plane` implementations. Numpy and SciPy are required. The full
model is translated to the board center before processing. All top nodes
satisfy `|z−H|≤1e-12 H`. The 81×81 grid spans `[-10,10]² mm`; the chamfer
exterior stays NaN. W_bfp removes the best-fit plane from the interpolated
surface, while W_bfp_nodes separately removes the plane from the nodal field.
Temperatures below are absolute; convergence, uncertainty, and cross-checks
use rises above 298.15 K.

| Candidate | W_bfp (m) | W_bfp_nodes (m) | T_max (K) | T_top_min (K) |
|---|---:|---:|---:|---:|
| L1 quarter | 1.0257898651236084e-5 | 1.0257421000000000e-5 | 319.377 | 306.534 |
| L2 quarter | 1.0257176519269437e-5 | 1.0256902000000002e-5 | 318.898 | 306.536 |
| L3 quarter | unavailable: structural solver killed | unavailable | 318.898 | 306.536 |
| L1 full | 1.0257955008533877e-5 | 1.0257386000000000e-5 | 319.377 | 306.534 |

There are three qualification blockers:

- L3 mechanics exited by SIGKILL (`-9`). The initial worker did not save its
  failure RSS; its peak cannot be claimed from this attempt. A concurrent box
  OOM is plausible, but exit -9 alone does not establish that cause or an 8 GiB
  limit violation. The worker now archives failure resource evidence as well as
  success evidence, and its explicit memory termination/reaping is regression
  tested. The prescribed run stops on this operational failure without writing
  a final reference. The L3 C3D20/C3D15 cross-check was consequently not run.
- L2 and L3 have identical temperature extrema in E12.5 ASCII FRD. Both
  temperature differences Q2−Q3 are zero, so strict monotonicity fails and
  observed order cannot be computed. No Richardson temperature, uncertainty
  qualification or valid final reference can be claimed from these records.
  The implementation explicitly rejects zero/nonpositive difference ratios;
  it does not fabricate an observed order or change the grid.
- The L1 full/quarter relative W_bfp difference is `5.494039248050384e-6`,
  above the prescribed `1e-6`. Temperature relative differences are both zero.
  Diagnostic reruns found identical mirrored nodal temperatures, small ASCII
  displacement differences after removing a rigid plane, and sensitivity to
  Delaunay point ordering. These observations do not establish compliance with
  the threshold; the result remains a failed symmetry check.

A complete run computes p from all three raw QoIs; p in [1.5,4.5] selects
Richardson, otherwise L3. It records `U_ref=|Q3−Q2|`, evaluates (a)–(f), and
writes a candidate JSON **only to the chosen work directory**. Invalid numerical
criteria are retained as `参考不合格` or `需讨论`; only `合格` may subsequently be
published. Operational failures do not write a final JSON. Schema users must
check both `schema_version` and `case`; the version string is shared with the
trilayer reference but the field sets differ.

Heat balance is reported, not used as a qualification gate. Bottom outward
heat is minus the *NODE PRINT bottom RFL total. Top outward heat is the exact
integral of the quadratic surface-temperature interpolation times h; quad
corner/midside weights are −1/12 and 1/3, triangle corner/midside weights 0
and 1/3. *SECTION PRINT additionally archives conduction surface flux for
inspection. The signed error is `(bottom + top − source)/source`:

| Model | Bottom outward (W) | Top convection outward (W) | Signed relative error |
|---|---:|---:|---:|
| L1 quarter | 2.483179 | 0.01682077000000003 | −9.2000000151e-8 |
| L2 quarter | 2.483180 | 0.01682009708333334 | 3.8833333349e-8 |
| L3 quarter, thermal only | 2.483179 | 0.01682064958333335 | −1.4016666672e-7 |
| L1 full | 9.932717 | 0.06728308000000012 | 7.9999999514e-9 |

Resources (**实测**, wall time per external solve, Linux ru_maxrss in KiB):

| Model | Thermal seconds | Thermal RSS | Mechanical seconds | Mechanical RSS |
|---|---:|---:|---:|---:|
| L1 quarter | 1.002551 | 79536 | 1.005327 | 100220 |
| L2 quarter | 4.010363 | 170912 | 13.011006 | 1266636 |
| L3 quarter | 75.055737 | 2348356 | failed, not captured | not captured |
| L1 full | 2.006110 | 79640 | 4.008832 | 506824 |

Each solver runs inside a fresh Python measuring process; measurements are not
contaminated by earlier solver children. The solver uses two threads. The
worker samples the solver and descendants once per second, terminates the
process group above 8 GiB, and reaps it. Nonzero solver exits, case-insensitive
`*ERROR`, missing fresh FRD, malformed records, duplicates within a block,
incomplete node sets, nonfinite fields, and missing mechanical DISP all fail.
Repeated complete temperature blocks in separate steps remain distinct.

The brief supplies a night-run T_max of 318.898 K (2026-11-19, baseline
5346215). **实测对照** only: candidate L1 differs by +0.479 K and candidate
L2/L3 thermal by 0.000 K at the printed precision. These supplied night-run
values are not reference inputs and were not used in the solver or selection.

To reproduce on a host with CalculiX 2.23, numpy and scipy, set `CHIPCAE_CCX`
to the installed executable (or leave it unset when `ccx` is on PATH) and set
`DEMO_GATE_WORK` to an external archive directory. The default local `work/`
is not ignored by the repository, so the recorded attempts used an external
work directory without changing `.gitignore`.

```sh
python3 tools/vv/calculix_demo_gate/generate.py --levels L1 --work "$DEMO_GATE_WORK"
python3 tools/vv/calculix_demo_gate/generate.py --work "$DEMO_GATE_WORK"
```

Recorded outcomes: L1 exit 0; two L1 decks compared byte-for-byte across fresh
runs with identical QoIs and no complete JSON. The complete three-level command
exited 1 at L3 mechanics. Separate L1 full-model execution exited 0. The version
query returned `This is Version 2.23` (version-only exit 201 is accepted).

The branch adds `add_subdirectory(calculix_demo_gate)` in `tests/vv/CMakeLists.txt`;
the child `CMakeLists.txt` uses `chipcae_auto_tests` to register these tests.
Use the shared build-slot shim on PATH,
plain command names, and at most two build jobs. This box retains the available
JSONCONS/HDF5 dependencies from the preceding acceptance-test pass:

```sh
cmake -S . -B build-vvccx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-vvccx -j2
ctest --test-dir build-vvccx -R '^vvccx_(reference_json|regen_l1)$' --output-on-failure
ctest --test-dir build-vvccx --output-on-failure -j2
```

The direct optional script runs eleven methods: eight cover fixed-width
parsing, malformed records, result separation, solver failures, version query,
failure resources, memory guard, and assessment status; the real L1 acceptance
method skips until a reference is published. Two additional methods check the
artifact availability and dependency skip/failure matrix. The standard-library JSON script
likewise skips its three artifact checks while the reference is absent, retaining
all original positive and negative assertions for a present artifact. The
availability regression checks that absent artifacts skip and that directories
and malformed JSON fail. Without a valid
archived reference the requested on-disk L3 +1% mutation followed by restore to
green cannot be performed; this verification remains outstanding, alongside
L3 mechanics, element cross-check, valid Richardson/reference selection and
artifact publication. During the original implementation, Release configure and
build both exited 0. Focused CTest exited 8 (both
DEMO-GATE tests fail). Full CTest exited 8: 209 total, 196 passed, 11 skipped,
2 failed; the only failures are `vvccx_reference_json` and `vvccx_regen_l1`.
The full suite took 45.66 s, with an isolated temporary directory for event-log
tests. No C/C++ files changed; clang-format 23.1.5 has an empty applicable file
list. Detailed command output is in the task handoff log.

This card does not change the night driver, chipcae's comparison postprocessing,
or product code. Loading a qualified reference in the night driver and aligning
its W_bfp comparison to the same 81×81 algorithm require the follow-up card.
V&V must resolve the observed failure/precision/symmetry issues before this
reference is used.
