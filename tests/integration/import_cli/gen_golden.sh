#!/usr/bin/env bash
# Use only with the ECAD ON/OFF binaries built from the README's baseline.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "usage: $0 <baseline-ecad-on-chipcae-import> <baseline-ecad-off-chipcae-import>" >&2
    exit 2
fi
here=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$here/golden"
# Reserved diagnostics intentionally differ from baseline: assert them directly,
# without generating goldens (see README.md).
for scenario in noop step help; do
    python3 "$here/cli_test.py" "$1" "$scenario" --generate
done
python3 "$here/cli_test.py" "$2" ecad_off --generate
