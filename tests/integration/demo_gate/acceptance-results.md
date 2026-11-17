# DEMO-GATE acceptance results

Observed with the Release build (MFEM, MPI and HDF5 enabled), using:

```sh
cmake --build /workspace/build-demogate-tdd-release -j2
ctest --test-dir /workspace/build-demogate-tdd-release -R '^demogate_' -V
```

Both acceptance tests passed (2/2, 115.80 seconds). The measured implementation
commit, machine, date (Asia/Shanghai), counts and numerical values are recorded
verbatim below. This report is a subsequent documentation-only change.

```text
实测 2026-11-18T01:25:16.395683+08:00 machine grok-bot-vm-509543457 commit c1a5c2023f56b211843c161e1eb42cddd591ed9e counts {'hex': {'cells': 1662, 'points': 2184}, 'cut': {'cells': 66, 'points': 516}} T_max 318.89818948540096 warpage_pv 1.1299775987914462e-05 W_bfp 1.0213879167944507e-05 relative_difference 0.09609896878764346 mesh_sha256 efc79a629779602538239635706a9f4293e897f8e46b3c2aeb43c22c6b6b9f4e main_wall_seconds 39.24968966199958
```

Temperature is in K; `warpage_pv` and `W_bfp` are in m. The relative difference
is an observation, not an accuracy acceptance threshold. These are numerical
results for the illustrative inputs, not physical measurements of a package.

The case-sensitivity negative control changed the heat-source target to
`layer:Die_layer` and observed:

```text
exit=14
chipcae-solve: invalid target: layer:Die_layer
```

All existing assertions passed, including geometry/field checks, repeat warpage
relative difference <= 1e-12, exact repeated mesh hash equality, and zero-source
maximum absolute temperature deviation from 298.15 K <= 1e-8 K. The last three
are passed bounds/comparisons; the test does not print their individual values.
No assertions or acceptance thresholds were changed.

A subsequent full-suite validation reproduced the same numerical values and hash.
The DEMO-GATE solve and all its controls passed in 197.97 seconds:

```text
实测 2026-11-18T02:29:43.810533+08:00 machine grok-bot-vm-509543457 commit c1a5c2023f56b211843c161e1eb42cddd591ed9e counts {'hex': {'cells': 1662, 'points': 2184}, 'cut': {'cells': 66, 'points': 516}} T_max 318.89818948540096 warpage_pv 1.1299775987914462e-05 W_bfp 1.0213879167944507e-05 relative_difference 0.09609896878764346 mesh_sha256 efc79a629779602538239635706a9f4293e897f8e46b3c2aeb43c22c6b6b9f4e main_wall_seconds 57.05623143300181
```
