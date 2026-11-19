<!-- SPDX-License-Identifier: Apache-2.0 -->
# DEMO-GATE CalculiX independent reference

**V-INT-6 reference acceptance: PASS.** The committed `REFERENCE.json` has
`status: "合格"` with all six original criteria satisfied. Its two L1
decks and formal W_bfp/T_max/T_top_min values are published here.
The solver is `This is Version 2.23`; package `calculix 2.23 pl5321h92dc11e_8`.
These are **参考值（独立代码）**, not measurements or analytic solutions.

Both CTests are registered unconditionally. `vvccx_reference_json` requires the
qualified JSON and archived L1 decks, independently recomputes (a)–(f), and runs
standard-library validator and mutation regressions. Missing artifacts fail.
`vvccx_regen_l1` verifies L2/L3 deck hashes, regenerates and solves L1, and runs
the generator regressions. Only absent ccx, absent numpy/scipy or a solver-version
mismatch can skip the optional solver checks. Applicable regressions run first;
failures take precedence over exit 77. Legacy test-mode environment variables
cannot disable artifact acceptance.

The original bytes of `examples/demo_gate/{case,stack,materials,profile}.json`
supply every model input and their SHA-256 hashes are recorded in the JSON.
The generator asserts the input contract before solving. The outline is a
20 mm square with four 2 mm chamfers. Layers are substrate 1 mm, bump equivalent
0.1 mm and die layer 0.5 mm. Silicon occupies the central 10 mm square in the die
layer; the rest is underfill. The quarter domain is `[0,10]² mm`, `x+y≤18 mm`,
with silicon in `[0,5]² mm`. Straight-sided quadratic bricks and C3D15 chamfer
wedges share every material-interface corner and midside node. Integer coordinate
keys, fixed traversal and sorted numbering make the decks deterministic.
Exact extrusion-volume integration checks positivity, total and die-layer
volumes, node coverage, interface conformity and chamfer endpoints.

| Model | h (mm) | Elements/layer in z | Mechanical bricks | C3D15 | Total | Nodes | Maximum edge ratio |
|---|---|---|---|---|---|---|---|
| L1 quarter | 1 | 2 | 582 | 12 | 594 | 3046 | 28.2842712475 |
| L2 quarter | 0.5 | 4 | 4680 | 48 | 4728 | 21487 | 28.2842712475 |
| L3 quarter | 0.25 | 8 | 37536 | 192 | 37728 | 161005 | 28.2842712475 |
| L1 full | 1 | 2 | 2328 | 48 | 2376 | 11345 | 28.2842712475 |
| L3 C3D20 cross | 0.25 | 8 | 37536 | 192 | 37728 | 161005 | 28.2842712475 |

The aspect ratio is longest geometrical edge divided by shortest geometrical
edge, including the wedge diagonal. Quarter area is 98 mm² and full area is
392 mm². `element_type` and `elements_by_type` describe the mechanical mesh;
`thermal_element_type` separately records the heat-conduction formulation.
The two decks have identical nodes and element connectivity. The L3 element
cross-check changes only mechanical bricks from C3D20R to C3D20.

Material tables are interpolated at T_ref=298.15 K, matching
`case_loader.cpp:interpolate_property`, and then held constant:

| Material | E (GPa) | ν | α (µK⁻¹) | k (W/(m K)) |
|---|---:|---:|---:|---:|
| organic_eq | 24.1975 | 0.30321 | 15.4815 | 3.1 |
| sac305_ball_eq | 43.5555 | 0.35321 | 22.7062 | 58 |
| underfill | 7.7432 | 0.32321 | 30.963 | 0.5 |
| silicon | 125.827 | 0.28321 | 2.68346 | 148 |

Heat transfer uses `*HEAT TRANSFER,STEADY STATE`, bottom temperature 298.15 K,
top convection h=10 W/(m² K), sink temperature 298.15 K and other faces insulated.
The uniform source in the entire die layer, including silicon and underfill, is
`q=10/(392e-6 × 0.0005)` W/m³. Integrating it over quarter elements gives 2.5 W
and over the full model gives 10 W, with the unchanged relative-error limit 1e-10.
`heat_source.die_layer_volume_m3` denotes that whole heated layer.

Thermal bricks use fully integrated C3D20 (27 Gauss points, equivalent to
DC3D20), while the prescribed structural bricks remain C3D20R (8 Gauss points).
The previous thermal deck reused mechanical reduced integration. It
underintegrates the conductivity bilinear form: on a unit cube with k=1 and
T=x²y, the exact energy is `∫|grad T|²=4/9+1/5=29/45`, whereas 2×2×2 quadrature
gives 23/36. The real-solver regression reproduced 0.63888893 before the fix
and passes the exact-energy check after the fix, allowing only DAT rounding.
The same-mesh DEMO-GATE control changes L1 warpage from 1.0257881105233173e-5 m
to 1.0240753737680228e-5 m and removes the L1→L2 reversal. Full thermal
integration changes no mesh spacing, layer count, node, connectivity, material,
load, thermal-to-structural transfer, QoI definition or qualification threshold.
See the official [CalculiX 2.23 manual](https://www.dhondt.de/ccx_2.23.pdf),
§6.2.4 C3D20, §6.2.5 C3D20R and §7.50 `*ELEMENT`, and its `e_c3d_th.f`
implementation of the thermal integration schemes.

Each level uses `<level>_quarter_thermal.inp`, followed by
`<level>_quarter_mech.inp`. The latter reads
`*TEMPERATURE,FILE=<level>_quarter_thermal.frd,BSTEP=1`, starts at 298.15 K and
sets `*EXPANSION,ZERO=298.15`. Thermal and structural NDTEMP fields must agree
at every node to ≤1e-3 K. Both decks request `*NODE OUTPUT,DOUBLE` at every node,
including midsides, to retain the differences lost in ASCII E12.5 output.
The strict FRD reader rejects duplicate/unknown nodes, incomplete/nonfinite
fields and truncated records. Binary layouts follow upstream
[frd.c](https://github.com/Dhondtguido/CalculiX/blob/master/src/frd.c) and
[frdselect.c](https://github.com/Dhondtguido/CalculiX/blob/master/src/frdselect.c).

Quarter constraints set ux=0 on x=0, uy=0 on y=0 and uz=0 at (0,0,0).
The full model uses original board coordinates; its bottom pins are A=(2,0) mm
(xyz), B=(18,0) mm (yz), C=(0,18) mm (z). The best-fit plane removes rigid tilt.
L1/L2 mechanics uses direct SPOOLES. L3 mechanics uses `SOLVER=ITERATIVE CHOLESKY`
to stay within 8 GiB. Its unloaded isothermal initialization step enables
residual correction, followed by two identical loaded `NLGEOM=NO` steps with
field controls 1e-10. The repeated temperature adds no load. The initialization
has zero displacement and unchanged temperature; the final linear solution and
complete displacement field are covered by an iterative/direct comparison at
the unchanged 1e-6 relative tolerance. Failed iterative status is fatal even if
ccx exits zero. Manual entries: §7.24 `*CONTROLS`, §7.123 `*STATIC`, §7.125 `*STEP`
and §7.132 `*TEMPERATURE`.

QoIs reuse `mirror_quarter`, `interp_grid`, `qoi_surface` and `best_fit_plane`.
Every top node satisfies `|z−H|≤1e-12 H`. Quarter results are mirrored to the
full board and interpolated in mm onto the 81×81 `[-10,10]² mm` grid; the chamfer
exterior stays NaN. W_bfp removes the best-fit plane from this grid.
W_bfp_nodes is a separate auxiliary nodal calculation and is not used for
qualification. Coordinates in mm are rounded to nine decimal places and sorted
by y then x before interpolation, avoiding translation/traversal differences
in co-circular Delaunay triangulations; displacements retain full precision.
T_max uses all thermal nodes; T_top_min uses all top nodes. Stored temperatures
are absolute; relative thermal criteria use rises above 298.15 K.

Primary and auxiliary QoIs (**参考值（独立代码）**):

| Model | W_bfp (m) | W_bfp_nodes (m), auxiliary | T_max (K) | T_top_min (K) |
|---|---|---|---|---|
| L1 quarter | 1.02407537376802e-05 | 1.02402000110074e-05 | 319.204650513428 | 306.526145441687 |
| L2 quarter | 1.02566390507191e-05 | 1.02563127501368e-05 | 318.897921649953 | 306.531374747403 |
| L3 quarter | 1.02679599619254e-05 | 1.02724209339762e-05 | 318.897829288905 | 306.534314357145 |
| L1 full | 1.02407537336736e-05 | 1.02402000138791e-05 | 319.204650513428 | 306.526145441687 |
| L3 C3D20 cross | 1.02691544345982e-05 | 1.02696924492875e-05 | 318.897829288905 | 306.534314357145 |

For each primary QoI, (a) requires strict monotonicity and L2→L3 change ≤0.2%.
(b) computes `p=ln((Q1−Q2)/(Q2−Q3))/ln 2`: choose Richardson only for
p∈[1.5,4.5], otherwise choose the finest value. Zero/nonmonotone differences
remain fatal in the independent validator. (c) requires `U_ref=|Q3−Q2|` to be
≤0.2% of the selected value (temperature rise for thermal QoIs).

| QoI | Observed p | Selected | Formal reference (m or K) | U_ref (m or K) | L2→L3 relative change |
|---|---|---|---|---|---|
| W_bfp_m | 0.488703437 | finest | 1.02679599619254e-05 | 1.13209112063e-08 | 0.00110254726823 |
| T_max_K | 11.6973918 | finest | 318.897829288905 | 9.23610476207e-05 | 4.45160051852e-06 |
| T_top_min_K | 0.830994778 | finest | 306.534314357145 | 0.00293960974221 | 0.000350608244991 |

| QoI | L1 full/quarter relative difference | L3 C3D20/C3D20R relative difference |
|---|---|---|
| W_bfp_m | 3.91246656323e-10 | 0.000116330086717 |
| T_max_K | 0 | 0 |
| T_top_min_K | 6.78634573105e-15 | 0 |

Criterion (e) uses the unchanged 1e-6 symmetry limit; (d) uses the unchanged
0.1% element-cross limit. (f) checks source integration at 1e-10 and temperature
transfer at 1e-3 K for every model. No acceptance assertion is disabled or relaxed.

| Model | Source relative error | Temperature transfer max abs ΔT (K) | Bottom outward (W) | Top outward (W) | Signed balance relative error |
|---|---|---|---|---|---|
| L1 quarter | 3.5527136788e-16 | 0 | 2.483194 | 0.0168061301253 | 5.20501030721e-08 |
| L2 quarter | 1.7763568394e-16 | 0 | 2.483182 | 0.01681826384 | 1.05535982975e-07 |
| L3 quarter | 5.3290705182e-16 | 0 | 2.48318 | 0.0168202562825 | 1.02513011591e-07 |
| L1 full | 3.5527136788e-16 | 0 | 9.932775 | 0.067224520501 | -4.79498970307e-08 |
| L3 C3D20 cross | 5.3290705182e-16 | 0 | 2.48318 | 0.0168202562825 | 1.02513011591e-07 |

**实测对照**: reference T_max minus the brief's 20261119 night-run
318.898 K is -0.000170711094825 K. This comparison is
reported only and is not a qualification criterion.

Heat balance is reported, not used as a qualification gate. Bottom outward
heat is minus the `*NODE PRINT` bottom RFL total. Top outward heat is the exact
integral of quadratic face temperature times h: quad corner/midside weights
−1/12 and 1/3, triangle weights 0 and 1/3. `*SECTION PRINT` also archives
conduction surface flux. Signed error is `(bottom + top − source)/source`.

Resources (**实测**, seconds and KiB per external solve):

| Model | Thermal seconds | Thermal peak RSS | Mechanical seconds | Mechanical peak RSS |
|---|---|---|---|---|
| L1 quarter | 1.001154 | 79812 | 1.002415 | 100452 |
| L2 quarter | 4.002722 | 190980 | 12.008866 | 1267412 |
| L3 quarter | 72.057511 | 2510444 | 867.146863 | 1200888 |
| L1 full | 2.002504 | 84312 | 4.003015 | 507160 |
| L3 C3D20 cross | 74.080272 | 2510840 | 615.767641 | 1470812 |

Each solve uses a fresh measuring process, at most two assembly/result threads,
`getrusage(RUSAGE_CHILDREN).ru_maxrss` and one-second process-tree RSS samples.
Above 8 GiB, the runner terminates and reaps the solver group. Nonzero exits,
`*ERROR`, iterative nonconvergence or missing fresh FRD fail the operation;
old FRD files are removed first. Operational failures never write a final JSON.

To rebuild, set `CHIPCAE_CCX` to the installed executable (or use `ccx` on PATH)
and `DEMO_GATE_WORK` to an external archive directory. Omitted `--work` creates
and prints a new system temporary directory. Only complete runs write an
assessment; L1-only runs do not write a partial REFERENCE.json. Publish only
`status == "合格"`, together with its L1 decks. The schema is
`chipcae.vv.demo_gate.reference/0.1`; the shared trilayer schema is unchanged.

```sh
python3 tools/vv/calculix_demo_gate/generate.py --levels L1 --work "$DEMO_GATE_WORK"
python3 tools/vv/calculix_demo_gate/generate.py --work "$DEMO_GATE_WORK"
```

Version query exit 201 is accepted when it reports the unique required
`This is Version 2.23`. L2/L3 decks, all solver outputs, native version string,
QoI tables, Richardson data, resource evidence and mirrored top CSVs are archived
locally in `runs/<date>_<commit>/DEMO-GATE/reference/`, outside Git. The task log
records the concrete archive and verification evidence.

Use the shared build-slot shim on PATH, plain command names and two jobs:

```sh
cmake -S . -B build-vvccx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_BUILD_TESTS=ON
cmake --build build-vvccx -j2
ctest --test-dir build-vvccx -R '^vvccx_(reference_json|regen_l1)$' --output-on-failure -j2
ctest --test-dir build-vvccx --output-on-failure -j2
```

The qualified-artifact mutation check temporarily multiplied the working-copy
L3 W_bfp by 1.01 and ran
`ctest --test-dir build-vvccx -R '^vvccx_reference_json$' --output-on-failure -j2`:
exit 8, as required. Restoring the original artifact and rerunning returned
exit 0. Every CI run also rejects bad stack hashes, reference types,
provenance paths, malformed/nonfinite fields and missing/incorrect thermal
formulation metadata; deck mutation regressions reject arbitrary L2/L3 hashes.
The task log records the Release build and full CTest results.

This card does not change the night driver, chipcae postprocessing or product
code. Loading the qualified reference and aligning the driver's W_bfp comparison
with the 81×81 algorithm require a separate card; a qualified independent
reference alone does not establish a passing chipcae comparison.
