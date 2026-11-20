#!/usr/bin/env python3
"""Black-box acceptance for the cutgeom-nc/1.0 fixture tools (standard library).

Run: python3 -m unittest discover -s tests/contract/fixtures/cutgeom-nc-1.0 -v
This test-first suite intentionally fails until gen.py, check.py and their data
are delivered. It neither implements nor substitutes for those tools. Temporary
copies isolate destructive probes; the source checkout is always read-only.

CUTGEOM_BRANCH_REF overrides the local #202 source ref. CUTGEOM_HANDOFF points
to the handoff fixture directory when it is outside the sibling handoff tree.
Runtime C++ assertions (rollback, bitwise fast path, sq_w and small_subs) remain
the consuming cards' responsibility, as required by the brief.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from decimal import Decimal, localcontext
from fractions import Fraction


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
RELATIVE = HERE.relative_to(REPO)
CASES = (
    "l_shape", "v_shape", "r_shape", "l1_corner", "convex_split2",
    "chamfer_lid_octagon", "errors",
)
BRANCH = os.environ.get("CUTGEOM_BRANCH_REF", "origin/mesh/cut-union-kernel")
SOURCE = "tests/unit/region_ls/test_plane_cut_union.cpp"


def manifest(root):
    """Exclude this harness and interpreter caches, neither of which is fixture data."""
    return {
        str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(root.rglob("*"))
        if p.is_file() and "__pycache__" not in p.parts
        and not p.name.startswith("test_")
    }


def leaves(value):
    if isinstance(value, dict):
        for child in value.values():
            yield from leaves(child)
    elif isinstance(value, list):
        for child in value:
            yield from leaves(child)
    else:
        yield value


def decimal_values(value):
    result = set()
    for child in leaves(value):
        if isinstance(child, str):
            try:
                result.add(Fraction(child))
            except ValueError:
                pass
    return result


class FixtureAcceptance(unittest.TestCase):
    def command(self, *args, env=None):
        return subprocess.run(
            [str(a) for a in args], capture_output=True, text=True,
            timeout=120, env=env,
        )

    def status(self, result, expected):
        self.assertEqual(
            result.returncode, expected,
            f"command: {result.args}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )

    def tool(self, name, root=HERE):
        script = root / name
        self.assertTrue(
            script.is_file(),
            f"missing cutgeom-nc/1.0 acceptance deliverable: {name}; "
            "fixture generation/checking has not been implemented",
        )
        return script

    def checker(self, root=HERE, repo=REPO, *extra, env=None):
        return self.command(
            sys.executable, self.tool("check.py", root), "--repo", repo,
            *extra, env=env,
        )

    def isolated(self, temporary):
        """Copy only the permitted fixture tree, preserving its repository location."""
        self.tool("check.py")
        repo = Path(temporary) / "repo"
        root = repo / RELATIVE
        shutil.copytree(HERE, root, ignore=shutil.ignore_patterns("__pycache__"))
        return repo, root

    def read_json(self, path):
        self.assertTrue(path.is_file(), f"missing fixture data: {path.relative_to(HERE)}")
        return json.loads(path.read_text(encoding="utf-8"))

    def mutate(self, relative, change):
        with tempfile.TemporaryDirectory() as tmp:
            repo, root = self.isolated(tmp)
            # Ensure a failure is caused by the mutation, not an invalid baseline.
            self.status(self.checker(root, repo), 0)
            path = root / relative
            data = json.loads(path.read_text(encoding="utf-8"))
            change(data)
            path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
            before = manifest(root)
            self.status(self.checker(root, repo), 1)
            self.assertEqual(manifest(root), before, "checker must not repair bad data")

    def test_checker_accepts_complete_fixture_without_writing(self):
        before = manifest(HERE)
        self.status(self.checker(), 0)
        self.assertEqual(manifest(HERE), before)
        self.read_json(HERE / "expected.json")
        for case in CASES:
            with self.subTest(case=case):
                self.read_json(HERE / case / "input.json")
                self.read_json(HERE / case / "expected.json")

    def test_checker_rejects_empty_data_with_assertion_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo, root = self.isolated(tmp)
            for path in root.rglob("*.json"):
                path.unlink()
            self.status(self.checker(root, repo), 1)

    def test_generator_is_deterministic_and_matches_committed_bytes(self):
        generator = self.tool("gen.py")
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / "first", Path(tmp) / "second"
            for destination in (first, second):
                self.status(self.command(sys.executable, generator, "--out", destination), 0)
            self.assertTrue(manifest(first), "generator must produce files")
            self.assertEqual(manifest(first), manifest(second))
            self.assertEqual(manifest(first), manifest(HERE))

    def test_handoff_and_repository_have_identical_hashes(self):
        self.tool("gen.py")
        handoff = Path(os.environ.get(
            "CUTGEOM_HANDOFF", REPO.parent / "handoff" / "fixtures" / HERE.name,
        ))
        self.assertTrue(handoff.is_dir(), "handoff fixture copy is missing")
        self.assertEqual(manifest(HERE), manifest(handoff))

    def test_exact_decimal_geometry_and_published_area_volume_oracles(self):
        # Literal analytic oracles from the brief, not the generator under test.
        golden = {
            "l_shape": ("0.00034519", "0.00000034519"),
            "v_shape": ("0.00036607", "0.00000036607"),
            "r_shape": ("0.00025671", "0.00000025671"),
        }
        for case, (area, volume) in golden.items():
            with self.subTest(case=case):
                data = self.read_json(HERE / case / "input.json")
                outline = data["outline"]
                self.assertTrue(all(isinstance(v, str) for p in outline for v in p))
                points = [tuple(Fraction(v) for v in p) for p in outline]
                twice_area = sum(
                    a[0] * b[1] - a[1] * b[0]
                    for a, b in zip(points, points[1:] + points[:1])
                )
                self.assertEqual(twice_area / 2, Fraction(area))
                self.assertEqual(twice_area / 2 * Fraction(".001"), Fraction(volume))
                expected = self.read_json(HERE / case / "expected.json")
                # The brief leaves expected.json's field names open. Require both
                # exact values as decimal strings without inventing schema keys.
                numeric = decimal_values(expected)
                self.assertIn(Fraction(area), numeric)
                self.assertIn(Fraction(volume), numeric)

    def test_embedded_area_excludes_mesh_box_edges(self):
        for case, exact in (("l_shape", ".000015"), ("r_shape", ".0000511"),
                            ("v_shape", None)):
            with self.subTest(case=case), localcontext() as context:
                context.prec = 60
                data = self.read_json(HERE / case / "input.json")
                points = [tuple(Decimal(v) for v in p) for p in data["outline"]]
                length = Decimal(0)
                for a, b in zip(points, points[1:] + points[:1]):
                    if any(a[d] == b[d] == edge for d in (0, 1)
                           for edge in (Decimal(0), Decimal(".02"))):
                        continue
                    length += ((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2).sqrt()
                area = length * Decimal(".001")
                want = Decimal(exact) if exact else (
                    2 * (Decimal(".0039") ** 2 + Decimal(".0087") ** 2).sqrt()
                    * Decimal(".001")
                )
                self.assertLessEqual(abs(area / want - 1), Decimal("1e-12"))
                values = decimal_values(self.read_json(HERE / case / "expected.json"))
                self.assertTrue(any(
                    abs(Decimal(v.numerator) / Decimal(v.denominator) / want - 1)
                    <= Decimal("1e-12") for v in values
                ), "expected.json must supply the embedded-area numerical oracle")

    def test_l1_uses_shifted_corner_and_exact_fraction_oracles(self):
        data = self.read_json(HERE / "l1_corner" / "input.json")
        self.assertIn(".012501", set(leaves(data["outline"])))
        expected = self.read_json(HERE / "l1_corner" / "expected.json")
        self.assertTrue(
            {Fraction(".00079984"), Fraction(".0004")} <= decimal_values(expected),
            "L1 expected data must distinguish the concave corner and strip fractions",
        )

    def test_error_fixture_records_required_paths_and_reasons(self):
        expected = self.read_json(HERE / "errors" / "expected.json")
        strings = {v for v in leaves(expected) if isinstance(v, str)}
        self.assertTrue({
            "cut/union", "invalid piece", "outline_phi size mismatch", "CUT_UNSUPPORTED",
        } <= strings, "error fixtures must record the brief's stable paths and messages")

    def test_pieces_are_strictly_convex_ccw_and_internal_matches_outline(self):
        def cross(a, b, c):
            return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

        def on_segment(p, a, b):
            return cross(a, b, p) == 0 and all(
                min(a[d], b[d]) <= p[d] <= max(a[d], b[d]) for d in (0, 1)
            )

        for case in CASES[:-1]:
            with self.subTest(case=case):
                data = self.read_json(HERE / case / "input.json")
                outline = [tuple(Fraction(v) for v in p) for p in data["outline"]]
                edges = list(zip(outline, outline[1:] + outline[:1]))
                self.assertEqual(len(data["pieces"]), len(data["internal"]))
                closure = [Fraction(0), Fraction(0)]
                for raw, flags in zip(data["pieces"], data["internal"]):
                    self.assertTrue(all(isinstance(v, str) for p in raw for v in p))
                    ring = [tuple(Fraction(v) for v in p) for p in raw]
                    self.assertGreaterEqual(len(ring), 3)
                    self.assertEqual(len(ring), len(flags))
                    for i, (a, b) in enumerate(zip(ring, ring[1:] + ring[:1])):
                        self.assertGreater(cross(a, b, ring[(i + 2) % len(ring)]), 0)
                        external = any(on_segment(a, x, y) and on_segment(b, x, y)
                                       for x, y in edges)
                        self.assertIs(type(flags[i]), bool)
                        self.assertEqual(flags[i], not external)
                        if external:
                            closure[0] += b[1] - a[1]
                            closure[1] += a[0] - b[0]
                self.assertEqual(closure, [0, 0], "external normal area vectors must close")

    def test_checker_rejects_wrong_case_key(self):
        def change(data):
            data["Outline"] = data.pop("outline")
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_unknown_expected_key(self):
        self.mutate("l_shape/expected.json", lambda data: data.update({"Unknown": True}))

    def test_checker_rejects_unknown_nested_option_key(self):
        def change(data):
            self.assertIsInstance(data["options"], dict)
            data["options"]["Unknown"] = True
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_internal_shape_mismatch(self):
        self.mutate("l_shape/input.json", lambda data: data["internal"][0].pop())

    def test_checker_rejects_empty_pieces_in_a_legal_case(self):
        def change(data):
            data["pieces"] = []
            data["internal"] = []
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_short_outline_in_a_legal_case(self):
        def change(data):
            data["outline"] = data["outline"][:2]
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_self_intersecting_outline_in_a_legal_case(self):
        def change(data):
            data["outline"] = [["0", "0"], [".02", ".02"],
                               ["0", ".02"], [".02", "0"]]
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_incorrect_internal_edge(self):
        def change(data):
            data["internal"][0][0] = not data["internal"][0][0]
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_overlapping_blocks(self):
        def change(data):
            data["pieces"].append(data["pieces"][0])
            data["internal"].append(data["internal"][0])
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_nonconvex_collinear_piece(self):
        def change(data):
            data["pieces"][0] = [["0", "0"], [".01", "0"], [".02", "0"]]
            data["internal"][0] = [False, False, False]
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_equal_area_piece_outside_outline(self):
        # Preserve all block areas and internal flags while moving one outside.
        # A checker that only sums areas must not accept this false partition.
        def change(data):
            data["pieces"][0] = [
                [str(Decimal(x) + 1), y] for x, y in data["pieces"][0]
            ]
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_tampered_expected_value(self):
        def change(data):
            def replace(value):
                if isinstance(value, dict):
                    for key, child in value.items():
                        if isinstance(child, str):
                            try:
                                Fraction(child)
                            except ValueError:
                                continue
                            value[key] = "123.456789"
                            return True
                        if replace(child):
                            return True
                elif isinstance(value, list):
                    return any(replace(child) for child in value)
                return False
            self.assertTrue(replace(data), "expected values must include exact decimal strings")
        self.mutate("l_shape/expected.json", change)

    def test_checker_cli_errors_are_distinct_from_failed_assertions(self):
        script = self.tool("check.py")
        self.status(self.command(sys.executable, script, "--unknown-option"), 2)
        with tempfile.TemporaryDirectory() as tmp:
            self.status(self.checker(HERE, Path(tmp) / "absent"), 2)

    def test_branch_source_matches_and_literal_drift_is_rejected(self):
        self.tool("check.py")
        # Read #202 without checking it out. Missing ref is an environment error,
        # not silently skipped; only absence of git permits the documented SKIP.
        self.assertIsNotNone(shutil.which("git"), "git required for source comparison")
        source = self.command("git", "-C", REPO, "show", f"{BRANCH}:{SOURCE}")
        self.status(source, 0)
        with tempfile.TemporaryDirectory() as tmp:
            repo, root = self.isolated(tmp)
            self.status(self.command("git", "init", repo), 0)
            path = repo / SOURCE
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source.stdout, encoding="utf-8")
            self.status(self.command("git", "-C", repo, "add", SOURCE), 0)
            self.status(self.command(
                "git", "-C", repo, "-c", "user.name=Fixture Acceptance",
                "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false",
                "commit", "-m", "Synthetic source-comparison sandbox",
            ), 0)
            self.status(self.checker(root, repo, "--branch-ref", "HEAD"), 0)
            for old, new in ((".0064", ".0065"), ("{{0, 0}, {.02, 0}, {.02, y}, {x, y}}",
                                                        "{{0, 0}, {.02, 0}, {x, y}, {.02, y}}")):
                with self.subTest(mutation=old):
                    self.assertIn(old, source.stdout)
                    path.write_text(source.stdout.replace(old, new, 1), encoding="utf-8")
                    self.status(self.command("git", "-C", repo, "add", SOURCE), 0)
                    self.status(self.command(
                        "git", "-C", repo, "-c", "user.name=Fixture Acceptance",
                        "-c", "user.email=fixture@example.invalid",
                        "-c", "commit.gpgsign=false", "commit", "-m", "Source drift probe",
                    ), 0)
                    self.status(self.checker(root, repo, "--branch-ref", "HEAD"), 1)
            self.status(self.checker(root, repo, "--branch-ref", "missing-ref"), 2)

    def test_branch_mode_without_git_explicitly_reports_skip(self):
        self.tool("check.py")
        with tempfile.TemporaryDirectory() as tmp:
            repo, root = self.isolated(tmp)
            env = dict(os.environ, PATH=tmp)
            result = self.checker(root, repo, "--branch-ref", BRANCH, env=env)
            self.status(result, 0)
            self.assertIn("SKIP", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
