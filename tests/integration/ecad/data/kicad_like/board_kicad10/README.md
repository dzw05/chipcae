# Self-authored KiCad 10 acceptance board

`board.kicad_pcb` was authored for this test, with no components or component pads.
Run `./regen.sh` with KiCad 10.0.8; `KICAD_CLI` may override the command.
The committed `board.xml` is the unmodified IPC-2581 export. Coordinates below
are in mm in the board; export negates Y.

Stack order: F.Cu (0.035), DIELECTRIC_1 (0.48), In1.Cu (0.035),
DIELECTRIC_2 (0.5), In2.Cu (0.035), DIELECTRIC_3 (0.48), B.Cu (0.035).
Only In2.Cu has a filled rectangular GND zone, (5,5)–(45,40).

Both F.Cu and B.Cu have these three separate ROUND signal tracks:

| Start | End | Length | Width |
| --- | --- | --- | --- |
| (10,10) | (20,10) | 10 | 0.2 |
| (10,15) | (20,15) | 10 | 0.4 |
| (10,20) | (20,20) | 10 | 0.2 |

There is one GND through via at (30,30), F.Cu–B.Cu, and one GND blind via
at (40,30), F.Cu–In1.Cu. Both have pad diameter **d = 1 mm**, drill diameter
0.5 mm. Each connected copper layer has a 0.1 mm long, 0.2 mm wide GND
stub from the via center towards increasing Y. These stubs, including their
ROUND caps, lie entirely inside the radius-0.5 pad and add no union area.
They ensure the exporter includes pads on all connected copper layers.
In1.Cu contains only these via pads and their contained GND stubs.

Measured in the exported XML: F.Cu has two CIRCLE_1 pad references, B.Cu
has one, In1.Cu has two, and In2.Cu has one. CIRCLE_1 diameter is 1.0.
The two drill spans each contain one Hole, without PLATING_THICKNESS.
Thus expected reports are `ViaPlatingMissing:F.Cu:B.Cu:1` and
`ViaPlatingMissing:F.Cu:In1.Cu:1`.

The automated area expectations are calculated as
`2*(10*0.2 + pi*0.2^2/4) + (10*0.4 + pi*0.4^2/4) + np*pi*1^2/4`,
where np is 2 for F.Cu and 1 for B.Cu. Signal tracks, pads and the opposite
via's pad do not touch or overlap. No arcs or non-ROUND ends occur here.
