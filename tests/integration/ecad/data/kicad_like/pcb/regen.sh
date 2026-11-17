#!/bin/sh
# Optional manual KiCad export comparison; automated tests use gen_kicad_like.py.
set -eu
cd "$(dirname "$0")"
for board in board_2layer board_4layer; do
    kicad-cli pcb export ipc2581 --output "${board}.ipc2581.xml" "${board}.kicad_pcb"
done
