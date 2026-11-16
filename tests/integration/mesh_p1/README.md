# K1 / P1 acceptance

P1 in `solvers/fem` is still a placeholder. These tests exercise MFEM directly;
they do not claim to validate the future P1 solver implementation.

Default Ninja build needs no optional dependencies. `k1_background` and
`k1_cut` are Tier A. Schema tests are registered only with JSONCONS. HDF5
readback and MFEM tests are registered only with their existing switches.
All numerical test tolerances come from `mesh/background/thresholds/k1.json`.

The MFEM elasticity test uses H1 order one, a multilayer multi-attribute
anisotropic tensor mesh, uniform positive Lamé constants, zero body load and
affine displacement on every exterior boundary. It checks displacement L2
and integrated symmetric-gradient relative errors <=1e-10. It runs at 1, 2 and 4 ranks with MPI; a serial variant is available without MPI. Each
case has far fewer than 50k cells and a 60-second CTest limit.

The MFEM cut-integral test checks >=1000 cut cells against an independent
analytic box/plane CDF and checks normal closure. It evaluates Jacobians
through ElementTransformation at reference coordinates [0,1]^3. EB weights
are already physical areas and must not be multiplied by the Jacobian.

CPU dependency job reproduction (requires MPI, Ninja, ccache, curl, tar):

```sh
tests/integration/mesh_p1/ci/build_deps_cpu.sh "$PWD/build-k1-deps/prefix"
cmake -S . -B build-k1-cpu -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/build-k1-deps/prefix" \
  -DCHIPCAE_ENABLE_MPI=ON -DCHIPCAE_ENABLE_MFEM=ON \
  -DCHIPCAE_ENABLE_HYPRE=ON -DCHIPCAE_ENABLE_HDF5=ON \
  -DCHIPCAE_ENABLE_JSONCONS=ON -DCHIPCAE_WARNINGS_AS_ERRORS=ON
cmake --build build-k1-cpu --parallel 2
timeout 600 ctest --test-dir build-k1-cpu -LE gpu --timeout 60 --output-on-failure
```

The dependency prefix is local, versioned by script digest and cached.
Partitioning uses `chipcae::par::ColumnExtrudePartitioner`. HDF5 is built parallel because the
existing root dependency policy requires it when MPI is enabled.

Optional-dependency mesh tests (schema, MFEM, HDF5, MPI) run in the weekday
nightly workflow `.github/workflows/nightly-mesh-fulldep.yml`, not on every
PR. `k1_patch` and `k1_cut_mfem` also have 1/2/4-rank MPI cases when MPI is
on.

GPU CI stays disabled on PRs. On a provisioned CUDA machine run
`nightly/run_gpu.sh /absolute/path/to/cuda-mfem-prefix [BUILD_DIR]`. The
script configures and builds; it exits 77 when no GPU is present, and it
does not exit 0 just because a build is missing. `k1_patch_gpu` also skips
unless the hook sets `CHIPCAE_RUN_GPU`. No machine is provisioned.

Field inventory was checked against design v0.2
(https://cursor.com/canvas/store-cb1250dc-2b01-44d2-a2e6-225d0cab8b5b/82a66aed-852d-4f73-8340-72bdf38a23b7).
See `PATCHLOG.md` for must-fix work, K9 §3 rulings, the hypre 3.2.1
workaround, and remaining deviations. Units are SI; numbering is 0-based;
index types follow A1 (`Index`/`MaterialId`/`BodyId`). EB normals are
outward from the body. Preparation-stage arrays are replicated; the
solver later distributes them. `ActivationSet` is consumed read-only.
