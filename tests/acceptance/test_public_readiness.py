"""Public-readiness acceptance checks; run with Python 3 and PyYAML.

Run from any directory: python3 tests/acceptance/test_public_readiness.py -v
The scope check intentionally enforces the brief, including its no-added-files rule.
"""

import re
import shutil
import subprocess
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
BASE = "6e20f16ab660d70d46852bff9eedefad4c7b69f8"
WORKFLOWS = tuple(
    f".github/workflows/{name}.yml"
    for name in ("ci", "mesh", "mac-heavy", "nightly-mpi-gpu")
)
ALLOWED = {"LICENSE", "README.md", *WORKFLOWS}
TARGETS = (
    (WORKFLOWS[0], "build-test"),
    (WORKFLOWS[0], "mesh"),
    (WORKFLOWS[1], "mesh-tests"),
    (WORKFLOWS[2], "build-test"),
    *((WORKFLOWS[3], f"tier-b-{kind}") for kind in ("mpi", "gpu", "perf")),
)
EVENT_GUARD = "github.event_name != 'pull_request'"
REPO_GUARD = "github.event.pull_request.head.repo.full_name == github.repository"


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True)


def compact(value):
    return " ".join(value.split())


def original(path):
    return yaml.safe_load(git("show", f"{BASE}:{path}"))


class PublicReadiness(unittest.TestCase):
    def test_license_readme_phrases(self):
        phrases = (
            "Copyright (c) 2026 Zhiwei Duan",
            "All rights reserved",
            "Source is published for viewing only",
            "no permission is granted to use, copy, modify, merge, publish, "
            "distribute, sublicense, or sell without prior written permission",
        )
        for path in ("LICENSE", "README.md"):
            text = (ROOT / path).read_text()
            normalized = compact(text)
            for phrase in phrases:
                with self.subTest(path=path, phrase=phrase):
                    self.assertIn(phrase, normalized)
            for pattern in (
                "保留所有权利", "仅供(?:查阅|查看)",
                r"未经.{0,8}(?:事先|预先)书面(?:许可|授权)",
                "使用", "复制", "修改", "合并", "发布", "分发", "再许可", "销售",
            ):
                with self.subTest(path=path, chinese=pattern):
                    self.assertRegex(normalized, pattern)
            with self.subTest(path=path, forbidden=True):
                self.assertNotRegex(text.lower(), "confidential|私有闭源|不得披露")
        license_text = (ROOT / "LICENSE").read_text()
        baseline_text = git("show", f"{BASE}:LICENSE")
        for line in baseline_text.splitlines():
            if line.startswith("- "):
                with self.subTest(exception=line):
                    self.assertIn(line, license_text)
        readme = (ROOT / "README.md").read_text()
        section = re.search(r"^##\s+许可[^\n]*\n(.*?)(?=^##\s|\Z)", readme, re.M | re.S)
        self.assertIsNotNone(section, "README must retain a 许可 section")
        self.assertIn("[LICENSE](LICENSE)", section.group(1))

    def test_workflow_fork_guard(self):
        for path, job_id in TARGETS:
            job = yaml.safe_load((ROOT / path).read_text())["jobs"][job_id]
            before = original(path)["jobs"][job_id]
            with self.subTest(path=path, job=job_id, check="guard"):
                condition = compact(str(job.get("if", "")))
                self.assertEqual(condition.count(EVENT_GUARD), 1, condition)
                self.assertEqual(condition.count(REPO_GUARD), 1, condition)
                self.assertIn(f"{EVENT_GUARD} || {REPO_GUARD}", condition)
                if "if" in before:
                    self.assertIn(compact(before["if"]), condition)
                    self.assertIn("&&", condition)
            with self.subTest(path=path, job=job_id, check="runner"):
                if path == WORKFLOWS[2]:
                    self.assertEqual(job["runs-on"], ["self-hosted", "macOS", "ARM64", "mac-heavy"])
                elif "runs-on" not in before:
                    self.assertNotIn("runs-on", job)
                else:
                    runner = compact(job["runs-on"])
                    fallback = compact(before["runs-on"])[3:-3].strip()
                    expected = (
                        "${{ github.event_name == 'pull_request' && "
                        "github.event.pull_request.head.repo.full_name != github.repository "
                        "&& 'ubuntu-latest' || " + fallback + " }}"
                    )
                    # The nonempty hosted string is truthy, so forks cannot fall
                    # through to the configured self-hosted string/JSON array.
                    self.assertEqual(runner, expected)
        for path in WORKFLOWS:
            with self.subTest(path=path, check="unchanged structure"):
                current = yaml.safe_load((ROOT / path).read_text())
                baseline = original(path)
                for target_path, job_id in TARGETS:
                    if target_path == path:
                        for key in ("if", "runs-on"):
                            current["jobs"][job_id].pop(key, None)
                            baseline["jobs"][job_id].pop(key, None)
                self.assertEqual(current, baseline)

    def test_yaml_valid(self):
        for path in WORKFLOWS:
            with self.subTest(path=path):
                parsed = yaml.safe_load((ROOT / path).read_text())
                self.assertIsInstance(parsed["jobs"], dict)
        if shutil.which("actionlint"):
            result = subprocess.run(["actionlint", *WORKFLOWS], cwd=ROOT,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            print("未安装 actionlint，已用 yaml.safe_load", flush=True)

    def test_diff_scope(self):
        paths = set(git("diff", "--name-only", "origin/main...HEAD").splitlines())
        self.assertFalse(paths - ALLOWED, f"Out-of-scope paths: {sorted(paths - ALLOWED)}")
        added = git("diff", "--diff-filter=A", "--name-only", "origin/main...HEAD")
        self.assertEqual(added, "", f"Added files forbidden by feature brief: {added}")


if __name__ == "__main__":
    unittest.main()
