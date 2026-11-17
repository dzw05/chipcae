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
    if generate:
        reference.mkdir(parents=True, exist_ok=True)
        (reference / "exit_code").write_text(f"{result.returncode}\n")
    else:
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
        elif case in ("ecad_off", "step"):
            source = FIXTURES / "board_2layer.xml" if case == "ecad_off" else HERE / "input.step"
            result = invoke(binary, [source, "-o", "stack.json"], directory)
            check(result.returncode == 7, f"{case}: expected exit 7, got {result.returncode}")
            record_or_compare(result, directory, golden / case, ("defects.json",), generate)
        elif case == "help":
            result = invoke(binary, ["--help"], directory)
            check(result.returncode == 0, f"help: exit {result.returncode}")
            check(result.stderr == b"", f"help: unexpected stderr {result.stderr!r}")
            if generate:
                (golden / "help.stdout").write_bytes(result.stdout)
            else:
                check(result.stdout == (golden / "help.stdout").read_bytes(), "help: stdout bytes differ")
        elif case == "reserved":
            failures = []
            for option, value in (("--accept", "x"), ("--t-min", "1e-5"),
                                  ("--stack-override", "x"), ("--assembly", "x")):
                result = invoke(binary, [FIXTURES / "board_2layer.xml", "-o", "stack.json",
                                         option, value], directory)
                if result.returncode != 2:
                    failures.append(f"{option}: expected exit 2, got {result.returncode}")
                if b"not implemented yet" not in result.stderr or option.encode() not in result.stderr:
                    failures.append(f"{option}: expected stderr containing 'not implemented yet' "
                                    f"and '{option}'; actual={result.stderr!r}")
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
