#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent CalculiX DEMO-GATE reference; never uses chipcae solver results.

The ASCII fixed-width reader below is adapted from
 tools/vv/calculix_trilayer/generate.py:parse_frd, extended for separate NDTEMP
and DISP blocks and strict node coverage. Double binary FRD retains fine-grid
temperature differences. Version detection/CCX are imported.
Only a complete run writes work/REFERENCE.json; publishing is a separate action.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import resource
import signal
import struct
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path

import numpy as np
import scipy.interpolate  # Required: no nearest-neighbour fallback in shared QoIs.

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))
from tools.vv.calculix_trilayer.generate import CCX, ccx_version
from tools.vv.warpage_qoi import (
    best_fit_plane,
    interp_grid,
    mirror_quarter,
    qoi_surface,
)

QOIS = ("W_bfp_m", "T_max_K", "T_top_min_K")
LEVELS = {"L1": (0.001, 2), "L2": (0.0005, 4), "L3": (0.00025, 8)}
RSS_LIMIT_KIB = 8 * 1024**2


def require(condition, message):
    if not condition:
        raise ValueError(message)


def close(actual, expected):
    return math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-15)


def same_input(actual, expected):
    """Assert input contracts without exact comparisons of floating point values."""
    if type(expected) in (int, float):
        return type(actual) in (int, float) and close(actual, expected)
    if isinstance(expected, dict):
        return (
            isinstance(actual, dict)
            and actual.keys() == expected.keys()
            and all(same_input(actual[k], v) for k, v in expected.items())
        )
    if isinstance(expected, list):
        return (
            isinstance(actual, list)
            and len(actual) == len(expected)
            and all(same_input(a, b) for a, b in zip(actual, expected))
        )
    return type(actual) is type(expected) and actual == expected


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_inputs():
    directory = REPO / "examples/demo_gate"
    inputs = {
        name: json.loads((directory / f"{name}.json").read_bytes())
        for name in ("case", "stack", "materials", "profile")
    }
    case, stack = inputs["case"], inputs["stack"]
    require(
        case["stack"] == {"ref": "stack.json"}
        and case["materials"] == {"ref": "materials.json"},
        "unexpected input references",
    )
    require(
        stack["unit"] == "m"
        and same_input(stack["outline"]["box"], [0, 0, 0.02, 0.02]),
        "outline box",
    )
    polygon = stack["outline"]["polygon"]
    require(
        same_input(
            polygon,
            [
                [0.002, 0],
                [0.018, 0],
                [0.02, 0.002],
                [0.02, 0.018],
                [0.018, 0.02],
                [0.002, 0.02],
                [0, 0.018],
                [0, 0.002],
            ],
        ),
        "chamfer polygon",
    )
    layers = stack["layers"]
    require(
        [a["name"] for a in layers] == ["substrate", "bump_eq", "die_layer"], "layers"
    )
    require(
        all(close(a["thickness"], b) for a, b in zip(layers, [0.001, 0.0001, 0.0005])),
        "thickness",
    )
    require(
        [a["material"] for a in layers]
        == ["organic_eq", "sac305_ball_eq", "underfill"],
        "layer materials",
    )
    require(
        all(a["n_elem_min"] == 2 and a["grading"] == 1 for a in layers),
        "layer mesh contract",
    )
    require(not any(a.get("regions") for a in layers[:2]), "unexpected regions")
    require(
        same_input(
            layers[2]["regions"],
            [
                {
                    "name": "die",
                    "material": "silicon",
                    "footprint": {"box": [0.005, 0.005, 0.015, 0.015]},
                }
            ],
        ),
        "die region",
    )
    thermal, mech = case["physics"]
    require(
        thermal == {"id": "thermal", "solver": "fem.conduction", "mode": "steady"},
        "thermal procedure",
    )
    require(
        same_input(
            mech,
            {
                "id": "mech",
                "solver": "fem.solid",
                "temp_from": "thermal",
                "T_ref": 298.15,
                "nonlinear": {"geometric": False},
            },
        ),
        "mechanical procedure",
    )
    require(case["constraints"] == {"rigid": "3-2-1"}, "constraints")
    require(
        same_input(
            case["loads"],
            [
                {"type": "fixed_temperature", "target": "face:zmin", "T": 298.15},
                {"type": "heat_source", "target": "layer:die_layer", "power": 10},
                {"type": "convection", "target": "face:zmax", "h": 10, "T_inf": 298.15},
            ],
        ),
        "loads",
    )
    require(
        inputs["profile"]
        == {
            "schema": "chipcae.run_profile/1.0",
            "ranks": 1,
            "backend": "cpu",
            "gpus": 0,
        },
        "profile",
    )
    tref = mech["T_ref"]
    materials = {}
    expected = {
        "organic_eq": (24.1975e9, 0.30321, 15.4815e-6, 3.1),
        "sac305_ball_eq": (43.5555e9, 0.35321, 22.7062e-6, 58),
        "underfill": (7.7432e9, 0.32321, 30.963e-6, 0.5),
        "silicon": (125.827e9, 0.28321, 2.68346e-6, 148),
    }
    for material in inputs["materials"]["materials"]:
        name = material["id"]
        require(name in expected and name not in materials, "material IDs")
        require(
            material["model"] == "thermo_elastic" and material["unit_system"] == "SI",
            "material model",
        )
        values = {}
        for section, column, key in (
            ("elastic", "E", "E_Pa"),
            ("elastic", "nu", "nu"),
            ("thermal_expansion", "alpha", "alpha_1_per_K"),
        ):
            part = material[section]
            table = part["table"]
            require(
                part["symmetry"] == "isotropic"
                and table["temperatures"] == [250, 400]
                and table["interpolation"] == "linear"
                and table["extrapolation"] == "clamp",
                "material table",
            )
            a, b = table["columns"][column]
            values[key] = a + (b - a) * (tref - 250) / 150
        require(
            material["thermal_expansion"]["cte_type"] == "tangent"
            and close(material["thermal_expansion"]["stress_free_temperature"], tref),
            "CTE convention",
        )
        values["conductivity_W_per_mK"] = material["conductivity"]
        require(
            all(close(a, b) for a, b in zip(values.values(), expected[name])),
            "interpolated material mismatch",
        )
        materials[name] = values
    require(set(materials) == set(expected), "material set")
    length = stack["outline"]["box"][2]
    chamfer = polygon[0][0]
    return {
        "raw": inputs,
        "hashes": {name: sha256(directory / f"{name}.json") for name in inputs},
        "materials": materials,
        "layers": layers,
        "T_ref": tref,
        "L": length,
        "c": chamfer,
        "H": sum(a["thickness"] for a in layers),
        "die_half": (layers[2]["regions"][0]["footprint"]["box"][2] - length / 2),
        "power": case["loads"][1]["power"],
        "h_top": case["loads"][2]["h"],
        "T_bottom": case["loads"][0]["T"],
        "T_inf": case["loads"][2]["T_inf"],
    }


HEX_EDGES = (
    (0, 1),
    (1, 2),
    (2, 3),
    (3, 0),
    (4, 5),
    (5, 6),
    (6, 7),
    (7, 4),
    (0, 4),
    (1, 5),
    (2, 6),
    (3, 7),
)
WEDGE_EDGES = ((0, 1), (1, 2), (2, 0), (3, 4), (4, 5), (5, 3), (0, 3), (1, 4), (2, 5))


def build_mesh(inputs, name, full=False, complete=False):
    h, nz = LEVELS[name]
    n = round(inputs["L"] / (2 * h))
    limit = round((inputs["L"] - inputs["c"]) / h) * 2
    zplanes = [0.0]
    for layer in inputs["layers"]:
        base = zplanes[-1]
        zplanes.extend(base + layer["thickness"] * k / nz for k in range(1, nz + 1))
    zs = [
        v
        for pair in zip(
            zplanes[:-1], [(a + b) / 2 for a, b in zip(zplanes, zplanes[1:])]
        )
        for v in pair
    ]
    zs.append(zplanes[-1])
    keyed_elements, used = [], set()
    # Every cut passes through opposite grid corners. Clip in integer coordinates.
    for k in range(3 * nz):
        li = k // nz
        for j in range(-n if full else 0, n):
            for i in range(-n if full else 0, n):
                poly = [
                    (2 * i, 2 * j),
                    (2 * i + 2, 2 * j),
                    (2 * i + 2, 2 * j + 2),
                    (2 * i, 2 * j + 2),
                ]
                sx, sy = (1 if i >= 0 else -1), (1 if j >= 0 else -1)
                clipped = []
                for a, b in zip(poly, poly[1:] + poly[:1]):
                    fa, fb = (
                        sx * a[0] + sy * a[1] - limit,
                        sx * b[0] + sy * b[1] - limit,
                    )
                    if fa <= 0:
                        clipped.append(a)
                    if fa * fb < 0:
                        t = -fa / (fb - fa)
                        clipped.append(
                            tuple(round(a[d] + t * (b[d] - a[d])) for d in (0, 1))
                        )
                poly = list(dict.fromkeys(clipped))
                if len(poly) < 3:
                    continue
                require(len(poly) in (3, 4), "invalid clipped cell")
                corners = [(x, y, 2 * k) for x, y in poly] + [
                    (x, y, 2 * k + 2) for x, y in poly
                ]
                edges = HEX_EDGES if len(poly) == 4 else WEDGE_EDGES
                keys = corners + [
                    tuple((corners[a][d] + corners[b][d]) // 2 for d in range(3))
                    for a, b in edges
                ]
                material = inputs["layers"][li]["material"]
                cx = sum(x for x, y in poly) / len(poly) * h / 2
                cy = sum(y for x, y in poly) / len(poly) * h / 2
                if (
                    li == 2
                    and abs(cx) < inputs["die_half"]
                    and abs(cy) < inputs["die_half"]
                ):
                    material = inputs["layers"][li]["regions"][0]["material"]
                kind = (
                    ("C3D20" if complete else "C3D20R") if len(poly) == 4 else "C3D15"
                )
                keyed_elements.append((kind, material, li, k, keys))
                used.update(keys)
    ordered = sorted(used, key=lambda a: (a[2], a[1], a[0]))
    ids = {key: index for index, key in enumerate(ordered, 1)}
    offset = inputs["L"] / 2 if full else 0.0
    nodes = {
        ids[key]: (key[0] * h / 2 + offset, key[1] * h / 2 + offset, zs[key[2]])
        for key in ordered
    }
    elements, die_volumes, aspects = [], [], []
    volumes = []
    for eid, (kind, material, li, k, keys) in enumerate(keyed_elements, 1):
        conn = [ids[a] for a in keys]
        corners = np.array([nodes[a] for a in conn[: 8 if kind != "C3D15" else 6]])
        # Straight-sided extrusions: constant Jacobian; exact volume integral.
        a, b, c, d = (0, 1, 2, 3) if kind == "C3D15" else (0, 1, 3, 4)
        volume = float(
            np.linalg.det(
                np.array(
                    [
                        corners[b] - corners[a],
                        corners[c] - corners[a],
                        corners[d] - corners[a],
                    ]
                )
            )
        )
        if kind == "C3D15":
            volume /= 2
        require(
            volume > 0 and len(conn) == len(set(conn)),
            "invalid connectivity or signed volume",
        )
        edges = WEDGE_EDGES if kind == "C3D15" else HEX_EDGES
        lengths = [float(np.linalg.norm(corners[b] - corners[a])) for a, b in edges]
        aspects.append(max(lengths) / min(lengths))
        volumes.append(volume)
        if li == 2:
            die_volumes.append(volume)
        elements.append(
            {
                "id": eid,
                "type": kind,
                "material": material,
                "layer": li,
                "k": k,
                "nodes": conn,
                "volume": volume,
            }
        )
    area = (inputs["L"] ** 2 - 2 * inputs["c"] ** 2) * (1 if full else 0.25)
    require(
        math.isclose(math.fsum(volumes), area * inputs["H"], rel_tol=1e-12),
        "total volume",
    )
    require(
        math.isclose(
            math.fsum(die_volumes),
            area * inputs["layers"][2]["thickness"],
            rel_tol=1e-12,
        ),
        "die-layer volume",
    )
    expected = {"L1": 594, "L2": 4728, "L3": 37728}[name] * (4 if full else 1)
    require(len(elements) == expected, "element count")
    require(
        len(nodes) == len(set(nodes.values()))
        and set(nodes) == {n for e in elements for n in e["nodes"]},
        "duplicate/orphan nodes",
    )
    # Integer keys merge interfaces and midsides globally; verify every layer face matches.
    for ki in (2 * nz, 4 * nz):
        below = {
            a for e in keyed_elements if e[3] == ki // 2 - 1 for a in e[4] if a[2] == ki
        }
        above = {
            a for e in keyed_elements if e[3] == ki // 2 for a in e[4] if a[2] == ki
        }
        require(below == above and below, "nonconforming material interface")
    for xy in (
        (2 * n - 2 * round(inputs["c"] / h), 2 * n),
        (2 * n, 2 * n - 2 * round(inputs["c"] / h)),
    ):
        require((*xy, 0) in used, "missing chamfer endpoint")
    q = inputs["power"] / (
        (inputs["L"] ** 2 - 2 * inputs["c"] ** 2) * inputs["layers"][2]["thickness"]
    )
    power = math.fsum(q * v for v in die_volumes)
    expected_power = inputs["power"] * (1 if full else 0.25)
    error = abs(power - expected_power) / expected_power
    require(error <= 1e-10, "source conservation")
    return {
        "nodes": nodes,
        "elements": elements,
        "name": name,
        "full": full,
        "nz": nz,
        "h_m": h,
        "n_elem_z_per_layer": [nz] * 3,
        "n_nodes": len(nodes),
        "n_elements": len(elements),
        "elements_by_type": dict(Counter(e["type"] for e in elements)),
        "max_aspect_ratio": max(aspects),
        "heat_source": {
            "die_layer_volume_m3": math.fsum(die_volumes),
            "q_W_per_m3": q,
            "integrated_power_W": power,
            "relative_error": error,
        },
    }


def append_set(lines, name, ids, kind="NSET"):
    lines.append(f"*{kind}, {kind}={name}")
    for i in range(0, len(ids), 16):
        lines.append(",".join(map(str, ids[i : i + 16])))


def write_decks(work, job, mesh, inputs, iterative=None):
    if iterative is None:
        iterative = mesh["name"] == "L3"
    nodes, elements = mesh["nodes"], mesh["elements"]
    common = [
        "** SPDX-License-Identifier: Apache-2.0",
        "*HEADING",
        "DEMO-GATE independent CalculiX reference (not measured)",
        "*NODE",
    ]
    for nid, (x, y, z) in nodes.items():
        common.append(f"{nid},{x:.15g},{y:.15g},{z:.15g}")
    for kind in sorted(mesh["elements_by_type"]):
        common.append(f"*ELEMENT,TYPE={kind},ELSET={kind}_ALL")
        for element in elements:
            if element["type"] == kind:
                ns = element["nodes"]
                common.extend(
                    [
                        f"{element['id']}," + ",".join(map(str, ns[:10])) + ",",
                        ",".join(map(str, ns[10:])),
                    ]
                )
    append_set(common, "NALL", list(nodes))
    bottom = [n for n, xyz in nodes.items() if close(xyz[2], 0)]
    top = [
        n
        for n, xyz in nodes.items()
        if abs(xyz[2] - inputs["H"]) <= 1e-12 * inputs["H"]
    ]
    append_set(common, "BOTTOM", bottom)
    append_set(common, "TOP", top)
    for name, mat in inputs["materials"].items():
        append_set(
            common,
            "E_" + name,
            [e["id"] for e in elements if e["material"] == name],
            "ELSET",
        )
        common += [
            f"*MATERIAL,NAME={name}",
            "*ELASTIC",
            f"{mat['E_Pa']:.15g},{mat['nu']:.15g}",
            f"*EXPANSION,ZERO={inputs['T_ref']:.15g}",
            f"{mat['alpha_1_per_K']:.15g}",
            "*CONDUCTIVITY",
            f"{mat['conductivity_W_per_mK']:.15g}",
            f"*SOLID SECTION,ELSET=E_{name},MATERIAL={name}",
        ]
    append_set(
        common, "DIE_LAYER", [e["id"] for e in elements if e["layer"] == 2], "ELSET"
    )
    for face, k in (("BOTTOM_SURF", 0), ("TOP_SURF", 3 * mesh["nz"] - 1)):
        common.append(f"*SURFACE,NAME={face},TYPE=ELEMENT")
        common.extend(
            f"{e['id']},S{1 if k == 0 else 2}" for e in elements if e["k"] == k
        )
    common += ["*INITIAL CONDITIONS,TYPE=TEMPERATURE", f"NALL,{inputs['T_ref']:.15g}"]
    # Heat conduction needs the fully integrated quadratic stiffness (27 Gauss
    # points). Reusing the mechanical C3D20R formulation underintegrates terms
    # such as (d(x²y)/dy)² = x⁴ and biases the coarse thermal field. C3D20 has
    # identical nodes/connectivity; mechanics retains the prescribed C3D20R.
    thermal = [
        line.replace("*ELEMENT,TYPE=C3D20R,", "*ELEMENT,TYPE=C3D20,")
        for line in common
    ] + [
        "*STEP",
        "*HEAT TRANSFER,STEADY STATE",
        "1.,1.",
        "*BOUNDARY",
        f"BOTTOM,11,11,{inputs['T_bottom']:.15g}",
        "*DFLUX",
        f"DIE_LAYER,BF,{mesh['heat_source']['q_W_per_m3']:.15g}",
        "*FILM",
    ]
    thermal.extend(
        f"{e['id']},F2,{inputs['T_inf']:.15g},{inputs['h_top']:.15g}"
        for e in elements
        if e["k"] == 3 * mesh["nz"] - 1
    )
    thermal += [
        "*NODE OUTPUT,DOUBLE",
        "NT",
        "*NODE PRINT,NSET=BOTTOM,TOTALS=ONLY",
        "RFL",
        "*SECTION PRINT,SURFACE=TOP_SURF,NAME=TOP_FLUX",
        "FLUX",
        "*END STEP",
    ]
    mech = list(common)
    if mesh["full"]:
        positions = {tuple(round(x, 12) for x in xyz): n for n, xyz in nodes.items()}
        a, b, c = [
            positions[tuple(round(v, 12) for v in xyz)]
            for xyz in (
                (inputs["c"], 0, 0),
                (inputs["L"] - inputs["c"], 0, 0),
                (0, inputs["L"] - inputs["c"], 0),
            )
        ]
        mech += ["*BOUNDARY", f"{a},1,3", f"{b},2,3", f"{c},3,3"]
    else:
        append_set(mech, "XSYM", [n for n, xyz in nodes.items() if close(xyz[0], 0)])
        append_set(mech, "YSYM", [n for n, xyz in nodes.items() if close(xyz[1], 0)])
        anchor = next(n for n, xyz in nodes.items() if all(close(a, 0) for a in xyz))
        mech += ["*BOUNDARY", "XSYM,1,1", "YSYM,2,2", f"{anchor},3,3"]
    if iterative:
        # Keep CalculiX's residual-correction loop active without geometric
        # nonlinearity in the loaded steps. This unloaded, isothermal step has
        # identically zero displacement; *STEP,NLGEOM=NO then restores the
        # prescribed small-strain equations (2.23 manual, *STEP).
        mech += [
            "*STEP,NLGEOM",
            "*STATIC,SOLVER=ITERATIVE CHOLESKY",
            "1.,1.",
            "*NODE OUTPUT,DOUBLE",
            "U,NT",
            "*END STEP",
        ]
    loaded_step = [
        "*STEP,NLGEOM=NO" if iterative else "*STEP",
        "*STATIC,SOLVER=ITERATIVE CHOLESKY" if iterative else "*STATIC",
    ]
    if iterative:
        loaded_step += [
            "1.,1.",
            "*CONTROLS,PARAMETERS=FIELD",
            "1.e-10,1.e-10,,,1.e-10",
        ]
    loaded_step += [
        f"*TEMPERATURE,FILE={job}_thermal.frd,BSTEP=1",
        "*NODE OUTPUT,DOUBLE",
        "U,NT",
        "*END STEP",
    ]
    mech += loaded_step
    if iterative:
        # A second step at the identical temperature corrects the remaining
        # linear-solve residual. It adds no load. CalculiX also has an absolute
        # displacement-correction stopping rule, so field controls alone do not
        # ensure sufficiently accurate fine-grid warpage differences.
        mech += loaded_step
    paths = []
    for suffix, lines in (("thermal", thermal), ("mech", mech)):
        path = work / f"{job}_{suffix}.inp"
        path.write_text("\n".join(lines) + "\n", encoding="ascii")
        paths.append(path)
    return paths


def parse_results(path):
    """Read ASCII E12.5 or native double FRD; keep each result block separate."""
    with Path(path).open("rb") as stream:
        for line in stream:
            if line.lstrip().startswith(b"2C"):
                if line.split()[-1:] == [b"3"]:
                    return parse_double_results(stream, line)
                break
    nodes, temps, disps = {}, [], []
    mode, target = None, None
    record = re.compile(r" -1[ 0-9]{10}(?:[ -][0-9]\.[0-9]{5}E[+-][0-9]{2})+")
    with Path(path).open(encoding="ascii") as stream:
        for line in stream:
            line = line.rstrip("\r\n")
            key = line.lstrip()
            if key.startswith("-1") and mode is not None:
                size = 1 if mode == "temp" else 3
                if not record.fullmatch(line) or len(line) != 13 + 12 * size:
                    raise ValueError(
                        f"unsupported/truncated {mode} FRD record: {line!r}"
                    )
                nid = int(line[3:13])
                require(
                    nid > 0 and nid not in target,
                    f"duplicate/invalid {mode} node {nid}",
                )
                values = tuple(
                    float(line[13 + 12 * i : 25 + 12 * i]) for i in range(size)
                )
                require(all(math.isfinite(v) for v in values), "nonfinite FRD")
                target[nid] = values[0] if size == 1 else values
            elif key.startswith("-3"):
                mode, target = None, None
            elif key.split()[:1] == ["2C"]:
                require(
                    mode is None and not nodes, "duplicate/unterminated coordinates"
                )
                mode, target = "coords", nodes
            elif key.startswith("-4"):
                require(mode is None, "unterminated FRD block")
                kind = key.split()[1]
                if kind == "NDTEMP":
                    target = {}
                    temps.append(target)
                    mode = "temp"
                elif kind == "DISP":
                    target = {}
                    disps.append(target)
                    mode = "disp"
    require(
        mode is None and nodes and temps, "missing/incomplete coordinates or NDTEMP"
    )
    for block in temps + disps:
        require(set(block) == set(nodes), "incomplete/unknown result node set")
    return {"nodes": nodes, "temperature_blocks": temps, "displacement_blocks": disps}


def parse_double_results(stream, coordinate_header):
    """Read native double FRD from *NODE OUTPUT,DOUBLE (CalculiX 2.23).

    The binary layouts follow upstream frd.c/frdselect.c: 32-bit IDs,
    unaligned native doubles, C3D20/C3D15 topology codes 4/5. ASCII reading
    remains available for historical results. Unsupported layouts fail closed.
    """

    def read_record(layout):
        record = struct.Struct("=" + layout)
        data = stream.read(record.size)
        require(len(data) == record.size, "truncated binary FRD record")
        return record.unpack(data)

    def field(count, size):
        require(count > 0, "empty binary FRD field")
        result = {}
        for _ in range(count):
            nid, *values = read_record("i" + "d" * size)
            require(nid > 0 and nid not in result, "duplicate/invalid binary FRD node")
            require(all(math.isfinite(v) for v in values), "nonfinite binary FRD")
            result[nid] = values[0] if size == 1 else tuple(values)
        return result

    nodes = field(int(coordinate_header.split()[1]), 3)
    topology = stream.readline().split()
    require(
        len(topology) == 3 and topology[0] == b"3C" and topology[2] == b"2",
        "unsupported binary FRD topology",
    )
    n_elements = int(topology[1])
    require(n_elements >= 0, "invalid binary FRD element count")
    for _ in range(n_elements):
        _, kind, _, _ = read_record("4i")
        require(kind in (4, 5), "unsupported binary FRD element")
        read_record("i" * (20 if kind == 4 else 15))
    temps, disps = [], []
    ended = False
    for line in stream:
        key = line.split()
        if key[:1] == [b"9999"]:
            ended = True
            break
        if key[:1] != [b"100CL"]:
            require(key[:1] == [b"1PSTEP"], "unexpected binary FRD header")
            continue
        require(key[-1:] == [b"3"], "FRD results must use double precision")
        count = int(line[24:36])
        header = stream.readline().split()
        require(
            len(header) == 4 and header[0] == b"-4", "missing binary FRD field header"
        )
        kind = header[1]
        require(kind in (b"NDTEMP", b"DISP"), "unsupported binary FRD field")
        components = int(header[2])
        require(components == (1 if kind == b"NDTEMP" else 4), "invalid FRD components")
        for _ in range(components):
            require(stream.readline().split()[:1] == [b"-5"], "missing FRD component")
        values = field(count, 1 if kind == b"NDTEMP" else 3)
        require(set(values) == set(nodes), "incomplete/unknown result node set")
        (temps if kind == b"NDTEMP" else disps).append(values)
    require(ended and temps, "missing/incomplete binary FRD results")
    return {"nodes": nodes, "temperature_blocks": temps, "displacement_blocks": disps}


def process_tree_rss(pid):
    """Summed RSS of solver and any descendants. Fail closed on monitoring errors."""
    pending, total = [pid], 0
    while pending:
        current = pending.pop()
        try:
            status = Path(f"/proc/{current}/status").read_text()
            matches = re.findall(r"^VmRSS:\s+(\d+) kB$", status, re.MULTILINE)
            if matches:
                total += int(matches[0])
            for task in Path(f"/proc/{current}/task").iterdir():
                pending.extend(map(int, (task / "children").read_text().split()))
        except FileNotFoundError:
            continue  # Process exited between sampling and reading /proc.
    return total


def measured_solver(work, job, executable):
    """Only called in a fresh Python process; ru_maxrss cannot include earlier jobs."""
    start = time.monotonic()
    env = dict(
        os.environ, OMP_NUM_THREADS="2", OPENBLAS_NUM_THREADS="1", CCX_NPROC_RESULTS="2"
    )
    stdout = work / f"{job}.ccx.stdout"
    stderr = work / f"{job}.ccx.stderr"
    sampled = 0
    proc = None

    def measurement():
        return {
            "label": "实测",
            "peak_rss_KiB": max(
                sampled, resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            ),
            "wall_seconds": time.monotonic() - start,
        }

    try:
        with stdout.open("w") as out, stderr.open("w") as err:
            proc = subprocess.Popen(
                [executable, job],
                cwd=work,
                stdout=out,
                stderr=err,
                env=env,
                start_new_session=True,
            )
            while proc.poll() is None:
                sampled = max(sampled, process_tree_rss(proc.pid))
                require(
                    sampled <= RSS_LIMIT_KIB,
                    f"solver exceeded 8 GiB RSS ({sampled} KiB)",
                )
                time.sleep(1)
        result = measurement()
        require(
            result["peak_rss_KiB"] <= RSS_LIMIT_KIB, "solver peak RSS exceeded 8 GiB"
        )
        output = stdout.read_text(errors="replace") + stderr.read_text(errors="replace")
        if proc.returncode != 0 or "*ERROR" in output.upper():
            raise RuntimeError(
                f"ccx {job} failed (exit {proc.returncode}):\n{output[-4000:]}"
            )
        conditions = re.findall(r"error condition \(0=good, 1=bad\)\s*=\s*(\d+)", output)
        if any(int(condition) != 0 for condition in conditions):
            raise RuntimeError(f"ccx {job}: iterative solver did not converge")
        if not (work / f"{job}.frd").is_file():
            raise RuntimeError(f"ccx {job} did not write fresh FRD")
        return result
    except BaseException as error:
        if proc is not None and proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        evidence = measurement()
        evidence.update(
            returncode=proc.returncode if proc is not None else None,
            error=str(error),
            sampled_tree_peak_rss_KiB=sampled,
        )
        (work / f"{job}.failure.json").write_text(
            json.dumps(evidence, ensure_ascii=False) + "\n"
        )
        raise


def run_ccx(work, job):
    work = Path(work).resolve()
    (work / f"{job}.frd").unlink(missing_ok=True)
    measurement = work / f"{job}.resources.json"
    measurement.unlink(missing_ok=True)
    (work / f"{job}.failure.json").unlink(missing_ok=True)
    result = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "--measure",
            str(work),
            job,
            CCX,
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"ccx measurement failed:\n{result.stdout}\n{result.stderr}")
    require((work / f"{job}.frd").is_file(), "missing fresh FRD")
    return json.loads(measurement.read_text())


def compute_qoi(thermal, mechanical, mesh, inputs, work, job):
    nodes = thermal["nodes"]
    require(
        set(nodes) == set(mesh["nodes"]) == set(mechanical["nodes"]),
        "solver changed node IDs",
    )
    require(
        all(
            all(close(a, b) for a, b in zip(xyz, mesh["nodes"][n]))
            for n, xyz in nodes.items()
        ),
        "FRD coordinates changed",
    )
    top = {
        n
        for n, xyz in nodes.items()
        if abs(xyz[2] - inputs["H"]) <= 1e-12 * inputs["H"]
    }
    expected = {
        n
        for n, xyz in mesh["nodes"].items()
        if abs(xyz[2] - inputs["H"]) <= 1e-12 * inputs["H"]
    }
    require(top == expected and top, "missing top midside nodes")
    temp = thermal["temperature_blocks"][-1]
    transfer = mechanical["temperature_blocks"][-1]
    require(mechanical["displacement_blocks"], "missing mechanical DISP")
    displacement = mechanical["displacement_blocks"][-1]
    delta = max(abs(temp[n] - transfer[n]) for n in nodes)
    require(delta <= 1e-3, "temperature transfer exceeds 1e-3 K")
    points = np.array([(*nodes[n], *displacement[n]) for n in sorted(top)]) * 1000
    if mesh["full"]:
        points[:, :2] -= inputs["L"] * 500
    else:
        points = mirror_quarter(points)
    # Co-circular surface nodes admit multiple Delaunay diagonals. Normalize
    # translation roundoff and traversal order so full and mirrored models use
    # the same triangulation, including under a rigid tilt.
    points[:, :3] = np.round(points[:, :3], 9)
    points = points[np.lexsort((points[:, 0], points[:, 1]))]
    xs = ys = np.linspace(-inputs["L"] * 500, inputs["L"] * 500, 81)
    x, y, w = interp_grid(points, xs, ys)
    outside = np.abs(x) + np.abs(y) > (inputs["L"] - inputs["c"]) * 1000
    require(
        np.isnan(w[outside]).all() and np.isfinite(w[~outside]).all(),
        "chamfer interpolation mask",
    )
    _, residual = best_fit_plane(points[:, 0], points[:, 1], points[:, 5])
    with (work / f"{job}_top.csv").open("w") as stream:
        writer = csv.writer(stream)
        writer.writerow(["x_mm", "y_mm", "z_mm", "ux_mm", "uy_mm", "uz_mm"])
        writer.writerows(points)
    return {
        "W_bfp_m": qoi_surface(x, y, w)["W_bfp"] / 1000,
        "W_bfp_nodes_m": float(np.ptp(residual)) / 1000,
        "T_max_K": max(temp.values()),
        "T_top_min_K": min(temp[n] for n in top),
        "temperature_transfer": {"n_nodes": len(nodes), "max_abs_delta_K": delta},
    }


def thermal_balance(work, job, inputs, mesh, thermal):
    dat = (work / f"{job}_thermal.dat").read_text()
    # *NODE PRINT,TOTALS=ONLY RFL is positive inward at the fixed-temperature boundary.
    match = re.search(
        r"total heat generation for set BOTTOM[^\n]*\n\s*\n\s*([+\-\d.Ee]+)",
        dat,
        re.IGNORECASE,
    )
    require(match is not None, "missing bottom RFL total")
    bottom = -float(match[1])
    temp = thermal["temperature_blocks"][-1]
    # Integrate quadratic face interpolation exactly. For a straight quad the
    # corner weights are -1/12 and midside weights 1/3; for a triangle 0 and 1/3.
    flows = []
    for e in mesh["elements"]:
        if e["k"] != 3 * mesh["nz"] - 1:
            continue
        ns = e["nodes"]
        if e["type"] == "C3D15":
            avg = sum(temp[ns[i]] for i in (9, 10, 11)) / 3
        else:
            avg = (
                -sum(temp[ns[i]] for i in (4, 5, 6, 7)) / 12
                + sum(temp[ns[i]] for i in (12, 13, 14, 15)) / 3
            )
        height = inputs["layers"][2]["thickness"] / mesh["nz"]
        flows.append(inputs["h_top"] * (avg - inputs["T_inf"]) * e["volume"] / height)
    top = math.fsum(flows)
    power = inputs["power"] * (1 if mesh["full"] else 0.25)
    return {
        "bottom_outward_W": bottom,
        "top_outward_W": top,
        "relative_error": (bottom + top - power) / power,
    }


def run_model(work, name, inputs, full=False, complete=False):
    job = name + ("_full" if full else "_cross" if complete else "_quarter")
    print(f"{job}: generating", flush=True)
    mesh = build_mesh(inputs, name, full, complete)
    decks = write_decks(work, job, mesh, inputs)
    thermal_resources = run_ccx(work, decks[0].stem)
    thermal = parse_results(decks[0].with_suffix(".frd"))
    require(
        set(thermal["nodes"]) == set(mesh["nodes"]), "thermal output lost mesh nodes"
    )
    thermal_t = thermal["temperature_blocks"][-1]
    thermal_top = {
        n
        for n, xyz in mesh["nodes"].items()
        if abs(xyz[2] - inputs["H"]) <= 1e-12 * inputs["H"]
    }
    thermal_evidence = {
        "T_max_K": max(thermal_t.values()),
        "T_top_min_K": min(thermal_t[n] for n in thermal_top),
        "heat_source": mesh["heat_source"],
        "resources": thermal_resources,
        "n_nodes": mesh["n_nodes"],
        "n_elements": mesh["n_elements"],
    }
    thermal_evidence["thermal_balance"] = thermal_balance(
        work, job, inputs, mesh, thermal
    )
    (work / f"{job}.thermal.json").write_text(
        json.dumps(thermal_evidence, ensure_ascii=False, indent=2) + "\n"
    )
    resources = [thermal_resources, run_ccx(work, decks[1].stem)]
    mechanical = parse_results(decks[1].with_suffix(".frd"))
    result = {
        key: mesh[key]
        for key in (
            "h_m",
            "n_elem_z_per_layer",
            "n_nodes",
            "n_elements",
            "elements_by_type",
            "max_aspect_ratio",
            "heat_source",
        )
    }
    result.update(
        name=name,
        element_type="C3D20 + C3D15" if complete else "C3D20R + C3D15",
        thermal_element_type="C3D20 + C3D15",
        decks=[{"file": path.name, "sha256": sha256(path)} for path in decks],
        resources={
            "label": "实测",
            "peak_rss_KiB": max(r["peak_rss_KiB"] for r in resources),
            "wall_seconds": sum(r["wall_seconds"] for r in resources),
        },
        solver_runs=resources,
    )
    result.update(compute_qoi(thermal, mechanical, mesh, inputs, work, job))
    result["thermal_balance"] = thermal_balance(work, job, inputs, mesh, thermal)
    (work / f"{job}.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    )
    print(f"{job}: " + json.dumps({k: result[k] for k in QOIS}), flush=True)
    return result


def assess_reference(levels, symmetry, cross, tref):
    observed, selected, uncertainty, reference = {}, {}, {}, {}
    convergence_ok = order_ok = uncertainty_ok = True

    def rise(value, key):
        return value if key == "W_bfp_m" else value - tref

    for key in QOIS:
        q1, q2, q3 = [rise(level[key], key) for level in levels]
        d12, d23 = q1 - q2, q2 - q3
        monotonic = d12 * d23 > 0
        convergence_ok = convergence_ok and monotonic and abs(d23) / abs(q3) <= 0.002
        order_ok = order_ok and monotonic
        if monotonic:
            p = math.log(d12 / d23) / math.log(2)
            choice = "richardson" if 1.5 <= p <= 4.5 else "finest"
            ref = q3 + (q3 - q2) / (2**p - 1) if choice == "richardson" else q3
        else:
            p, choice, ref = None, "finest", q3
        observed[key], selected[key] = p, choice
        uncertainty[key] = abs(d23)
        reference[key] = ref if key == "W_bfp_m" else ref + tref
        uncertainty_ok = uncertainty_ok and abs(d23) <= 0.002 * abs(ref)
    for obj, main in ((symmetry, levels[0]), (cross, levels[2])):
        obj["relative_difference"] = {
            key: abs(obj[key] - main[key]) / abs(rise(main[key], key)) for key in QOIS
        }
    symmetry_ok = all(v <= 1e-6 for v in symmetry["relative_difference"].values())
    cross_ok = all(v <= 0.001 for v in cross["relative_difference"].values())
    heat_ok = all(
        level["heat_source"]["relative_error"] <= 1e-10
        and level["temperature_transfer"]["max_abs_delta_K"] <= 1e-3
        for level in levels + [symmetry, cross]
    )
    criteria = dict(
        a=convergence_ok,
        b=order_ok,
        c=uncertainty_ok,
        d=cross_ok,
        e=symmetry_ok,
        f=heat_ok,
    )
    status = (
        "参考不合格"
        if not all(criteria[k] for k in "abcef")
        else "需讨论" if not cross_ok else "合格"
    )
    return {
        "observed_order": observed,
        "selected": selected,
        "U_ref": uncertainty,
        "reference": reference,
        "status": status,
        "criteria": criteria,
    }


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--measure":
        work, job, executable = Path(sys.argv[2]), sys.argv[3], sys.argv[4]
        result = measured_solver(work, job, executable)
        (work / f"{job}.resources.json").write_text(json.dumps(result) + "\n")
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--levels", default="L1,L2,L3")
    parser.add_argument(
        "--work", type=Path,
        help="solver output directory (default: a new system temporary directory)",
    )
    args = parser.parse_args()
    names = args.levels.split(",")
    require(
        names and len(names) == len(set(names)) and set(names) <= LEVELS.keys(),
        "invalid levels",
    )
    work = args.work.resolve() if args.work else Path(tempfile.mkdtemp(prefix="chipcae-demo-gate-"))
    work.mkdir(parents=True, exist_ok=True)
    print(f"Working directory: {work}", flush=True)
    # Never leave a stale reference when an operation fails or only L1 is rebuilt.
    (work / "REFERENCE.json").unlink(missing_ok=True)
    inputs = load_inputs()
    version = ccx_version()
    require(version == "This is Version 2.23", "reference requires CalculiX 2.23")
    (work / "calculix-version.txt").write_text(version + "\n")
    levels = [run_model(work, name, inputs) for name in LEVELS if name in names]
    if set(names) != set(LEVELS):
        return
    symmetry = run_model(work, "L1", inputs, full=True)
    cross = run_model(work, "L3", inputs, complete=True)
    package = "unknown"
    for path in (Path(CCX).resolve().parent.parent / "conda-meta").glob(
        "calculix-*.json"
    ):
        meta = json.loads(path.read_text())
        package = f"{meta['name']} {meta['version']} {meta.get('build','')}"
    result = {
        "SPDX-License-Identifier": "Apache-2.0",
        "schema_version": "chipcae.vv.demo_gate.reference/0.1",
        "kind": "REFERENCE",
        "case": "DEMO-GATE",
        "reference_type": "独立代码",
        "label": "CalculiX 独立参考值，不是实测，不是解析解",
        "model_scope": "quarter",
        "calculix_version": version,
        "ccx_package": package,
        "host": f"box {platform.system()} {platform.machine()}",
        "element_type": "C3D20R + C3D15",
        "thermal_element_type": "C3D20 + C3D15",
        "inputs_sha256": inputs["hashes"],
        "materials_at_T_ref": inputs["materials"],
        "levels": levels,
        "symmetry_check": symmetry,
        "element_cross": cross,
    }
    result.update(assess_reference(levels, symmetry, cross, inputs["T_ref"]))
    (work / "REFERENCE.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    )
    print(result["status"], result["criteria"], flush=True)


if __name__ == "__main__":
    main()
