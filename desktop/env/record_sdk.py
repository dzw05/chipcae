"""Export an installed SDK lock and informational package metadata."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("prefix", type=Path)
args = parser.parse_args()
output = Path(__file__).resolve().parent
packages = []
for path in sorted((args.prefix / "conda-meta").glob("*.json")):
    meta = json.loads(path.read_text())
    packages.append({key: meta.get(key) for key in
                     ("name", "version", "build", "url", "sha256")})
if not packages:
    parser.error("prefix contains no conda package records")
lock = subprocess.check_output(
    ["micromamba", "env", "export", "-p", str(args.prefix)], text=True)
lock = "\n".join(line for line in lock.splitlines()
                 if not line.startswith(("name:", "prefix:"))).rstrip() + "\n"
if "dependencies:" not in lock:
    raise RuntimeError("micromamba did not return an environment lock")
(output / "desktop-linux.lock.yml").write_text(lock)
(output / "desktop-linux.sdk.json").write_text(
    json.dumps({"format": 1, "packages": packages}, indent=2) + "\n")
