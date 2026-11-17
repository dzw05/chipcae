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
maximum temperature (K), warpage and vertex best-fit-plane warpage (m), their
relative difference, mesh hash, and main-solve wall time. The relative warpage
difference is reported without an acceptance threshold. The negative control
requires exit 14 and `chipcae-solve: invalid target: layer:Die_layer` on stderr.

Historical runs against other revisions or uncommitted fixes are omitted.
This file describes how to collect acceptance evidence; it makes no numerical
acceptance claims for the current revision. The validation summary and final
commit SHA are recorded in `/workspace/tasks/logs/demo-gate-sample-last.md`.
