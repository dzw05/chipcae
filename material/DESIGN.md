# K3 material-point design (v0)

Kernels are const value objects satisfying `MaterialModel`; eigenstrain sources
satisfy `EigenstrainSource`. Their update/init methods use
`KOKKOS_INLINE_FUNCTION`, fixed local storage and read-only Kokkos parameter
snapshots. The host parses JSON and checks semantics before constructing a model.
No device path uses a virtual call, allocation, exception, recursion or host I/O.
A small indexed aggregate holds heterogeneous sources without calling
`std::tuple` from device code. Runtime schema/layout errors are host exceptions;
constitutive failures return matching `StatusCode` and `PointOutput.status`.

## State and composition

| Region | Reals | Transferable |
| --- | ---: | --- |
| Activation prefix: active, T_act, strain_ref, step_id_act | 9 | all |
| ThermoElastic | 0 | — |
| Prony elastic_bulk: h_dev, e_dev_n, theta_n, xi_n | 6N + 8 | all except diagnostic xi_n |
| Prony constant_poisson_from_E: above plus q_vol | 7N + 8 | q_vol also transferable |
| EquivalentCureShrinkage: s_applied | 1 per source | yes |

The complete factory layout is
`[ActivationRecord | M state | eigenstrain source states]`. Activation defaults to
active=1, step_id_act=-1; the latter means use the material's original thermal
zero. A disabled activation flag uses `WithActivation<M,false>` to reserve the
same prefix while bypassing activation transforms. With activation enabled,
strain_ref is subtracted and T_act overrides the thermal zero. Activation events
reset all subsequent state, including viscoelastic history and s_applied.

`WithEigenstrains` caches source offsets at construction and concatenates field
metadata. It subtracts old/new chemical strain before updating M and reports the
sum at the new time. Tangents are unchanged. PointInput's optional thermal-zero
override propagates through composition without modifying shared parameters.
Metadata capacity is 64 fields; schedule tables allow 256 knots. Oversized inputs
are rejected on the host.

`QuadratureStateStore<MS>` owns two explicit-space Views of shape `[n_reals][N]`,
LayoutRight. `commit()` swaps handles; `discard()` invalidates event preparation;
neither copies state. Event preparation lazily copies committed to trial once.
The event's point index list is copied to MS before launching its kernel. Solver
Newton evaluations keep committed fixed; an event snapshot can serve as the old
state for evaluation into trial. Commit/discard accepts/rejects both together.

Checkpoint headers contain magic/version, endian mark, canonical FNV-1a layout
hash, parameter hash, point count, component count and numbering scheme. Only
committed data is saved, after a device-to-host mirror copy. Restore rejects
incompatible headers. Parameter hashes cover input JSON bytes plus an explicit
process binding. The store accepts material identity/hash as constructor inputs;
the driver obtains them from IMaterial. Transfer checks material identity and
layout, fits a centered affine 3D field using pivoted linear least squares, and
writes only transferable fields. Degenerate neighborhoods are errors.

## Dispatch and drivers

The host factory switches on the two ModelKind values, then on has_shrinkage and
has_activation, producing eight concrete adapter instances. Every instance is
concept-checked. IMaterial virtual calls end at the host adapter, whose Kokkos
parallel_for captures the concrete kernel by value. Batch and point paths use
identical kernels, strides and floating-point options.

The batch adapter enforces optional fit temperature bounds and exposes a host
warning when clamping is selected.

The single-point driver supports strain control and a pivoted 6x6 local Newton
stress solve. Newton iterations reuse committed history; only a converged
iterate commits. Its context exposes the process clock. Process recipe strings
must be resolved to ordered numeric step ids and actual step-local seconds with
`MaterialParams::bind_cure_step` before factory construction. An optional
process_step_ids registry resolves ordinals directly in the parser; standalone
cards without one use an opaque FNV id and retain s_applied outside that id.

CPU snapshots currently use DefaultExecutionSpace::memory_space, matching the
installed Serial/OpenMP Kokkos. Independently placed CPU and CUDA parameter
snapshots/factory instances are not validated here. Device-space Views and mirror
checkpoint code are present, but CUDA compilation and T25 remain pending.
