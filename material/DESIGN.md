# DESIGN (v0) — CHUNK 1 skeleton

## Layering

- Device kernels: concept-constrained `MaterialModel` types (`KOKKOS_INLINE_FUNCTION`,
  no virtuals, no heap, no exceptions).
- Host shell: `IMaterial<MS>`, `MaterialAdapter<M, MS>` launches one `parallel_for`;
  `make_material` switches on `ModelKind`.

## State layout

- `QuadratureStateStore<MS>`: double-buffered `Soa<Real, MS>` of shape `[n_reals][N]`.
- `commit()` swaps the two views (O(1)); `discard()` does nothing (trial is recomputed).
- Combinator layout (later chunks): `[ActivationRecord (9) | M state | eigenstrain sources]`.

## Mandel

- Order `11,22,33,23,13,12`, shear scaled by √2; `σ:ε = σ_M·ε_M`.
- Voigt (Abaqus `11,22,33,12,13,23`) conversion via index permutation and diagonal
  √2 scaling: `C_M = D_s P C_V D_e P`.
- `Q_M(R)` built column-by-column from rotated Mandel basis tensors; orthogonal, det = 1.

## Serialization (D12)

- POD blob: header with magic, format version, endianness marker, layout hash,
  parameter hash (TODO), n_points, n_reals; then raw committed reals.
- 64-bit FNV-1a for layout/parameter hashes (D13).
