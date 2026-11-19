<!-- SPDX-License-Identifier: Apache-2.0 -->
# DEMO-GATE CalculiX independent reference

**Status: 参考不合格 — prescribed warpage sequence is nonmonotone.**
L3 mechanics now completes within the unchanged memory guard, and all three
levels plus the C3D20 cross-check have been solved. The complete candidate
fails the original monotonicity/order criteria. No qualified `REFERENCE.json`,
archived L1 decks or formal reference values are published. Both mandatory
artifact acceptance tests still fail. These candidate values must not be used
to judge chipcae results; V&V disposition is required under the brief.

Exactly two CTests are registered unconditionally: `vvccx_reference_json` and
`vvccx_regen_l1`. The first uses only the standard library and independently
recomputes all acceptance criteria; it never skips. The second checks rebuilt
L2/L3 deck hashes without a solver, tests the generator/parser/failure paths,
and repeats the L1 solves when CalculiX 2.23, numpy and scipy are available.
Missing dependencies or a recorded-version mismatch return 77 for the affected
solve tests, as required by the brief; missing or invalid artifacts fail.
There is no acceptance bypass. Synthetic validator tests supplement the real
artifact checks. No acceptance threshold or prescribed mesh level was changed.

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
Thermal and structural NDTEMP blocks are compared node by node; maximum transfer
difference is 0 K in every model, including all 161005 L3 nodes.

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

L1/L2 mechanics uses the direct SPOOLES solver. L3 mechanics (both C3D20R and
C3D20) uses `SOLVER=ITERATIVE CHOLESKY` to avoid the direct factorization's memory
cost. Plain iterative solution was rejected during diagnostics: on L2 it changed
W_bfp by about 0.013% relative to SPOOLES, larger than the mesh differences.

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
SPOOLES at unchanged 1e-6 relative tolerance. An additional L2 diagnostic
compared the final refined W_bfp with the direct solution: relative difference
2.7263e-11 at 172044 KiB peak RSS (实测). A failed iterative solver status
is fatal even if ccx exits zero and writes a fresh FRD.

**参考值（独立代码）候选，未合格**, all temperatures absolute:

| Model | W_bfp (m) | W_bfp_nodes (m) | T_max (K) | T_top_min (K) |
|---|---:|---:|---:|---:|
| L1 quarter | 1.02578811052332e-05 | 1.02573121567569e-05 | 319.376785421088 | 306.533652393553 |
| L2 quarter | 1.02571923832038e-05 | 1.02568652229504e-05 | 318.897751945929 | 306.535625001088 |
| L3 quarter | 1.02675787497901e-05 | 1.02720254640636e-05 | 318.897741985273 | 306.536392187331 |
| L1 full | 1.02578811012246e-05 | 1.0257312159631e-05 | 319.376785421088 | 306.533652393553 |
| L3 C3D20 cross | 1.02691544345982e-05 | 1.02696924492876e-05 | 318.897829288905 | 306.534314357145 |

Candidate selection and uncertainty (temperature criteria use rises above
298.15 K; selected temperature candidates below are absolute):

| QoI | Observed p | Selected | Candidate value | U_ref = abs(L3 − L2) | L2→L3 relative rise change |
|---|---:|---|---:|---:|---:|
| W_bfp_m | undefined (nonmonotone) | finest | 1.02675787497901e-05 | 1.03863665863e-08 | 0.00101156921601 |
| T_max_K | 15.5535261201 | finest | 318.897741985273 | 9.96065648451e-06 | 4.80083880529e-07 |
| T_top_min_K | 1.36245519578 | finest | 306.536392187331 | 0.000767186242058 | 9.14798908662e-05 |

W_bfp decreases from L1 to L2, then increases at L3. Thus criterion (a)
fails even though its ≤0.2% L2→L3 change bound holds; criterion (b) also
fails because an observed warpage order cannot be computed. The recorded
`finest` warpage is a diagnostic fallback, not a qualified reference.
Temperature orders are outside [1.5,4.5], so their selection rule chooses L3.
Criterion (c) requires U_ref ≤0.2% of selected warpage or temperature rise.
All original selection and acceptance rules remain unchanged.
The complete assessment is (a)=fail, (b)=fail, (c)=pass, (d)=pass, (e)=pass,
(f)=pass, giving `status: "参考不合格"`.

The final additional L3 refinement changed W_bfp by 1.1482e-11 m, versus
1.0386e-8 m for L2→L3. Both loaded-step results show the direction reversal.
The surface peak also moves between levels (nodal y coordinates 5.5, 5.75
and 5.625 mm). These observations support a mesh/QoI convergence limitation;
they do not authorize changing the prescribed meshes or the monotonicity rule.
The brief explicitly requires reporting a nonmonotone result to V&V.
No altered mesh sequence, tolerance, acceptance switch or replacement QoI
was introduced to make the gate pass.

| Check | W_bfp relative difference | ΔT_max relative difference | ΔT_top_min relative difference | Limit |
|---|---:|---:|---:|---:|
| (e) L1 full/quarter | 3.90777374623e-10 | 0 | 1.35605381026e-14 | 1e-6 |
| (d) L3 C3D20/C3D20R | 0.000153462159532 | 4.20786188279e-06 | 0.000247762105488 | 0.001 |

Criterion (f) passes: maximum relative source error is 5.3290705182e-16
(limit 1e-10), and maximum thermal/mechanical temperature-transfer difference
is 0 K (limit 1e-3 K).

Heat balance is reported, not used as a qualification gate. Bottom outward
heat is minus the *NODE PRINT bottom RFL total. Top outward heat is the exact
integral of the quadratic surface temperature times h: quad corner/midside
weights −1/12 and 1/3, triangle weights 0 and 1/3. *SECTION PRINT also archives
conduction surface flux. Signed error is `(bottom + top − source)/source`.

| Model | Bottom outward (W) | Top convection outward (W) | Signed relative error |
|---|---:|---:|---:|
| L1 quarter | 2.483179 | 0.0168207505681 | -9.97727408247e-08 |
| L2 quarter | 2.48318 | 0.016820062126 | 2.48503805977e-08 |
| L3 quarter | 2.483179 | 0.0168206316378 | -1.47344874435e-07 |
| L1 full | 9.932717 | 0.0672830022726 | 2.27259278063e-10 |
| L3 C3D20 cross | 2.48318 | 0.0168202562825 | 1.02513011591e-07 |

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

The brief supplies a night-run T_max of 318.898 K (2026-11-19, baseline
5346215). **实测对照** only: the candidate finest thermal result differs by
-0.000258014727478 K. This supplied, rounded measurement
was not an input to the reference solve or selection.

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
ctest --test-dir build-vvccx -R '^vvccx_(reference_json|regen_l1)$' --output-on-failure
ctest --test-dir build-vvccx --output-on-failure -j2
```

Negative validation covers L3 W_bfp +1%, bad input hashes, forbidden provenance
paths and incorrect reference type, plus invalid fields and nonfinite values.
Deck-hash regressions reject arbitrary digests for every L2/L3 deck. The real
qualified-artifact mutation/restore check remains blocked because there is no
qualified baseline to restore; synthetic validator mutations do not replace it.

Review-fix verification (2026-11-19): Release configure and build exited 0.
Full CTest exited 8: 209 registered, 196 passed, 11 existing skips, and two
failures (`vvccx_reference_json`, `vvccx_regen_l1`) due to the required absent
qualified artifact. The JSON script ran eight methods (five passed, three
missing-artifact failures); regeneration ran nineteen (seventeen passed, two
missing-artifact failures), including real L1 solves and both new solver
regressions. No other regression was observed. Neither DEMO-GATE CTest skipped.
The complete generation command exited 0 and wrote the unqualified assessment
only to the work archive. The unchanged independent validator, pointed at that
archive and its real L1 deck bytes, rejected it specifically for nonmonotone
warpage. Python compilation and whitespace checks passed; clang-format 23.1.5
has no applicable changed C/C++ files.

This card does not change the night driver, chipcae comparison postprocessing,
or product code. Reference qualification remains outstanding pending V&V
disposition. Loading a qualified reference in the night driver and aligning its
W_bfp comparison with this 81×81 algorithm require the follow-up card.
