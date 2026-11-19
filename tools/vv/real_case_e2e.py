#!/usr/bin/env python3
"""Local FVM acceptance driver. Only generated inputs belong in the repository.

Numerical packages and the shared readers are imported at the point of use so
an incomplete runtime can still write a dependency record.
"""
from __future__ import annotations

import argparse
import copy
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

SOURCE_ROOT = Path(__file__).resolve().parents[2]
QOI_KEYS = ("warpage_pv", "T_max", "U_mag_max")
THERMAL_ONLY = "not solved: fvm thermal-only stage"
WARPAGE_METHOD = "FVM top-face centroids; least-squares plane removal"
TEMPERATURE_ATOL = 1e-8
PARALLEL_RTOL = 1e-8
DEFAULT_LOADS = [
    {"type": "fixed_temperature", "target": "face:zmin", "T": 298.15},
    {"type": "convection", "target": "face:zmax", "h": 1000, "T_inf": 348.15},
]
ILLUSTRATIVE_NOTE = "示意材料参数（BT/TIM/胶），数值只证明链路，不作物理结论"
DOF_SOURCE = "derived: FVM, thermal 1 / mech 3 unknowns per control volume"


def _json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
                    encoding="utf-8")


def _shared(name):
    # Also support direct invocation outside the repository's current directory.
    if str(SOURCE_ROOT) not in sys.path:
        sys.path.insert(0, str(SOURCE_ROOT))
    import importlib
    return importlib.import_module("tools.vv." + name)


def _exe(value):
    if not value:
        return None
    found = shutil.which(str(value))
    return Path(found).resolve() if found else None


def readiness(kind: str, source_root: Path) -> tuple[bool, str]:
    if kind not in ("thermal", "warpage", "assembly"):
        return False, "unsupported kind"
    source_root = Path(source_root).resolve()
    pipeline = source_root / "apps/chipcae-solve/src/fvm_pipeline.cpp"
    if not pipeline.is_file():
        return False, "T: fvm-thermal-cutcell / fvm-case-e2e upstream missing fvm_pipeline.cpp"
    if kind == "warpage":
        elastic = source_root / "solvers/fvm/elastic/include/chipcae/solvers/fvm/elastic.hpp"
        if not elastic.is_file():
            return False, "W: fvm-thermoelastic-cutcell upstream missing elastic.hpp"
        try:
            if THERMAL_ONLY in pipeline.read_text(encoding="utf-8"):
                return False, "W: fvm-wire-elastic upstream still uses thermal-only stage"
        except (OSError, UnicodeError) as exc:
            return False, f"W: cannot read pipeline: {exc}"
    if kind == "assembly":
        exe = _exe(os.environ.get("CHIPCAE_IMPORT_EXE"))
        if exe is None:
            return False, "A: CHIPCAE_IMPORT_EXE missing or not executable"
        for parent in exe.parents:
            cap = parent / "import-assets/assembly-capabilities.json"
            if cap.exists():
                try:
                    if _json(cap).get("geometry") == "ON":
                        return True, "T pipeline and nearest import geometry capability available"
                    return False, "A: nearest import capability geometry is not ON (OCCT required)"
                except (OSError, ValueError, AttributeError) as exc:
                    return False, f"A: invalid nearest import capability: {exc}"
        return False, "A: no assembly-capabilities.json in resolved import executable ancestors"
    return True, "upstream available"


def classify_solve_rc(rc: int | str) -> str | None:
    return None if rc == 0 else "CUT_UNSUPPORTED" if rc == 13 else "error"


class _EventCursor:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, fmt):
        try:
            result = struct.unpack_from("<" + fmt, self.data, self.pos)
        except struct.error as exc:
            raise ValueError("truncated event payload") from exc
        self.pos += struct.calcsize("<" + fmt)
        return result[0] if len(result) == 1 else result

    def string(self):
        size = self.take("I")
        end = self.pos + size
        if end > len(self.data):
            raise ValueError("truncated event string")
        value = self.data[self.pos:end].decode("utf-8")
        self.pos = end
        return value


def decode_events(path: Path) -> list[dict]:
    """Read validated CAELOG1 records; torn/CRC-invalid tails preserve the prefix.

    ResidualBatch points are flattened in wire order. Other event kinds retain
    their position but their payload does not participate in the digest.
    """
    result = []
    with Path(path).open("rb") as stream:
        while True:
            header = stream.read(16)
            if len(header) < 8:
                break
            if header[:8] != b"CAELOG1\0":
                if result:
                    break  # Preserve the validated prefix; never scan past damage.
                raise ValueError("invalid event log magic")
            if len(header) < 16:
                break
            size, crc = struct.unpack_from("<II", header, 8)
            if size > 64 * 1024 * 1024:
                break
            body = stream.read(size)
            if len(body) != size or zlib.crc32(body) != crc:
                break
            cur = _EventCursor(body)
            seq, wall, kind = cur.take("QqB")
            meta = {"kind": kind, "seq": seq, "wall_time_unix_ns": wall}
            decoded = []
            if kind == 18:
                window, event_time, count = cur.take("QdI")
                if count > (len(body) - cur.pos) // 28:
                    raise ValueError("truncated residual batch")
                for _ in range(count):
                    solver, field = cur.string(), cur.string()
                    it, residual, flags, linear, windows = cur.take("IdIII")
                    decoded.append(dict(meta, solver=solver, field=field, iterations=it,
                                        residual=residual, flags=flags, linear_iterations=linear,
                                        window_iterations=windows, window=window, time=event_time))
                dropped = cur.take("I")
                for point in decoded:
                    point["dropped_intermediate"] = dropped
            elif kind == 20:
                level, rank = cur.take("ii")
                message, code = cur.string(), cur.string()
                decoded.append(dict(meta, level=level, rank=rank, message=message,
                                    code=code, error_code=cur.take("I")))
            else:
                event = dict(meta)
                if kind == 15:
                    event["run_id"], event["attempt"] = cur.string(), cur.take("I")
                    event["protocols"] = [cur.string() for _ in range(cur.take("I"))]
                    event["kernel_version"], event["schema_bundle_hash"] = cur.string(), cur.string()
                    event["plugins"] = [dict(key=cur.string(), version=cur.string()) for _ in range(cur.take("I"))]
                    event["ranks"], event["event_log_format"] = cur.take("II")
                    event["vtkhdf_version"] = [cur.take("I") for _ in range(cur.take("I"))]
                    event["case_sha256"], event["git_sha"], event["protocol_version"] = cur.string(), cur.string(), cur.string()
                    event["features"] = [cur.string() for _ in range(cur.take("I"))]
                elif kind == 10:
                    event["window"], event["time"], event["dt"], event["fraction"] = cur.take("Qddd")
                    event["message"] = cur.string()
                elif kind == 11:
                    event["solver"], event["field"] = cur.string(), cur.string()
                    event["window"], event["iterations"], event["residual"], event["flags"] = cur.take("QIdI")
                    event["code"], event["error_code"] = cur.string(), cur.take("I")
                elif kind == 17:
                    event["cmd_seq"], event["client_request_id"] = cur.take("Q"), cur.string()
                    event["status"], event["effective_step"] = cur.take("IQ")
                    event["message"], event["effective_time"] = cur.string(), cur.take("d")
                elif kind == 19:
                    event["exit_code"], event["last_completed_window"] = cur.take("iQ")
                    event["reason"] = cur.string()
                elif kind == 21:
                    event["state"], event["window"], event["time"] = cur.take("BQd")
                elif kind == 22:
                    event["window"], event["time"] = cur.take("Qd")
                    event["path"], event["checksum"] = cur.string(), cur.string()
                else:
                    raise ValueError(f"unknown event payload kind {kind}")
                decoded.append(event)
            if cur.pos != len(body):
                raise ValueError("event record has trailing data")
            result.extend(decoded)
    return result


def _manifest(run_dir):
    return _json(Path(run_dir) / "manifest.json")


def _vtk_path(run_dir, manifest=None):
    manifest = _manifest(run_dir) if manifest is None else manifest
    return Path(run_dir) / manifest["steps"][0]["file"]


def _summary(run_dir):
    entries = _manifest(run_dir)["summary"]
    result = {}
    for item in entries:
        if item["name"] in result:
            raise ValueError("duplicate summary " + item["name"])
        result[item["name"]] = item
    return result


def _datasets(root, prefix="VTKHDF"):
    """Digest geometry and field arrays, excluding Assembly and reserved fields."""
    import h5py
    for block_name in sorted(root):
        if block_name == "Assembly" or not isinstance(root[block_name], h5py.Group):
            continue
        block = root[block_name]
        for field in sorted(("Points", "Connectivity", "Offsets", "Types")):
            if field in block:
                yield f"{prefix}/{block_name}/{field}", block[field]
        for group in ("CellData", "PointData"):
            if group in block:
                for field in sorted(block[group]):
                    if field != "reserved" and isinstance(block[group][field], h5py.Dataset):
                        yield f"{prefix}/{block_name}/{group}/{field}", block[group][field]


def run_digest(run_dir: Path, events: list[dict]) -> dict:
    import h5py
    import numpy as np
    manifest, summary = _manifest(run_dir), _summary(run_dir)
    fields = {}
    reserved = {item["name"] for item in manifest.get("fields", []) if item.get("reserved")}
    with h5py.File(_vtk_path(run_dir, manifest), "r") as output:
        for path, dataset in _datasets(output["VTKHDF"]):
            if path.split("/")[-1] in reserved:
                continue
            values = dataset[...]
            if values.dtype.hasobject:
                raise ValueError("digest arrays must have a value dtype: " + path)
            values = np.ascontiguousarray(values, dtype=values.dtype.newbyteorder("<"))
            fields[path] = hashlib.sha256(values.tobytes(order="C")).hexdigest()
    iterations, residual = {}, {}
    for event in events:
        if "field" in event and "iterations" in event and "residual" in event:
            iterations[event["field"]] = event["iterations"]
            residual[event["field"]] = float(event["residual"]).hex()
    return dict(mesh_sha256=manifest.get("mesh", {}).get("mesh_sha256"),
                summary={key: float(summary[key]["value"]).hex() for key in QOI_KEYS if key in summary},
                fields=dict(sorted(fields.items())), iterations=iterations, residual=residual)


def digest_sha256(d: dict) -> str:
    return hashlib.sha256(json.dumps(d, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


# Reuse the shared implementation when V-INT-10 is present; absence is normal.
if (SOURCE_ROOT / "tools/vv/nightly/determinism.py").is_file():
    _determinism = _shared("nightly.determinism")
    run_digest = _determinism.run_digest
    digest_sha256 = _determinism.digest_sha256


def leak_check(diff_text: str, patterns: list[str]) -> list[str]:
    folded = diff_text.casefold()
    return [pattern for pattern in patterns if pattern.casefold() in folded]


def _volume_blocks(output):
    for name in ("hex", "cut"):
        yield output["VTKHDF/" + name]


def _check_c2(run_dir: Path, events: list[dict]) -> bool:
    import h5py
    reader = _shared("result_summary")
    try:
        if _manifest(run_dir).get("schema_version") != "chipcae.results/1.0":
            return False
        summary = _summary(run_dir)
        if not set(QOI_KEYS) <= summary.keys() or summary["T_max"].get("method") != "FVM cell centers":
            return False
        mechanical = [summary[k] for k in ("warpage_pv", "U_mag_max")]
        if any(item.get("method") == THERMAL_ONLY for item in mechanical):
            if not all(item.get("method") == THERMAL_ONLY and item["value"] == 0 for item in mechanical):
                return False
            if not any(e.get("level") == 2 and e.get("message") == THERMAL_ONLY for e in events):
                return False
        elif (mechanical[0].get("method") != WARPAGE_METHOD
              or mechanical[1].get("method") != "FVM cell centers"):
            return False
        reader.summarize(Path(run_dir))
        with h5py.File(_vtk_path(run_dir), "r") as output:
            for block in _volume_blocks(output):
                if not all(path in block for path in (
                    "PointData/T", "PointData/U", "CellData/layer_id", "CellData/material_id",
                    "CellData/cell_kind", "CellData/elem_gid", "CellData/volume")):
                    return False
        return True
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, reader.ResultSummaryError):
        return False


def _check_c3(events: list[dict]) -> bool:
    solvers = [e["solver"] for e in events if "solver" in e and e.get("kind", 18) == 18]
    return "fvm.conduction" in solvers and not any(s.startswith("fem.") for s in solvers)


def _energy(events):
    lines = [e for e in events if e.get("message", "").startswith("FVM_ENERGY ")]
    if len(lines) != 1 or lines[0].get("level") != 1:
        return None
    try:
        value = json.loads(lines[0]["message"][len("FVM_ENERGY "):])
        return value if isinstance(value, dict) else None
    except ValueError:
        return None


def _finite(value):
    if isinstance(value, dict):
        return all(_finite(item) for item in value.values())
    if isinstance(value, (list, tuple)):
        return all(_finite(item) for item in value)
    return type(value) is int or (type(value) is float and math.isfinite(value))


def _check_c4(events: list[dict]) -> bool:
    energy = _energy(events)
    return bool(energy is not None
                and {"heat_in", "heat_out", "imbalance", "iterations"} <= energy.keys()
                and _finite(energy) and abs(energy["imbalance"]) <= 1e-9)


def _check_c5(run_dir: Path, energy: dict) -> bool:
    import h5py
    import numpy as np
    try:
        if not _finite(energy) or not all(_finite(q["value"]) for q in _summary(run_dir).values()):
            return False
        with h5py.File(_vtk_path(run_dir), "r") as output:
            for block in _volume_blocks(output):
                for field in ("PointData/T", "PointData/U"):
                    if field in block and not np.isfinite(block[field][...]).all():
                        return False
                for dataset in block["CellData"].values():
                    if not np.isfinite(dataset[...]).all():
                        return False
        return True
    except (OSError, ValueError, KeyError, TypeError):
        return False


def _check_c6(run_dir: Path, loads: list[dict]) -> bool | None:
    import h5py
    import numpy as np
    # Only the default no-source maximum-principle bound is specified.
    if loads != DEFAULT_LOADS:
        return None
    try:
        lower = []
        with h5py.File(_vtk_path(run_dir), "r") as output:
            for block in _volume_blocks(output):
                if "CellData/T_cell" in block and block["CellData/T_cell"].size:
                    lower.append(float(np.min(block["CellData/T_cell"][...])))
        return bool(all(t >= 298.15 - TEMPERATURE_ATOL for t in lower)
                    and _summary(run_dir)["T_max"]["value"] <= 348.15 + TEMPERATURE_ATOL)
    except (OSError, ValueError, KeyError, TypeError):
        return False


def _check_c10(result_path: Path, data_class: str) -> bool:
    expected = ["real result must stay local (D03)"] if data_class == "real" else []
    return _shared("check_result_data_class").check_file(Path(result_path)) == expected


def _check_w1(run_dir: Path, events: list[dict]) -> bool:
    try:
        summary = _summary(run_dir)
        return (any(e.get("solver") == "fvm.solid" and e.get("kind", 18) == 18 for e in events)
                and summary["warpage_pv"].get("method") == WARPAGE_METHOD
                and summary["U_mag_max"].get("method") == "FVM cell centers")
    except (OSError, ValueError, KeyError, TypeError):
        return False


def _check_w2(run_dir: Path) -> bool:
    import h5py
    import numpy as np
    try:
        summary = _summary(run_dir)
        if not all(_finite(summary[k]["value"]) and summary[k]["value"] >= 0
                   for k in ("warpage_pv", "U_mag_max")):
            return False
        with h5py.File(_vtk_path(run_dir), "r") as output:
            return all(bool(np.isfinite(b["PointData/U"][...]).all())
                       for b in _volume_blocks(output))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def _points_u(run_dir):
    import h5py
    import numpy as np
    with h5py.File(_vtk_path(run_dir), "r") as output:
        blocks = list(_volume_blocks(output))
        return (np.concatenate([b["Points"][...] for b in blocks]),
                np.concatenate([b["PointData/U"][...] for b in blocks]))


def _check_w3(run_dir: Path, constraints: dict) -> bool | None:
    import numpy as np
    if constraints.get("rigid") == "3-2-1":
        return None
    if {"target": "face:zmin"} not in constraints.get("fixed", []):
        return None
    try:
        points, u = _points_u(run_dir)
        return bool(len(points) and np.all(u[points[:, 2] == points[:, 2].min()] == 0))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def _bodies(stack):
    return sorted({body for layer in stack["layers"] for body in
                   [layer.get("body"), *(r.get("body") for r in layer.get("regions", []))] if body})


def _check_a1(import_report_path: Path, stack_path: Path, import_rc: int,
              expected_body_ids: list[str], accept_b_tier: bool) -> bool:
    try:
        report, stack = _json(import_report_path), _json(stack_path)
        count = report.get("welded_interfaces")
        defects = report["defects"]
        return bool(import_rc == 0 and type(count) is int and count >= 0
                    and set(expected_body_ids) <= set(_bodies(stack))
                    and all(d.get("tier") != "C" and (d.get("tier") != "B" or accept_b_tier)
                            for d in defects))
    except (OSError, ValueError, KeyError, TypeError):
        return False


class _UsageError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise _UsageError(message)


def _numeric(value, minimum=None, *, integer=False):
    return ((type(value) is int if integer else type(value) in (int, float))
            and (type(value) is int or math.isfinite(value))
            and (minimum is None or value >= minimum))


def _finite_parameters(value, path="params"):
    if isinstance(value, dict):
        for key, item in value.items():
            _finite_parameters(item, f"{path}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            _finite_parameters(item, f"{path}[{index}]")
    elif type(value) is float:
        _require(math.isfinite(value), f"{path} must be finite")


def _input_path(value, name, base=None):
    _require(isinstance(value, str) and bool(value), f"{name} must be a nonempty path string")
    path = ((Path.cwd() if base is None else base) / value).resolve()
    _require(path.is_file() or (name.endswith("input") and path.is_dir()), f"{name} does not exist: {path}")
    return path


def _material_content(library):
    _require(isinstance(library, dict) and isinstance(library.get("materials"), list),
             "materials must be a material library")
    result = {}
    for material in library["materials"]:
        _require(isinstance(material, dict) and isinstance(material.get("id"), str),
                 "invalid material entry")
        ident = material["id"]
        _require(ident and "." not in ident and ident not in result, "invalid or duplicate material id")
        result[ident] = material
    _require(bool(result), "material library is empty")
    _finite_parameters(library, "materials")
    return result


def _assembly_parts(document):
    parts = list(document.get("parts", []))
    for body, part in document.get("parts_from_step", {}).get("bodies", {}).items():
        parts.append(dict(part, id=part.get("id", body)))
    return parts


def _assembly_refs(document):
    yield document["board"], "path"
    yield document["board"], "material_map"
    yield document, "materials"
    for part in document.get("parts", []):
        yield part, "step"
    if "parts_from_step" in document:
        yield document["parts_from_step"], "step"


def _in_worktree(path):
    path = Path(path).resolve()
    probe = path if path.is_dir() else path.parent
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    result = subprocess.run(["git", "-C", str(probe), "rev-parse", "--is-inside-work-tree"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return result.returncode == 0 and result.stdout.strip() == "true"


def _validate(kind, params, out_root, nps, repeat):
    _require(kind in ("thermal", "warpage", "assembly"), "unsupported kind")
    _require(isinstance(params, dict), "params must be an object")
    for field in ("case_id", "data_class", "input", "material_map", "materials", "mesh"):
        _require(field in params, f"missing required params.{field}")
    _require(isinstance(params["case_id"], str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", params["case_id"]),
             "case_id must be an identifier")
    _require(params["data_class"] in ("synthetic", "real"), "data_class must be synthetic or real")
    _require(isinstance(nps, list) and nps and all(type(n) is int and n in (1, 2) for n in nps)
             and len(set(nps)) == len(nps) and 1 in nps, "np must include 1 and optionally 2, without duplicates")
    _require(_numeric(repeat, 1, integer=True), "repeat must be a positive integer")
    _require(_numeric(params.get("timeout_s", 3600)) and params.get("timeout_s", 3600) > 0,
             "timeout_s must be finite and positive")
    _require(_numeric(params.get("T_ref", 298.15)), "T_ref must be a finite number")
    _require(type(params.get("accept_b_tier", False)) is bool, "accept_b_tier must be boolean")
    mesh = params["mesh"]
    _require(isinstance(mesh, dict), "mesh must be an object")
    for key, minimum in (("nx", 1), ("ny", 1), ("levels", 0), ("min_elems_per_layer", 1)):
        _require(_numeric(mesh.get(key), minimum, integer=True), f"mesh.{key} must be an integer >= {minimum}")
    _require(_numeric(mesh.get("max_ratio"), 1), "mesh.max_ratio must be finite and >= 1")
    if "cell_budget" in mesh:
        _require(_numeric(mesh["cell_budget"], 1, integer=True), "mesh.cell_budget must be a positive integer")
    _require(isinstance(params.get("loads", DEFAULT_LOADS), list)
             and all(isinstance(item, dict) for item in params.get("loads", DEFAULT_LOADS)),
             "loads must be a list of objects")
    for index, load in enumerate(params.get("loads", DEFAULT_LOADS)):
        _require(isinstance(load.get("type"), str) and isinstance(load.get("target"), str),
                 f"loads[{index}] requires type and target strings")
        for key in ("T", "T_inf", "h", "q_vol", "q", "T_ref", "T_target"):
            if key in load:
                _require(_numeric(load[key]), f"loads[{index}].{key} must be a finite number")
    _require(isinstance(params.get("constraints", {}), dict), "constraints must be an object")
    constraints = params.get("constraints", {"rigid": "3-2-1"})
    _require(not ("rigid" in constraints and "fixed" in constraints), "constraints cannot mix rigid and fixed")
    if "rigid" in constraints:
        _require(constraints["rigid"] == "3-2-1", "unsupported rigid constraint")
    if "fixed" in constraints:
        _require(isinstance(constraints["fixed"], list) and all(isinstance(f, dict)
                 and isinstance(f.get("target"), str) for f in constraints["fixed"]), "invalid fixed constraints")
    linear = params.get("linear", {"preconditioner": "jacobi", "rtol": 1e-12})
    _require(isinstance(linear, dict), "linear must be an object")
    _require(linear.get("preconditioner", "jacobi") in ("jacobi", "gamg"),
             "FVM preconditioner must be jacobi or gamg")
    if "rtol" in linear:
        _require(_numeric(linear["rtol"]) and linear["rtol"] > 0, "linear.rtol must be positive")
    if "max_iter" in linear:
        _require(_numeric(linear["max_iter"], 1, integer=True), "linear.max_iter must be positive integer")
    _finite_parameters(params)
    resolved = dict(input=_input_path(params["input"], "input"))
    if params["data_class"] == "real":
        _require(not _in_worktree(out_root), "real output must be outside every Git worktree")
    if params["materials"] is not None:
        resolved["materials"] = _input_path(params["materials"], "materials")
        resolved["library"] = _json(resolved["materials"])
        _material_content(resolved["library"])
    if kind != "assembly":
        resolved["map"] = _input_path(params["material_map"], "material_map")
        _require(isinstance(_json(resolved["map"]), dict), "material_map must be an object")
    else:
        _require(params["material_map"] is None or isinstance(params["material_map"], str),
                 "assembly material_map must be null or a path string")
        document = _json(resolved["input"])
        _require(isinstance(document, dict) and isinstance(document.get("board"), dict), "invalid assembly document")
        _require(isinstance(document.get("parts", []), list), "assembly parts must be a list")
        _require(isinstance(document.get("parts_from_step", {}), dict), "invalid parts_from_step")
        _finite_parameters(document, "assembly")
        for obj, field in _assembly_refs(document):
            _input_path(obj.get(field), "assembly." + field, resolved["input"].parent)
        library_path = _input_path(document["materials"], "assembly.materials", resolved["input"].parent)
        library = _json(library_path)
        content = _material_content(library)
        if params["materials"] is not None:
            _require(_material_content(resolved["library"]) == content,
                     "params.materials differs from assembly material content")
        parts = _assembly_parts(document)
        _require(all(isinstance(p.get("id"), str) and p.get("material") in content for p in parts),
                 "assembly part id or material is invalid")
        _require(len({p["id"] for p in parts}) == len(parts), "duplicate assembly part id")
        resolved.update(document=document, library=library, materials=library_path)
    return resolved


def _illustrative_materials():
    materials = []
    for name, young, nu, alpha, k in (("cu", 110e9, .34, 17e-6, 390),
                                     ("fr4_eq", 25e9, .3, 1e-5, .3)):
        def table(columns):
            return dict(temperatures=[298.15], columns={key: [value] for key, value in columns.items()},
                        interpolation="constant", extrapolation="clamp")
        materials.append(dict(
            id=name, schema_version="1", unit_system="SI", model="thermo_elastic",
            density=2000, conductivity=k,
            elastic=dict(symmetry="isotropic", table=table(dict(E=young, nu=nu))),
            thermal_expansion=dict(symmetry="isotropic", cte_type="tangent", stress_free_temperature=298.15,
                                   table=table(dict(alpha=alpha))),
            provenance=dict(source_type="illustrative", confidence="illustrative")))
    return dict(schema="chipcae.material_library/1", materials=materials)


def _copy_input(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        shutil.copytree(source, target)
    else:
        shutil.copyfile(source, target)


def _copy_inputs(inputs, kind, resolved):
    if kind == "assembly":
        document = copy.deepcopy(resolved["document"])
        refs = list(_assembly_refs(document))
        sources = [(resolved["input"].parent / obj[key]).resolve() for obj, key in refs]
        # Keep the dependency tree's relative geometry, including parent refs,
        # under inputs/dependencies; only the root document is relocated.
        common = Path(os.path.commonpath([str(p.parent) for p in sources]))
        for (obj, key), source in zip(refs, sources):
            dest = inputs / "dependencies" / source.relative_to(common)
            if not dest.exists():
                _copy_input(source, dest)
            obj[key] = dest.relative_to(inputs).as_posix()
        _write_json(inputs / "assembly.json", document)
        # Import and solve reference the same parsed assembly material library.
        _write_json(inputs / "materials.json", resolved["library"])
    else:
        source = resolved["input"]
        _copy_input(source, inputs / ("board" + ("".join(source.suffixes) if source.is_file() else "")))
        _copy_input(resolved["map"], inputs / "material_map.json")
        _write_json(inputs / "materials.json", resolved.get("library", _illustrative_materials()))


def _case(params):
    return dict(schema="chipcae.case/1.0", stack=dict(ref="stack.json"), materials=dict(ref="materials.json"),
                mesh=dict(type="aligned_cartesian_cut", **copy.deepcopy(params["mesh"])),
                physics=[dict(id="thermal", solver="fvm.conduction", mode="steady"),
                         dict(id="mech", solver="fvm.solid", temp_from="thermal", T_ref=params.get("T_ref", 298.15))],
                loads=copy.deepcopy(params.get("loads", DEFAULT_LOADS)),
                constraints=copy.deepcopy(params.get("constraints", {"rigid": "3-2-1"})),
                solver=dict(linear=copy.deepcopy(params.get("linear", {"preconditioner": "jacobi", "rtol": 1e-12}))),
                output=dict(format="vtkhdf"))


def _measured(argv, stage, folder, record, timeout_s):
    """One child, redirected logs, per-child wait4 metrics and a group watchdog."""
    argv = [str(arg) for arg in argv]
    logs = folder / "logs"
    logs.mkdir(exist_ok=True)
    stdout_path, stderr_path = (logs / f"{stage}.{suffix}.log" for suffix in ("stdout", "stderr"))
    entry = dict(stage=stage, argv=argv, command=shlex.join(argv), exit_code=None,
                 wall_s=0., peak_rss_kib=None, status_detail=None)
    record["commands"].append(entry)
    record["logs"].extend(str(p.relative_to(folder)) for p in (stdout_path, stderr_path))
    with (folder / "cmd.txt").open("a", encoding="utf-8") as commands:
        commands.write(entry["command"] + "\n")
    done, timed_out = threading.Event(), threading.Event()
    start = time.monotonic()
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(argv, shell=False, cwd=folder, stdout=stdout, stderr=stderr,
                                   start_new_session=True)
        def watchdog():
            if not done.wait(timeout_s):
                timed_out.set()
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        watcher = threading.Thread(target=watchdog, daemon=True)
        watcher.start()
        try:
            _, status, usage = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
        finally:
            done.set()
            watcher.join()
        entry.update(exit_code="timeout" if timed_out.is_set() else process.returncode,
                     wall_s=time.monotonic() - start, peak_rss_kib=usage.ru_maxrss)
        if timed_out.is_set():
            entry["status_detail"] = "timeout"
        elif process.returncode < 0:
            entry["status_detail"] = f"signal:{-process.returncode}"
    return entry


def _sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def _digest_diff(first, second, prefix=""):
    result = []
    for key in sorted(first.keys() | second.keys()):
        name = f"{prefix}.{key}" if prefix else key
        if key not in first or key not in second:
            result.append(name)
        elif isinstance(first[key], dict) and isinstance(second[key], dict):
            result.extend(_digest_diff(first[key], second[key], name))
        elif first[key] != second[key]:
            result.append(name)
    return result


_REQUIRED = ("schema", "kind", "case_id", "data_class", "materials_provenance", "commit", "date",
             "host", "readiness", "needs_attention", "status_detail", "attempts", "exit_code", "inputs",
             "commands", "logs", "mesh", "dofs", "solver", "energy", "summary", "temperature",
             "displacement", "assembly", "digests", "file_sha256", "checks", "verdict", "notes", "missing")
_SECTION_KEYS = {
    "host": ("nproc", "mem_total_kib"), "readiness": ("ready", "reason"),
    "mesh": ("preview_cell_count", "n_cells_hex", "n_cells_cut", "nx0", "ny0", "L", "nz", "padded_cells", "mesh_sha256"),
    "dofs": ("thermal", "mech", "source"), "energy": ("heat_in", "heat_out", "imbalance"),
    "summary": (*QOI_KEYS, "methods"),
    "temperature": ("T_vertex_min", "T_vertex_max", "T_cell_min", "T_cell_max"),
    "displacement": ("U_fixed_face_max_abs", "U_mean", "warpage_pv_vertex_recompute"),
    "assembly": ("welded_interfaces", "bodies", "split_layers", "b_tier_defects"),
    "file_sha256": ("manifest", "vtkhdf"),
}


def _check_names(kind, w_ready):
    names = [f"C{i}" for i in range(1, 11)]
    if kind == "warpage":
        names.extend(f"W{i}" for i in range(1, 6))
    if kind == "assembly":
        names.extend(("A1", "A2", "A3"))
        if w_ready:
            names.extend(("W1", "W2", "W4"))
    return names


def _missing(record, *, pending_checks=()):
    """List unavailable applicable data, independently of caller-supplied missing[]."""
    missing = []
    notes = " ".join(record.get("notes", []))
    inactive = {"status_detail"}
    if record.get("kind") != "assembly":
        inactive.add("assembly")
    if record.get("kind") == "thermal" or "W1" not in record.get("checks", {}):
        inactive.add("displacement")
    for key in _REQUIRED:
        if key in inactive:
            continue
        if key not in record or record[key] is None:
            missing.append(key)
    def walk(value, path):
        if isinstance(value, dict):
            for key, item in value.items():
                name = f"{path}.{key}"
                if item is None:
                    if path.startswith("commands[") and key == "status_detail":
                        continue
                    if name.startswith("checks."):
                        check = key
                        if check in pending_checks:
                            continue
                        if check == "C8" and "mpi" in notes.lower():
                            continue
                        if check in ("C6", "W3", "W5") and check in notes:
                            continue
                        if check == "W4" and record.get("data_class") == "real" and "W4" in notes:
                            continue
                    if name in ("temperature.T_cell_min", "temperature.T_cell_max") and "T_cell" in notes:
                        continue
                    if name == "displacement.U_fixed_face_max_abs" and "Rigid321" in notes:
                        continue
                    if name == "displacement.warpage_pv_vertex_recompute" and "vertex diagnostic unavailable" in notes:
                        continue
                    missing.append(name)
                else:
                    walk(item, name)
        elif isinstance(value, list):
            for index, item in enumerate(value):
                walk(item, f"{path}[{index}]")
    for section, keys in _SECTION_KEYS.items():
        if section in inactive or not isinstance(record.get(section), dict):
            continue
        for key in keys:
            if key not in record[section]:
                missing.append(f"{section}.{key}")
    for key in ("host", "readiness", "mesh", "dofs", "solver", "energy", "summary", "temperature",
                "displacement", "assembly", "file_sha256", "commands", "checks"):
        if key not in inactive and record.get(key) is not None:
            walk(record[key], key)
    return sorted(set(missing))


def _check_c9(record: dict) -> bool | None:
    if record.get("needs_attention") and record.get("checks", {}).get("C1") is None:
        return None
    return not record.get("missing") and not _missing(record, pending_checks=("C9",))


def _host():
    memory = None
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal:"):
                memory = int(line.split()[1])
    except (OSError, ValueError):
        pass
    return dict(nproc=os.cpu_count(), mem_total_kib=memory)


def _commit(source_root):
    result = subprocess.run(["git", "-C", str(source_root), "rev-parse", "HEAD"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def _new_folder(out_root, commit, kind):
    stamp = datetime.now().strftime("%Y%m%d") + "_" + commit[:7]
    for index in range(100000):
        suffix = "" if index == 0 else "_" + datetime.now().strftime("%H%M%S") + (f"_{index}" if index > 1 else "")
        parent = out_root / (stamp + suffix)
        if parent.is_symlink():
            continue
        candidate = parent / kind
        try:
            candidate.mkdir(parents=True, exist_ok=False)
            (candidate / "inputs").mkdir()
            return candidate
        except FileExistsError:
            continue
    raise OSError("cannot allocate unused run directory")


def _blank_record(kind, params, source_root, w_ready):
    params = params if isinstance(params, dict) else {}
    record = dict.fromkeys(_REQUIRED)
    record.update(schema="chipcae.vv_real_case_run/0.1", kind=kind, case_id=params.get("case_id"),
                  data_class=params.get("data_class"), materials_provenance=None,
                  commit=_commit(source_root), date=datetime.now().strftime("%Y%m%d"), host=_host(),
                  readiness=dict(ready=False, reason="not evaluated: input validation pending"),
                  needs_attention=[], status_detail=None, attempts=[], exit_code=2, inputs={}, commands=[], logs=[],
                  mesh={key: None for key in _SECTION_KEYS["mesh"]},
                  dofs=dict(thermal=None, mech=None, source=DOF_SOURCE), solver=[],
                  energy={key: None for key in _SECTION_KEYS["energy"]},
                  summary={key: None for key in QOI_KEYS} | {"methods": {}},
                  temperature={key: None for key in _SECTION_KEYS["temperature"]},
                  digests={}, file_sha256=dict(manifest=None, vtkhdf=None),
                  checks=dict.fromkeys(_check_names(kind, w_ready)), missing=[], verdict="fail", notes=[])
    if record["commit"] == "unknown":
        record["notes"].append("source commit unavailable; commit is unknown")
    return record


def _add_dependency(record, item, reason):
    entry = dict(item=item, reason=reason)
    if entry not in record["needs_attention"]:
        record["needs_attention"].append(entry)


def _merge_check(record, key, value):
    previous = record["checks"].get(key)
    record["checks"][key] = False if previous is False or value is False else value if previous is None else previous


def _stage_error(record, entry, folder):
    record["checks"]["C1"] = False
    record["status_detail"] = record["status_detail"] or entry.get("status_detail") or "error"
    stderr = (folder / "logs" / f"{entry['stage']}.stderr.log").read_text(encoding="utf-8", errors="replace")
    record["notes"].append(f"{entry['stage']} exit {entry['exit_code']}: " + "\n".join(stderr.splitlines()[-20:]))


def _material_membership(run_dir, expected):
    import h5py
    import numpy as np
    present = set()
    with h5py.File(_vtk_path(run_dir), "r") as output:
        for block in _volume_blocks(output):
            ids = block["CellData/material_id"][...]
            if not ids.size:
                continue
            names = block["FieldData/material_names"].asstr()[...].tolist()
            # AttrTable and ResultsWriter IDs are one-based, independent of
            # the caller's material-library order.
            if not np.all((ids >= 1) & (ids <= len(names))):
                return False
            present.update(names[int(ident) - 1] for ident in ids)
    return set(expected) <= present


def _expected_materials(stack, parts):
    # Board materials and requested part materials must both enter the mesh.
    materials = {p["material"] for p in parts}
    for layer in stack["layers"]:
        if layer.get("body", "board") == "board":
            materials.add(layer["material"])
    return materials


def _displacement(run_dir, constraints):
    import h5py
    import numpy as np
    points, u = _points_u(run_dir)
    if not len(points):
        return dict.fromkeys(_SECTION_KEYS["displacement"])
    fixed = _check_w3(run_dir, constraints)
    fixed_abs = float(np.max(np.abs(u[points[:, 2] == points[:, 2].min()]))) if fixed is not None else None
    # Cell-volume quadrature distributes each exported cell's volume uniformly
    # over its vertices. Partition-local duplicates keep their local weights.
    weights, base = np.zeros(len(points)), 0
    with h5py.File(_vtk_path(run_dir), "r") as output:
        for block in _volume_blocks(output):
            offset_base, conn_base, point_base = 0, 0, 0
            conn, offsets = block["Connectivity"][...], block["Offsets"][...]
            volumes = block["CellData/volume"][...]
            cell = 0
            for nc, np_ in zip(block["NumberOfCells"][...], block["NumberOfPoints"][...]):
                local_offsets = offsets[offset_base:offset_base + int(nc) + 1]
                for i in range(int(nc)):
                    vertices = np.unique(conn[conn_base + local_offsets[i]:conn_base + local_offsets[i + 1]])
                    if vertices.size:
                        np.add.at(weights, base + point_base + vertices, volumes[cell] / vertices.size)
                    cell += 1
                conn_base += int(local_offsets[-1])
                offset_base += int(nc) + 1
                point_base += int(np_)
            base += block["Points"].shape[0]
    mean = np.average(u, axis=0, weights=weights).tolist() if weights.sum() > 0 else None
    top = np.flatnonzero(points[:, 2] == points[:, 2].max())
    _, first = np.unique(points[top], axis=0, return_index=True)
    top = top[first]
    pv = None
    if len(top) >= 3:
        _, residual = _shared("warpage_qoi").best_fit_plane(points[top, 0], points[top, 1], u[top, 2])
        pv = float(np.ptp(residual))
    return dict(U_fixed_face_max_abs=fixed_abs, U_mean=mean, warpage_pv_vertex_recompute=pv)


def _collect_primary(record, run_dir, events, case, mechanical):
    import h5py
    import numpy as np
    manifest, summary = _manifest(run_dir), _summary(run_dir)
    for key in ("nx0", "ny0", "L", "nz", "padded_cells", "mesh_sha256"):
        record["mesh"][key] = manifest.get("mesh", {}).get(key)
    record["summary"] = {key: summary.get(key, {}).get("value") for key in QOI_KEYS}
    record["summary"]["methods"] = {key: summary.get(key, {}).get("method") for key in QOI_KEYS}
    temperatures, cell_temperatures, counts, volume_sums = [], [], {}, {}
    active_hex, fallback_cut = 0, False
    with h5py.File(_vtk_path(run_dir, manifest), "r") as output:
        for name in ("hex", "cut"):
            block = output["VTKHDF/" + name]
            counts[name] = int(block["NumberOfCells"][...].sum())
            if name == "hex":
                kinds = block["CellData/cell_kind"][...]
                active_hex = int(np.count_nonzero(kinds == 0))
                fallback_cut = bool(np.any(kinds == 1))
            temperatures.extend(block["PointData/T"][...].tolist())
            if "CellData/T_cell" in block:
                cell_temperatures.extend(block["CellData/T_cell"][...].tolist())
            volume_sums[name] = float(np.sum(block["CellData/volume"][...]))
    record["mesh"].update(n_cells_hex=counts["hex"], n_cells_cut=counts["cut"])
    record["cell_volume_sums"] = volume_sums
    record["temperature"] = dict(
        T_vertex_min=min(temperatures) if temperatures else None,
        T_vertex_max=max(temperatures) if temperatures else None,
        T_cell_min=min(cell_temperatures) if cell_temperatures else None,
        T_cell_max=max(cell_temperatures) if cell_temperatures else None)
    if not cell_temperatures:
        record["notes"].append("T_cell optional field unavailable; cell extrema not measured")
    # Current geometric exporter counts cut polyhedra before FVM agglomeration.
    # Its counts cannot establish the number of independent merged unknowns.
    if counts["cut"] or fallback_cut:
        record["notes"].append("cut-case DoFs pending FVM merged-control-volume exporter count contract")
    else:
        record["dofs"].update(thermal=active_hex, mech=3 * active_hex)
    last = {}
    for event in events:
        if "solver" in event:
            last[event["solver"]] = event
    linear = case["solver"]["linear"]
    record["solver"] = [dict(name=p["solver"], preconditioner=linear.get("preconditioner", "jacobi"),
                             rtol=linear.get("rtol", 1e-12),
                             iterations=last.get(p["solver"], {}).get("iterations"),
                             final_residual=last.get(p["solver"], {}).get("residual"))
                        for p in case["physics"] if p["solver"] in last or mechanical or p["id"] == "thermal"]
    energy = _energy(events) or {}
    record["energy"] = {key: energy.get(key) for key in _SECTION_KEYS["energy"]}
    record["file_sha256"] = dict(manifest=_sha(run_dir / "manifest.json"), vtkhdf=_sha(_vtk_path(run_dir)))
    if mechanical and record["kind"] != "thermal":
        record["displacement"] = _displacement(run_dir, case["constraints"])
        if record["displacement"]["U_fixed_face_max_abs"] is None:
            record["notes"].append("Rigid321: fixed-face diagnostic inapplicable; U_mean is diagnostic, threshold 待定")
        if record["displacement"]["warpage_pv_vertex_recompute"] is None:
            record["notes"].append("vertex diagnostic unavailable: fewer than three top vertices")


def _primary_checks(record, run_dir, events, case, mechanical, expected_materials):
    checks = {"C2": _check_c2(run_dir, events), "C3": _check_c3(events), "C4": _check_c4(events),
              "C5": _check_c5(run_dir, _energy(events) or {}), "C6": _check_c6(run_dir, case["loads"])}
    if checks["C6"] is None:
        note = "C6: custom loads / body-source bounds 待定; no invented temperature threshold"
        if note not in record["notes"]:
            record["notes"].append(note)
    if "W1" in record["checks"] and mechanical:
        checks.update(W1=_check_w1(run_dir, events), W2=_check_w2(run_dir))
    if record["kind"] == "assembly":
        checks["A2"] = _material_membership(run_dir, expected_materials)
    for name, value in checks.items():
        _merge_check(record, name, value)
        if value is False:
            record["notes"].append(f"{run_dir.name}: {name} failed")


def _parallel_check(record, serial, parallel, mechanical):
    passed = True
    undefined = False
    keys = ["T_max"] + (["warpage_pv"] if mechanical and record["kind"] != "thermal" else [])
    for first in serial:
        m1, m2 = _manifest(first), _manifest(parallel)
        if (m1.get("mesh", {}).get("mesh_sha256") != m2.get("mesh", {}).get("mesh_sha256")
                or m1.get("partitions", {}).get("per_step") != [1]
                or m2.get("partitions", {}).get("per_step") != [2]):
            passed = False
            record["notes"].append("C8: mesh SHA or partitions differ from serial [1] / parallel [2]")
        q1, q2 = _summary(first), _summary(parallel)
        for key in keys:
            a, b = q1[key]["value"], q2[key]["value"]
            if not _finite([a, b]):
                passed = False
            elif a == 0:
                if b != 0:
                    # A relative error with a zero denominator has no specified
                    # tolerance. Do not substitute an arbitrary epsilon.
                    undefined = True
                    record["notes"].append(f"C8: {key} serial denominator zero; relative bound 待定")
            elif abs(b - a) / abs(a) > PARALLEL_RTOL:
                passed = False
                record["notes"].append(f"C8: {key} relative difference exceeds 1e-8")
    return False if not passed else None if undefined else True


def _solve_run(record, folder, inputs, output, stage, ranks, exes, mpi, timeout_s, source_root):
    command = [exes["solve"], "--case", inputs / "case.json", "--profile", inputs / f"profile_r{ranks}.json",
               "--out", output]
    if ranks == 2:
        command = [mpi, "-n", "2", *command]
    entry = _measured(command, "solve_" + stage, folder, record, timeout_s)
    if entry["exit_code"]:
        if record["kind"] == "assembly" and entry["exit_code"] == 13:
            _add_dependency(record, "A", "切割路径未合入（fvm-thermal-cutcell 阶段 B / fvm-thermoelastic-cutcell 切割段）")
            record["status_detail"] = record["status_detail"] or "CUT_UNSUPPORTED"
            record["notes"].append(f"solve_{stage}: CUT_UNSUPPORTED; downstream checks interrupted")
        else:
            _stage_error(record, entry, folder)
        return entry, None
    readback = _measured([sys.executable, source_root / "tools/vv/result_summary.py", output,
                          "-o", output / "readback.json"], "readback_" + stage, folder, record, timeout_s)
    if readback["exit_code"]:
        _stage_error(record, readback, folder)
        record["notes"].append(f"readback_{stage}: downstream checks and digest interrupted")
        return entry, None
    return entry, decode_events(output / "events.bin")


def _resource_retry(record, entry, folder, inputs, case, exes, mpi, timeout_s, source_root):
    # Only solve resource failures on real data enter this path. A successful
    # retry remains diagnostic and cannot erase the original failure.
    # Continuing primary runs must not overwrite the invocation's single retry.
    if record["data_class"] != "real" or not entry.get("status_detail") or record["attempts"]:
        return
    original_output = Path(entry["argv"][entry["argv"].index("--out") + 1]).relative_to(folder).as_posix()
    record["attempts"].append(dict(mesh=copy.deepcopy(case["mesh"]), exit_code=entry["exit_code"],
                                   status_detail=entry["status_detail"], output=original_output))
    retry = folder / "attempt_2"
    retry_inputs = retry / "inputs"
    shutil.copytree(inputs, retry_inputs)
    reduced = copy.deepcopy(case)
    for axis in ("nx", "ny"):
        reduced["mesh"][axis] = max(1, reduced["mesh"][axis] // 2)
    _write_json(retry_inputs / "case.json", reduced)
    ranks = 2 if "-n" in entry["argv"] else 1
    retried, _ = _solve_run(record, folder, retry_inputs, retry / f"np{ranks}_r1", "attempt_2", ranks,
                            exes, mpi, timeout_s, source_root)
    record["attempts"].append(dict(mesh=copy.deepcopy(reduced["mesh"]), exit_code=retried["exit_code"],
                                   status_detail=retried.get("status_detail"), output=f"attempt_2/np{ranks}_r1"))
    record["notes"].append("real solve retried once with halved nx/ny; original resource failure retained")


def _auxiliary(record, folder, inputs, case, exes, mpi, timeout_s, source_root, primary):
    import numpy as np
    if record["kind"] == "thermal" or "W1" not in record["checks"]:
        return
    if record["data_class"] != "synthetic":
        if record["kind"] == "warpage":
            if primary is not None:
                record["checks"]["W3"] = _check_w3(primary, case["constraints"])
            record["notes"].append("W3: Rigid321 mean diagnostic only, tolerance 待定")
            record["notes"].append("W5: FEM comparison is a synthetic noncut assertion; real result diagnostic only")
        record["notes"].append("W4: uniform T_ref auxiliary assertion applies to synthetic inputs")
        return
    variants = ["W3", "W4", "W5"] if record["kind"] == "warpage" else ["W4"]
    for name in variants:
        if name == "W5" and primary is None:
            record["notes"].append("W5: primary np1_r1 unavailable; FEM comparison interrupted")
            continue
        if name == "W5" and record["mesh"]["n_cells_cut"] != 0:
            record["notes"].append("W5: noncut FEM comparison inapplicable to cut input")
            continue
        variant = copy.deepcopy(case)
        if name == "W3":
            variant["constraints"] = {"fixed": [{"target": "face:zmin"}]}
        elif name == "W4":
            variant["loads"] = [dict(type="fixed_temperature", target=face, T=variant["physics"][1]["T_ref"])
                                for face in ("face:zmin", "face:zmax")]
        else:
            for physics in variant["physics"]:
                physics["solver"] = physics["solver"].replace("fvm.", "fem.")
            variant["solver"]["linear"]["preconditioner"] = "direct"
        aux = folder / "auxiliary" / name
        aux_inputs = aux / "inputs"
        shutil.copytree(inputs, aux_inputs)
        _write_json(aux_inputs / "case.json", variant)
        out = aux / "np1_r1"
        entry, events = _solve_run(record, folder, aux_inputs, out, name, 1, exes, mpi, timeout_s, source_root)
        if events is None:
            break
        if name == "W3":
            record["checks"][name] = _check_w3(out, variant["constraints"])
        elif name == "W4":
            _, u = _points_u(out)
            record["checks"][name] = bool(np.all(u == 0) and _summary(out)["warpage_pv"]["value"] == 0)
        else:
            fem = _summary(out)["warpage_pv"]["value"]
            fvm = _summary(primary)["warpage_pv"]["value"]
            record["diagnostic_fem_comparison"] = dict(fvm_warpage_pv=fvm, fem_warpage_pv=fem,
                                                       relative_difference=abs(fvm - fem) / fem if fem else None)
            if fem == 0:
                record["checks"][name] = None
                record["notes"].append("W5: zero FEM warpage denominator; comparison undefined")
            else:
                record["checks"][name] = bool(_finite([fem, fvm]) and fem > 0 and abs(fvm - fem) / fem <= .05)
        if record["checks"][name] is False:
            record["notes"].append(name + " auxiliary assertion failed")


def _pipeline(record, folder, kind, params, resolved, nps, repeat, source_root, w_ready):
    state = readiness(kind, source_root)
    record["readiness"] = dict(ready=state[0], reason=state[1])
    item = {"thermal": "T", "warpage": "W", "assembly": "A"}[kind]
    if not state[0]:
        _add_dependency(record, item, state[1])
        record["notes"].append("dependency interrupted all pipeline stages")
        return
    if kind == "assembly" and not w_ready:
        _add_dependency(record, "W", readiness("warpage", source_root)[1])
    exes = {name: _exe(os.environ.get(f"CHIPCAE_{name.upper()}_EXE") or f"chipcae-{name}")
            for name in ("import", "preview", "solve")}
    for name, exe in exes.items():
        if exe is None:
            _add_dependency(record, item, f"environment missing CHIPCAE_{name.upper()}_EXE executable")
    try:
        import numpy  # noqa: F401
        import h5py  # noqa: F401
        _shared("result_summary")
    except ImportError as exc:
        _add_dependency(record, item, f"environment missing numerical package: {exc}")
    if any(exe is None for exe in exes.values()) or any("package" in d["reason"] for d in record["needs_attention"]):
        record["notes"].append("dependency interrupted all pipeline stages")
        return
    mpi = _exe(os.environ.get("CHIPCAE_MPIEXEC") or shutil.which("mpiexec") or shutil.which("mpirun"))
    run_nps = [1]
    if 2 in nps and mpi is not None:
        run_nps.append(2)
    else:
        record["notes"].append("C8: MPI launcher unavailable" if 2 in nps else "C8: MPI not requested")
    inputs, timeout_s = folder / "inputs", params.get("timeout_s", 3600)
    _copy_inputs(inputs, kind, resolved)
    if kind == "assembly":
        command = [exes["import"], "--assembly", inputs / "assembly.json", "-o", inputs / "stack.json",
                   "--report", inputs / "defects.json"]
    else:
        board = next(p for p in inputs.iterdir() if p.name == "board" or p.name.startswith("board."))
        command = [exes["import"], board, "-o", inputs / "stack.json", "--polyset", inputs / "board.polyset",
                   "--report", inputs / "defects.json", "--material-map", inputs / "material_map.json"]
    if params.get("accept_b_tier", False):
        command.append("--accept-b-tier")
    imported = _measured(command, "import", folder, record, timeout_s)
    parts = _assembly_parts(resolved["document"]) if kind == "assembly" else []
    if kind == "assembly":
        _merge_check(record, "A1", _check_a1(inputs / "import_report.json", inputs / "stack.json",
                                           imported["exit_code"], [p["id"] for p in parts], params.get("accept_b_tier", False)))
        if record["checks"]["A1"] is False:
            record["notes"].append("A1: required bodies, weld count or accepted defects absent/invalid; report upstream M1")
        report = _json(inputs / "import_report.json") if (inputs / "import_report.json").is_file() else {}
        record["assembly"] = dict(welded_interfaces=report.get("welded_interfaces"),
                                   bodies=_bodies(_json(inputs / "stack.json")) if (inputs / "stack.json").is_file() else None,
                                   split_layers=report.get("parts", {}).get("split_layers"),
                                   b_tier_defects=[d.get("defect_id") for d in report.get("defects", []) if d.get("tier") == "B"])
        expected_checks = set(_check_names("assembly", w_ready))
        record["checks"]["A3"] = set(record["checks"]) == expected_checks and not {"W3", "W5"} & set(record["checks"])
    if imported["exit_code"]:
        _stage_error(record, imported, folder)
        return
    case = _case(params)
    _write_json(inputs / "case.json", case)
    for ranks in (1, 2):
        _write_json(inputs / f"profile_r{ranks}.json", dict(schema="chipcae.run_profile/1.0", backend="cpu", ranks=ranks, gpus=0))
    mesh = case["mesh"]
    preview = _measured([exes["preview"], inputs / "stack.json", "-o", folder / "preview", "--nx", mesh["nx"],
                         "--ny", mesh["ny"], "--levels", mesh["levels"], "--max-ratio", mesh["max_ratio"]],
                        "preview", folder, record, timeout_s)
    if preview["exit_code"]:
        _stage_error(record, preview, folder)
        record["notes"].append("preview rejected input; its independent grid uses the default cell budget")
        return
    for line in (folder / "logs/preview.stdout.log").read_text(encoding="utf-8").splitlines():
        try:
            event = json.loads(line)
            if "cell_count" in event:
                record["mesh"]["preview_cell_count"] = event["cell_count"]
        except (ValueError, TypeError):
            continue
    mechanical = w_ready
    expected_materials = _expected_materials(_json(inputs / "stack.json"), parts) if kind == "assembly" else set()
    runs, digests = {}, {}
    for ranks in run_nps:
        for index in range(1, (repeat if ranks == 1 else 1) + 1):
            name = f"np{ranks}_r{index}"
            out = folder / name
            try:
                entry, events = _solve_run(record, folder, inputs, out, name, ranks, exes, mpi, timeout_s, source_root)
                if events is None:
                    _resource_retry(record, entry, folder, inputs, case, exes, mpi, timeout_s, source_root)
                    continue
                _primary_checks(record, out, events, case, mechanical, expected_materials)
                digest = run_digest(out, events)
                digests[name] = digest
                record["digests"][name] = digest_sha256(digest)
                runs[name] = out
                if name == "np1_r1":
                    _collect_primary(record, out, events, case, mechanical)
                    record["mechanical_qoi_eligible"] = bool(w_ready and _check_w1(out, events))
            except Exception as exc:
                record["checks"]["C1"] = False
                record["status_detail"] = record["status_detail"] or "error"
                record["notes"].append(f"{name} driver exception: {type(exc).__name__}: {exc}")
    repeated = [f"np1_r{i}" for i in range(1, repeat + 1)]
    if all(name in digests for name in repeated):
        record["checks"]["C7"] = len({record["digests"][name] for name in repeated}) == 1
    if record["checks"]["C7"] is False:
        for name in repeated[1:]:
            different = _digest_diff(digests[repeated[0]], digests[name])
            if different:
                record["notes"].append(f"C7: {repeated[0]} / {name}: " + ", ".join(different) + "; report M3")
    serial = [runs[name] for name in repeated if name in runs]
    if "np2_r1" in runs and serial:
        record["checks"]["C8"] = _parallel_check(record, serial, runs["np2_r1"], mechanical)
    _auxiliary(record, folder, inputs, case, exes, mpi, timeout_s, source_root, runs.get("np1_r1"))
    if record["checks"]["C1"] is not False and not record["needs_attention"]:
        record["checks"]["C1"] = True


def _safe_temp_root():
    base = Path(tempfile.gettempdir()).resolve()
    while _in_worktree(base) and base != base.parent:
        base = base.parent
    return Path(tempfile.mkdtemp(prefix="chipcae-realcase-", dir=base))


def _clean_json(value, path, missing, notes):
    if isinstance(value, dict):
        return {key: _clean_json(item, f"{path}.{key}" if path else key, missing, notes)
                for key, item in value.items()}
    if isinstance(value, list):
        return [_clean_json(item, f"{path}[{index}]", missing, notes) for index, item in enumerate(value)]
    if type(value) is float and not math.isfinite(value):
        missing.append(path)
        notes.append(f"{path}: nonfinite value recorded as null")
        return None
    return value


def _result(record):
    qoi = {}
    if _numeric(record["summary"].get("T_max")):
        qoi["T_max"] = record["summary"]["T_max"]
    if record.get("mechanical_qoi_eligible"):
        for key in ("warpage_pv", "U_mag_max"):
            if _numeric(record["summary"].get(key)):
                qoi[key] = record["summary"][key]
    notes = list(record["notes"])
    notes.append("无解析解（物理合理性检查）; reference is null because no v0.2 reference category describes this check")
    if record["materials_provenance"] == "illustrative" and ILLUSTRATIVE_NOTE not in notes:
        notes.append(ILLUSTRATIVE_NOTE)
    if record["verdict"] == "needs_attention":
        notes.append("needs_attention: " + "; ".join(item["reason"] for item in record["needs_attention"]))
    # A usage diagnostic has no simulated data. Retain absent input metadata as
    # null in run_record; identify the result envelope explicitly as diagnostic.
    case_id = record["case_id"]
    data_class = record["data_class"]
    if not isinstance(case_id, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", case_id):
        case_id = "invalid-input"
        notes.append("diagnostic case identifier: caller case_id unavailable/invalid")
    if data_class not in ("synthetic", "real"):
        data_class = "synthetic"
        notes.append("empty diagnostic envelope: caller data_class unavailable/invalid; no source data or QoIs")
        qoi = {}
    return dict(schema="chipcae.vv_result/0.2", case_id=case_id, data_class=data_class, label="实测",
                date=record["date"], commit=record["commit"], qoi=qoi,
                verdict="skip" if record["verdict"] == "needs_attention" else record["verdict"],
                reference=None,
                tolerance=dict(value=None, temperature_atol_K=TEMPERATURE_ATOL,
                               parallel_rtol=PARALLEL_RTOL, energy_imbalance=1e-9,
                               synthetic_noncut_warpage_rtol=.05,
                               source="F-1130 / fvm-io/1.0 / fvm-thermoelastic-cutcell"),
                notes=notes)


def _exit_status(record, usage):
    if usage:
        return 2
    if any(value is False for value in record["checks"].values()):
        return 1
    return 77 if record["needs_attention"] else 0


def _set_verdict(record, usage):
    record["exit_code"] = _exit_status(record, usage)
    record["verdict"] = {0: "pass", 1: "fail", 2: "fail", 77: "needs_attention"}[record["exit_code"]]


def _markdown_table(title, columns, rows):
    def cell(value):
        text = json.dumps(value, ensure_ascii=False) if not isinstance(value, str) else value
        return text.replace("\\", "\\\\").replace("|", "\\|").replace("\r", "").replace("\n", "<br>")

    rows = list(rows) or [[None] * len(columns)]
    return (f"\n## {title}\n\n| " + " | ".join(columns) + " |\n|" + "|".join("---" for _ in columns)
            + "|\n" + "".join("| " + " | ".join(cell(value) for value in row) + " |\n" for row in rows))


def _metric_tables(record):
    tables = _markdown_table("Inputs", ("File", "SHA-256"), record["inputs"].items())
    mesh = list(record["mesh"].items()) + [("dofs." + key, value) for key, value in record["dofs"].items()]
    tables += _markdown_table("Mesh", ("Metric", "Value"), mesh)
    tables += _markdown_table("Solver", ("Name", "Preconditioner", "rtol", "Iterations", "Final residual"),
                              ([solver.get(key) for key in ("name", "preconditioner", "rtol", "iterations", "final_residual")]
                               for solver in record["solver"]))
    tables += _markdown_table("Energy", ("Metric", "Value"), record["energy"].items())
    tables += _markdown_table("Summary / QoIs", ("QoI", "Value", "Unit", "Method"),
                              ((key, record["summary"][key], "K" if key == "T_max" else "m",
                                record["summary"]["methods"].get(key)) for key in QOI_KEYS))
    digests = [("run." + key, value) for key, value in record["digests"].items()]
    digests += [("file." + key, value) for key, value in record["file_sha256"].items()]
    tables += _markdown_table("Digests", ("Source", "SHA-256"), digests)
    return tables


def _finalize(record, folder, usage):
    inputs = folder / "inputs"
    record["inputs"] = {p.relative_to(inputs).as_posix(): _sha(p) for p in sorted(inputs.rglob("*")) if p.is_file()}
    invalid_numbers, notes = [], []
    cleaned = _clean_json(record, "", invalid_numbers, notes)
    record.clear()
    record.update(cleaned)
    record["notes"].extend(notes)
    _set_verdict(record, usage)
    result_path = folder / "result.json"
    _write_json(result_path, _result(record))
    # Unknown commit cannot be truthfully mapped to a hexadecimal SHA.
    # This one schema conflict remains explicit instead of inventing one.
    if record["commit"] == "unknown" and not usage:
        record["notes"].append("C10 blocked: unknown source commit is rejected by frozen result schema")
        if not record["needs_attention"]:
            record["checks"]["C10"] = False
    else:
        record["checks"]["C10"] = _check_c10(result_path, record["data_class"])
    # C9 depends on the finalized data gate; only its own result is pending.
    record["missing"] = sorted(set(_missing(record, pending_checks=("C9",)) + invalid_numbers))
    if not usage:
        record["checks"]["C9"] = _check_c9(record)
    record["missing"] = sorted(set(_missing(record) + invalid_numbers))
    _set_verdict(record, usage)
    _write_json(result_path, _result(record))
    # Revalidate the actual finalized file; no status can be changed after this
    # check without regenerating result.json as well.
    if record["checks"]["C10"] is not None:
        record["checks"]["C10"] = _check_c10(result_path, record["data_class"])
        _set_verdict(record, usage)
        _write_json(result_path, _result(record))
    _write_json(folder / "run_record.json", record)
    title = f"# 实测 — {record['kind']} — {record['verdict']}\n\n"
    if record["materials_provenance"] == "illustrative":
        title += ILLUSTRATIVE_NOTE + "\n\n"
    # The complete JSON is intentionally present in the human record so no
    # metrics, missing fields or dependency reasons disappear from that view.
    checks = "| Check | Result |\n|---|---|\n" + "".join(
        f"| {key} | {value} |\n" for key, value in record["checks"].items())
    commands = "\n| Stage | Exit | Wall s | Peak RSS KiB |\n|---|---|---|---|\n" + "".join(
        f"| {c['stage']} | {c['exit_code']} | {c['wall_s']:.6f} | {c['peak_rss_kib']} |\n"
        for c in record["commands"])
    (folder / "run_record.md").write_text(title + checks + commands + _metric_tables(record) + "\n```json\n" +
                                          json.dumps(record, ensure_ascii=False, indent=2, allow_nan=False) +
                                          "\n```\n", encoding="utf-8")
    return record


def run_item(kind: str, params: dict, out_root: Path, nps: list[int], repeat: int,
             source_root: Path) -> dict:
    return _run_item(kind, params, out_root, nps, repeat, source_root)[0]


def _run_item(kind, params, out_root, nps, repeat, source_root, *, usage_error=None):
    """Keep the CLI's output location separate from the persisted run record."""
    source_root = Path(source_root).resolve()
    w_ready = readiness("warpage", source_root)[0]
    record = _blank_record(kind, params, source_root, w_ready)
    usage = False
    try:
        out_root = Path(out_root).resolve() if out_root is not None else _safe_temp_root()
        _require(usage_error is None, usage_error)
        resolved = _validate(kind, params, out_root, nps, repeat)
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as exc:
        usage = True
        record["notes"].append(f"usage error: {exc}")
        resolved = None
    # No file is created at a rejected real destination, even through aliases.
    if (out_root is None or not isinstance(out_root, Path)
            or (isinstance(params, dict) and params.get("data_class") == "real" and _in_worktree(out_root))):
        out_root = _safe_temp_root()
        record["notes"].append("error record redirected to a safe temporary output directory")
    try:
        folder = _new_folder(out_root, record["commit"], kind if kind in ("thermal", "warpage", "assembly") else "invalid")
    except OSError as exc:
        usage = True
        record["notes"].append(f"usage error: output unavailable: {exc}; using safe temporary output")
        folder = _new_folder(_safe_temp_root(), record["commit"], "invalid")
    if not usage:
        params = copy.deepcopy(params)
        approximate = kind == "assembly" and any(p.get("material_approx") is True
                                                  for p in _assembly_parts(resolved["document"]))
        record["materials_provenance"] = "illustrative" if params["materials"] is None or approximate else "provided"
        if record["materials_provenance"] == "illustrative":
            record["notes"].append(ILLUSTRATIVE_NOTE)
        try:
            _pipeline(record, folder, kind, params, resolved, nps, repeat, source_root, w_ready)
        except Exception as exc:
            record["checks"]["C1"] = False
            record["status_detail"] = record["status_detail"] or "error"
            record["notes"].append(f"driver exception: {type(exc).__name__}: {exc}")
    return _finalize(record, folder, usage), folder


class _Parser(argparse.ArgumentParser):
    def error(self, message):
        raise _UsageError(message)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = _Parser(description=__doc__)
    parser.add_argument("kind", choices=("thermal", "warpage", "assembly"))
    parser.add_argument("--params", required=True, type=Path)
    parser.add_argument("--source-root", type=Path, default=SOURCE_ROOT)
    parser.add_argument("--out-root", type=Path)
    parser.add_argument("--np", type=int, nargs="+", default=[1], dest="nps")
    parser.add_argument("--repeat", type=int, default=2)
    try:
        args = parser.parse_args(argv)
        params = _json(args.params)
        _require(isinstance(params, dict), "params must be an object")
        # CLI paths are relative to params.json; assembly's own tree is resolved
        # relative to assembly.json by the shared validator.
        for key in ("input", "material_map", "materials"):
            if isinstance(params.get(key), str):
                params[key] = str((args.params.resolve().parent / params[key]).resolve())
        record, folder = _run_item(args.kind, params, args.out_root, args.nps, args.repeat, args.source_root)
    except (OSError, ValueError, TypeError) as exc:
        def flag(name, default=None):
            try:
                return argv[argv.index(name) + 1]
            except (ValueError, IndexError):
                return default
        # Malformed CLI input still follows the shared usage-record finalizer.
        source = Path(flag("--source-root", SOURCE_ROOT)).resolve()
        out = flag("--out-root")
        record, folder = _run_item(argv[0] if argv else "invalid", {}, Path(out) if out else None,
                                   [1], 2, source, usage_error=f"CLI usage error: {exc}")
    print(f"{record['verdict']}: {folder}")
    return record["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
