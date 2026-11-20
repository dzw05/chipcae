#!/usr/bin/env python3
"""Black-box acceptance for the cutgeom-nc/1.0 fixture tools (standard library).

Run: python3 -m unittest discover -s tests/contract/fixtures/cutgeom-nc-1.0 -v
This test-first suite covers gen.py, check.py, their data, literal provenance,
and complete-directory reproduction. It neither implements nor substitutes for
those tools. Temporary copies isolate destructive probes; the source checkout
is always read-only.

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
SOURCE_TIP = "0d97b5974c6f81d3b2765c631f7b4b6d4962bedf"
BRANCH = os.environ.get("CUTGEOM_BRANCH_REF", SOURCE_TIP)
SOURCE = "tests/unit/region_ls/test_plane_cut_union.cpp"
# Independent, ordered transcription of #202 at 0d97b59. Indices name the
# outline vertices of each block; no generator/parser under test supplies them.
SOURCE_TABLES = {
    "l_shape": (
        (("0", "0"), (".02", "0"), (".02", ".0137"), (".0113", ".0137"),
         (".0113", ".02"), ("0", ".02")),
        ((0, 1, 2, 3), (0, 3, 4, 5)),
    ),
    "v_shape": (
        (("0", "0"), (".0064", "0"), (".0103", ".0087"), (".0142", "0"),
         (".02", "0"), (".02", ".02"), ("0", ".02")),
        ((0, 1, 2, 6), (2, 3, 4, 5), (2, 5, 6)),
    ),
    "r_shape": (
        (("0", "0"), (".0086", "0"), (".0086", ".0043"), (".0043", ".0043"),
         (".0043", ".0157"), (".0157", ".0157"), (".0157", ".0043"),
         (".0117", ".0043"), (".0117", "0"), (".02", "0"), (".02", ".02"),
         ("0", ".02")),
        ((0, 1, 2, 3), (0, 3, 4, 11), (11, 4, 5, 10), (10, 5, 6, 9),
         (8, 9, 6, 7)),
    ),
}


def manifest(root, include_harness=False):
    """Hash payloads, or the complete deliverable including this test harness."""
    return {
        str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(root.rglob("*"))
        if p.is_file() and "__pycache__" not in p.parts
        and (include_harness or p.name != Path(__file__).name)
    }


def snapshot(root):
    """Read-only checks also detect caches, helper files and newly made directories."""
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            if p.is_file() else None for p in sorted(root.rglob("*"))}


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


def records(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from records(child)
    elif isinstance(value, list):
        for child in value:
            yield from records(child)


def points(raw):
    return [tuple(Fraction(v) for v in point) for point in raw]


def cross(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def area(ring):
    return sum((a[0] * b[1] - a[1] * b[0]
                for a, b in zip(ring, ring[1:] + ring[:1])), Fraction(0)) / 2


def intersection(subject, convex):
    """Test-only exact half-plane clipping; never import geometry from gen.py."""
    out = list(subject)
    for a, b in zip(convex, convex[1:] + convex[:1]):
        incoming, out = out, []
        if not incoming:
            break
        for p, q in zip(incoming, incoming[1:] + incoming[:1]):
            sp, sq = cross(a, b, p), cross(a, b, q)
            if (sp < 0) != (sq < 0):
                t = sp / (sp - sq)
                out.append(tuple(p[d] + t * (q[d] - p[d]) for d in (0, 1)))
            if sq >= 0:
                out.append(q)
    return out


def source_shape(case):
    name = "l_shape" if case == "l1_corner" else case
    raw, indices = SOURCE_TABLES[name]
    ring = points(raw)
    if case == "l1_corner":
        ring = [tuple(Fraction(".012501") if v in (Fraction(".0113"), Fraction(".0137"))
                      else v for v in p) for p in ring]
    return ring, [[ring[i] for i in block] for block in indices]


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
            before = snapshot(root)
            self.status(self.checker(root, repo), 1)
            self.assertEqual(snapshot(root), before, "checker must not repair or write bad data")

    def test_checker_accepts_complete_fixture_without_writing(self):
        before = snapshot(HERE)
        self.status(self.checker(), 0)
        self.assertEqual(snapshot(HERE), before)
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
        before = snapshot(HERE)
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / "first", Path(tmp) / "second"
            for destination in (first, second):
                self.status(self.command(sys.executable, generator, "--out", destination), 0)
            self.assertTrue(manifest(first), "generator must produce files")
            generated = manifest(first, include_harness=True)
            self.assertEqual(generated, manifest(second, include_harness=True))
            self.assertEqual(
                set(generated), set(manifest(HERE, include_harness=True)),
                "full-directory reproduction must include test_acceptance.py",
            )
            self.assertEqual(generated, manifest(HERE, include_harness=True))
        self.assertEqual(snapshot(HERE), before, "--out must leave the source fixture unchanged")

    def test_manifest_enumerates_the_complete_deliverable_including_tests(self):
        expected = self.read_json(HERE / "expected.json")
        self.assertEqual(len(expected["files"]), len(set(expected["files"])),
                         "manifest paths must be unique")
        self.assertEqual(
            set(expected["files"]), set(manifest(HERE, include_harness=True)),
            "the generated manifest must cover every delivered file, including its tests",
        )

    def test_handoff_and_repository_have_identical_hashes(self):
        self.tool("gen.py")
        handoff = Path(os.environ.get(
            "CUTGEOM_HANDOFF", REPO.parent / "handoff" / "fixtures" / HERE.name,
        ))
        self.assertTrue(handoff.is_dir(), "handoff fixture copy is missing")
        self.assertEqual(manifest(HERE, include_harness=True),
                         manifest(handoff, include_harness=True))

    def test_source_tables_preserve_every_vertex_and_block_in_order(self):
        for case in (*SOURCE_TABLES, "l1_corner"):
            with self.subTest(case=case):
                data = self.read_json(HERE / case / "input.json")
                outline, blocks = source_shape(case)
                self.assertEqual(points(data["outline"]), outline)
                self.assertEqual([points(p) for p in data["pieces"]], blocks)
                # Numeric equality alone misses a loss of the source spelling.
                raw, indices = SOURCE_TABLES["l_shape" if case == "l1_corner" else case]
                literal_ring = [[".012501" if case == "l1_corner" and
                                 v in (".0113", ".0137") else v for v in p] for p in raw]
                self.assertEqual(data["outline"], literal_ring)
                self.assertEqual(data["pieces"],
                                 [[literal_ring[i] for i in block] for block in indices])

    def test_fixture_manifest_records_source_tip_and_portable_paths(self):
        expected = self.read_json(HERE / "expected.json")
        strings = [v for v in leaves(expected) if isinstance(v, str)]
        self.assertTrue(any(v in (SOURCE_TIP, SOURCE_TIP[:7]) for v in strings),
                        "metadata must identify the #202 fixture source tip")
        for relative in manifest(HERE):
            with self.subTest(path=relative):
                payload = (HERE / relative).read_bytes()
                text = payload.decode("utf-8")
                self.assertNotIn(b"\r", payload)
                self.assertTrue(payload.endswith(b"\n"), "UTF-8 text must end in LF")
                for prefix in ("/workspace/", "/home/box/", "/Users/"):
                    self.assertNotIn(prefix, text, "fixture output must be portable")

    def test_json_keys_use_lowercase_throughout_the_fixture(self):
        self.read_json(HERE / "expected.json")
        for relative in manifest(HERE):
            if relative.endswith(".json"):
                for record in records(self.read_json(HERE / relative)):
                    for key in record:
                        self.assertEqual(key, key.lower(), f"{relative}: uppercase key {key}")

    def test_blocks_partition_the_outline_without_overlap_or_outside_area(self):
        for case in CASES[:-1]:
            with self.subTest(case=case):
                data = self.read_json(HERE / case / "input.json")
                outline = points(data["outline"])
                blocks = [points(p) for p in data["pieces"]]
                self.assertTrue(blocks, "a legal shape needs at least one piece")
                # L/V/R/L1 have nonconvex outlines. The hand-transcribed disjoint
                # source blocks let us check containment, not merely area sums.
                reference = source_shape(case)[1] if case in (
                    *SOURCE_TABLES, "l1_corner",
                ) else [outline]
                self.assertEqual(sum(map(area, blocks), Fraction(0)), area(outline))
                for i, block in enumerate(blocks):
                    self.assertEqual(
                        sum((area(intersection(block, r)) for r in reference), Fraction(0)),
                        area(block), f"{case} block {i} extends outside the outline",
                    )
                    for other in blocks[:i]:
                        self.assertEqual(area(intersection(block, other)), 0,
                                         f"{case} has overlapping pieces")

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
                if exact:
                    self.assertIn(Fraction(exact), values,
                                  "L/R embedded area is exact, not just within float tolerance")
                self.assertTrue(any(
                    abs(Decimal(v.numerator) / Decimal(v.denominator) / want - 1)
                    <= Decimal("1e-12") for v in values
                ), "expected.json must supply the embedded-area numerical oracle")
                if case == "v_shape":
                    strings = leaves(self.read_json(HERE / case / "expected.json"))
                    approximations = []
                    for value in strings:
                        if not isinstance(value, str):
                            continue
                        try:
                            number = Decimal(value)
                        except ArithmeticError:
                            continue
                        if number.is_finite() and abs(number / want - 1) <= Decimal("1e-12"):
                            approximations.append(number)
                    self.assertTrue(any(len(v.as_tuple().digits) == 17 for v in approximations),
                                    "V embedded-area oracle must retain 17 significant digits")

    def test_named_geometry_oracles_keep_exact_decimals_and_piece_counts(self):
        golden = {
            "l_shape": ("0.00034519", "0.00000034519", 2),
            "v_shape": ("0.00036607", "0.00000036607", 3),
            "r_shape": ("0.00025671", "0.00000025671", 5),
            "chamfer_lid_octagon": ("0.000382", "0.000000382", 1),
        }
        for case, (area_value, volume, count) in golden.items():
            with self.subTest(case=case):
                expected = self.read_json(HERE / case / "expected.json")
                self.assertEqual(expected["area"], area_value)
                self.assertEqual(expected["volume"], volume)
                self.assertEqual(expected["piece_count"], count)
                self.assertEqual(expected["exterior_area_vector_xy"], ["0", "0"])
                for field in ("area_volume_rel", "embedded_area_rel", "closure_abs"):
                    self.assertEqual(Fraction(expected["tolerance"][field]), Fraction("1e-12"))

    def test_l1_uses_shifted_corner_and_exact_fraction_oracles(self):
        data = self.read_json(HERE / "l1_corner" / "input.json")
        self.assertIn(Fraction(".012501"), decimal_values(data["outline"]))
        expected = self.read_json(HERE / "l1_corner" / "expected.json")
        self.assertTrue(
            {Fraction(".00079984"), Fraction(".0004")} <= decimal_values(expected),
            "L1 expected data must distinguish the concave corner and strip fractions",
        )

    def test_l1_completed_union_selects_two_corners_eight_strips_and_no_duplicates(self):
        data = self.read_json(HERE / "l1_corner" / "input.json")
        self.assertEqual(data["grid"], {"h_xy": "0.0025", "nx": 8, "ny": 8, "nz": 2})
        blocks = [points(p) for p in data["pieces"]]
        h = Fraction(".0025")
        fractions = {}
        # #202 stack has two z cells; this extruded xy cut has the same fraction
        # in both. Membership is computed after summing every block in a cell.
        for j in range(8):
            for i in range(8):
                cell = [(i * h, j * h), ((i + 1) * h, j * h),
                        ((i + 1) * h, (j + 1) * h), (i * h, (j + 1) * h)]
                fraction = sum((area(intersection(b, cell)) for b in blocks), Fraction(0)) / h**2
                for k in range(2):
                    fractions[i, j, k] = fraction
        corners = [v for (i, j, _), v in fractions.items() if (i, j) == (5, 5)]
        strips = [v for (i, j, _), v in fractions.items()
                  if (i >= 6 and j == 5) or (i == 5 and j >= 6)]
        self.assertEqual(corners, [Fraction(".00079984")] * 2)
        self.assertEqual(strips, [Fraction(".0004")] * 8)
        # These data fields distinguish counts from the geometric tolerances.
        expected = self.read_json(HERE / "l1_corner" / "expected.json")
        self.assertEqual(expected["corner_count"], 2)
        self.assertEqual(expected["strip_count"], 8)
        recorded = {tuple(c["cell"]): Fraction(c["sub_volfrac"])
                    for c in expected["cut_cells"]}
        self.assertEqual(len(recorded), len(expected["cut_cells"]),
                         "cell ids must remain unique after union accumulation")
        self.assertEqual(recorded, {cell: v for cell, v in fractions.items() if 0 < v < 1})
        self.assertIs(expected["small_subs_unique"], True)
        self.assertEqual(Fraction(expected["tolerance"]["sub_volfrac_rel"]), Fraction("1e-10"))
        for threshold, count in (("0.001", 10), ("0.0005", 8)):
            selected = {cell for cell, v in fractions.items() if 0 < v < Fraction(threshold)}
            self.assertEqual(len(selected), count)
            self.assertEqual(expected["small_subs"][threshold], count)

    def test_convex_split2_records_same_outline_and_strict_comparison_tolerance(self):
        data = self.read_json(HERE / "convex_split2" / "input.json")
        ring = points(data["outline"])
        self.assertTrue(all(cross(ring[i - 1], ring[i], ring[(i + 1) % len(ring)]) > 0
                            for i in range(len(ring))))
        self.assertEqual(len(data["pieces"]), 2)
        # Single-block input is explicit, so a consumer can run both variants.
        self.assertEqual([points(p) for p in data["single_piece"]], [ring])
        expected = self.read_json(HERE / "convex_split2" / "expected.json")
        self.assertEqual(Fraction(expected["tolerance"]["sub_volfrac_rel"]), Fraction("1e-14"))
        self.assertEqual(Fraction(expected["tolerance"]["sq_w_rel"]), Fraction("1e-14"))

    def test_octagon_is_single_external_piece_with_exact_area_and_fast_path_assertions(self):
        data = self.read_json(HERE / "chamfer_lid_octagon" / "input.json")
        ring = points(data["outline"])
        self.assertEqual(len(ring), 8)
        self.assertEqual(area(ring), Fraction(".000382"))
        self.assertEqual([points(p) for p in data["pieces"]], [ring])
        self.assertEqual(data["internal"], [[False] * 8])
        expected = self.read_json(HERE / "chamfer_lid_octagon" / "expected.json")
        self.assertIn(Fraction(".000382"), decimal_values(expected))
        # Declarative expectations for the later C++ consuming card. A fixture
        # checker passing does not demonstrate that the C++ path satisfies them.
        self.assertIs(expected["legacy_all_fields_bitwise_equal"], True)
        self.assertEqual(expected["internal_side_minus1_faces"], 0)

    def test_error_fixture_records_required_paths_and_reasons(self):
        expected = self.read_json(HERE / "errors" / "expected.json")
        pairs = {(r["path"], r["reason"]) for r in records(expected)
                 if "path" in r and "reason" in r}
        self.assertTrue({
            ("cut/union", "invalid piece"),
            ("cut/union", "outline_phi size mismatch"),
            ("cut/plane", "nonfinite or zero normal"),
            ("cut/options", "invalid thresholds"),
            ("cut/mesh", "positive affine box required"),
            ("outline.unsupported", "outline: disconnected solid in one cell"),
            ("CUT_UNSUPPORTED", "body0: duplicate piece union"),
            ("CUT_UNSUPPORTED", "region0: degenerate polygon"),
            ("CUT_UNSUPPORTED", "region0: self-intersecting polygon"),
            ("CUT_UNSUPPORTED", "region0: multiple rings"),
        } <= pairs, "each error must keep its own exact path/reason pair")

    def test_runtime_assertions_are_declared_for_cpp_consumers(self):
        for case in CASES[:-1]:
            with self.subTest(case=case):
                expected = self.read_json(HERE / case / "expected.json")
                self.assertEqual(expected["internal_side_minus1_faces"], 0)
                assertions = expected["consumer_assertions_only"]
                self.assertIn("internal_side_minus1_faces", assertions)
                required = {
                    "l1_corner": {"small_subs"},
                    "convex_split2": {"sub_volfrac", "sq_w", "duplicate_points_equivalent"},
                    "chamfer_lid_octagon": {"legacy_all_fields_bitwise_equal"},
                }.get(case, set())
                self.assertTrue(required <= set(assertions))
        errors = self.read_json(HERE / "errors" / "expected.json")
        self.assertIs(errors["consumer_assertions_only"], True)
        self.assertIs(errors["mesh_all_fields_and_order_unchanged"], True)
        for record in errors["cases"]:
            layer = "i11" if record["path"] == "CUT_UNSUPPORTED" else "i10"
            self.assertEqual(record["layer"], layer)

    def test_error_inputs_cover_each_invalid_shape_and_keep_body_id_numeric(self):
        data = self.read_json(HERE / "errors" / "input.json")
        inputs = {r["case"]: r for r in records(data) if isinstance(r.get("case"), str)}
        expected = self.read_json(HERE / "errors" / "expected.json")
        outputs = {r["case"]: (r["path"], r["reason"]) for r in records(expected)
                   if all(k in r for k in ("case", "path", "reason"))}
        required = {
            "empty_pieces": ("cut/union", "invalid piece"),
            "internal_mismatch": ("cut/union", "invalid piece"),
            "nonzero_plane_body": ("cut/union", "invalid piece"),
            "outline_phi_mismatch": ("cut/union", "outline_phi size mismatch"),
            "short_ring": ("CUT_UNSUPPORTED", "region0: degenerate polygon"),
            "zero_area_ring": ("CUT_UNSUPPORTED", "region0: degenerate polygon"),
            "self_intersection": ("CUT_UNSUPPORTED", "region0: self-intersecting polygon"),
            "multiple_rings": ("CUT_UNSUPPORTED", "region0: multiple rings"),
            "duplicate_body": ("CUT_UNSUPPORTED", "body0: duplicate piece union"),
        }
        for name, pair in required.items():
            with self.subTest(case=name):
                self.assertIn(name, inputs)
                self.assertEqual(outputs.get(name), pair)
        self.assertEqual(inputs["empty_pieces"]["pieces"], [])
        mismatch = inputs["internal_mismatch"]
        self.assertTrue(len(mismatch["pieces"]) != len(mismatch["internal"]) or any(
            len(p) != len(flags) for p, flags in zip(mismatch["pieces"], mismatch["internal"])
        ))
        self.assertLess(len(set(points(inputs["short_ring"]["outline"]))), 3)
        self.assertEqual(area(points(inputs["zero_area_ring"]["outline"])), 0)
        # Nonzero algebraic area isolates self-intersection from degeneracy.
        ring = points(inputs["self_intersection"]["outline"])
        self.assertNotEqual(area(ring), 0)
        edges = list(zip(ring, ring[1:] + ring[:1]))
        self.assertTrue(any(
            cross(a, b, c) * cross(a, b, d) < 0 and cross(c, d, a) * cross(c, d, b) < 0
            for i, (a, b) in enumerate(edges) for c, d in edges[i + 1:]
        ), "self-intersection fixture must contain crossing edges")
        self.assertTrue(any(r.get("body", 0) != 0
                            for r in records(inputs["nonzero_plane_body"])))
        phi = inputs["outline_phi_mismatch"]
        self.assertNotEqual(len(phi["outline_phi"]), phi["node_count"])
        self.assertGreater(len(inputs["multiple_rings"]["rings"]), 1)
        unions = inputs["duplicate_body"]["unions"]
        bodies = [u["body"] for u in unions]
        self.assertLess(len(set(bodies)), len(bodies))
        for record in records(data):
            if "body" in record:
                self.assertIs(type(record["body"]), int,
                              "body0 is a display label, not an API BodyId")

    def test_repeated_closing_point_is_an_explicit_consumer_equivalence_case(self):
        data = self.read_json(HERE / "convex_split2" / "input.json")
        canonical = data["outline"]
        self.assertEqual(data["repeated_closing_outline"], canonical + canonical[:1])
        # Adjacent duplicates have the same equivalence rule as a closing point.
        self.assertEqual(data["consecutive_duplicate_outline"], canonical[:1] + canonical)
        expected = self.read_json(HERE / "convex_split2" / "expected.json")
        self.assertIs(expected["duplicate_points_equivalent"], True)

    def test_pieces_are_strictly_convex_ccw_and_internal_matches_outline(self):
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
                            closure[0] += (b[1] - a[1]) * Fraction(".001")
                            closure[1] += (a[0] - b[0]) * Fraction(".001")
                self.assertEqual(closure, [0, 0], "external normal area vectors must close")

    def test_checker_rejects_wrong_case_key(self):
        def change(data):
            data["Outline"] = data.pop("outline")
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_unknown_expected_key(self):
        self.mutate("l_shape/expected.json", lambda data: data.update({"Unknown": True}))

    def test_checker_rejects_lowercase_unknown_input_key(self):
        self.mutate("l_shape/input.json", lambda data: data.update({"unknown": True}))

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

    def test_checker_rejects_nonconsecutive_repeated_vertex(self):
        def change(data):
            data["outline"].insert(3, data["outline"][0])
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_self_intersecting_outline_in_a_legal_case(self):
        def change(data):
            data["outline"] = [["0", "0"], [".02", ".02"],
                               ["0", ".02"], [".015", "0"]]
            self.assertNotEqual(area(points(data["outline"])), 0)
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
                                number = Fraction(child)
                            except ValueError:
                                continue
                            if number == Fraction(".00034519"):
                                value[key] = "0.00034520"
                                return True
                        if replace(child):
                            return True
                elif isinstance(value, list):
                    return any(replace(child) for child in value)
                return False
            self.assertTrue(replace(data), "L area must be recorded as an exact decimal string")
        self.mutate("l_shape/expected.json", change)

    def test_checker_rejects_rational_oracle_drift_below_float_tolerance(self):
        for field, altered in (
            ("area", "0.000345190000000000000001"),
            ("volume", "0.000000345190000000000001"),
            ("embedded_area", "0.000015000000000000000001"),
        ):
            with self.subTest(field=field):
                def change(data):
                    before, after = Fraction(data[field]), Fraction(altered)
                    self.assertNotEqual(before, after)
                    self.assertLess(abs(after / before - 1), Fraction("1e-12"))
                    data[field] = altered
                self.mutate("l_shape/expected.json", change)

    def test_checker_rejects_reversed_ccw_piece(self):
        def change(data):
            data["pieces"][0].reverse()
            data["internal"][0].reverse()
        self.mutate("l_shape/input.json", change)

    def test_checker_rejects_missing_required_case_without_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo, root = self.isolated(tmp)
            self.status(self.checker(root, repo), 0)
            shutil.rmtree(root / "l1_corner")
            before = snapshot(root)
            self.status(self.checker(root, repo), 1)
            self.assertEqual(snapshot(root), before)

    def test_checker_cli_errors_are_distinct_from_failed_assertions(self):
        script = self.tool("check.py")
        self.status(self.command(sys.executable, script, "--unknown-option"), 2)
        with tempfile.TemporaryDirectory() as tmp:
            self.status(self.checker(HERE, Path(tmp) / "absent"), 2)

    def test_generator_cli_and_io_errors_exit_two(self):
        generator = self.tool("gen.py")
        self.status(self.command(sys.executable, generator, "--unknown-option"), 2)
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp) / "file"
            destination.write_text("occupied\n", encoding="utf-8")
            self.status(self.command(sys.executable, generator, "--out", destination), 2)
            self.assertEqual(destination.read_text(encoding="utf-8"), "occupied\n")

    def test_branch_source_matches_and_literal_drift_is_rejected(self):
        self.tool("check.py")
        # Only this auxiliary drift probe needs #202 locally. Default acceptance
        # and the explicit missing-ref / missing-git tests remain independent.
        if shutil.which("git") is None:
            self.skipTest("SKIP: git is unavailable for the auxiliary drift probe")
        source = self.command("git", "-C", REPO, "show", f"{BRANCH}:{SOURCE}")
        if source.returncode:
            self.skipTest(f"SKIP: optional #202 source ref {BRANCH} is unavailable")
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
            for old, new in (
                (".0064", ".006400000000000000000001"),
                ("Real x = .0113", "Real x = .0114"),
                ("l_shape(.012501, .012501)", "l_shape(.012502, .012501)"),
                ("{{0, 0}, {.02, 0}, {.02, y}, {x, y}}",
                 "{{0, 0}, {.02, 0}, {x, y}, {.02, y}}"),
                ("{{0, 0}, {.02, 0}, {.02, y}, {x, y}}, {{0, 0}, {x, y}, {x, .02}, {0, .02}}",
                 "{{0, 0}, {x, y}, {x, .02}, {0, .02}}, {{0, 0}, {.02, 0}, {.02, y}, {x, y}}"),
            ):
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
            # The brief requires literal provenance as well as numeric equality,
            # including parameter defaults and both L1 arguments.
            for old, new in (
                (".0064", "0.0064"),
                (".0137", "1.37e-2"),
                ("Real x = .0113", "Real x = 0.0113"),
                ("l_shape(.012501, .012501)", "l_shape(0.012501, .012501)"),
                ("l_shape(.012501, .012501)", "l_shape(.012501, 0.012501)"),
            ):
                with self.subTest(literal_drift=(old, new)):
                    self.assertIn(old, source.stdout)
                    path.write_text(source.stdout.replace(old, new, 1), encoding="utf-8")
                    self.status(self.command("git", "-C", repo, "add", SOURCE), 0)
                    self.status(self.command(
                        "git", "-C", repo, "-c", "user.name=Fixture Acceptance",
                        "-c", "user.email=fixture@example.invalid",
                        "-c", "commit.gpgsign=false", "commit", "-m", "Source literal drift probe",
                    ), 0)
                    self.status(self.checker(root, repo, "--branch-ref", "HEAD"), 1)
            path.unlink()
            self.status(self.command("git", "-C", repo, "add", "-u"), 0)
            self.status(self.command(
                "git", "-C", repo, "-c", "user.name=Fixture Acceptance",
                "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false",
                "commit", "-m", "Existing ref without source tables",
            ), 0)
            self.status(self.checker(root, repo, "--branch-ref", "HEAD"), 1)

    def test_branch_mode_missing_ref_explicitly_reports_skip_and_succeeds(self):
        self.tool("check.py")
        with tempfile.TemporaryDirectory() as tmp:
            repo, root = self.isolated(tmp)
            self.status(self.checker(root, repo), 0)
            if shutil.which("git"):
                self.status(self.command("git", "init", repo), 0)
            result = self.checker(root, repo, "--branch-ref", "refs/heads/absent-cutgeom-fixture-ref")
            self.status(result, 0)
            self.assertIn("SKIP", result.stdout + result.stderr)

    def test_default_checker_does_not_require_git_or_a_source_ref(self):
        self.tool("check.py")
        with tempfile.TemporaryDirectory() as tmp:
            repo, root = self.isolated(tmp)
            before = snapshot(root)
            result = self.checker(root, repo, env=dict(os.environ, PATH=tmp))
            self.status(result, 0)
            self.assertNotIn("SKIP", result.stdout + result.stderr)
            self.assertEqual(snapshot(root), before)

    def test_branch_mode_without_git_explicitly_reports_skip(self):
        self.tool("check.py")
        with tempfile.TemporaryDirectory() as tmp:
            repo, root = self.isolated(tmp)
            env = dict(os.environ, PATH=tmp)
            result = self.checker(root, repo, "--branch-ref", BRANCH, env=env)
            self.status(result, 0)
            self.assertIn("SKIP", result.stdout + result.stderr)

    def test_optional_source_skip_cannot_hide_invalid_default_data(self):
        for missing in ("git", "ref"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as tmp:
                repo, root = self.isolated(tmp)
                self.status(self.checker(root, repo), 0)
                if missing == "ref" and shutil.which("git"):
                    self.status(self.command("git", "init", repo), 0)
                path = root / "l_shape" / "expected.json"
                data = json.loads(path.read_text(encoding="utf-8"))
                data["area"] = "0.00034520"
                path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
                before = snapshot(root)
                env = dict(os.environ, PATH=tmp) if missing == "git" else None
                result = self.checker(root, repo, "--branch-ref",
                                      "refs/heads/absent-cutgeom-fixture-ref", env=env)
                self.status(result, 1)
                self.assertNotIn("SKIP", result.stdout + result.stderr)
                self.assertEqual(snapshot(root), before)


if __name__ == "__main__":
    unittest.main()
