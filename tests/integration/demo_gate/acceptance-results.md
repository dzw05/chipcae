# DEMO-GATE regression validation

Configure a Release build with MFEM, MPI and HDF5, then run:

```sh
cmake --build <build-dir> -j2
ctest --test-dir <build-dir> --output-on-failure
ctest --test-dir <build-dir> -R '^demogate_' -V
```

The verbose log marks observations 实测 and includes date (UTC+8), machine,
revision (best effort), counts, temperature, warpage, mesh hash and wall time.

`baseline.stack.json` and `baseline.materials.json` freeze the brief's templates,
with SHA256 db79e596ad6a1260378e9ce58b16ddf418645dd785f99dbc96d23f6d85d711d2
and 6c6c06161fdf12df26021bfe6f8146cc08ab633dcf0f0da5529612e99d8bbe4f.
Citation text was checked against the supplied thermal-properties brief §2.

The writer interface is documented in io/README.md: hex/cut PDC blocks,
VTK types 12/42, one-based classification IDs, dense Morton column * nz + k
identities. Spatial centroids must match the frozen layer heights and die box;
all three layers and four materials must occur. The frozen mesh has 288 aligned
columns and six thickness intervals, so IDs must cover 0 through 1727 once.

Numerical bands are coarse regression guards, not physical validation of a real
package. Substrate resistance t/(kA) is 0.823 K/W; allowed temperature rise is
0.1 to 5 times P*R, with the existing maximum-principle bound retained and
T_max below 340 K. Mean temperatures increase away from the cold bottom.
Displacement magnitude is between 1 nm and 100 um. Thermal mismatch scale
alpha*deltaT*L^2/H motivates a broad warpage band of 0.01 to 100 um.
PV has no sign; the fitted top-surface center residual must be positive in that
same band. This checks upward center curvature without defining S_cc on a cut
outline. Vertex W_bfp is reported, with no relative-error acceptance criterion
against the solver's Q2/quadrature-sampled PV. Repeat T/U and PV must agree to
relative tolerance 1e-12. The zero-source and case-sensitive target controls
retain their original thresholds.
