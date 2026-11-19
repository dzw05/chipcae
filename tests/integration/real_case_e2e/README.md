V-INT-12 automated acceptance tests, TEST-FIRST pass. The driver module contains
only empty signature stubs, so new contract assertions intentionally fail until
the implementation pass. No real input or generated result belongs in this tree.

From the repository root, with the build queue wrappers first on PATH:

```sh
cmake -S . -B build-realcase -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-realcase -j2
ctest --test-dir build-realcase --output-on-failure -R '^realcase_real_case_unit$'
ctest --test-dir build-realcase --output-on-failure -R '^realcase_real_case_(thermal|warpage|assembly)$'
ctest --test-dir build-realcase --output-on-failure -L integration -R 'f1130|demo_gate|odbpp|assembly'
ctest --test-dir build-realcase --output-on-failure
```

The unit test generates valid temporary manifest/VTKHDF/event fixtures. It needs
numpy and h5py, but no compiled solver, MPI or CAD backend. Temporary executable
doubles exercise the actual driver's process and record boundaries; no numerical
solver result is inferred from those doubles.

The three E2Es check readiness before preparing inputs. Once implemented, an
unavailable upstream prints `SKIP: <kind> upstream not merged: <reason>
(NEEDS-ATTENTION)` and returns 77. Current-build executable paths come from CTest.
Thermal and warpage use the public synthetic four-layer board with a 4 by 4,
level-zero mesh, two serial runs and one two-rank run when MPI is available.
Assembly generates a lid and ring and derives the attachment layer by importing
the synthetic board. Warpage additionally verifies exact fixed-face zeros,
exact zero-dT displacement and the existing 5% noncut FEM comparison. Assembly
applies W1/W2/W4 when available, with W3/W5 excluded.

Expected driver exits are 0 (pass), 1 (evaluated failure or execution error),
2 (usage error), and 77 (dependency unavailable). A dependency never masks an
evaluated failure. All exits must produce records. Thresholds stay at 1e-8 K,
1e-8 parallel relative error, 1e-9 energy imbalance and exact digest equality.

Implementation prerequisites still need resolution: the writer's requested
reference type and dependency verdict conflict with the frozen result schema;
the importer must expose its actual welded-interface count. C10 fixtures use
an already valid schema value to test the data gate independently of that
writer decision. No validator is changed by this test pass. Custom-load C6
bounds, zero-denominator C8 behavior and rejected-output record destinations
remain unspecified and are not assigned invented policies by these tests.
