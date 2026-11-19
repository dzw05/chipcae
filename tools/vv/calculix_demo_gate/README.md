<!-- SPDX-License-Identifier: Apache-2.0 -->
# DEMO-GATE CalculiX independent reference

**Status: reference qualification is incomplete.** No qualified `REFERENCE.json`,
archived L1 decks, or formal W_bfp/T_max/T_top_min reference values are published.
The mandatory artifact acceptance currently fails; the candidate numbers below
must not be used to judge chipcae results.

Exactly two CTests are registered: `vvccx_reference_json` and `vvccx_regen_l1`.
Both always include their real-artifact acceptance methods. There is no CMake
scope switch or environment-variable bypass. A missing or unqualified reference
fails acceptance. The JSON test uses only the standard library and never skips;
the optional L1 regeneration returns 77 only for missing ccx/numpy/scipy or a
solver-version mismatch. Synthetic validator fixtures and generator regressions
run in addition to the mandatory artifact checks.

The original attempted archive is `runs/20261119_7a00f37/DEMO-GATE/reference/`
under the local task archive root. Double-precision evidence is in
`runs/20261119_97ba560_qualification/DEMO-GATE/reference/` and
`runs/20261119_97ba560_diagnostics/DEMO-GATE/reference/`; the direct-solver
retries are in the corresponding `20261119_97ba560_retry` and
`20261119_97ba560_serial` archives. These contain decks, solver logs, FRD,
resource measurements, available QoI JSON/CSV and generator snapshots. They
contain no qualified final reference. L2/L3 decks and solver output stay outside
Git. Only a reference satisfying all original criteria may be published.

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
Both decks request `*NODE OUTPUT,DOUBLE` and output every node, including top
midsides. Native double FRD retains the fine-grid differences lost in E12.5
ASCII output. The strict reader supports double records and the original fixed
ASCII format; it rejects duplicate/unknown nodes, incomplete fields, nonfinite
values and truncated records. The binary layouts follow upstream
[frd.c](https://github.com/Dhondtguido/CalculiX/blob/master/src/frd.c) and
[frdselect.c](https://github.com/Dhondtguido/CalculiX/blob/master/src/frdselect.c).
Thermal and structural NDTEMP blocks are compared node by node; maximum transfer
difference is 0 K for L1 (3046 nodes), L2 (21487), and L1 full (11345).

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
Temperatures below are absolute; convergence, uncertainty, and cross-checks
use rises above 298.15 K.

**参考值（独立代码）候选，未合格** (double-precision FRD):

| Candidate | W_bfp (m) | W_bfp_nodes (m) | T_max (K) | T_top_min (K) |
|---|---:|---:|---:|---:|
| L1 quarter | 1.025788110523318e-5 | 1.0257312156756893e-5 | 319.37678542108773 | 306.5336523935535 |
| L2 quarter | 1.0257192383203835e-5 | 1.0256865222950445e-5 | 318.89775194592903 | 306.5356250010884 |
| L3 quarter | unavailable: structural solver killed | unavailable | 318.89774198527255 | 306.53639218733053 |
| L1 full | 1.0257881101224625e-5 | 1.0257312159631037e-5 | 319.37678542108773 | 306.5336523935534 |

The precision fixes resolve two earlier diagnostic failures:

- Both temperature sequences are now strictly monotonic. Their observed orders
  are 15.5535261201 (T_max) and 1.36245519563 (T_top_min), with L2-to-L3 relative
  rise changes of 4.8008388053e-7 and 9.1479890873e-5. Both orders are outside
  [1.5,4.5], so criterion (b) selects the finest temperature, without Richardson
  extrapolation. These partial thermal observations do not qualify the reference.
- The L1 full/quarter relative W_bfp difference is 3.9077853066e-10, below the
  unchanged 1e-6 threshold. Temperature relative differences are 0 and
  1.3560538103e-14. The original E12.5/order-sensitive postprocessing reported
  5.4940392481e-6 for warpage. A regression independently constructs equal full
  and mirrored fields with an added rigid plane to check triangulation invariance.

L3 mechanics remains the qualification blocker. Fresh direct-SPOOLES attempts
with the prescribed 37,728-element, 161,005-node mesh both exited -9 (SIGKILL):

| Equation-solver threads | Peak RSS (KiB), 实测 | Wall seconds, 实测 | Exit |
|---|---:|---:|---:|
| 2 | 7420040 | 245.987516 | -9 |
| 1 | 5677960 | 854.264720 | -9 |

The available evidence establishes the signal and measurements, not its cause.
Neither recorded peak establishes an 8 GiB guard violation. The guard remains
unchanged and operational failures do not write a final reference. The L3
C3D20/C3D15 cross-check has not run, and no complete warpage convergence,
Richardson selection or uncertainty qualification can be claimed. Qualified JSON,
archived L1 decks and the real-artifact acceptance remain outstanding.

A complete run computes p from all three raw QoIs; p in [1.5,4.5] selects
Richardson, otherwise L3. It records `U_ref=|Q3−Q2|`, evaluates (a)–(f), and
writes a candidate JSON **only to the chosen work directory**. Invalid numerical
criteria are retained as `参考不合格` or `需讨论`; only `合格` may subsequently be
published. Operational failures do not write a final JSON. The unpublished
DEMO-GATE contract now uses `schema_version: "chipcae.vv.demo_gate.reference/0.1"`
and `case: "DEMO-GATE"`. This distinct identifier replaces the original brief's
reuse of the incompatible trilayer identifier `chipcae.vv.reference/0.1`.
The generator and independent validator agree on it; trilayer artifacts and
consumers are unchanged.

Heat balance is reported, not used as a qualification gate. Bottom outward
heat is minus the *NODE PRINT bottom RFL total. Top outward heat is the exact
integral of the quadratic surface-temperature interpolation times h; quad
corner/midside weights are −1/12 and 1/3, triangle corner/midside weights 0
and 1/3. *SECTION PRINT additionally archives conduction surface flux for
inspection. The signed error is `(bottom + top − source)/source`:

| Model | Bottom outward (W) | Top convection outward (W) | Signed relative error |
|---|---:|---:|---:|
| L1 quarter | 2.483179 | 0.01682075056814808 | −9.9772740825e-8 |
| L2 quarter | 2.483180 | 0.01682006212595158 | 2.4850380598e-8 |
| L3 quarter, thermal only | 2.483179 | 0.016820631637814068 | −1.4734487443e-7 |
| L1 full | 9.932717 | 0.06728300227259232 | 2.2725927806e-10 |

Resources (**实测**, wall time per external solve, Linux ru_maxrss in KiB):

| Model | Thermal seconds | Thermal RSS | Mechanical seconds | Mechanical RSS |
|---|---:|---:|---:|---:|
| L1 quarter | 1.001148 | 79944 | 1.001206 | 100984 |
| L2 quarter | 4.003194 | 171884 | 12.007974 | 1266560 |
| L3 quarter | 68.048121 | 2350940 | failed; retries above | retries above |
| L1 full | 2.001726 | 79956 | 4.003466 | 507172 |

Each solver runs inside a fresh Python measuring process; measurements are not
contaminated by earlier solver children. Assembly/results use at most two threads;
the serial retry uses one equation-solver thread. The worker samples the solver
and descendants once per second, terminates the
process group above 8 GiB, and reaps it. Nonzero solver exits, case-insensitive
`*ERROR`, missing fresh FRD, malformed records, duplicates within a block,
incomplete node sets, nonfinite fields, and missing mechanical DISP all fail.
Repeated complete temperature blocks in separate steps remain distinct.

The brief supplies a night-run T_max of 318.898 K (2026-11-19, baseline
5346215). **实测对照** only: candidate L1 differs by +0.478785 K,
L2 by −0.000248 K, and L3 thermal by −0.000258 K (the supplied night-run
number has only three decimal places). These supplied night-run
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
and both subsequent L3 mechanics retries exited 1 at the Python driver level.
Separate L1 full-model execution exited 0. The version query returned
`This is Version 2.23` (version-only exit 201 is accepted). To reproduce the
serial-equation-solver attempt, set `CCX_NPROC_EQUATION_SOLVER=1` before the
same generation command. No grid, constraint or numerical threshold was changed.

The branch appends `add_subdirectory(calculix_demo_gate)` in
`tests/vv/CMakeLists.txt`; the child uses `chipcae_auto_tests` with the original
vv labels and 600-second timeout. Use the shared build-slot shim on PATH, plain
command names and at most two build jobs. This box retains the available
JSONCONS/HDF5 dependencies from the preceding acceptance-test pass:

```sh
cmake -S . -B build-vvccx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-vvccx -j2
ctest --test-dir build-vvccx -R '^vvccx_(reference_json|regen_l1)$' --output-on-failure
ctest --test-dir build-vvccx --output-on-failure -j2
```

Review-fix verification (2026-11-19): Release configure and build exited 0.
Focused CTest exited 8: both mandated tests failed on the required missing
artifact. Setting the former `CHIPCAE_DEMO_GATE_TEST_MODE=generator` did not
bypass acceptance. The JSON module ran eight methods (five passed, three missing
artifact failures); regeneration ran fifteen (fourteen passed, one missing
artifact failure), including a real L1 solve. Full CTest exited 8: 209 registered,
196 passed, 11 existing skips and the same two failures (50.41 s). No other
regression was observed. Neither DEMO-GATE CTest skipped. No C/C++ files changed;
clang-format 23.1.5 has an empty applicable file list. Python compilation and
whitespace checks passed.

The required real-artifact L3 +1% mutation/restore check cannot run until a
qualified artifact exists. The same mutation is tested against the synthetic
validator fixture, alongside bad input hashes, forbidden provenance paths and
incorrect reference type. This unit coverage does not replace real acceptance.

This card does not change the night driver, chipcae's comparison postprocessing,
or product code. Loading a qualified reference in the night driver and aligning
its W_bfp comparison to the same 81×81 algorithm require the follow-up card.
V&V must resolve L3 mechanics and complete all original checks before this
reference is used.
