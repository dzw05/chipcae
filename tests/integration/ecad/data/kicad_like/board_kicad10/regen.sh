#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
read -r -a cli <<< "${KICAD_CLI:-/tmp/kicad10/kicad-10.0.8-x86_64-lite.AppImage kicad-cli}"
[[ "$("${cli[@]}" version)" == "10.0.8" ]] || { echo 'KiCad 10.0.8 required' >&2; exit 1; }
"${cli[@]}" pcb export ipc2581 --output board.xml board.kicad_pcb
