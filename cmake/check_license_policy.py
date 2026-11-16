#!/usr/bin/env python3
"""Fail if excluded or GPL-risk dependency names appear in build files."""

from __future__ import annotations

import re
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
    "parmetis",
    "libigl",
)

SCAN_SUFFIXES = {".cmake", ".txt", ".yml", ".yaml", ".sh", ".py", ".md"}
SKIP_DIRS = {".git", "build", "build-debug", "build-release", ".cache"}
# Policy documents may mention excluded names while explaining why they are banned.
ALLOW_MENTION_DIRS = {"docs"}
ALLOW_MENTION_FILES = {
    "cmake/ChipcaeLicensePolicy.cmake",
    "cmake/check_license_policy.py",
    "cmake/dependencies.allowlist",
}
# pkg_check_modules is handled separately: the first argument is a prefix, not a package name.
DEP_RE = re.compile(
    r"(find_package|FetchContent_Declare|ExternalProject_Add)\s*\(\s*([A-Za-z0-9_.\-]+)",
    re.I,
)
PKG_RE = re.compile(r"pkg_check_modules\s*\(([^)]*)\)", re.I | re.S)
PKG_FLAGS = {
    "required",
    "quiet",
    "imported_target",
    "global",
    "no_cmake_path",
    "no_cmake_environment_path",
}


def normalize(name):
    return "python" if name.lower() == "python3" else name.lower()


def should_skip(path: Path, root: Path) -> bool:
    rel = path.relative_to(root)
    return any(part in SKIP_DIRS or part.startswith("build") for part in rel.parts)


def is_policy_doc(path: Path, root: Path) -> bool:
    rel = path.relative_to(root)
    return rel.as_posix() in ALLOW_MENTION_FILES or (rel.parts and rel.parts[0] in ALLOW_MENTION_DIRS)


def iter_files(root: Path):
    for path in root.rglob("*"):
        if not path.is_file() or should_skip(path, root):
            continue
        if path.suffix.lower() in SCAN_SUFFIXES or path.name in {"CMakeLists.txt"}:
            yield path


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    allowed = {
        normalize(line.split("#", 1)[0].strip())
        for line in (root / "cmake/dependencies.allowlist").read_text().splitlines()
        if line.split("#", 1)[0].strip()
    }
    violations = []
    for path in iter_files(root):
        if is_policy_doc(path, root):
            continue
        text = path.read_text(encoding="utf-8", errors="replace").lower()
        for _, dependency in DEP_RE.findall(text):
            if normalize(dependency) not in allowed:
                violations.append(
                    f"{path.relative_to(root)}: dependency '{dependency}' is not allowlisted"
                )
        for args in PKG_RE.findall(text):
            tokens = args.split()
            if not tokens:
                continue
            for token in tokens[1:]:  # skip the prefix
                if token.lower() in PKG_FLAGS or token.startswith("$"):
                    continue
                name = token.split(">")[0]  # drop version constraints like foo>=1.0
                if name and normalize(name) not in allowed:
                    violations.append(
                        f"{path.relative_to(root)}: dependency '{name}' is not allowlisted"
                    )
        for name in EXCLUDED:
            # Require a token-like occurrence so comments about "not using X" in
            # CMake still get caught if someone adds find_package(AMReX).
            if name in text:
                violations.append(
                    f"{path.relative_to(root)}: mentions excluded dependency '{name}'"
                )
    if violations:
        print("License policy check failed:")
        for item in violations:
            print(f"  {item}")
        return 1
    print("License policy check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
