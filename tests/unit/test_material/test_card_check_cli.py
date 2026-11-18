"""Exercise material_card_check dispatch, diagnostics, and argument handling."""

import json
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def diagnostic_lines(stderr):
    """Remove Kokkos notices and their indented continuation lines only."""
    lines = []
    in_notice = False
    for line in stderr.splitlines():
        if line.startswith("Kokkos::"):
            in_notice = True
            continue
        if in_notice and line.startswith("  "):
            continue
        in_notice = False
        lines.append(line)
    return lines


def check_case(executable, arguments, expected_rc, stderr_rule=None, expected_text=None):
    result = subprocess.run(
        [executable, *map(str, arguments)], capture_output=True, text=True
    )
    lines = diagnostic_lines(result.stderr)
    context = (
        f"expected rc={expected_rc}, actual rc={result.returncode}, "
        f"stderr={result.stderr!r}, filtered stderr={lines!r}"
    )
    if result.returncode != expected_rc:
        raise ValueError(context)
    if stderr_rule == "first" and (not lines or lines[0] != expected_text):
        raise ValueError(f"expected first stderr line == {expected_text!r}; {context}")
    if stderr_rule == "empty" and any(lines):
        raise ValueError(f"expected no nonempty stderr lines; {context}")
    if stderr_rule == "nonempty" and not any(lines):
        raise ValueError(f"expected at least one nonempty stderr line; {context}")
    if stderr_rule == "prefix" and not any(
        line.startswith(expected_text) for line in lines
    ):
        raise ValueError(f"expected stderr line starting with {expected_text!r}; {context}")


def main():
    executable = os.environ.get("CHIPCAE_MATERIAL_CARD_CHECK", "")
    if not executable or not Path(executable).is_file():
        print("SKIP matcli_card_check_cli: material_card_check not built")
        return 77

    name = "setup"
    try:
        root = Path(sys.argv[1])
        valid_card = root / "material/tests/valid_elastic.json"
        library = root / "schema/examples/fcbga_demo.materials.json"
        negative_library = root / "material/schema/examples"
        with TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            missing = temporary / "nope.json"
            name = "empty_file"
            empty = temporary / "empty.json"
            empty.write_bytes(b"")
            name = "library_duplicate_id"
            card = json.loads(valid_card.read_text(encoding="utf-8"))
            duplicate = temporary / "duplicate.json"
            duplicate.write_text(
                json.dumps(
                    {"schema": "chipcae.material_library/1", "materials": [card, card]}
                ),
                encoding="utf-8",
            )

            cases = [
                (
                    "usage", [], 2, "first",
                    "usage: material_card_check card-or-library.json [--delivery]",
                ),
                (
                    "missing_file", [missing], 1, "first",
                    f"{missing}: cannot open file",
                ),
                ("single_card_ok", [valid_card], 0, "empty", None),
                (
                    "single_card_bad", [root / "material/tests/negative/constant.json"],
                    1, "nonempty", None,
                ),
                ("library_ok", [library], 0, "empty", None),
                (
                    "library_missing_schema",
                    [negative_library / "negative-library-missing-schema.json"],
                    1, "prefix", "schema: ",
                ),
                (
                    "library_invalid_card",
                    [negative_library / "negative-library-invalid-material.json"],
                    1, "prefix", "materials[0]",
                ),
                (
                    "library_duplicate_id", [duplicate], 1, "prefix",
                    "materials[1].id: ",
                ),
                ("delivery_rejects_illustrative", [library, "--delivery"], 1, None, None),
                ("delivery_case", [library, "--Delivery"], 0, None, None),
                ("delivery_third_arg", [library, "extra", "--delivery"], 0, None, None),
                ("empty_file", [empty], 1, "nonempty", None),
            ]
            for name, arguments, expected_rc, stderr_rule, expected_text in cases:
                check_case(executable, arguments, expected_rc, stderr_rule, expected_text)
    except (OSError, ValueError, UnicodeError, IndexError) as error:
        print(f"FAIL matcli_card_check_cli: {name}: {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
