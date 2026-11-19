"""V-INT-12 contracts; generated inputs need no compiled backend."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

import h5py
import numpy as np

SOURCE = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(SOURCE))
from tools.vv import real_case_e2e as driver
from tools.vv.check_result_data_class import check_file
from tools.vv.result_summary import summarize
from tools.vv.result_summary_result import result_json_from_summary

THERMAL_ONLY = "not solved: fvm thermal-only stage"
WARPAGE_METHOD = "FVM top-face centroids; least-squares plane removal"
QOIS = ("warpage_pv", "T_max", "U_mag_max")
DEFAULT_LOADS = [
    {"type": "fixed_temperature", "target": "face:zmin", "T": 298.15},
    {"type": "convection", "target": "face:zmax", "h": 1000, "T_inf": 348.15},
]
ENERGY = {"heat_in": 1., "heat_out": 1., "imbalance": 1e-9, "iterations": 7}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def string(value):
    raw = value.encode("utf-8")
    return struct.pack("<I", len(raw)) + raw


def frame(kind, payload, seq=1, wall=100):
    body = struct.pack("<QqB", seq, wall, kind) + payload
    return b"CAELOG1\0" + struct.pack("<II", len(body), zlib.crc32(body)) + body


def residual_frame(points, seq=1):
    payload = struct.pack("<QdI", 0, 0., len(points))
    for solver, field, iterations, residual in points:
        payload += string(solver) + string(field)
        payload += struct.pack("<IdIII", iterations, residual, 0, iterations, 1)
    return frame(18, payload + struct.pack("<I", 0), seq)


def log_frame(message, level=1, seq=2):
    return frame(20, struct.pack("<ii", level, 0) + string(message)
                 + string("") + struct.pack("<I", 0), seq)


def event_bytes(solid=True, energy=None):
    points = [("fvm.conduction", "T", 7, 1e-13)]
    if solid:
        points.append(("fvm.solid", "U", 9, 2e-13))
    return (residual_frame(points)
            + log_frame("FVM_ENERGY " + json.dumps(ENERGY if energy is None else energy)))


def decoded_fixture(solid=True, energy=None):
    points = [{"solver": "fvm.conduction", "field": "T", "iterations": 7, "residual": 1e-13}]
    if solid:
        points.append({"solver": "fvm.solid", "field": "U", "iterations": 9, "residual": 2e-13})
    return points + [{"level": 1, "rank": 0,
                      "message": "FVM_ENERGY " + json.dumps(ENERGY if energy is None else energy)}]


def make_run(root, *, thermal_only=False, endian="<", compressed=False):
    """Two hex control volumes, an empty cut block and valid exporter metadata."""
    root.mkdir(parents=True, exist_ok=True)
    filename = root / "out" / "step-0.vtkhdf"
    filename.parent.mkdir(exist_ok=True)
    cube = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0],
                     [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]], dtype=float)
    points = np.concatenate((cube, cube + [1, 0, 0]))
    with h5py.File(filename, "w") as output:
        vtk = output.create_group("VTKHDF")
        vtk.attrs["Version"] = [2, 8]
        vtk.attrs["Type"] = np.bytes_("PartitionedDataSetCollection")
        for index, name in enumerate(("hex", "cut")):
            block = vtk.create_group(name)
            block.attrs.update(Version=[2, 8], Type=np.bytes_("UnstructuredGrid"), Index=index)
            n, c = (16, 2) if name == "hex" else (0, 0)
            arrays = {
                "NumberOfPoints": np.array([n], dtype="i8"),
                "NumberOfCells": np.array([c], dtype="i8"),
                "NumberOfConnectivityIds": np.array([8 * c], dtype="i8"),
                "Points": points[:n], "Connectivity": np.arange(8 * c, dtype="i8"),
                "Offsets": np.arange(c + 1, dtype="i8") * 8,
                "Types": np.full(c, 12 if name == "hex" else 42, dtype="u1"),
                "PointData/T": np.full(n, 320., dtype="f8"),
                "PointData/U": np.zeros((n, 3), dtype="f8"),
                "CellData/T_cell": np.full(c, 320., dtype="f8"),
                "CellData/layer_id": np.ones(c, dtype="i4"),
                "CellData/material_id": np.arange(1, c + 1, dtype="i4"),
                "CellData/cell_kind": np.zeros(c, dtype="i4"),
                "CellData/elem_gid": np.arange(c, dtype="i8"),
                "CellData/volume": np.ones(c, dtype="f8"),
            }
            for path, value in arrays.items():
                value = value.astype(value.dtype.newbyteorder(endian))
                options = {"compression": "gzip", "chunks": True} if compressed and value.size else {}
                block.create_dataset(path, data=value, **options)
            for name_, values in (("material_names", ["copper", "fr4_eq"]),
                                  ("layer_names", ["board"])):
                block.create_dataset("FieldData/" + name_, data=np.array(values, dtype="S16"))
        for index, name in ((2, "eb_surface"), (3, "particles")):
            block = vtk.create_group(name)
            block.attrs.update(Version=[2, 8], Type=np.bytes_("PolyData"), Index=index)
            block.create_dataset("NumberOfPoints", data=np.array([0], dtype="i8"))
            block.create_dataset("Points", data=np.empty((0, 3), dtype="f8"))
            block.create_dataset("Polygons/NumberOfCells", data=np.array([0], dtype="i8"))
    summary = [
        {"name": "warpage_pv", "value": 0. if thermal_only else 1e-5, "unit": "m",
         "method": THERMAL_ONLY if thermal_only else WARPAGE_METHOD},
        {"name": "T_max", "value": 320., "unit": "K", "method": "FVM cell centers"},
        {"name": "U_mag_max", "value": 0. if thermal_only else 2e-5, "unit": "m",
         "method": THERMAL_ONLY if thermal_only else "FVM cell centers"},
    ]
    manifest = {"schema_version": "chipcae.results/1.0", "steps": [{"n": 0, "file": "out/step-0.vtkhdf"}],
                "summary": summary, "mesh": {"nx0": 2, "ny0": 1, "L": 0, "nz": 1,
                                            "padded_cells": 2, "mesh_sha256": "a" * 64},
                "partitions": {"per_step": [1], "solver_fixed_per_run": True}}
    write_json(root / "manifest.json", manifest)
    raw = event_bytes(solid=not thermal_only)
    if thermal_only:
        raw += log_frame(THERMAL_ONLY, level=2, seq=3)
    (root / "events.bin").write_bytes(raw)
    return root


def vtk_path(run):
    return run / read_json(run / "manifest.json")["steps"][0]["file"]


def result_fixture(data_class="synthetic"):
    # Retain a legacy v0.2 fixture to exercise backward compatibility.
    return {"schema": "chipcae.vv_result/0.2", "data_class": data_class,
            "case_id": "generated-board", "label": "实测", "date": "20261119",
            "commit": "1234567", "qoi": {"T_max": 320.}, "verdict": "pass",
            "reference": {"type": "回归", "source": "generated physical checks", "value": None},
            "notes": []}


class ContractTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="chipcae-realcase-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.run = make_run(self.root / "run")

    def events(self, raw=None):
        if raw is None:
            return decoded_fixture()
        path = self.root / "events.bin"
        path.write_bytes(event_bytes() if raw is None else raw)
        decoded = driver.decode_events(path)
        self.assertIsInstance(decoded, list, "decode_events must decode CAELOG1 records")
        return decoded

    def digest(self, run=None, events=None):
        result = driver.run_digest(self.run if run is None else run,
                                   self.events() if events is None else events)
        self.assertIsInstance(result, dict, "run_digest must return deterministic components")
        return result

    def edit_manifest(self, callback, run=None):
        path = (self.run if run is None else run) / "manifest.json"
        value = read_json(path)
        callback(value)
        write_json(path, value)


class FixtureValidity(ContractTest):
    def test_generated_vtkhdf_is_accepted_by_existing_reader(self):
        self.assertIsInstance(summarize(self.run), dict)
        path = write_json(self.root / "result.json", result_fixture())
        self.assertEqual(check_file(path), [])


class DigestContracts(ContractTest):
    def test_digest_components_and_identical_arrays(self):
        first = self.digest()
        other = make_run(self.root / "other")
        self.assertEqual(first, self.digest(other))
        self.assertEqual(first["summary"], {k: float(v).hex() for k, v in
                                          zip(QOIS, (1e-5, 320., 2e-5))})
        self.assertEqual(first["iterations"], {"T": 7, "U": 9})
        self.assertEqual(first["residual"], {"T": float(1e-13).hex(), "U": float(2e-13).hex()})
        self.assertEqual(list(first["fields"]), sorted(first["fields"]))
        self.assertEqual(first["mesh_sha256"], "a" * 64)
        actual = driver.digest_sha256(first)
        self.assertIsInstance(actual, str)
        self.assertEqual(actual, driver.digest_sha256(self.digest(other)))

    def test_digest_sha256_uses_canonical_json(self):
        value = {"b": 1, "a": {"T": "0x1.4p+8"}}
        expected = hashlib.sha256(b'{"a":{"T":"0x1.4p+8"},"b":1}').hexdigest()
        self.assertEqual(driver.digest_sha256(value), expected)

    def test_one_ulp_temperature_changes_only_its_field_component(self):
        before = self.digest()
        with h5py.File(vtk_path(self.run), "r+") as output:
            values = output["VTKHDF/hex/PointData/T"]
            values[0] = np.nextafter(values[0], np.inf)
        after = self.digest()
        changed = [key for key in before["fields"] if before["fields"][key] != after["fields"][key]]
        self.assertEqual(changed, ["VTKHDF/hex/PointData/T"])
        self.assertNotEqual(driver.digest_sha256(before), driver.digest_sha256(after))

    def test_times_paths_logs_attributes_and_unlisted_qoi_are_excluded(self):
        before = self.digest()
        self.edit_manifest(lambda m: m.update(wall_s=100, peak_rss_kib=234,
                                             path="changed-path", date="changed-date"))
        self.edit_manifest(lambda m: m["summary"].append({"name": "debug", "value": 100}))
        with h5py.File(vtk_path(self.run), "r+") as output:
            output["VTKHDF"].attrs["clock"] = 999
        ev = self.events(event_bytes() + log_frame("irrelevant timing/path text", seq=3))
        self.assertEqual(before, self.digest(events=ev))

    def test_endian_and_compressed_storage_normalize(self):
        other = make_run(self.root / "big-endian", endian=">", compressed=True)
        first = self.digest()
        self.assertEqual(first, self.digest(other))
        with h5py.File(vtk_path(self.run)) as output:
            value = output["VTKHDF/hex/PointData/T"][...]
        expected = hashlib.sha256(np.ascontiguousarray(value, dtype="<f8").tobytes()).hexdigest()
        self.assertEqual(first["fields"]["VTKHDF/hex/PointData/T"], expected)

    def test_assembly_links_and_reserved_fields_are_excluded(self):
        before = self.digest()
        with h5py.File(vtk_path(self.run), "r+") as output:
            assembly = output["VTKHDF"].create_group("Assembly")
            for group, name in (("volume", "hex"), ("volume", "cut"),
                                ("boundary", "eb_surface"), ("lagrangian", "particles")):
                assembly.require_group(group)[name] = h5py.SoftLink("/VTKHDF/" + name)
            output.create_dataset("VTKHDF/hex/PointData/reserved", data=np.full(16, 42.))
            output.create_dataset("VTKHDF/hex/CellData/reserved", data=np.full(2, 43.))
        self.assertEqual(before, self.digest())

    def test_final_residual_is_last_event_per_field(self):
        raw = event_bytes() + residual_frame([("fvm.conduction", "T", 11, 4e-15)], seq=3)
        digest = self.digest(events=self.events(raw))
        self.assertEqual(digest["iterations"], {"T": 11, "U": 9})
        self.assertEqual(digest["residual"]["T"], float(4e-15).hex())


class EventContracts(ContractTest):
    def test_residual_batch_18_and_logline_20_decode_in_order(self):
        events = self.events(event_bytes())
        self.assertEqual([(e["solver"], e["field"], e["iterations"], e["residual"])
                          for e in events if "solver" in e],
                         [("fvm.conduction", "T", 7, 1e-13), ("fvm.solid", "U", 9, 2e-13)])
        self.assertEqual(events[-1]["level"], 1)
        self.assertTrue(events[-1]["message"].startswith("FVM_ENERGY "))

    def test_lifecycle_events_from_real_solver_do_not_interrupt_batch_decoding(self):
        hello = (string("run") + struct.pack("<I", 0) + struct.pack("<I", 1) + string("v1")
                 + string("kernel") + string("schema") + struct.pack("<I", 0)
                 + struct.pack("<III", 1, 1, 2) + struct.pack("<II", 2, 8)
                 + string("case") + string("git") + string("protocol") + struct.pack("<I", 0))
        prefix = frame(15, hello, seq=1) + frame(21, struct.pack("<BQd", 2, 0, 0.), seq=2)
        suffix = frame(19, struct.pack("<iQ", 0, 0) + string("completed"), seq=5)
        events = self.events(prefix + event_bytes() + suffix)
        self.assertEqual([e["kind"] for e in events], [15, 21, 18, 18, 20, 19])
        self.assertEqual([e["iterations"] for e in events if "solver" in e], [7, 9])
        self.assertEqual(events[-1]["exit_code"], 0)

    def test_incomplete_and_crc_invalid_tail_stop_without_losing_valid_prefix(self):
        prefix = event_bytes()
        expected = self.events(prefix)
        tail = log_frame("tail", seq=3)
        corrupt = bytearray(tail)
        corrupt[-1] ^= 1
        for suffix in (tail[:7], tail[:19], bytes(corrupt)):
            with self.subTest(suffix=suffix[:16]):
                self.assertEqual(self.events(prefix + suffix + tail), expected)


class ThermalChecks(ContractTest):
    def test_c2_accepts_required_schema_methods_and_fields(self):
        self.assertIs(driver._check_c2(self.run, self.events()), True)

    def test_c2_rejects_missing_fields_wrong_schema_and_method(self):
        for name in ("PointData/T", "PointData/U", "CellData/layer_id", "CellData/material_id",
                     "CellData/cell_kind", "CellData/elem_gid", "CellData/volume"):
            with self.subTest(missing=name):
                run = make_run(self.root / name.replace("/", "_"))
                with h5py.File(vtk_path(run), "r+") as output:
                    del output["VTKHDF/hex/" + name]
                self.assertIs(driver._check_c2(run, self.events()), False)
        for change in (lambda m: m.update(schema_version="wrong"),
                       lambda m: m["summary"][1].update(method="FEM nodes"),
                       lambda m: m["summary"].pop(0)):
            run = make_run(self.root / "bad-manifest")
            self.edit_manifest(change, run)
            self.assertIs(driver._check_c2(run, self.events()), False)

    def test_c2_accepts_optional_absent_cell_temperature(self):
        with h5py.File(vtk_path(self.run), "r+") as output:
            for block in ("hex", "cut"):
                del output["VTKHDF/" + block + "/CellData/T_cell"]
        self.assertIs(driver._check_c2(self.run, decoded_fixture()), True)

    def test_c2_thermal_only_zeros_require_matching_level_two_log(self):
        run = make_run(self.root / "thermal-only", thermal_only=True)
        base = decoded_fixture(solid=False)
        matching = base + [{"level": 2, "message": THERMAL_ONLY}]
        self.assertIs(driver._check_c2(run, matching), True)
        for events in (base, base + [{"level": 1, "message": THERMAL_ONLY}],
                       base + [{"level": 2, "message": "another message"}]):
            self.assertIs(driver._check_c2(run, events), False)
        self.edit_manifest(lambda m: m["summary"][0].update(value=1e-30), run)
        self.assertIs(driver._check_c2(run, matching), False)

    def test_c3_accepts_conduction_and_rejects_fem_or_absent_residual(self):
        self.assertIs(driver._check_c3(self.events()), True)
        self.assertIs(driver._check_c3([{"level": 1, "message": "no residual"}]), False)
        events = decoded_fixture() + [{"solver": "fem.solid", "field": "U", "iterations": 1, "residual": 0.}]
        self.assertIs(driver._check_c3(events), False)

    def test_c4_exactly_one_level_one_energy_log_with_four_keys_and_threshold(self):
        self.assertIs(driver._check_c4(self.events()), True)
        bad = dict(ENERGY, imbalance=2e-9)
        missing = dict(ENERGY)
        del missing["iterations"]
        for events in (decoded_fixture(energy=bad), decoded_fixture(energy=missing), [],
                       decoded_fixture() + [decoded_fixture()[-1]],
                       [{"level": 2, "message": "FVM_ENERGY " + json.dumps(ENERGY)}]):
            with self.subTest(events=events):
                self.assertIs(driver._check_c4(events), False)

    def test_c5_finite_fields_summary_and_energy_pass(self):
        self.assertIs(driver._check_c5(self.run, ENERGY), True)

    def test_c5_nonfinite_point_and_cell_fields_fail(self):
        for path in ("PointData/T", "PointData/U", "CellData/volume", "CellData/T_cell"):
            for value in (np.nan, np.inf):
                with self.subTest(path=path, value=value):
                    run = make_run(self.root / "nonfinite")
                    with h5py.File(vtk_path(run), "r+") as output:
                        output["VTKHDF/hex/" + path][0] = value
                    self.assertIs(driver._check_c5(run, ENERGY), False)

    def test_c5_nonfinite_summary_and_energy_fail(self):
        for value in (float("nan"), float("inf")):
            self.assertIs(driver._check_c5(self.run, dict(ENERGY, heat_in=value)), False)
            self.edit_manifest(lambda m: m["summary"][1].update(value=value))
            self.assertIs(driver._check_c5(self.run, ENERGY), False)

    def test_c6_cell_min_and_summary_max_use_one_e_minus_eight_tolerance(self):
        self.assertIs(driver._check_c6(self.run, DEFAULT_LOADS), True)
        with h5py.File(vtk_path(self.run), "r+") as output:
            output["VTKHDF/hex/CellData/T_cell"][0] = 298.15 - 1e-7
        self.assertIs(driver._check_c6(self.run, DEFAULT_LOADS), False)
        make_run(self.run)
        self.edit_manifest(lambda m: m["summary"][1].update(value=348.15 + 1e-8))
        self.assertIs(driver._check_c6(self.run, DEFAULT_LOADS), True)
        self.edit_manifest(lambda m: m["summary"][1].update(value=348.15 + 1e-7))
        self.assertIs(driver._check_c6(self.run, DEFAULT_LOADS), False)

    def test_c6_vertex_overshoot_is_diagnostic_and_body_source_is_null(self):
        with h5py.File(vtk_path(self.run), "r+") as output:
            output["VTKHDF/hex/PointData/T"][0] = 1000.
        self.assertIs(driver._check_c6(self.run, DEFAULT_LOADS), True)
        loads = DEFAULT_LOADS + [{"type": "heat_source", "target": "layer:board", "q_vol": 1.}]
        self.assertIsNone(driver._check_c6(self.run, loads))


class WarpageChecks(ContractTest):
    def test_w1_requires_solid_residual_and_both_fvm_methods(self):
        self.assertIs(driver._check_w1(self.run, self.events()), True)
        self.assertIs(driver._check_w1(self.run, decoded_fixture(solid=False)), False)
        for index in (0, 2):
            run = make_run(self.root / "wrong-method")
            self.edit_manifest(lambda m: m["summary"][index].update(method="wrong"), run)
            self.assertIs(driver._check_w1(run, self.events()), False)

    def test_w2_accepts_finite_nonnegative_qoi_and_rejects_negative_or_nonfinite(self):
        self.assertIs(driver._check_w2(self.run), True)
        for index in (0, 2):
            run = make_run(self.root / "negative")
            self.edit_manifest(lambda m: m["summary"][index].update(value=-1e-30), run)
            self.assertIs(driver._check_w2(run), False)
        for value in (np.nan, np.inf):
            with h5py.File(vtk_path(self.run), "r+") as output:
                output["VTKHDF/hex/PointData/U"][0, 2] = value
            self.assertIs(driver._check_w2(self.run), False)

    def test_w3_bottom_vertices_must_be_exactly_zero(self):
        constraints = {"fixed": [{"target": "face:zmin"}]}
        self.assertIs(driver._check_w3(self.run, constraints), True)
        with h5py.File(vtk_path(self.run), "r+") as output:
            output["VTKHDF/hex/PointData/U"][0, 2] = 1e-30
        self.assertIs(driver._check_w3(self.run, constraints), False)

    def test_w3_rigid321_has_no_invented_tolerance(self):
        self.assertIsNone(driver._check_w3(self.run, {"rigid": "3-2-1"}))


class AssemblyChecks(ContractTest):
    def test_a1_bodies_weld_count_exit_code_and_b_tier_policy(self):
        report = {"welded_interfaces": 2, "defects": [], "parts": {"split_layers": {}}}
        stack = {"layers": [{"name": "board", "body": "board", "regions": [
            {"body": "lid", "material": "copper"}, {"body": "stiffener", "material": "copper"}]}]}
        report_path, stack_path = self.root / "import_report.json", self.root / "stack.json"

        def check(r, s, rc=0, accept=False):
            write_json(report_path, r)
            write_json(stack_path, s)
            return driver._check_a1(report_path, stack_path, rc, ["lid", "stiffener"], accept)

        self.assertIs(check(report, stack), True)
        self.assertIs(check(report, stack, rc=7), False)
        absent = copy.deepcopy(stack)
        absent["layers"][0]["regions"].pop()
        self.assertIs(check(report, absent), False)
        no_count = copy.deepcopy(report)
        del no_count["welded_interfaces"]
        self.assertIs(check(no_count, stack), False)
        report["defects"] = [{"defect_id": "lid/generated-b", "tier": "B"}]
        self.assertIs(check(report, stack), False)
        self.assertIs(check(report, stack, accept=True), True)


class ReadinessAndLeaks(ContractTest):
    def ready(self, kind, source):
        result = driver.readiness(kind, source)
        self.assertIsInstance(result, tuple, "readiness must return (ready, reason)")
        self.assertEqual(len(result), 2)
        self.assertIs(type(result[0]), bool)
        self.assertIsInstance(result[1], str)
        if not result[0]:
            self.assertTrue(result[1], "unready dependencies need an explanation")
        return result[0]

    def test_readiness_requires_pipeline_elastic_and_removal_of_thermal_only_marker(self):
        source = self.root / "source"
        source.mkdir()
        self.assertFalse(self.ready("thermal", source))
        pipeline = source / "apps/chipcae-solve/src/fvm_pipeline.cpp"
        pipeline.parent.mkdir(parents=True)
        pipeline.write_text(THERMAL_ONLY)
        self.assertTrue(self.ready("thermal", source))
        self.assertFalse(self.ready("warpage", source))
        elastic = source / "solvers/fvm/elastic/include/chipcae/solvers/fvm/elastic.hpp"
        elastic.parent.mkdir(parents=True)
        elastic.touch()
        self.assertFalse(self.ready("warpage", source))
        pipeline.write_text("// elastic wiring present\n")
        self.assertTrue(self.ready("warpage", source))

    def test_assembly_uses_nearest_capability_from_resolved_import_only(self):
        source = self.root / "source"
        pipeline = source / "apps/chipcae-solve/src/fvm_pipeline.cpp"
        pipeline.parent.mkdir(parents=True)
        pipeline.touch()
        exe = self.root / "build/nested/bin/import"
        exe.parent.mkdir(parents=True)
        exe.touch(mode=0o755)
        cap = self.root / "build/import-assets/assembly-capabilities.json"
        near = exe.parent / "import-assets/assembly-capabilities.json"
        write_json(source / "import-assets/assembly-capabilities.json", {"geometry": "ON"})
        with patch.dict(os.environ, {"CHIPCAE_IMPORT_EXE": str(exe)}):
            self.assertFalse(self.ready("assembly", source))
            write_json(cap, {"geometry": "ON"})
            self.assertTrue(self.ready("assembly", source))
            write_json(near, {"geometry": "OFF"})
            self.assertFalse(self.ready("assembly", source))
            near.write_text("invalid JSON")
            self.assertFalse(self.ready("assembly", source))
            write_json(near, {"geometry": "ON"})
            self.assertTrue(self.ready("assembly", source))
        alias = self.root / "import-alias"
        alias.symlink_to(exe)
        with patch.dict(os.environ, {"CHIPCAE_IMPORT_EXE": str(alias)}):
            self.assertTrue(self.ready("assembly", source))
        with patch.dict(os.environ):
            os.environ.pop("CHIPCAE_IMPORT_EXE", None)
            self.assertFalse(self.ready("assembly", source))

    def test_leak_patterns_match_case_insensitive_substrings(self):
        self.assertEqual(driver.leak_check("+FAKE_BOARD_XYZ and Fake_Part_Abc", [
            "fake_board_xyz", "fake_part_abc", "absent"]), ["fake_board_xyz", "fake_part_abc"])
        self.assertEqual(driver.leak_check("+generated fixture", ["fake_board_xyz"]), [])

    def test_solver_return_code_classification(self):
        for rc, expected in ((0, None), (13, "CUT_UNSUPPORTED"), (14, "error"),
                             (-9, "error"), ("timeout", "error"), ("signal:15", "error")):
            with self.subTest(rc=rc):
                self.assertEqual(driver.classify_solve_rc(rc), expected)


def complete_record():
    return {
        "schema": "chipcae.vv_real_case_run/0.1", "kind": "thermal", "case_id": "generated-board",
        "data_class": "synthetic", "materials_provenance": "illustrative", "commit": "1234567",
        "date": "20261119", "host": {"nproc": 2, "mem_total_kib": 1024},
        "readiness": {"ready": True, "reason": "available"}, "needs_attention": [],
        "status_detail": None, "attempts": [], "exit_code": 0,
        "inputs": {"case.json": "a" * 64},
        "commands": [{"stage": "solve", "argv": ["solve"], "exit_code": 0,
                      "wall_s": .1, "peak_rss_kib": 100}],
        "logs": ["logs/solve.stdout.log"],
        "mesh": {"preview_cell_count": 2, "n_cells_hex": 2, "n_cells_cut": 0,
                 "nx0": 2, "ny0": 1, "L": 0, "nz": 1, "padded_cells": 2, "mesh_sha256": "a" * 64},
        "dofs": {"thermal": 2, "mech": 6,
                 "source": "derived: FVM, thermal 1 / mech 3 unknowns per control volume"},
        "solver": [{"name": "fvm.conduction", "preconditioner": "jacobi", "rtol": 1e-12,
                    "iterations": 7, "final_residual": 1e-13}],
        "energy": {k: v for k, v in ENERGY.items() if k != "iterations"},
        "summary": {"T_max": 320., "warpage_pv": 0., "U_mag_max": 0.,
                    "methods": {"T_max": "FVM cell centers", "warpage_pv": THERMAL_ONLY,
                                "U_mag_max": THERMAL_ONLY}},
        "temperature": {"T_vertex_min": 320., "T_vertex_max": 320.,
                        "T_cell_min": 320., "T_cell_max": 320.},
        "displacement": None, "assembly": None,
        "digests": {"np1_r1": "a" * 64, "np1_r2": "a" * 64},
        "file_sha256": {"manifest": "b" * 64, "vtkhdf": "c" * 64},
        "checks": {**{f"C{i}": True for i in range(1, 11)}, "C8": None},
        "missing": [], "verdict": "pass", "notes": ["MPI not requested"],
    }


class RecordChecks(ContractTest):
    def test_c9_complete_applicable_record_and_unexplained_null(self):
        record = complete_record()
        self.assertIs(driver._check_c9(record), True)
        record["energy"]["heat_in"] = None
        # Empty missing[] must not conceal an unexplained null in an applicable field.
        self.assertIs(driver._check_c9(record), False)

    def test_c9_dependency_interruption_allows_unavailable_fields(self):
        record = complete_record()
        record.update(exit_code=77, verdict="needs_attention", energy=None,
                      readiness={"ready": False, "reason": "fvm-case-e2e unavailable"},
                      needs_attention=[{"item": "T", "reason": "fvm-case-e2e unavailable"}],
                      missing=["energy"], notes=["dependency interrupted all solve stages"])
        record["checks"] = {key: None for key in record["checks"]}
        self.assertIsNone(driver._check_c9(record))

    def test_c10_synthetic_empty_and_real_exact_d03_singleton(self):
        for data_class, reasons in (("synthetic", []),
                                    ("real", ["real result must stay local (D03)"])):
            with self.subTest(data_class=data_class):
                path = write_json(self.root / "result.json", result_fixture(data_class))
                self.assertEqual(check_file(path), reasons)
                self.assertIs(driver._check_c10(path, data_class), True)
                value = result_fixture(data_class)
                value["commit"] = "invalid"
                write_json(path, value)
                self.assertIs(driver._check_c10(path, data_class), False)


# The external programs are the only substituted boundary. Driver orchestration,
# records, readers, checks and resource measurement run unchanged.
FAKE_CLI = r'''
import json, os, pathlib, shutil, signal, sys, time
root = pathlib.Path(os.environ['REALCASE_TEST_ROOT'])
args = sys.argv[1:]
name = pathlib.Path(sys.argv[0]).name
trace = root / 'calls.jsonl'
previous = trace.read_text().splitlines() if trace.exists() else []
with trace.open('a') as stream:
    stream.write(json.dumps({'name': name, 'args': args}) + '\n')
mode = os.environ.get('REALCASE_TEST_MODE', 'ok')
def arg(flag):
    return pathlib.Path(args[args.index(flag) + 1])
def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))
if name == 'fake-mpi':
    assert args[:2] == ['-n', '2'], args
    os.execv(args[2], args[2:])
if name == 'fake-generator':
    pathlib.Path(args[0]).mkdir(parents=True, exist_ok=True)
    for part in ('lid', 'stiffener'):
        (pathlib.Path(args[0]) / (part + '.step')).write_text('generated STEP double')
elif name == 'fake-import':
    if mode == 'import7':
        sys.exit(7)
    if mode == 'import9':
        sys.exit(9)
    stack = json.loads((root / 'stack.json').read_text())
    write(arg('-o'), stack)
    report = {'welded_interfaces': 2, 'defects': [], 'parts': {'split_layers': {}}}
    if mode == 'no_weld':
        del report['welded_interfaces']
    write(arg('-o').parent / 'import_report.json', report)
    if '--report' in args:
        write(arg('--report'), report)
    if '--polyset' in args:
        shutil.copyfile(root / 'board.polyset', arg('--polyset'))
elif name == 'fake-preview':
    print(json.dumps({'stage': 'success', 'cell_count': 2}))
elif name == 'fake-solve':
    case = json.loads(arg('--case').read_text())
    profile = json.loads(arg('--profile').read_text())
    with (root / 'cases.jsonl').open('a') as stream:
        stream.write(json.dumps({'case': case, 'profile': profile, 'out': str(arg('--out'))}) + '\n')
    if mode.startswith('rc'):
        sys.exit(int(mode[2:]))
    if mode == 'timeout':
        time.sleep(30)
    if mode == 'signal':
        os.kill(os.getpid(), signal.SIGTERM)
    out = arg('--out')
    shutil.copytree(root / 'template', out, dirs_exist_ok=True)
    manifest_path = out / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    ranks = profile['ranks']
    manifest['partitions']['per_step'] = [ranks]
    if ranks == 2:
        import h5py, numpy as np
        with h5py.File(out / 'out/step-0.vtkhdf', 'r+') as f:
            changes = {
                'hex/NumberOfPoints': [8, 8], 'hex/NumberOfCells': [1, 1],
                'hex/NumberOfConnectivityIds': [8, 8], 'hex/Offsets': [0, 8, 0, 8],
                'hex/Connectivity': list(range(8)) * 2,
                'cut/NumberOfPoints': [0, 0], 'cut/NumberOfCells': [0, 0],
                'cut/NumberOfConnectivityIds': [0, 0], 'cut/Offsets': [0, 0],
                'eb_surface/NumberOfPoints': [0, 0], 'eb_surface/Polygons/NumberOfCells': [0, 0],
                'particles/NumberOfPoints': [0, 0], 'particles/Polygons/NumberOfCells': [0, 0]}
            for path, value in changes.items():
                del f['VTKHDF/' + path]
                f.create_dataset('VTKHDF/' + path, data=np.array(value, dtype='i8'))
    loads = case.get('loads', [])
    tref = next((p.get('T_ref') for p in case.get('physics', []) if p.get('id') == 'mech'), None)
    if len(loads) == 2 and all(load.get('type') == 'fixed_temperature'
                               and load.get('T') == tref for load in loads):
        import h5py
        for qoi in manifest['summary']:
            qoi['value'] = tref if qoi['name'] == 'T_max' else 0.
        with h5py.File(out / 'out/step-0.vtkhdf', 'r+') as f:
            for block in ('hex', 'cut'):
                f['VTKHDF/' + block + '/PointData/T'][...] = tref
                f['VTKHDF/' + block + '/PointData/U'][...] = 0.
                f['VTKHDF/' + block + '/CellData/T_cell'][...] = tref
    nsolve = sum(json.loads(line)['name'] == name for line in previous)
    if mode == 'repeat_energy' and nsolve == 1:
        shutil.copyfile(root / 'bad-events.bin', out / 'events.bin')
    if mode == 'repeat_qoi' and nsolve == 1:
        manifest['summary'][1]['value'] += 1e-5
    if ranks == 2 and mode == 'parallel_mesh':
        manifest['mesh']['mesh_sha256'] = 'b' * 64
    if ranks == 2 and mode == 'parallel_partitions':
        manifest['partitions']['per_step'] = [1]
    if ranks == 2 and mode == 'parallel_qoi':
        manifest['summary'][1]['value'] *= 1 + 1e-7
    write(manifest_path, manifest)
    if mode == 'bad_material':
        import h5py
        with h5py.File(out / 'out/step-0.vtkhdf', 'r+') as f:
            f['VTKHDF/hex/CellData/material_id'][...] = 2
else:
    raise AssertionError('unexpected external stage: ' + name)
'''


class DriverControlFlow(ContractTest):
    def setUp(self):
        super().setUp()
        self.source = self.root / "source"
        self.source.mkdir()
        subprocess.run(["git", "init", "-q", str(self.source)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(self.source), "-c", "user.name=Acceptance Test",
                        "-c", "user.email=acceptance@example.invalid", "commit", "--allow-empty",
                        "-qm", "synthetic fixture"], check=True, capture_output=True)
        # Readback is the existing production summary reader, even with a temporary source tree.
        (self.source / "tools").symlink_to(SOURCE / "tools", target_is_directory=True)
        self.pipeline = self.source / "apps/chipcae-solve/src/fvm_pipeline.cpp"
        self.pipeline.parent.mkdir(parents=True)
        self.pipeline.touch()
        elastic = self.source / "solvers/fvm/elastic/include/chipcae/solvers/fvm/elastic.hpp"
        elastic.parent.mkdir(parents=True)
        elastic.touch()
        make_run(self.root / "template")
        (self.root / "bad-events.bin").write_bytes(event_bytes(energy=dict(ENERGY, imbalance=2e-9)))
        stack = {"id": "generated", "unit": "m", "outline": {"box": [0, 0, 2, 1]},
                 "symmetry": "none", "package_type": "FCBGA", "layers": [
                     {"name": "board", "kind": "Patterned", "material": "fr4_eq", "thickness": 1.,
                      "n_elem_min": 1, "grading": 1,
                      "copper": {"metal": "copper", "polygons": "board.polyset"},
                      "regions": [{"name": "lid", "body": "lid", "material": "copper",
                                   "box": [0, 0, 1, 1]},
                                  {"name": "ring", "body": "stiffener", "material": "copper",
                                   "box": [1, 0, 2, 1]}]}]}
        write_json(self.root / "stack.json", stack)
        (self.root / "board.polyset").write_bytes(b"synthetic sidecar retained by driver")
        self.bin = self.root / "programs with spaces/bin"
        self.bin.mkdir(parents=True)
        for name in ("import", "preview", "solve", "mpi", "generator"):
            exe = self.bin / ("fake-" + name)
            exe.write_text("#!" + sys.executable + "\n" + FAKE_CLI)
            exe.chmod(0o755)
        write_json(self.bin.parent / "import-assets/assembly-capabilities.json", {"geometry": "ON"})
        self.env = {
            "CHIPCAE_IMPORT_EXE": str(self.bin / "fake-import"),
            "CHIPCAE_PREVIEW_EXE": str(self.bin / "fake-preview"),
            "CHIPCAE_SOLVE_EXE": str(self.bin / "fake-solve"),
            "CHIPCAE_MPIEXEC": str(self.bin / "fake-mpi"),
            "SP4_FIXTURE_GENERATOR": str(self.bin / "fake-generator"),
            "REALCASE_TEST_ROOT": str(self.root), "REALCASE_TEST_MODE": "ok",
        }
        env = patch.dict(os.environ, self.env)
        env.start()
        self.addCleanup(env.stop)
        self.params = {"case_id": "generated-board", "data_class": "synthetic",
                       "input": str(SOURCE / "tests/integration/ecad/data/board_4layer.xml"),
                       "material_map": str(SOURCE / "tests/integration/f1130_uncut_cli/material_map.json"),
                       "materials": None, "mesh": {"nx": 4, "ny": 4, "levels": 0,
                       "max_ratio": 100, "min_elems_per_layer": 1}, "timeout_s": 3}
        self.out = self.root / "outputs"

    def calls(self, name=None):
        path = self.root / "calls.jsonl"
        rows = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
        return rows if name is None else [row for row in rows if row["name"] == "fake-" + name]

    def invoke(self, kind="thermal", params=None, nps=None, repeat=2):
        record = driver.run_item(kind, self.params if params is None else params,
                                 self.out, [1] if nps is None else nps, repeat, self.source)
        self.assertIsInstance(record, dict, "run_item must finalize and return a record")
        self.assertIn("exit_code", record)
        return record

    def persisted(self, record):
        paths = list(self.out.rglob("run_record.json"))
        self.assertTrue(paths, "every invocation must persist run_record.json")
        matches = [p for p in paths if read_json(p) == record]
        self.assertTrue(matches, "returned record must be the finalized persisted record")
        folder = matches[-1].parent
        self.assertTrue((folder / "run_record.md").is_file())
        self.assertTrue((folder / "result.json").is_file())
        return folder

    def assembly_params(self):
        inputs = self.root / "assembly inputs"
        inputs.mkdir(exist_ok=True)
        shutil.copyfile(SOURCE / "tests/integration/ecad/data/board_2layer.xml", inputs / "board.xml")
        write_json(inputs / "map.json", {"FR4": "fr4_eq", "copper": "copper"})
        # Library order deliberately differs from the material_names ID table.
        library = read_json(SOURCE / "schema/examples/fcbga_demo.materials.json")
        base = library["materials"][0]
        library["materials"] = [dict(copy.deepcopy(base), id=name) for name in ("fr4_eq", "copper")]
        write_json(inputs / "materials.json", library)
        for part in ("lid", "stiffener"):
            (inputs / (part + ".step")).write_text("generated fixture")
        document = {"schema": "chipcae.assembly/0.1",
                    "board": {"path": "board.xml", "format": "ipc2581", "material_map": "map.json"},
                    "materials": "materials.json", "parts": [
                        {"id": part, "step": part + ".step", "material": "copper",
                         "placement": {"attach": "layer:board@top"}} for part in ("lid", "stiffener")]}
        write_json(inputs / "assembly.json", document)
        params = dict(self.params, input=str(inputs / "assembly.json"), material_map=None)
        return params

    def test_external_fixtures_round_trip_through_existing_reader(self):
        # This passing smoke test rules out malformed fixtures as the cause of red contracts.
        stack = self.root / "fixture.stack.json"
        imported = subprocess.run([self.env["CHIPCAE_IMPORT_EXE"], self.params["input"],
                                   "-o", str(stack), "--polyset", str(self.root / "fixture.polyset")],
                                  capture_output=True, text=True, timeout=5)
        self.assertEqual(imported.returncode, 0, imported.stderr)
        preview = subprocess.run([self.env["CHIPCAE_PREVIEW_EXE"], str(stack), "-o",
                                  str(self.root / "preview"), "--nx", "4", "--ny", "4",
                                  "--levels", "0", "--max-ratio", "100"],
                                 capture_output=True, text=True, timeout=5)
        self.assertEqual(preview.returncode, 0, preview.stderr)
        self.assertEqual(json.loads(preview.stdout)["cell_count"], 2)
        case = write_json(self.root / "fixture.case.json", {"schema": "chipcae.case/1.0"})
        profile = write_json(self.root / "fixture.profile.json", {
            "schema": "chipcae.run_profile/1.0", "backend": "cpu", "ranks": 1, "gpus": 0})
        out = self.root / "fixture-out"
        solved = subprocess.run([self.env["CHIPCAE_SOLVE_EXE"], "--case", str(case),
                                 "--profile", str(profile), "--out", str(out)],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(solved.returncode, 0, solved.stderr)
        self.assertIsInstance(summarize(out), dict)
        self.assertEqual(len(self.calls("solve")), 1)
        write_json(profile, {"schema": "chipcae.run_profile/1.0", "backend": "cpu", "ranks": 2, "gpus": 0})
        out2 = self.root / "fixture-np2"
        parallel = subprocess.run([self.env["CHIPCAE_MPIEXEC"], "-n", "2", self.env["CHIPCAE_SOLVE_EXE"],
                                  "--case", str(case), "--profile", str(profile), "--out", str(out2)],
                                 capture_output=True, text=True, timeout=5)
        self.assertEqual(parallel.returncode, 0, parallel.stderr)
        self.assertIsInstance(summarize(out2), dict)

    def test_cell_budget_is_preserved_or_omitted_and_invalid_values_start_no_stages(self):
        for budget in (100000, None):
            with self.subTest(budget=budget):
                params = copy.deepcopy(self.params)
                if budget is not None:
                    params["mesh"]["cell_budget"] = budget
                record = self.invoke(params=params, repeat=1)
                self.assertEqual(record["exit_code"], 0, {k: record[k] for k in ("checks", "missing", "notes")})
                case = read_json(self.persisted(record) / "inputs/case.json")
                if budget is None:
                    self.assertNotIn("cell_budget", case["mesh"])
                else:
                    self.assertIs(type(case["mesh"]["cell_budget"]), int)
                    self.assertEqual(case["mesh"]["cell_budget"], 100000)
        (self.root / "calls.jsonl").unlink()
        for budget in (0, -1, 1.5, True):
            with self.subTest(invalid_budget=budget):
                params = copy.deepcopy(self.params)
                params["mesh"]["cell_budget"] = budget
                record = self.invoke(params=params)
                self.assertEqual(record["exit_code"], 2)
                self.assertEqual(self.calls(), [])
                self.assertFalse((self.persisted(record) / "inputs/case.json").exists())

    def test_missing_required_params_writes_usage_record_without_stages(self):
        for missing in ("case_id", "data_class", "input", "material_map", "materials", "mesh"):
            with self.subTest(missing=missing):
                params = copy.deepcopy(self.params)
                del params[missing]
                record = self.invoke(params=params)
                self.assertEqual(record["exit_code"], 2)
                self.assertEqual(record["verdict"], "fail")
                self.assertTrue(all(value is None for value in record["checks"].values()))
                self.assertEqual(record["needs_attention"], [])
                self.assertTrue(record["notes"])
                self.assertEqual(self.calls(), [])
                self.persisted(record)

    def test_invalid_mesh_types_ranks_repeat_timeout_and_nonfinite_values_start_no_stages(self):
        for key, bad in (("nx", 0), ("ny", -1), ("levels", -1), ("nx", True),
                         ("max_ratio", float("nan")), ("min_elems_per_layer", 0)):
            with self.subTest(key=key, bad=bad):
                params = copy.deepcopy(self.params)
                params["mesh"][key] = bad
                self.assertEqual(self.invoke(params=params)["exit_code"], 2)
                self.assertEqual(self.calls(), [])
        for nps, repeat in (([3], 2), ([1], 0), ([1], -1)):
            self.assertEqual(self.invoke(nps=nps, repeat=repeat)["exit_code"], 2)
        for key, bad in (("timeout_s", 0), ("timeout_s", float("inf")),
                         ("T_ref", float("nan")), ("accept_b_tier", "yes")):
            self.assertEqual(self.invoke(params=dict(self.params, **{key: bad}))["exit_code"], 2)
        self.assertEqual(self.calls(), [])

    def test_bad_numeric_load_values_are_usage_before_import(self):
        for field, value in (("T", "hot"), ("T", True), ("T_inf", "warm"), ("h", [])):
            with self.subTest(field=field):
                loads = copy.deepcopy(DEFAULT_LOADS)
                loads[0 if field == "T" else 1][field] = value
                record = self.invoke(params=dict(self.params, loads=loads))
                self.assertEqual(record["exit_code"], 2)
                self.assertEqual(self.calls(), [])
                self.persisted(record)

    def test_missing_weld_count_is_failure_even_if_assembly_w_is_unready(self):
        self.pipeline.write_text(THERMAL_ONLY)
        make_run(self.root / "template", thermal_only=True)
        os.environ["REALCASE_TEST_MODE"] = "no_weld"
        record = self.invoke("assembly", params=self.assembly_params())
        self.assertEqual(record["exit_code"], 1)
        self.assertIs(record["checks"]["A1"], False)
        self.assertIsNone(record["assembly"]["welded_interfaces"])
        self.assertTrue(record["needs_attention"])
        self.persisted(record)

    def test_invalid_kind_and_required_field_types_are_usage_without_stages(self):
        self.assertEqual(self.invoke(kind="unsupported")["exit_code"], 2)
        for key, bad in (("case_id", 12), ("data_class", "public"), ("input", []),
                         ("materials", True), ("mesh", []), ("material_map", 42)):
            with self.subTest(key=key):
                self.assertEqual(self.invoke(params=dict(self.params, **{key: bad}))["exit_code"], 2)
                self.assertEqual(self.calls(), [])

    def test_real_output_inside_git_worktree_and_symlink_alias_is_usage(self):
        params = dict(self.params, data_class="real")
        forbidden = self.source / "forbidden-output"
        alias = self.root / "source-alias"
        alias.symlink_to(self.source, target_is_directory=True)
        for target in (forbidden, alias / "forbidden-output"):
            with self.subTest(target=target):
                record = driver.run_item("thermal", params, target, [1], 2, self.source)
                self.assertIsInstance(record, dict)
                self.assertEqual(record.get("exit_code"), 2)
                self.assertEqual(record.get("verdict"), "fail")
                self.assertEqual(self.calls(), [])
                self.assertFalse(forbidden.exists(), "rejected real outputs must not enter Git worktrees")

    def test_assembly_material_mismatch_is_usage_before_import(self):
        params = self.assembly_params()
        other = read_json(Path(params["input"]).parent / "materials.json")
        other["materials"][0]["density"] += 1
        params["materials"] = str(write_json(self.root / "other-materials.json", other))
        record = self.invoke("assembly", params=params)
        self.assertEqual(record["exit_code"], 2)
        self.assertEqual(record["verdict"], "fail")
        self.assertEqual(self.calls(), [])
        self.persisted(record)

    def test_dependency_alone_is_77_with_reason_and_no_stages(self):
        self.pipeline.unlink()
        record = self.invoke()
        self.assertEqual(record["exit_code"], 77)
        self.assertEqual(record["verdict"], "needs_attention")
        self.assertTrue(record["needs_attention"])
        self.assertTrue(record["needs_attention"][0]["reason"])
        self.assertIsNone(record["checks"]["C1"])
        self.assertIsNone(record["checks"]["C9"])
        self.assertEqual(self.calls(), [])
        self.persisted(record)

    def test_missing_executable_is_dependency_without_running_stages(self):
        os.environ["CHIPCAE_SOLVE_EXE"] = str(self.root / "nonexistent-solve")
        record = self.invoke()
        self.assertEqual(record["exit_code"], 77)
        self.assertTrue(record["needs_attention"])
        self.assertEqual(self.calls(), [])
        self.persisted(record)

    def test_result_preserves_dependency_verdict(self):
        self.pipeline.unlink()
        record = self.invoke()
        self.assertEqual(record["exit_code"], 77)
        self.assertEqual(record["verdict"], "needs_attention")
        self.assertTrue(record["needs_attention"])
        result_path = self.persisted(record) / "result.json"
        result = read_json(result_path)
        self.assertEqual(result["verdict"], "skip")
        self.assertEqual(result["schema"], "chipcae.vv_result/0.2")
        self.assertTrue(any("needs_attention" in note for note in result["notes"]))
        self.assertTrue(any(record["needs_attention"][0]["reason"] in note for note in result["notes"]))
        self.assertEqual(check_file(result_path), [])
        self.assertIs(record["checks"]["C10"], True)
        self.assert_exportable_metadata(result)

    def test_result_identifies_physical_check_reference(self):
        record = self.invoke()
        self.assertEqual(record["exit_code"], 0)
        result_path = self.persisted(record) / "result.json"
        result = read_json(result_path)
        self.assertIsNone(result["reference"])
        self.assertTrue(any("无解析解（物理合理性检查）" in note for note in result["notes"]))
        self.assertEqual(result["schema"], "chipcae.vv_result/0.2")
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(check_file(result_path), [])
        self.assert_exportable_metadata(result)

    def assert_exportable_metadata(self, result):
        metadata = {key: result[key] for key in ("case_id", "data_class", "label", "date", "commit",
                                               "verdict", "reference", "tolerance", "notes")}
        exported = result_json_from_summary(summarize(self.root / "template"), **metadata)
        self.assertEqual(exported["schema"], "chipcae.vv_result/0.2")
        for key, value in metadata.items():
            self.assertEqual(exported.get(key), value)

    def test_result_checker_still_rejects_unversioned_contract_extensions(self):
        record = self.invoke()
        result = read_json(self.persisted(record) / "result.json")
        legacy = dict(result, schema="chipcae.vv_result/0.2", verdict="pass", reference=None)
        for fields, expected in (({"schema": "chipcae.vv_result/0.3"}, ["invalid schema"]),
                                 ({"verdict": "needs_attention"}, ["invalid verdict"]),
                                 ({"reference": {"value": None, "type": "无解析解（物理合理性检查）",
                                                 "source": "C/W/A acceptance checks"}}, ["invalid reference.type"])):
            with self.subTest(fields=fields):
                path = write_json(self.root / "invalid-result.json", dict(legacy, **fields))
                self.assertEqual(check_file(path), expected)

    def test_real_record_keeps_output_location_out_of_persisted_fields(self):
        record = self.invoke(params=dict(self.params, data_class="real"))
        self.assertEqual(record["exit_code"], 0)
        folder = self.persisted(record)
        self.assertNotIn("record_directory", record)
        self.assertNotIn(str(folder), read_json(folder / "run_record.json").values())
        self.assertNotIn('"record_directory"', (folder / "run_record.md").read_text())
        self.assertTrue(record["inputs"])
        self.assertTrue(all(not Path(name).is_absolute() and ".." not in Path(name).parts
                            for name in record["inputs"]))

    def test_assembly_temperature_runs_when_only_w_is_unready(self):
        self.pipeline.write_text(THERMAL_ONLY)
        make_run(self.root / "template", thermal_only=True)
        record = self.invoke("assembly", params=self.assembly_params())
        self.assertEqual(record["exit_code"], 77)
        self.assertTrue(record["readiness"]["ready"])
        self.assertTrue(any(item["item"] == "W" for item in record["needs_attention"]))
        self.assertTrue(self.calls("solve"))
        self.assertIs(record["checks"]["A2"], True)
        self.assertFalse({"W1", "W2", "W3", "W4", "W5"} & set(record["checks"]))
        self.persisted(record)

    def test_assembly_w_dependency_does_not_hide_solver_error_or_replace_a_readiness(self):
        self.pipeline.write_text(THERMAL_ONLY)
        os.environ["REALCASE_TEST_MODE"] = "rc14"
        record = self.invoke("assembly", params=self.assembly_params())
        self.assertEqual(record["exit_code"], 1)
        self.assertEqual(record["verdict"], "fail")
        self.assertTrue(record["readiness"]["ready"])
        self.assertTrue(any(item["item"] == "W" for item in record["needs_attention"]))
        self.assertIs(record["checks"]["C1"], False)
        self.persisted(record)

    def test_rc13_only_assembly_is_dependency_with_a2_null(self):
        os.environ["REALCASE_TEST_MODE"] = "rc13"
        for kind in ("thermal", "warpage", "assembly"):
            with self.subTest(kind=kind):
                params = self.assembly_params() if kind == "assembly" else self.params
                record = self.invoke(kind, params=params)
                self.assertEqual(record["exit_code"], 77 if kind == "assembly" else 1)
                if kind == "assembly":
                    self.assertIsNone(record["checks"]["A2"])
                    self.assertTrue(any(item["item"] == "A" for item in record["needs_attention"]))
                else:
                    self.assertIs(record["checks"]["C1"], False)
                self.persisted(record)

    def test_repeated_digest_mismatch_fails_c7_and_names_changed_component(self):
        os.environ["REALCASE_TEST_MODE"] = "repeat_qoi"
        record = self.invoke()
        self.assertEqual(record["exit_code"], 1)
        self.assertIs(record["checks"]["C7"], False)
        self.assertIn("T_max", " ".join(record["notes"]))
        self.assertEqual(len(self.calls("solve")), 2)

    def test_failure_in_any_primary_run_fails_even_when_digests_match(self):
        os.environ["REALCASE_TEST_MODE"] = "repeat_energy"
        record = self.invoke()
        self.assertEqual(record["exit_code"], 1)
        self.assertIs(record["checks"]["C4"], False)
        self.assertIs(record["checks"]["C7"], True)
        self.assertEqual(len(self.calls("solve")), 2)

    def test_parallel_mesh_partition_and_qoi_mismatch_each_fail_c8(self):
        for mode in ("parallel_mesh", "parallel_partitions", "parallel_qoi"):
            with self.subTest(mode=mode):
                os.environ["REALCASE_TEST_MODE"] = mode
                record = self.invoke(nps=[1, 2])
                self.assertIs(record["checks"]["C8"], False)
                self.assertEqual(record["exit_code"], 1)

    def test_absent_mpi_leaves_c8_null_without_failure(self):
        os.environ.pop("CHIPCAE_MPIEXEC", None)
        actual_which = shutil.which
        with patch("shutil.which", side_effect=lambda name, *a, **kw:
                   None if name in ("mpiexec", "mpirun") else actual_which(name, *a, **kw)):
            record = self.invoke(nps=[1, 2])
        self.assertEqual(record["exit_code"], 0, {k: record[k] for k in ("checks", "missing", "notes")})
        self.assertIsNone(record["checks"]["C8"])
        self.assertTrue(any("mpi" in note.lower() for note in record["notes"]))
        self.assertEqual(len(self.calls("solve")), 2)

    def test_success_pipeline_retains_sidecar_settings_logs_metrics_and_repeat_counts(self):
        self.params.update(loads=DEFAULT_LOADS, constraints={"rigid": "3-2-1"},
                           linear={"preconditioner": "jacobi", "rtol": 1e-10}, T_ref=300.)
        original = copy.deepcopy(self.params)
        record = self.invoke(nps=[1, 2])
        self.assertEqual(record["exit_code"], 0, {k: record[k] for k in ("checks", "missing", "notes")})
        self.assertEqual(record["verdict"], "pass")
        self.assertEqual(self.params, original)
        self.assertEqual(record["needs_attention"], [])
        self.assertEqual(record["missing"], [])
        self.assertIs(record["checks"]["C7"], True)
        self.assertIs(record["checks"]["C8"], True)
        folder = self.persisted(record)
        self.assertEqual(len(self.calls("import")), 1)
        self.assertEqual(len(self.calls("preview")), 1)
        self.assertEqual(len(self.calls("solve")), 3)
        names = [call["name"] for call in self.calls()]
        self.assertLess(names.index("fake-import"), names.index("fake-preview"))
        self.assertLess(names.index("fake-preview"), names.index("fake-solve"))
        cases = [json.loads(line) for line in (self.root / "cases.jsonl").read_text().splitlines()]
        self.assertEqual([row["profile"]["ranks"] for row in cases], [1, 1, 2])
        case = cases[0]["case"]
        self.assertEqual([p["solver"] for p in case["physics"]], ["fvm.conduction", "fvm.solid"])
        self.assertEqual(case["physics"][1]["temp_from"], "thermal")
        self.assertEqual(case["physics"][1]["T_ref"], 300.)
        self.assertEqual(case["loads"], DEFAULT_LOADS)
        self.assertEqual(case["solver"]["linear"], self.params["linear"])
        stack = read_json(folder / "inputs/stack.json")
        self.assertIn("polygons", stack["layers"][0]["copper"])
        self.assertNotIn("fraction", stack["layers"][0]["copper"])
        self.assertTrue(list((folder / "inputs").rglob("*.polyset")))
        self.assertEqual(record["dofs"]["thermal"], 2)
        self.assertEqual(record["dofs"]["mech"], 6)
        self.assertEqual(record["materials_provenance"], "illustrative")
        self.assertEqual(record["temperature"]["T_cell_min"], 320.)
        self.assertEqual(record["temperature"]["T_vertex_min"], 320.)
        readbacks = [c for c in record["commands"] if c["stage"].startswith("readback")]
        self.assertEqual(len(readbacks), 3)
        command_text = (folder / "cmd.txt").read_text()
        for command in record["commands"]:
            self.assertIsInstance(command["argv"], list)
            self.assertIn(shlex.join(command["argv"]), command_text)
            self.assertGreaterEqual(command["wall_s"], 0)
            self.assertGreater(command["peak_rss_kib"], 0)
        self.assertGreaterEqual(len(list((folder / "logs").glob("*.stdout.log"))), 6)
        self.assertGreaterEqual(len(list((folder / "logs").glob("*.stderr.log"))), 6)
        self.assertEqual(check_file(folder / "result.json"), [])

    def test_void_exported_cells_do_not_count_as_fvm_unknowns(self):
        with h5py.File(vtk_path(self.root / "template"), "r+") as output:
            output["VTKHDF/hex/CellData/cell_kind"][0] = 2
        record = self.invoke(repeat=1)
        self.assertEqual(record["exit_code"], 0, record["notes"])
        self.assertEqual(record["mesh"]["n_cells_hex"], 2)
        self.assertEqual(record["dofs"]["thermal"], 1)
        self.assertEqual(record["dofs"]["mech"], 3)

    def test_thermal_only_result_never_reports_zero_mechanical_qoi(self):
        self.pipeline.write_text(THERMAL_ONLY)
        make_run(self.root / "template", thermal_only=True)
        record = self.invoke()
        self.assertEqual(record["exit_code"], 0, {k: record[k] for k in ("checks", "missing", "notes")})
        folder = self.persisted(record)
        qoi = read_json(folder / "result.json")["qoi"]
        self.assertIn("T_max", qoi)
        self.assertNotIn("warpage_pv", qoi)
        self.assertNotIn("U_mag_max", qoi)

    def test_assembly_material_names_not_library_order_and_applicable_check_policy(self):
        params = self.assembly_params()
        record = self.invoke("assembly", params=params)
        self.assertEqual(record["exit_code"], 0, {k: record[k] for k in ("checks", "missing", "notes")})
        self.assertIs(record["checks"]["A2"], True)
        self.assertIs(record["checks"]["A3"], True)
        self.assertTrue({"W1", "W2", "W4"} <= set(record["checks"]))
        self.assertFalse({"W3", "W5"} & set(record["checks"]))
        folder = self.persisted(record)
        document = read_json(folder / "inputs/assembly.json")
        for ref in (document["board"]["path"], document["board"]["material_map"],
                    document["materials"], *(p["step"] for p in document["parts"])):
            self.assertTrue((folder / "inputs" / ref).is_file(), ref)
        imported = [c for c in self.calls("import") if "--assembly" in c["args"]]
        self.assertTrue(imported)
        self.assertTrue(all("--material-map" not in c["args"] for c in imported))
        import_document = Path(imported[-1]["args"][imported[-1]["args"].index("--assembly") + 1])
        import_library = read_json(import_document.parent / read_json(import_document)["materials"])
        for command in record["commands"]:
            argv = command["argv"]
            if "--case" in argv:
                case_path = Path(argv[argv.index("--case") + 1])
                case = read_json(case_path)
                self.assertEqual(read_json(case_path.parent / case["materials"]["ref"]), import_library)
        os.environ["REALCASE_TEST_MODE"] = "bad_material"
        bad = self.invoke("assembly", params=params)
        self.assertEqual(bad["exit_code"], 1)
        self.assertIs(bad["checks"]["A2"], False)

    def test_assembly_equal_material_content_is_accepted_and_approximation_is_illustrative(self):
        params = self.assembly_params()
        document_path = Path(params["input"])
        library = read_json(document_path.parent / "materials.json")
        params["materials"] = str(write_json(self.root / "same-materials.json", library))
        document = read_json(document_path)
        document["parts"][0]["material_approx"] = True
        write_json(document_path, document)
        record = self.invoke("assembly", params=params)
        self.assertNotEqual(record["exit_code"], 2)
        self.assertEqual(record["materials_provenance"], "illustrative")
        self.assertTrue(self.calls("import"))

    def test_parts_from_step_approximation_controls_provenance(self):
        params = self.assembly_params()
        document_path = Path(params["input"])
        library = read_json(document_path.parent / "materials.json")
        params["materials"] = str(write_json(self.root / "same-materials.json", library))
        record = self.invoke("assembly", params=params)
        self.assertEqual(record["materials_provenance"], "provided")
        document = read_json(document_path)
        document["parts"] = []
        document["parts_from_step"] = {"step": "lid.step", "frame": "step", "origin": [0, 0, 0],
            "bodies": {"lid": {"id": "lid", "material": "copper", "material_approx": True}}}
        write_json(document_path, document)
        record = self.invoke("assembly", params=params)
        self.assertNotEqual(record["exit_code"], 2)
        self.assertEqual(record["materials_provenance"], "illustrative")

    def test_import_error_is_fail_with_record(self):
        for mode in ("import7", "import9"):
            with self.subTest(mode=mode):
                os.environ["REALCASE_TEST_MODE"] = mode
                record = self.invoke()
                self.assertEqual(record["exit_code"], 1)
                self.assertIs(record["checks"]["C1"], False)
                self.persisted(record)

    def test_colliding_invocations_preserve_previous_artifacts(self):
        first = self.invoke()
        folder = self.persisted(first)
        marker = folder / "retain.txt"
        marker.write_text("first invocation")
        snapshot = (folder / "run_record.json").read_bytes()
        self.invoke()
        self.assertEqual(marker.read_text(), "first invocation")
        self.assertEqual((folder / "run_record.json").read_bytes(), snapshot)
        self.assertEqual(len(list(self.out.rglob("run_record.json"))), 2)

    def test_timeout_and_signal_record_metrics_and_synthetic_has_no_retry(self):
        for mode, detail in (("timeout", "timeout"), ("signal", f"signal:{signal.SIGTERM}")):
            with self.subTest(mode=mode):
                (self.root / "calls.jsonl").unlink(missing_ok=True)
                os.environ["REALCASE_TEST_MODE"] = mode
                record = self.invoke(params=dict(self.params, timeout_s=.2), repeat=1)
                self.assertEqual(record["exit_code"], 1)
                self.assertEqual(record["status_detail"], detail)
                self.assertEqual(record["attempts"], [])
                self.assertEqual(len(self.calls("solve")), 1)
                failed = [c for c in record["commands"] if c["stage"].startswith("solve")]
                self.assertTrue(failed)
                self.assertGreater(failed[0]["wall_s"], 0)
                self.assertGreater(failed[0]["peak_rss_kib"], 0)
                self.persisted(record)

    def test_real_warpage_explained_auxiliary_nulls_do_not_fail_record_completeness(self):
        record = self.invoke("warpage", params=dict(self.params, data_class="real"))
        self.assertEqual(record["exit_code"], 0, {k: record[k] for k in ("checks", "missing", "notes")})
        self.assertIs(record["checks"]["C9"], True)
        self.assertEqual(record["missing"], [])
        for name in ("W3", "W4", "W5"):
            self.assertIsNone(record["checks"][name])
            self.assertTrue(any(name in note for note in record["notes"]))
        self.assertEqual(len(self.calls("solve")), 2)
        folder = self.persisted(record)
        self.assertEqual(check_file(folder / "result.json"), ["real result must stay local (D03)"])

    def test_real_timeout_and_signal_retry_once_with_halved_mesh_and_still_fail(self):
        for mode in ("timeout", "signal"):
            with self.subTest(mode=mode):
                (self.root / "calls.jsonl").unlink(missing_ok=True)
                (self.root / "cases.jsonl").unlink(missing_ok=True)
                os.environ["REALCASE_TEST_MODE"] = mode
                params = dict(self.params, data_class="real", timeout_s=.2)
                record = self.invoke(params=params, repeat=1)
                self.assertEqual(record["exit_code"], 1)
                self.assertEqual(record["verdict"], "fail")
                self.assertEqual(len(self.calls("solve")), 2)
                self.assertEqual(len(record["attempts"]), 2)
                self.assertEqual([(a["mesh"]["nx"], a["mesh"]["ny"])
                                  for a in record["attempts"]], [(4, 4), (2, 2)])
                cases = [json.loads(line) for line in (self.root / "cases.jsonl").read_text().splitlines()]
                self.assertEqual([(r["case"]["mesh"]["nx"], r["case"]["mesh"]["ny"])
                                  for r in cases], [(4, 4), (2, 2)])
                self.assertNotEqual(cases[0]["out"], cases[1]["out"])
                self.persisted(record)


    def test_cli_usage_and_dependency_exits_propagate_with_records(self):
        params_path = write_json(self.root / "params.json", self.params)
        for usage in (True, False):
            with self.subTest(usage=usage):
                value = copy.deepcopy(self.params)
                if usage:
                    del value["case_id"]
                else:
                    self.pipeline.unlink(missing_ok=True)
                write_json(params_path, value)
                out = self.root / ("cli-usage" if usage else "cli-dependency")
                result = subprocess.run([sys.executable, str(SOURCE / "tools/vv/real_case_e2e.py"),
                                         "thermal", "--params", str(params_path), "--source-root",
                                         str(self.source), "--out-root", str(out)],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2 if usage else 77, result.stderr)
                paths = list(out.rglob("run_record.json"))
                self.assertEqual(len(paths), 1)
                self.assertEqual(read_json(paths[0])["exit_code"], result.returncode)
                self.assertEqual(self.calls(), [])

    def test_malformed_cli_allocates_exactly_one_usage_record(self):
        params_path = self.root / "params.json"
        variants = (("{", []), ("[]", []), (json.dumps(self.params), ["--unknown"]),
                    (json.dumps(self.params), ["--np", "invalid"]))
        for index, (content, extra) in enumerate(variants):
            with self.subTest(content=content, extra=extra):
                params_path.write_text(content)
                out = self.root / f"malformed-cli-{index}"
                result = subprocess.run([sys.executable, str(SOURCE / "tools/vv/real_case_e2e.py"),
                                         "thermal", "--params", str(params_path), "--source-root",
                                         str(self.source), "--out-root", str(out), *extra],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2, result.stderr)
                paths = list(out.rglob("run_record.json"))
                self.assertEqual(len(paths), 1)
                record = read_json(paths[0])
                self.assertEqual(record["exit_code"], 2)
                self.assertEqual(record["verdict"], "fail")
                self.assertTrue(any("CLI usage error" in note for note in record["notes"]))
                self.assertEqual(result.stdout.strip(), f"fail: {paths[0].parent}")
                self.assertEqual(list(out.glob("*/*/inputs")), [paths[0].parent / "inputs"])
                self.assertEqual(self.calls(), [])

    def test_missing_numeric_dependency_is_77_and_import_remains_lazy(self):
        params_path = write_json(self.root / "params.json", self.params)
        for missing in ("numpy", "h5py"):
            with self.subTest(missing=missing):
                script = ("import sys,importlib.abc,json; from pathlib import Path\n"
                          "class Block(importlib.abc.MetaPathFinder):\n"
                          " def find_spec(self, fullname, path=None, target=None):\n"
                          "  if fullname.split('.')[0] == sys.argv[1]: raise ImportError(fullname)\n"
                          "sys.meta_path.insert(0, Block())\n"
                          "from tools.vv.real_case_e2e import run_item\n"
                          "p=json.loads(Path(sys.argv[2]).read_text())\n"
                          "r=run_item('thermal',p,Path(sys.argv[3]),[1],2,Path(sys.argv[4]))\n"
                          "print(json.dumps(r))\n")
                out = self.root / ("missing-" + missing)
                result = subprocess.run([sys.executable, "-c", script, missing, str(params_path),
                                         str(out), str(self.source)], cwd=SOURCE,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                record = json.loads(result.stdout.splitlines()[-1])
                self.assertIsInstance(record, dict)
                self.assertEqual(record["exit_code"], 77)
                self.assertTrue(record["needs_attention"])
                self.assertEqual(self.calls(), [])
                self.assertTrue(list(out.rglob("run_record.json")))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
