# MFEM handoff

Enable the existing `CHIPCAE_ENABLE_MFEM` flag; MPI additionally needs
`CHIPCAE_ENABLE_MPI` and a parallel MFEM build. Public headers expose A1's
`Communicator`, never an MPI handle. Only its world snapshot is supported.

The adapter owns the arrays. MFEM's array constructor borrows `vertices`
(internally `MakeRef`) and copies connectivity/attributes. Do not resize this
buffer while either mesh exists. `Finalize(false,false)` preserves numbering.
ParMesh receives the supplied column partition; no repartition is requested.
Attribute sets include layer, inclusion, body and activation-group unions.

`elem_gid()` is local-element ordered. `backfill_gp` matches global GID pairs
to local faces, including rank interfaces, after ParMesh creation. Pairs not
incident on this rank retain face -1; distribute/filter the arrays before use.
The initial replicated arrays are the preparation-stage API, not a scalable
parallel mesh reader. Cut quadrature weights are reference-volume weights;
EB normals point outward from body, EB weights are physical areas. Local IDs
are zero-based int32, GIDs use A1 Index. SI throughout.

Pending without installed MFEM: adapter roundtrip/positive Jacobian/GP tests
at 1, 2 and 4 ranks. These tests are registered only when dependencies are ON.
P1 still has a placeholder solver, so solver ownership, distributed cut-array
shipping and the final CV field inventory remain to align.

API reference checked against MFEM 4.10:
https://github.com/mfem/mfem/blob/v4.10/mesh/mesh.hpp
https://github.com/mfem/mfem/blob/v4.10/mesh/attribute_sets.hpp
