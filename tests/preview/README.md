# Preview STK-2 contract (version 1)

This document defines public preview guarantees for the STK-2 acceptance case.
With HDF5 and jsoncons enabled, run from the repository root:

```sh
chipcae-preview schema/examples/fcbga_demo.stack.json --nx 4 --ny 4 --levels 1 --max-ratio 10 -o grid
```

For this fixture and these options, `grid.vtkhdf` must contain exactly
17 × 17 × 6 linear hexahedral cells (1,734 cells total). Its points must occupy
18 distinct x planes, 18 distinct y planes, and 7 distinct z planes. These are
versioned guarantees for this acceptance case, including the explicit
`--levels 1` option; they do not specify dimensions for other fixtures or options.
Changes to these guarantees require an explicit revision of this contract and
its acceptance criteria.

The z planes must include the bottom, top, and cumulative layer interfaces.
Each layer must have at least its fixture `n_elem_min` z cell layers. Cell layer
and material IDs follow the public VTKHDF mapping in
[the preview documentation](../../preview/README.md), including the fixture's
die region material override. Total cell volume must equal the fixture outline
area times total thickness. Interface and volume tolerances come from the
`interface_relative` and `volume_relative` keys in
`mesh/background/thresholds/k1.json` at runtime.

`zalign.cpp` enforces this contract through the preview CLI and VTKHDF readback;
CTest registers it as `preview_zalign` when tests, HDF5, and jsoncons are enabled.
