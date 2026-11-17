#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/../../.." && pwd)
cli=${KICAD_CLI:-/tmp/kicad10/kicad-10.0.8-x86_64-lite.AppImage}
run_cli() {
    if [[ "$cli" == *.AppImage ]]; then
        "$cli" kicad-cli "$@"
    else
        "$cli" "$@"
    fi
}
version=$(run_cli version)
if [[ "$version" != 10.0.8 ]]; then
    printf 'Expected KiCad 10.0.8; detected: %s\n' "$version" >&2
    exit 1
fi
out="$repo/tests/integration/ecad/data/odbpp"
mkdir -p "$out"
run_cli pcb drc --refill-zones --save-board -o "${TMPDIR:-/tmp}/kicad4l-drc.rpt" "$here/kicad4l.kicad_pcb"
run_cli pcb export odb --compression tgz -o "$out/kicad4l.tgz" "$here/kicad4l.kicad_pcb"
run_cli pcb export odb --compression zip -o "$out/kicad4l.zip" "$here/kicad4l.kicad_pcb"
run_cli pcb export ipc2581 -o "$out/kicad4l.ipc2581.xml" "$here/kicad4l.kicad_pcb"
python3 - "$out" <<'PY'
import pathlib, shutil, subprocess, sys, tempfile, tarfile, zipfile
out = pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory() as temporary:
    subprocess.run(['cmake', '-E', 'tar', 'xf', str(out / 'kicad4l.tgz')], cwd=temporary, check=True)
    job, = pathlib.Path(temporary).iterdir()
    # KiCad 10.0.8 omits job attributes; supply an empty attribute list.
    (job / 'misc/attrlist').write_text('')
    with tarfile.open(out / 'kicad4l.tgz', 'w:gz') as archive:
        archive.add(job, arcname=job.name)
    with zipfile.ZipFile(out / 'kicad4l.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(job.rglob('*')):
            if file.is_file():
                archive.write(file, file.relative_to(job.parent))
    if (out / 'kicad4l').exists():
        shutil.rmtree(out / 'kicad4l')
    shutil.move(str(job), str(out / 'kicad4l'))
PY
