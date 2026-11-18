#!/usr/bin/env python3
"""Byte-level CLI acceptance tests; generation is only for the documented baseline."""
import argparse
import itertools
import pathlib
import shutil
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
FIXTURES = HERE.parent / "ecad" / "data"
ARTIFACTS = ("stack.json", "stack.polyset", "defects.json",
             "import_report.json", "import_report.csv")
BOARDS = ("board_2layer", "board_4layer", "board_arc", "unsupported")


def invoke(binary, args, directory):
    return subprocess.run([str(binary), *map(str, args)], cwd=directory,
                          capture_output=True, check=False)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def record_or_compare(result, directory, reference, artifacts, generate):
    (directory / "stdout").write_bytes(result.stdout)
    (directory / "stderr").write_bytes(result.stderr)
    if generate:
        reference.mkdir(parents=True, exist_ok=True)
        (reference / "exit_code").write_text(f"{result.returncode}\n")
    else:
        check((reference / "exit_code").is_file(), f"missing golden: {reference / 'exit_code'}")
        expected = int((reference / "exit_code").read_text())
        check(result.returncode == expected,
              f"{reference.name}: exit {result.returncode}, expected {expected}; "
              f"stderr={result.stderr!r}")
    for name in artifacts:
        actual, golden = directory / name, reference / name
        if generate:
            if actual.exists():
                shutil.copyfile(actual, golden)
            elif golden.exists():
                golden.unlink()
        else:
            check(actual.exists() == golden.exists(),
                  f"{reference.name}/{name}: presence differs")
            if golden.exists():
                check(actual.read_bytes() == golden.read_bytes(),
                      f"{reference.name}/{name}: bytes differ")


def run_case(binary, case, generate=False):
    golden = HERE / "golden"
    with tempfile.TemporaryDirectory(prefix="import-cli-") as temporary:
        directory = pathlib.Path(temporary)
        if case == "noop":
            for board in BOARDS:
                work = directory / board
                work.mkdir()
                result = invoke(binary, [FIXTURES / f"{board}.xml", "-o", "stack.json",
                                         "--material-map", HERE / "material_map.json"], work)
                record_or_compare(result, work, golden / board, (*ARTIFACTS, "stdout", "stderr"), generate)
            for name in ("empty_input", "truncated_input"):
                work = directory / name
                work.mkdir()
                shutil.copyfile(HERE / f"{name}.xml", work / "input.xml")
                result = invoke(binary, ["input.xml", "-o", "stack.json",
                                         "--material-map", HERE / "material_map.json"], work)
                record_or_compare(result, work, golden / name,
                                  (*ARTIFACTS, "stdout", "stderr"), generate)
            variants = (
                ("accept_b_tier", ["--accept-b-tier"], ARTIFACTS),
                ("out_alias", [], ARTIFACTS),
                ("custom_paths", ["--polyset", "custom.polyset", "--report", "custom.json"],
                 (*ARTIFACTS, "custom.polyset", "custom.json")),
                ("missing_material_map", [], ARTIFACTS),
                ("duplicate_input", ["b.xml"], ARTIFACTS),
                ("duplicate_output", ["-o", "other.json", "-o", "stack.json"], ARTIFACTS),
                ("duplicate_material_map", ["--material-map", HERE / "material_map.json"], ARTIFACTS),
                ("duplicate_accept_b_tier", ["--accept-b-tier", "--accept-b-tier"], ARTIFACTS),
            )
            for name, flags, artifacts in variants:
                work = directory / name
                work.mkdir()
                args = [FIXTURES / "board_2layer.xml",
                        "--out" if name == "out_alias" else "-o", "stack.json"]
                if name != "missing_material_map":
                    args += ["--material-map", HERE / "material_map.json"]
                result = invoke(binary, [*args, *flags], work)
                record_or_compare(result, work, golden / name,
                                  (*artifacts, "stdout", "stderr"), generate)
                if name == "accept_b_tier":
                    check(b'"b_tier_accepted":"all"' in (work / "import_report.json").read_bytes(),
                          "accept_b_tier: expected accepted report")
        elif case in ("ecad_off", "step"):
            source = FIXTURES / "board_2layer.xml" if case == "ecad_off" else HERE / "input.step"
            result = invoke(binary, [source, "-o", "stack.json"], directory)
            check(result.returncode == 7, f"{case}: expected exit 7, got {result.returncode}")
            record_or_compare(result, directory, golden / case,
                              ("defects.json",) if case == "ecad_off" else
                              ("defects.json", "stdout", "stderr"), generate)
        elif case == "help":
            for name, args, code in (
                ("help", ["--help"], 0), ("help_alias", ["-h"], 0),
                ("missing_both", [], 2),
                ("missing_input", ["-o", "stack.json"], 2),
                ("missing_output", [FIXTURES / "board_2layer.xml"], 2),
                ("missing_out_value", [FIXTURES / "board_2layer.xml", "-o"], 2),
                *((f"missing_{option[2:].replace('-', '_')}_value",
                   ["a.xml", "-o", "stack.json", option], 2)
                  for option in ("--polyset", "--report", "--material-map")),
            ):
                result = invoke(binary, args, directory)
                check(result.returncode == code, f"{name}: exit {result.returncode}")
                check(result.stderr == b"", f"{name}: unexpected stderr {result.stderr!r}")
                record_or_compare(result, directory, golden / name, ("stdout", "stderr"), generate)
            result = invoke(binary, ["a.xml", "-o", "b", "--help"], directory)
            record_or_compare(result, directory, golden / "mixed_help",
                              ("stdout", "stderr"), generate)
        elif case == "incomplete_reserved":
            # Earlier baseline errors must win over later reserved options.
            for option in ("--accept", "--t-min", "--stack-override", "--assembly"):
                for name, prefix in (
                    ("duplicate_before_output", ["a.xml", "b.xml", "-o", "stack.json"]),
                    ("duplicate_after_output", ["a.xml", "-o", "stack.json", "b.xml"]),
                    ("unknown", ["a.xml", "-o", "stack.json", "--unknown"]),
                    ("mixed_help", ["a.xml", "-o", "stack.json", "--help"]),
                ):
                    result = invoke(binary, [*prefix, option, "xyz"], directory)
                    record_or_compare(result, directory,
                                      golden / f"precedence_{name}_{option[2:]}",
                                      (*ARTIFACTS, "stdout", "stderr"), generate)
            # Without input or assembly, preserve the first unknown reserved token.
            for first, second in itertools.permutations(
                    ("--accept", "--t-min", "--stack-override"), 2):
                for output_option in ("-o", "--out"):
                    result = invoke(binary, [output_option, "stack.json", first, "x",
                                             second, "y"], directory)
                    record_or_compare(result, directory,
                                      golden / f"precedence_{output_option.lstrip('-')}_"
                                      f"{first[2:]}_{second[2:]}",
                                      (*ARTIFACTS, "stdout", "stderr"), generate)
            for option in ("--accept", "--t-min", "--stack-override", "--assembly"):
                for name, prefix in (("bare", []), ("input", ["a.xml"]),
                                     ("output", ["-o", "stack.json"])):
                    if option == "--assembly" and name == "output":
                        continue  # Intentional diagnostic change, checked in reserved.
                    result = invoke(binary, [*prefix, option, "xyz"], directory)
                    record_or_compare(result, directory,
                                      golden / f"incomplete_{option[2:]}_{name}",
                                      ("stdout", "stderr"), generate)
            for option in ("--accept", "--t-min", "--stack-override", "--assembly"):
                for name, prefix in (("bare", []), ("valid", ["a.xml", "-o", "stack.json"]),
                                     ("conflict", ["a.xml", "-o", "stack.json", "--accept-b-tier"])):
                    result = invoke(binary, [*prefix, option], directory)
                    record_or_compare(result, directory,
                                      golden / f"missing_reserved_{option[2:]}_{name}",
                                      ("stdout", "stderr"), generate)
            for index, flags in enumerate((("--accept", "--accept-b-tier", "--accept"),
                                           ("--accept", "x", "--accept-b-tier", "--accept"))):
                result = invoke(binary, ["a.xml", "-o", "stack.json", *flags], directory)
                record_or_compare(result, directory, golden / f"repeated_missing_accept_{index}",
                                  ("stdout", "stderr"), generate)
        elif case == "reserved":
            check(not generate, "reserved diagnostics are assertions, not baseline goldens")
            failures = []
            result = invoke(binary, ["--assembly", "assembly.json", "-o", "out.stack.json"], directory)
            check(result.returncode == 7 and b"IMPORT_REJECTED: assembly=" in result.stderr and
                  b"json_path=$" in result.stderr, "assembly invocation must reach the real handler")
            result = invoke(binary, ["-o", "stack.json", "--assembly", "xyz"], directory)
            check(result.returncode == 7 and result.stdout == b"" and
                  b"IMPORT_REJECTED: assembly=" in result.stderr,
                  "assembly output-first must reach the real handler")
            # Assembly validates its supported/reserved options after mode selection.
            for option in ("--accept", "--t-min", "--stack-override"):
                for output_option in ("-o", "--out"):
                    groups = ((output_option, "stack.json"), (option, "xyz"),
                              ("--assembly", "assembly.json"))
                    for ordering in itertools.permutations(groups):
                        args = [arg for group in ordering for arg in group]
                        result = invoke(binary, args, directory)
                        if option == "--accept":
                            check(result.returncode == 7 and result.stdout == b"" and
                                  b"IMPORT_REJECTED: assembly=" in result.stderr,
                                  f"assembly accept order {args}: {result.stderr!r}")
                        else:
                            check(result.returncode == 2 and result.stdout == b"" and
                                  b"not implemented yet" in result.stderr and option.encode() in result.stderr,
                                  f"assembly reserved option {args}: {result.stderr!r}")
            for value in ("xyz", "1e9999", "1e-5", ""):
                result = invoke(binary, ["a.xml", "-o", "stack.json", "--t-min", value], directory)
                check(result.returncode == 2 and b"not implemented yet" in result.stderr and
                      b"--t-min" in result.stderr,
                      f"--t-min {value!r}: reserved diagnostic differs: {result.stderr!r}")
            for option, value in (("--accept", "x"), ("--t-min", "1e-5"),
                                  ("--stack-override", "x"), ("--assembly", "x")):
                result = invoke(binary, [FIXTURES / "board_2layer.xml", "-o", "stack.json",
                                         option, value], directory)
                if result.returncode != 2:
                    failures.append(f"{option}: expected exit 2, got {result.returncode}")
                if b"not implemented yet" not in result.stderr or option.encode() not in result.stderr:
                    failures.append(f"{option}: expected stderr containing 'not implemented yet' "
                                    f"and '{option}'; actual={result.stderr!r}")
            # Like existing valued options, reserved options consume even flag-shaped values.
            for option in ("--accept", "--t-min", "--stack-override", "--assembly"):
                for value in ("--accept-b-tier", "--report", "--help", "-1e-5"):
                    result = invoke(binary, ["a.xml", "-o", "stack.json", option, value], directory)
                    check(result.returncode == 2 and result.stdout == b"" and
                          b"not implemented yet" in result.stderr and option.encode() in result.stderr,
                          f"{option} value {value}: reserved diagnostic differs: {result.stderr!r}")
                result = invoke(binary, ["a.xml", "-o", "stack.json", option,
                                         "--report", "defects.json"], directory)
                check(result.returncode == 2 and result.stdout == b"" and
                      result.stderr == b"unknown argument: defects.json\n",
                      f"{option}: flag-shaped value was not consumed: {result.stderr!r}")
            for flags in (("--accept-b-tier", "--accept", "x"),
                          ("--accept", "x", "--accept-b-tier")):
                result = invoke(binary, [FIXTURES / "board_2layer.xml", "-o", "stack.json", *flags], directory)
                if result.returncode != 2:
                    failures.append(f"conflict {flags}: expected exit 2, got {result.returncode}")
                check(result.stdout == b"" and result.stderr == b"",
                      f"conflict {flags}: expected silent usage failure")
            check(not failures, "\n".join(failures))
        else:
            raise ValueError(case)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("case", choices=("noop", "ecad_off", "step", "help", "reserved", "incomplete_reserved"))
    parser.add_argument("--generate", action="store_true")
    args = parser.parse_args()
    run_case(args.binary.resolve(), args.case, args.generate)
