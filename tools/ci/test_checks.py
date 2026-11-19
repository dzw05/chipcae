"""Tests for the CI config and baseline-label guards."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import yaml

from tools.ci.check_baseline_label import requiring_label
from tools.ci.check_configs import check_build_parallel, check_errors, check_perf

ROOT = Path(__file__).resolve().parents[2]


class CheckConfigTest(unittest.TestCase):
    def test_shipped_perf_config(self):
        self.assertEqual(check_perf(ROOT / "bench" / "perf_config.yaml"), [])

    def test_shipped_error_table(self):
        self.assertEqual(check_errors(ROOT / "protocol" / "errors.yaml"), [])


class BuildParallelTest(unittest.TestCase):
    SOURCE = "fixture.yml"
    STEP = "Build fixture"
    GUARDED_JOBS = textwrap.dedent('''\
        if ! jobs="$(nproc)"; then
          jobs=2
        fi
        case "$jobs" in
          ''|*[!0-9]*|0) jobs=2 ;;
          [1-4]) ;;
          *) jobs=4 ;;
        esac
        echo "Build jobs: $jobs"
        ''')

    @classmethod
    def _workflow(cls, script):
        return yaml.safe_dump({"jobs": {"build": {"steps": [
            {"name": cls.STEP, "run": script},
        ]}}})

    def _check(self, script):
        return check_build_parallel(self._workflow(script), self.SOURCE)

    def _assert_one_violation(self, script, command):
        errors = self._check(script)
        self.assertEqual(len(errors), 1, errors)
        for detail in (self.SOURCE, self.STEP, command, "--parallel"):
            self.assertIn(detail, errors[0])

    def test_ci_yml_build_parallel_is_bounded(self):
        path = ROOT / ".github" / "workflows" / "ci.yml"
        self.assertEqual(
            check_build_parallel(path.read_text(encoding="utf-8"), str(path)), [],
            "Build and Build Kokkos must both be bounded; desktop --parallel 2 is valid",
        )

    def test_mesh_yml_build_parallel_is_bounded(self):
        path = ROOT / ".github" / "workflows" / "mesh.yml"
        self.assertEqual(
            check_build_parallel(path.read_text(encoding="utf-8"), str(path)), [],
            "Build mesh test targets must have an explicit 1-4 job bound",
        )

    def test_unbounded_parallel_is_rejected(self):
        command = "cmake --build build --parallel"
        errors = check_build_parallel(self._workflow(command + "\n"))
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("--parallel", errors[0])

    def test_explicit_bounded_limits_are_accepted(self):
        for jobs in ("1", "2", "3", "4"):
            with self.subTest(jobs=jobs):
                self.assertEqual(self._check(f"cmake --build build --parallel {jobs}\n"), [])
        with self.subTest(command="desktop launcher"):
            self.assertEqual(self._check(
                'micromamba run -p "$DESKTOP_PREFIX" '
                'cmake --build build-desktop --parallel 2\n'
            ), [])

    def test_bounded_variable_assignments_are_accepted(self):
        for assignment in ("jobs=1", "jobs=2", 'jobs="3"', "jobs='4'", "jobs=4\njobs=2"):
            with self.subTest(assignment=assignment):
                self.assertEqual(self._check(
                    assignment + '\ncmake --build build --parallel "$jobs"\n'
                ), [])
        with self.subTest(assignment="guarded nproc, fallback and cap"):
            self.assertEqual(self._check(
                self.GUARDED_JOBS + 'cmake --build build --parallel "$jobs"\n'
            ), [])
        with self.subTest(variable="braced"):
            self.assertEqual(self._check('jobs=4\ncmake --build build --parallel "${jobs}"\n'), [])

    def test_invalid_limits_are_rejected(self):
        for value in ('""', "''", "0", "5", "-1", "text", "999999999999999999999999999999"):
            command = f"cmake --build build --parallel {value}"
            with self.subTest(value=value):
                self._assert_one_violation(command + "\n", command)

    def test_option_or_next_command_is_not_a_parallel_argument(self):
        for tail in ("\n4\n", "\necho 4\n", "\n--parallel 4\n", " --target all\n", "; echo 4\n", " && echo 4\n"):
            with self.subTest(tail=tail):
                self._assert_one_violation(
                    "cmake --build build --parallel" + tail,
                    "cmake --build build --parallel",
                )

    def test_unknown_and_unbounded_variables_are_rejected(self):
        command = 'cmake --build build --parallel "$jobs"'
        for assignment in ("", 'jobs=""', "jobs=0", "jobs=5", "jobs=-1", "jobs=text", 'jobs="$(nproc)"'):
            with self.subTest(assignment=assignment):
                self._assert_one_violation(assignment + "\n" + command + "\n", command)
        with self.subTest(variable="undefined other name"):
            other = 'cmake --build build --parallel "$unknown"'
            self._assert_one_violation("jobs=4\n" + other + "\n", other)

    def test_later_assignment_invalidates_a_variable_bound(self):
        command = 'cmake --build build --parallel "$jobs"'
        for reassignment in ('jobs=""', "jobs=8", 'jobs="$(nproc)"', "unset jobs"):
            with self.subTest(reassignment=reassignment):
                self._assert_one_violation("jobs=4\n" + reassignment + "\n" + command + "\n", command)

    def test_each_command_uses_its_preceding_variable_bound(self):
        valid = 'cmake --build bounded --parallel "$jobs"'
        invalid = 'cmake --build unbounded --parallel "$jobs"'
        self._assert_one_violation("jobs=4\n" + valid + "\njobs=8\n" + invalid + "\n", invalid)

    def test_assignment_must_precede_the_command_in_the_same_step(self):
        command = 'cmake --build build --parallel "$jobs"'
        with self.subTest(order="assignment after command"):
            self._assert_one_violation(command + "\njobs=4\n", command)
        workflow = yaml.safe_dump({"jobs": {"build": {"steps": [
            {"name": "Set jobs", "run": "jobs=4\n"},
            {"name": self.STEP, "run": command + "\n"},
        ]}}})
        with self.subTest(scope="another step"):
            errors = check_build_parallel(workflow, self.SOURCE)
            self.assertEqual(len(errors), 1, errors)
            self.assertIn(self.STEP, errors[0])
            self.assertIn(command, errors[0])

    def test_arbitrary_conditional_assignment_is_not_proven_bounded(self):
        command = 'cmake --build build --parallel "$jobs"'
        for assignment in ("if false; then jobs=4; fi", "jobs=4\nif true; then jobs=8; fi"):
            with self.subTest(assignment=assignment):
                self._assert_one_violation(assignment + "\n" + command + "\n", command)

    def test_unsupported_shell_cannot_establish_or_preserve_a_bound(self):
        command = 'cmake --build build --parallel "$jobs"'
        for prefix in (
            "if false; then\n  jobs=4\nfi\n",
            "false && jobs=4\n",
            "jobs=4\neval 'jobs=8'\n",
            'jobs=4\necho "$((jobs=8))"\n',
            "jobs=4\nread jobs\n",
            "jobs=4\nprintf -v jobs 8\n",
        ):
            with self.subTest(prefix=prefix):
                self._assert_one_violation(prefix + command + "\n", command)

    def test_incomplete_or_modified_cap_does_not_prove_a_bound(self):
        command = 'cmake --build build --parallel "$jobs"'
        for cap in (
            self.GUARDED_JOBS.replace("*) jobs=4 ;;", "*) jobs=8 ;;"),
            self.GUARDED_JOBS.replace("''|*[!0-9]*|0) jobs=2 ;;", "''|0) jobs=2 ;;"),
            self.GUARDED_JOBS.replace("if ! jobs=", "if jobs="),
        ):
            with self.subTest(cap=cap):
                self._assert_one_violation(cap + command + "\n", command)

    def test_quoted_shell_words_do_not_supply_commands_or_extra_arguments(self):
        with self.subTest(quoted_command="example"):
            self.assertEqual(self._check('echo "cmake --build example --parallel"\n'), [])
        for argument in ('$jobs', "'$jobs'", '"$jobs"suffix'):
            command = f"cmake --build build --parallel {argument}"
            with self.subTest(argument=argument):
                self._assert_one_violation("jobs=4\n" + command + "\n", command)

    def test_empty_environment_does_not_bound_a_bare_flag(self):
        command = "cmake --build build --parallel"
        self._assert_one_violation('CMAKE_BUILD_PARALLEL_LEVEL="" ' + command + "\n", command)

    def test_every_invalid_command_has_a_diagnostic(self):
        first = "cmake --build first --parallel"
        second = "cmake --build second --parallel 5"
        desktop = 'micromamba run -p "$DESKTOP_PREFIX" cmake --build build-desktop --parallel 2'
        for separator in ("\n", "; ", " && "):
            with self.subTest(separator=separator):
                errors = self._check(separator.join((first, desktop, second)) + "\n")
                self.assertEqual(len(errors), 2, errors)
                for command in (first, second):
                    self.assertEqual(sum(command in error for error in errors), 1, errors)
                for error in errors:
                    self.assertIn(self.SOURCE, error)
                    self.assertIn(self.STEP, error)

    def test_a_valid_desktop_command_does_not_hide_an_invalid_build(self):
        command = 'micromamba run -p "$PREFIX" cmake --build bad --parallel'
        self._assert_one_violation(
            "cmake --build build-desktop --parallel 2\n" + command + "\n",
            "cmake --build bad --parallel",
        )

    def test_shell_and_yaml_comments_are_ignored(self):
        script = "# cmake --build ignored --parallel\ncmake --build build --parallel 4 # --parallel\n"
        with self.subTest(comments="shell"):
            self.assertEqual(self._check(script), [])
        with self.subTest(comments="yaml"):
            self.assertEqual(check_build_parallel(
                "# cmake --build ignored --parallel\n" + self._workflow(":\n"), self.SOURCE
            ), [])
        with self.subTest(comments="cannot supply a missing argument"):
            self._assert_one_violation(
                "cmake --build build --parallel # 4\n", "cmake --build build --parallel",
            )
        with self.subTest(comments="a comment cannot continue onto a command"):
            command = "cmake --build build --parallel"
            self._assert_one_violation("# example \\\n" + command + "\n", command)

    def test_continued_commands_keep_argument_validation(self):
        for argument in ("4", '"$jobs"'):
            with self.subTest(argument=argument):
                self.assertEqual(self._check(
                    'jobs=4\ncmake --build build \\\n  --parallel \\\n  ' + argument + "\n"
                ), [])
        for suffix in ("\n", " --target all\n", " 5\n"):
            with self.subTest(suffix=suffix):
                errors = self._check("cmake --build build \\\n  --parallel" + suffix)
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("--parallel", errors[0])

    def test_matching_is_case_sensitive_and_only_inspects_run_scripts(self):
        with self.subTest(matching="case sensitive"):
            self.assertEqual(self._check(
                "CMAKE --build build --parallel\n"
                "cmake --BUILD build --parallel\n"
                "cmake --build build --PARALLEL\n"
            ), [])
        workflow = yaml.safe_dump({"jobs": {"build": {"steps": [{
            "name": "Example cmake --build build --parallel",
            "uses": "actions/checkout@v4",
        }]}}})
        with self.subTest(matching="run scripts only"):
            self.assertEqual(check_build_parallel(workflow, self.SOURCE), [])

    def test_entry_point_checks_both_workflows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ("bench/perf_config.yaml", "protocol/errors.yaml"):
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text((ROOT / relative).read_text(encoding="utf-8"), encoding="utf-8")
            workflows = root / ".github" / "workflows"
            workflows.mkdir(parents=True)
            for invalid in (None, "ci.yml", "mesh.yml"):
                with self.subTest(invalid=invalid):
                    for name in ("ci.yml", "mesh.yml"):
                        command = "cmake --build build --parallel" + ("" if name == invalid else " 4")
                        (workflows / name).write_text(self._workflow(command + "\n"), encoding="utf-8")
                    result = subprocess.run(
                        [sys.executable, str(ROOT / "tools" / "ci" / "check_configs.py"), str(root)],
                        capture_output=True, text=True, check=False,
                    )
                    self.assertEqual(result.returncode, 0 if invalid is None else 1, result.stdout + result.stderr)
                    if invalid is not None:
                        self.assertIn(invalid, result.stdout)
                        self.assertIn("--parallel", result.stdout)

    def test_workflow_cap_blocks_handle_nproc_under_strict_bash(self):
        cases = (
            ("one core", "1", 0, "1"), ("two cores", "2", 0, "2"),
            ("three cores", "3", 0, "3"), ("four cores", "4", 0, "4"),
            ("eight cores", "8", 0, "4"), ("failure", "", 1, "2"),
            ("missing nproc", "", 127, "2"), ("empty", "", 0, "2"),
            ("text", "unknown", 0, "2"), ("zero", "0", 0, "2"),
            ("negative", "-1", 0, "2"),
            ("huge decimal", "99999999999999999999999999999999999999999999999999", 0, "4"),
        )
        for filename, names in (
            ("ci.yml", ("Build", "Build Kokkos")),
            ("mesh.yml", ("Build mesh test targets",)),
        ):
            workflow = yaml.safe_load((ROOT / ".github" / "workflows" / filename).read_text(encoding="utf-8"))
            steps = {step.get("name"): step for job in workflow["jobs"].values() for step in job.get("steps", [])}
            for name in names:
                with self.subTest(workflow=filename, step=name):
                    self.assertIn(name, steps)
                    script = steps[name]["run"]
                    build = re.search(r"\bcmake\s+--build\b", script)
                    self.assertIsNotNone(build, f"{filename}: {name}: build command missing")
                    prefix = script[:build.start()]
                    assignment = re.search(r'(?m)^\s*(?:if\s+!?\s*)?jobs="\$\(nproc\)"', prefix)
                    self.assertIsNotNone(assignment, f"{filename}: {name}: guarded nproc job cap is missing")
                    cap = prefix[assignment.start():]
                    # Only run the cap block; dependency downloads/configuration must stay outside it.
                    self.assertNotRegex(cap, r"(?m)^\s*(?:cmake|curl|tar|mkdir|ccache)\b")
                    for label, output, status, expected in cases:
                        with self.subTest(nproc=label):
                            env = os.environ.copy()
                            env.pop("BASH_ENV", None)
                            env.update(MOCK_NPROC_OUTPUT=output, MOCK_NPROC_STATUS=str(status))
                            harness = (
                                'nproc() { printf "%s\\n" "$MOCK_NPROC_OUTPUT"; return "$MOCK_NPROC_STATUS"; }\n'
                                + cap + '\nprintf "__TEST_JOBS__=%s\\n" "$jobs"\n'
                            )
                            result = subprocess.run(
                                ["bash", "--noprofile", "--norc", "-euo", "pipefail", "-c", harness],
                                env=env, capture_output=True, text=True, check=False,
                            )
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            printed, marker, value = result.stdout.rpartition("__TEST_JOBS__=")
                            self.assertTrue(marker, result.stdout)
                            self.assertEqual(value.strip(), expected, result.stdout)
                            self.assertRegex(printed, rf"\b{expected}\b", "cap block must print the selected jobs")


class BaselineLabelTest(unittest.TestCase):
    def test_new_file_does_not_need_the_label(self):
        self.assertEqual(requiring_label(["bench/baselines/new.json"], set()), [])

    def test_existing_file_needs_the_label(self):
        changed = ["bench/baselines/poisson_microbenchmark_gcc-release.json"]
        existed = set(changed)
        self.assertEqual(requiring_label(changed, existed), changed)


if __name__ == "__main__":
    unittest.main()
