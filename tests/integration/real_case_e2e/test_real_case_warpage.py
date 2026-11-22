"""Synthetic fixed-face, zero-dT and independent FEM warpage acceptance."""
import contextlib
import io
import math
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

from test_real_case_thermal import (
    driver, read_json, run_synthetic, solve_runs, source_root, write_json,
)

FVM_METHODS = ("FVM top-face centroids; least-squares plane removal", "fvm_cell_centers_lsq")
KNOWN_RED = "已知红：板级翘曲精度缺口，转 solve-elastic-accuracy"
W5_FEM_RTOL = 0.05


def top_direction(out, manifest):
    """Independent oracle: local Points targets, top nodes, closest block wins."""
    import h5py
    import numpy as np
    selected = [None] * 5
    with h5py.File(out / manifest["steps"][0]["file"]) as output:
        for name in ("hex", "cut"):
            block = output["VTKHDF/" + name]
            points = block["Points"][...]
            if points.size:
                u = block["PointData/U"][...]
                top = points[:, 2] == points[:, 2].max()
                xmin, ymin = points[:, :2].min(axis=0)
                xmax, ymax = points[:, :2].max(axis=0)
                targets = [((xmin + xmax) / 2, (ymin + ymax) / 2),
                           (xmin, ymin), (xmin, ymax), (xmax, ymin), (xmax, ymax)]
                for role, (x, y) in enumerate(targets):
                    distance, node = min(((points[i, 0] - x) ** 2 + (points[i, 1] - y) ** 2, i)
                                         for i in np.flatnonzero(top))
                    # Strict improvement preserves hex/cut order on equal distances.
                    if selected[role] is None or distance < selected[role][0]:
                        selected[role] = (distance, float(u[node, 2]))
    assert all(item is not None for item in selected), "W5 requires top-face nodes"
    delta = selected[0][1] - sum(item[1] for item in selected[1:]) / 4
    assert math.isfinite(delta), f"W5 nonfinite direction delta={delta}"
    return delta, int(np.sign(delta))


def inspect_w5(record, primary, reference):
    fvm = read_json(primary / "manifest.json")
    fem = read_json(reference / "manifest.json")
    qoi = next(q for q in fvm["summary"] if q["name"] == "warpage_pv")
    fvm_pv = qoi["value"]
    fem_pv = next(q["value"] for q in fem["summary"] if q["name"] == "warpage_pv")
    for side, pv in (("fvm", fvm_pv), ("fem", fem_pv)):
        assert math.isfinite(pv) and pv > 0, f"W5 {side}_pv={pv!r} must be finite and positive"
    fvm_delta, fvm_sign = top_direction(primary, fvm)
    fem_delta, fem_sign = top_direction(reference, fem)
    assert fvm_sign and fem_sign, f"W5 方向不可判: {fvm_delta=}, {fem_delta=}"
    assert fvm_sign == fem_sign, f"W5 opposite directions: {fvm_delta=}, {fem_delta=}"
    relative = abs(fvm_pv - fem_pv) / fem_pv
    diagnostic = record["diagnostic_fem_comparison"]
    for key, value in (("fvm_warpage_pv", fvm_pv), ("fem_warpage_pv", fem_pv),
                       ("relative_difference", relative), ("fvm_direction_delta", fvm_delta),
                       ("fem_direction_delta", fem_delta)):
        assert math.isclose(diagnostic[key], value, rel_tol=1e-12, abs_tol=1e-15), (key, diagnostic)
    assert diagnostic["method"] == qoi.get("method")
    assert diagnostic["fvm_direction_sign"] == fvm_sign
    assert diagnostic["fem_direction_sign"] == fem_sign
    if qoi.get("method") in FVM_METHODS:
        assert record["checks"]["W5"] == "PROVISIONAL", record["checks"]
        assert diagnostic["status"] == "PROVISIONAL", diagnostic
        assert diagnostic["nx"] == fvm["mesh"]["nx0"]
        assert diagnostic["ny"] == fvm["mesh"]["ny0"]
        assert diagnostic["nz"] == fvm["mesh"]["nz"]
        assert diagnostic["mesh_source"] == "primary manifest mesh.nx0/ny0"
        notes = "\n".join(record["notes"])
        for text in ("W5 PROVISIONAL", "fvm_pv", "fem_pv", "relative_difference", "nx", "ny",
                     qoi["method"], KNOWN_RED, "nx≥64", "每层≥2", "Richardson", "0.05"):
            assert text in notes, (text, notes)
    else:
        assert record["checks"]["W5"] is True, record["checks"]
        assert relative <= W5_FEM_RTOL, (fvm_pv, fem_pv, relative)


def inspect_warpage(record, folder):
    import h5py
    import numpy as np
    runs = list(solve_runs(record))
    fixed = [(case, out, manifest) for case, out, manifest in runs
             if case.get("constraints") == {"fixed": [{"target": "face:zmin"}]}]
    uniform = [(case, out, manifest) for case, out, manifest in runs
               if len(case.get("loads", [])) == 2
               and {load.get("target") for load in case["loads"]} == {"face:zmin", "face:zmax"}
               and all(load.get("type") == "fixed_temperature"
                       and load.get("T") == case["physics"][1]["T_ref"] for load in case["loads"])]
    fem = [(case, out, manifest) for case, out, manifest in runs
           if all(physics["solver"].startswith("fem.") for physics in case["physics"])]
    assert fixed, "W3 requires a separate fixed-bottom solve"
    assert uniform, "W4 requires a separate zero-dT solve"
    assert fem, "W5 requires the synthetic noncut FEM comparison"
    for _, out, manifest in fixed:
        with h5py.File(out / manifest["steps"][0]["file"]) as output:
            blocks = [output["VTKHDF/" + name] for name in ("hex", "cut")]
            zmin = min(block["Points"][..., 2].min() for block in blocks if block["Points"].size)
            for block in blocks:
                points, u = block["Points"][...], block["PointData/U"][...]
                if points.size:
                    assert np.all(u[points[:, 2] == zmin] == 0), "W3 tolerance is zero"
    for _, out, manifest in uniform:
        qoi = {q["name"]: q["value"] for q in manifest["summary"]}
        assert qoi["warpage_pv"] == 0, qoi
        with h5py.File(out / manifest["steps"][0]["file"]) as output:
            for block in ("hex", "cut"):
                assert np.all(output["VTKHDF/" + block + "/PointData/U"][...] == 0), "W4 tolerance is zero"
    with h5py.File(fem[0][1] / fem[0][2]["steps"][0]["file"]) as output:
        assert np.sum(output["VTKHDF/cut/NumberOfCells"][...]) == 0, "W5 reference must be noncut"
    inspect_w5(record, folder / "np1_r1", fem[0][1])
    assert "warpage_pv_vertex_recompute" in record["displacement"]



def write_w5_run(out, pv, method, delta=1., blocks=None):
    """Only W5's manifest and VTKHDF fields; no solver or recomputed PV."""
    import h5py
    import numpy as np
    out.mkdir(parents=True, exist_ok=True)
    summary = {"name": "warpage_pv", "value": pv, "unit": "m"}
    if method is not None:
        summary["method"] = method
    write_json(out / "manifest.json", {
        "schema_version": "chipcae.results/1.0", "summary": [summary],
        "steps": [{"n": 0, "file": "step.vtkhdf"}],
        "mesh": {"nx0": 4, "ny0": 6, "nz": 12},
    })
    if blocks is None:
        # Deliberately scramble storage order; the bottom node is a direction decoy.
        points = [[1, 1, 1], [0, 2, 1], [2, 0, 1], [0, 0, 1], [2, 2, 1], [1, 1, 0]]
        blocks = {"hex": (points, [delta, 0, 0, 0, 0, -100 * delta])}
    with h5py.File(out / "step.vtkhdf", "w") as output:
        for name in ("hex", "cut"):
            points, uz = blocks.get(name, ([], []))
            points = np.asarray(points, dtype=float).reshape((-1, 3))
            u = np.zeros_like(points)
            u[:, 2] = uz
            block = output.create_group("VTKHDF/" + name)
            block.create_dataset("Points", data=points)
            block.create_dataset("PointData/U", data=u)
            block.create_dataset("NumberOfCells", data=[1 if len(points) else 0])


class W5Acceptance(unittest.TestCase):
    """Exercise the existing W5 consumer; replace solving, never W5 evaluation."""

    def compare(self, *, method=FVM_METHODS[0], fvm_pv=2., fem_pv=11.,
                fvm_delta=1., fem_delta=2., blocks=None, reference_method="FEM nodes",
                missing_dimensions=()):
        with tempfile.TemporaryDirectory(prefix="chipcae-w5-contract-") as tmp:
            root = Path(tmp)
            inputs = root / "inputs"
            inputs.mkdir()
            primary = root / "np1_r1"
            write_w5_run(primary, fvm_pv, method, fvm_delta, blocks)
            manifest = read_json(primary / "manifest.json")
            for key in missing_dimensions:
                del manifest["mesh"][key]
            write_json(primary / "manifest.json", manifest)
            case = {"mesh": {"nx": 99, "ny": 98, "min_elems_per_layer": 2},
                    "physics": [{"solver": "fvm.conduction"},
                                {"solver": "fvm.elasticity", "T_ref": 298.15}],
                    "solver": {"linear": {"preconditioner": "jacobi"}},
                    "constraints": {}, "loads": []}
            record = {"kind": "warpage", "data_class": "synthetic", "notes": [],
                      "checks": dict.fromkeys(("W1", "W2", "W3", "W4", "W5")),
                      "mesh": {"n_cells_cut": 0, "nx0": 97, "ny0": 96, "nz": 12}}

            def solved(record, folder, inputs, output, stage, *args):
                write_w5_run(output, fem_pv, reference_method, fem_delta)
                return {"exit_code": 0}, {}

            # W3/W4 are covered by the independent E2E below; isolate W5 here.
            with patch.object(driver, "_solve_run", side_effect=solved), \
                    patch.object(driver, "_check_w3", return_value=True), \
                    patch.object(driver, "_check_w4", return_value=True), \
                    contextlib.redirect_stdout(io.StringIO()) as printed:
                driver._auxiliary(record, root, inputs, case, {}, None, 30, source_root(), primary)
            delta = record.get("diagnostic_fem_comparison", {}).get("fvm_direction_delta")
            if delta is not None and math.isfinite(delta):
                oracle_delta, _ = top_direction(primary, read_json(primary / "manifest.json"))
                self.assertTrue(math.isclose(delta, oracle_delta, rel_tol=1e-12, abs_tol=1e-15))
            return record, printed.getvalue()

    def test_fvm_methods_keep_large_pv_gap_provisional(self):
        for method in FVM_METHODS:
            with self.subTest(method=method):
                record, _ = self.compare(method=method)
                self.assertEqual(record["checks"]["W5"], "PROVISIONAL")
                self.assertAlmostEqual(record["diagnostic_fem_comparison"]["relative_difference"], 9 / 11)

    def test_only_primary_exact_fvm_method_can_be_provisional(self):
        for method in ("FEM nodes", None, "", "FVM cell centers", "fvm_cell_centers_lsq "):
            with self.subTest(method=method):
                record, _ = self.compare(method=method, reference_method=FVM_METHODS[0])
                self.assertIs(record["checks"]["W5"], False)

    def test_fem_five_percent_boundary_is_inclusive(self):
        # 1 and 20 are exact binary numbers; 1 / 20 rounds identically to .05.
        for pv, expected in ((21., True), (21.0000001, False)):
            for method in ("FEM nodes", None, ""):
                with self.subTest(pv=pv, method=method):
                    record, _ = self.compare(method=method, fvm_pv=pv, fem_pv=20.)
                    self.assertIs(record["checks"]["W5"], expected)

    def test_each_side_requires_finite_strictly_positive_manifest_pv(self):
        for method in (*FVM_METHODS, "FEM nodes"):
            for side in ("fvm", "fem"):
                for value in (0., -1., math.nan, math.inf, -math.inf):
                    with self.subTest(method=method, side=side, pv=value):
                        record, _ = self.compare(method=method, **{side + "_pv": value})
                        self.assertIs(record["checks"]["W5"], False)
                        diagnostic = record["diagnostic_fem_comparison"]
                        actual = diagnostic[side + "_warpage_pv"]
                        if math.isnan(value):
                            self.assertTrue(math.isnan(actual))
                        else:
                            self.assertEqual(actual, value)
                        self.assertIn(str(value), " ".join(record["notes"]))

    def test_both_nonzero_direction_signs_are_accepted(self):
        for method, expected in ((FVM_METHODS[0], "PROVISIONAL"), ("FEM nodes", True)):
            for sign in (1, -1):
                with self.subTest(method=method, sign=sign):
                    record, _ = self.compare(method=method, fvm_pv=20., fem_pv=20.,
                                             fvm_delta=sign, fem_delta=2 * sign)
                    self.assertEqual(record["checks"]["W5"], expected)

    def test_opposite_zero_and_nonfinite_directions_fail_even_with_matching_pv(self):
        for method in (*FVM_METHODS, "FEM nodes"):
            for a, b in ((1., -2.), (-1., 2.), (0., 2.), (1., 0.), (0., 0.),
                         (math.nan, 2.), (1., math.inf)):
                with self.subTest(method=method, fvm_delta=a, fem_delta=b):
                    record, _ = self.compare(method=method, fvm_pv=20., fem_pv=20.,
                                             fvm_delta=a, fem_delta=b)
                    self.assertIs(record["checks"]["W5"], False)
                    diagnostic = record["diagnostic_fem_comparison"]
                    self.assertIn("fvm_direction_delta", diagnostic)
                    self.assertIn("fem_direction_delta", diagnostic)
                    for key, expected in (("fvm_direction_delta", a), ("fem_direction_delta", b)):
                        if math.isfinite(expected):
                            self.assertAlmostEqual(diagnostic[key], expected)
                    if a == 0 or b == 0:
                        self.assertIn("方向不可判", " ".join(record["notes"]))

    def test_top_nodes_use_xy_coordinates_and_stable_hex_then_cut_ties(self):
        # Both blocks are nonempty and have their own top z. The two center
        # candidates in each block tie at distance .5 from its own center.
        # First hex node wins both the node-order and hex/cut ties.
        corners = [[0, 0, 1], [0, 2, 1], [2, 0, 1], [2, 2, 1]]
        cut_corners = [[10, 10, 2], [10, 12, 2], [12, 10, 2], [12, 12, 2]]
        for order, expected in (([1., -1.], True), ([-1., 1.], False)):
            with self.subTest(order=order):
                blocks = {"hex": ([[.5, 1, 1], [1.5, 1, 1]] + corners, order + [0.] * 4),
                          "cut": ([[10.5, 11, 2], [11.5, 11, 2]] + cut_corners,
                                  [-100., 100.] + [0.] * 4)}
                record, _ = self.compare(method="FEM nodes", fvm_pv=20., fem_pv=20., blocks=blocks)
                self.assertIs(record["checks"]["W5"], expected)
                self.assertAlmostEqual(record["diagnostic_fem_comparison"]["fvm_direction_delta"], order[0])

    def test_block_local_targets_reverse_the_global_bounds_direction(self):
        # Local centers/corners tie exactly between blocks, so hex supplies
        # 1 - mean(0, 0, 0, -4) = +2. Global bounds instead put the center at
        # (6, 6), select hex (2, 2), and produce -4 - mean(0, 0, 0, 0) = -4.
        blocks = {"hex": ([[1, 1, 1], [0, 0, 1], [0, 2, 1], [2, 0, 1], [2, 2, 1]],
                          [1., 0., 0., 0., -4.]),
                  "cut": ([[11, 11, 2], [10, 10, 2], [10, 12, 2], [12, 10, 2], [12, 12, 2]],
                          [-100., 0., 0., 0., 0.])}
        for method, expected in ((FVM_METHODS[0], "PROVISIONAL"),
                                 (FVM_METHODS[1], "PROVISIONAL"), ("FEM nodes", True)):
            with self.subTest(method=method):
                record, _ = self.compare(method=method, fvm_pv=20., fem_pv=20., blocks=blocks)
                self.assertEqual(record["checks"]["W5"], expected)
                diagnostic = record["diagnostic_fem_comparison"]
                self.assertAlmostEqual(diagnostic["fvm_direction_delta"], 2.)
                self.assertEqual(diagnostic["fvm_direction_sign"], 1)

    def test_cut_candidate_wins_when_closer_to_its_block_target(self):
        # Hex has only off-center top nodes. Cut has an exact center; its
        # lower-z corners define its Points bounds but cannot supply Uz.
        # Hex's exact corners win over cut's distant top node for each corner.
        blocks = {"hex": ([[.5, 1, 1], [1.5, 1, 1], [0, 0, 1], [0, 2, 1], [2, 0, 1], [2, 2, 1]],
                          [-1., -1., 0., 0., 0., 0.]),
                  "cut": ([[11, 11, 2], [10, 10, 0], [10, 12, 0], [12, 10, 0], [12, 12, 0]],
                          [2., -100., -100., -100., -100.])}
        record, _ = self.compare(method="FEM nodes", fvm_pv=20., fem_pv=20., blocks=blocks)
        self.assertIs(record["checks"]["W5"], True)
        self.assertAlmostEqual(record["diagnostic_fem_comparison"]["fvm_direction_delta"], 2.)

    def test_provisional_diagnostics_notes_and_stdout_preserve_manifest_values(self):
        record, printed = self.compare()
        self.assertEqual(record["checks"]["W5"], "PROVISIONAL")
        diagnostic = record["diagnostic_fem_comparison"]
        self.assertEqual(diagnostic["status"], "PROVISIONAL")
        self.assertEqual(diagnostic["method"], FVM_METHODS[0])
        for key, expected in (("nx", 4), ("ny", 6), ("nz", 12), ("min_elems_per_layer", 2),
                              ("fvm_direction_sign", 1), ("fem_direction_sign", 1)):
            self.assertEqual(diagnostic[key], expected)
        self.assertAlmostEqual(diagnostic["fvm_warpage_pv"], 2.)
        self.assertAlmostEqual(diagnostic["fem_warpage_pv"], 11.)
        self.assertAlmostEqual(diagnostic["fvm_direction_delta"], 1.)
        self.assertAlmostEqual(diagnostic["fem_direction_delta"], 2.)
        self.assertEqual(diagnostic["mesh_source"], "primary manifest mesh.nx0/ny0")
        for text in ("W5 PROVISIONAL", "fvm_pv", "fem_pv", "relative_difference", "nx", "ny",
                     FVM_METHODS[0], KNOWN_RED, "nx≥64", "每层≥2", "Richardson", "0.05"):
            self.assertIn(text, " ".join(record["notes"]))
        for text in ("W5 PROVISIONAL", "fvm_pv", "fem_pv", "relative_difference", "nx", "ny",
                     FVM_METHODS[0]):
            self.assertIn(text, printed)
        for output in (" ".join(record["notes"]), printed):
            for key, expected in (("fvm_pv", 2.), ("fem_pv", 11.),
                                  ("relative_difference", 9 / 11), ("nx", 4), ("ny", 6)):
                value = re.search(rf"\b{key}\s*=\s*([-+0-9.eE]+)", output)
                self.assertIsNotNone(value, (key, output))
                self.assertTrue(math.isclose(float(value[1]), expected, rel_tol=1e-3, abs_tol=1e-12))


    def test_missing_manifest_dimensions_never_report_case_requested_dimensions(self):
        for missing in (("nx0",), ("ny0",), ("nx0", "ny0")):
            with self.subTest(missing=missing):
                record, printed = self.compare(missing_dimensions=missing)
                self.assertEqual(record["checks"]["W5"], "PROVISIONAL")
                diagnostic = record["diagnostic_fem_comparison"]
                for key, value in (("nx", None if "nx0" in missing else 4),
                                   ("ny", None if "ny0" in missing else 6)):
                    self.assertEqual(diagnostic[key], value)
                    for output in (" ".join(record["notes"]), printed):
                        self.assertIn(f"{key}={value}", output)
                self.assertEqual(diagnostic["nz"], 12)
                self.assertIn("primary manifest", diagnostic["mesh_source"])
                for output in (diagnostic["mesh_source"], " ".join(record["notes"]), printed):
                    self.assertIn("actual FVM dimensions unavailable", output)


if __name__ == "__main__":
    raise SystemExit(run_synthetic("warpage", source_root(), inspect=inspect_warpage))
