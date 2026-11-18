# DEMO-GATE acceptance validation

Acceptance is checked by `demogate_inputs` and, when MFEM, MPI, HDF5 and
`chipcae-solve` are available, `demogate_solve`. Run the Release build and tests
with the shared-slot command wrappers:

```sh
cmake --build /workspace/build-demogate-tdd-release -j2
ctest --test-dir /workspace/build-demogate-tdd-release --output-on-failure
ctest --test-dir /workspace/build-demogate-tdd-release -R '^demogate_' -V
```

The verbose solve log prints 实测, the date in Asia/Shanghai, machine, HEAD,
input implementation revision, working-tree status, cell and point counts,
maximum temperature (K), solver warpage (m), mesh hash, and main-solve wall time.
The solver evaluates its best-fit-plane QoI on Q2 nodes and cut-face quadrature
points; VTKHDF contains only vertices. These sampling sets can produce different
fitted planes and extrema, so a vertex fit cannot independently cross-check the
solver QoI to a justified relative tolerance. The former unasserted vertex-fit
comparison has been removed. Solver warpage must remain finite and positive,
and repeated runs must agree to relative tolerance 1e-12.

Both output blocks must contain nonempty integer cell_kind, layer_id,
material_id, and elem_gid arrays with one entry per cell; cell_kind must be 0
for hex cells and 1 for cut cells. The zero-source control requires constant
298.15 K temperature within 1e-8 K and displacement magnitude at most 1e-12 m.
The negative control requires exit 14 and `chipcae-solve: invalid target: layer:Die_layer` on stderr.

Historical runs against other revisions or uncommitted fixes are omitted.
This file describes how to collect acceptance evidence; it makes no numerical
acceptance claims for the current revision. The validation summary and final
commit SHA are recorded in `/workspace/tasks/logs/demo-gate-sample-last.md`.
