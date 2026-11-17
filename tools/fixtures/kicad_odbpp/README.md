# Self-authored four-layer fixture

Created for ChipCAE with KiCad **10.0.8**; the board was saved by that
version in KiCad 10 format (20260206). No third-party design is included.
The 40 × 30 mm board has F.Cu/B.Cu signal layers, In1.Cu/In2.Cu power
layers, a circular plated hole, an oblong plated slot, a semicircular
copper track, and filled copper zones with a rectangular negative cutout.
KiCad represents the cutout in the exported surface with a doubled bridge
rather than a separate negative surface record.

Regenerate from the repository root:

```sh
tools/fixtures/kicad_odbpp/regen.sh
```

The default CLI is `/tmp/kicad10/kicad-10.0.8-x86_64-lite.AppImage`
(invoked with `kicad-cli`). `KICAD_CLI=/path/to/kicad-cli` overrides it;
an override ending in `.AppImage` is invoked with the same subcommand.
The script checks the actual version before writing outputs and refuses
anything other than 10.0.8. Python 3 and plain `cmake` are needed for
archive normalization. It refills and saves the board before both exports.

The stackup explicitly specifies 0.035 mm for each copper layer,
0.20 / 1.06 / 0.20 mm for FR4 dielectrics, and 0.01 mm per solder mask
(total 1.62 mm including mask; board thickness 1.6 mm excludes mask).

Measured (实测), 2026-11-17, KiCad 10.0.8: the three exported dielectric
attrlists contain `.layer_dielectric=0.20`, `.layer_dielectric=1.06`, and
`.layer_dielectric=0.20` respectively (millimeters). **None is 0.0.**
Zone refill and DRC reported zero violations and zero unconnected items.
The exported IPC-2581 imports successfully with exactly four copper layers.

Deviation: this KiCad version emits job `misc/info` but no `misc/attrlist`.
To satisfy the required fixture layout, regeneration adds an empty
`misc/attrlist`, then repacks the exported job into both tgz and zip and
copies the same job to the extracted directory. All other exported files
are preserved. The native archive root is `odb/`; the extracted copy is
named `kicad4l/`. Archive metadata and ordering are not contractual.
