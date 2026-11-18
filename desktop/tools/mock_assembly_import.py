#!/usr/bin/env python3
"""Synthetic assembly importer for desktop acceptance; no STEP geometry is read."""
import argparse
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import sys


class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.print_usage(sys.stderr)
        self.exit(1, f"{self.prog}: error: {message}\n")


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def unsafe_path(value):
    if not isinstance(value, str):
        return True
    return (PurePosixPath(value).is_absolute()
            or PureWindowsPath(value).is_absolute()
            or ".." in PurePosixPath(value.replace("\\", "/")).parts)


def rejection(assembly):
    if not isinstance(assembly, dict) or assembly.get("schema") != "chipcae.assembly/0.1":
        return "invalid assembly schema"
    board = assembly.get("board", {})
    parts = assembly.get("parts", [])
    if not isinstance(board, dict) or not isinstance(parts, list):
        return "invalid board or parts"
    paths = [board[key] for key in ("path", "material_map") if key in board]
    if "materials" in assembly:
        paths.append(assembly["materials"])
    ids = set()
    for part in parts:
        if not isinstance(part, dict) or not isinstance(part.get("id"), str):
            return "invalid part"
        if part["id"] in ids:
            return "duplicate part id"
        ids.add(part["id"])
        if "step" in part:
            paths.append(part["step"])
    if any(unsafe_path(path) for path in paths):
        return "unsafe internal path"
    return ""


def make_stack(parts):
    # Deterministic synthetic geometry in metres, independent of STEP contents.
    regions = []
    for index, part in enumerate(parts):
        x = index * 0.002
        regions.append({"name": part["id"], "body": part["id"],
                        "material": "silicon",
                        "footprint": {"box": [x, 0, x + 0.001, 0.001]}})
        if "bond" in part.get("placement", {}):
            regions.append({"name": part["id"] + ".bond", "body": part["id"] + ".bond",
                            "material": "underfill",
                            "footprint": {"box": [x, 0.002, x + 0.001, 0.003]}})
    return {"id": "mock_assembly", "unit": "m", "symmetry": "none",
            "package_type": "FCBGA",
            "outline": {"box": [0, 0, max(0.02, len(parts) * 0.002), 0.02]},
            "layers": [{"name": "assembly", "thickness": 0.0005, "kind": "Explicit",
                        "material": "underfill", "n_elem_min": 2, "grading": 1,
                        "regions": regions}]}


def main():
    parser = Parser(allow_abbrev=False)
    parser.add_argument("--assembly", required=True)
    parser.add_argument("-o", required=True)
    parser.add_argument("--report")
    parser.add_argument("--polyset")
    parser.add_argument("--accept-b-tier", action="store_true")
    args = parser.parse_args()
    try:
        contents = Path(args.assembly).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        print(error, file=sys.stderr)
        return 1
    try:
        assembly = json.loads(contents)
        reason = rejection(assembly)
    except (ValueError, TypeError):
        reason = "invalid assembly JSON"
    try:
        if reason:
            if args.report:
                write_json(args.report, {"code": "IMPORT_REJECTED", "message": reason,
                                         "defects": [{"message": reason}]})
            return 7
        parts = assembly.get("parts", [])
        write_json(args.o, make_stack(parts))
        if args.report:
            write_json(args.report, {"code": "OK", "message": "", "defects": [],
                                     "parts": [{"id": part["id"],
                                                "material_approx": part.get("material_approx") is True}
                                               for part in parts]})
    except (OSError, TypeError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
