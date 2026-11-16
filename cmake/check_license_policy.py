#!/usr/bin/env python3
"""Fail if excluded or GPL-risk dependency names appear in build files."""

from __future__ import annotations

import sys
from pathlib import Path

# Names that must not be introduced as project dependencies.
EXCLUDED = (
    "amrex",
    "p4est",
    "t8code",
    "basilisk",
    "dealii",
    "deal.ii",
    "cgal",
    "openfoam",
)

SCAN_SUFFIXES = {".cmake", ".txt", ".yml", ".yaml", ".sh", ".py", ".md"}
SCAN_FILES = {"CMakeLists.txt", "CMakeLists.tx"}
SKIP_DIRS = {".git", "build", "build-debug", "build-release", ".cache"}
# Policy documents may mention excluded names while explaining why they are banned.
ALLOW_MENTION_DIRS = {"docs", "cmake"}


def should_skip(path: Path, root: Path) -> bool:
    rel = path.relative_to(root)
    return any(part in SKIP_DIRS or part.startswith("build") for part in rel.parts)


def is_policy_doc(path: Path, root: Path) -> bool:
    rel = path.relative_to(root)
    return rel.parts and rel.parts[0] in ALLOW_MENTION_DIRS


def iter_files(root: Path):
    for path in root.rglob("*"):
        if not path.is_file() or should_skip(path, root):
            continue
        if path.suffix.lower() in SCAN_SUFFIXES or path.name in {"CMakeLists.txt"}:
            yield path


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    violations = []
    for path in iter_files(root):
        if is_policy_doc(path, root):
            continue
        text = path.read_text(encoding="utf-8", errors="replace").lower()
        for name in EXCLUDED:
            # Require a token-like occurrence so comments about "not using X" in
            # CMake still get caught if someone adds find_package(AMReX).
            if name in text:
                violations.append(f"{path.relative_to(root)}: mentions excluded dependency '{name}'")
    if violations:
        print("License policy check failed:")
        for item in violations:
            print(f"  {item}")
        return 1
    print("License policy check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
