#!/usr/bin/env python3
"""Byte-level CLI acceptance tests; generation is only for the documented baseline."""
import argparse
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
                record_or_compare(result, work, golden / board, ARTIFACTS, generate)
            variants = (
                ("accept_b_tier", ["--accept-b-tier"], ARTIFACTS),
                ("out_alias", [], ARTIFACTS),
                ("custom_paths", ["--polyset", "custom.polyset", "--report", "custom.json"],
                 (*ARTIFACTS, "custom.polyset", "custom.json")),
                ("missing_material_map", [], ARTIFACTS),
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
            record_or_compare(result, directory, golden / case, ("defects.json",), generate)
        elif case == "help":
            for name, args, code in (
                ("help", ["--help"], 0), ("help_alias", ["-h"], 0),
                ("missing_both", [], 2),
                ("missing_input", ["-o", "stack.json"], 2),
                ("missing_output", [FIXTURES / "board_2layer.xml"], 2),
                ("missing_out_value", [FIXTURES / "board_2layer.xml", "-o"], 2),
            ):
                result = invoke(binary, args, directory)
                check(result.returncode == code, f"{name}: exit {result.returncode}")
                check(result.stderr == b"", f"{name}: unexpected stderr {result.stderr!r}")
                record_or_compare(result, directory, golden / name, ("stdout", "stderr"), generate)
            result = invoke(binary, ["a.xml", "-o", "b", "--help"], directory)
            check(result.returncode == 2, "mixed help: expected exit 2")
            check(result.stdout == b"", "mixed help: unexpected stdout")
            check(result.stderr == b"unknown argument: --help\n", "mixed help: stderr differs")
            if not generate:
                for option in ("--accept", "--t-min", "--stack-override", "--assembly"):
                    for prefix in ([], ["a.xml"], ["-o", "stack.json"]):
                        result = invoke(binary, [*prefix, option, "xyz"], directory)
                        check(result.returncode == 2, f"{option}: missing required argument exit")
                        check(result.stdout == (golden / "help" / "stdout").read_bytes(),
                              f"{option}: missing required argument help differs")
                        check(result.stderr == b"", f"{option}: missing required argument stderr")
        elif case == "reserved":
            check(not generate, "reserved diagnostics are assertions, not baseline goldens")
            failures = []
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
            for option in ("--accept", "--t-min", "--stack-override", "--assembly"):
                for prefix in ([], ["a.xml", "-o", "stack.json"],
                               ["a.xml", "-o", "stack.json", "--accept-b-tier"]):
                    result = invoke(binary, [*prefix, option], directory)
                    check(result.returncode == 2, f"missing {option}: exit {result.returncode}")
                    if prefix and not (option == "--accept" and "--accept-b-tier" in prefix):
                        check(b"not implemented yet" in result.stderr and
                              option.encode() in result.stderr,
                              f"missing {option}: reserved diagnostic differs: {result.stderr!r}")
            for flags in (("--accept-b-tier", "--accept", "x"),
                          ("--accept", "x", "--accept-b-tier")):
                result = invoke(binary, [FIXTURES / "board_2layer.xml", "-o", "stack.json", *flags], directory)
                if result.returncode != 2:
                    failures.append(f"conflict {flags}: expected exit 2, got {result.returncode}")
            check(not failures, "\n".join(failures))
        else:
            raise ValueError(case)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("case", choices=("noop", "ecad_off", "step", "help", "reserved"))
    parser.add_argument("--generate", action="store_true")
    args = parser.parse_args()
    run_case(args.binary.resolve(), args.case, args.generate)
