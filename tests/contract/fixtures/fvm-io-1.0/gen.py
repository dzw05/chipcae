#!/usr/bin/env python3
"""Generate the complete synthetic fvm-io/1.0 package with --out DIR.

NumPy is required. JSON is UTF-8/LF, sorted, finite and timestamp-free.
Fixture IDs ((k*ny)+j)*nx+i are local keys, not Morton elem_gid.
The checker has its own assembly and never imports this module.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys


def write(out, relative, value):
    path = out / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(value, indent=2, sort_keys=True, allow_nan=False)
                      + "\n").encode("utf-8"))


def grid(x, y, z, conductivity):
    nx, ny, nz = len(x) - 1, len(y) - 1, len(z) - 1
    return {"x_m": x, "y_m": y, "z_m": z,
            "boundary": {"bottom_T_K": 300, "top_q_in_W_m2": 100, "sides": "insulated"},
            "cells": [{"id": ((k * ny) + j) * nx + i, "ijk": [i, j, k],
                       "conductivity_diag": conductivity(i, j, k)}
                      for k in range(nz) for j in range(ny) for i in range(nx)]}


def solve(inp):
    """Conservative symmetric face assembly; each internal face is visited once."""
    import numpy as np
    axes = [inp[a + "_m"] for a in "xyz"]
    shape = [len(a) - 1 for a in axes]
    widths = [[b - a for a, b in zip(axis, axis[1:])] for axis in axes]
    count = len(inp["cells"])
    matrix, rhs = np.zeros((count, count)), np.zeros(count)
    cells = {cell["id"]: cell for cell in inp["cells"]}
    for cell in cells.values():
        c, ijk, kd = cell["id"], cell["ijk"], cell["conductivity_diag"]
        for axis, stride in enumerate([1, shape[0], shape[0] * shape[1]]):
            area = 1.0
            for d in range(3):
                if d != axis:
                    area *= widths[d][ijk[d]]
            distance = widths[axis][ijk[axis]] * 0.5
            if ijk[axis] < shape[axis] - 1:
                neighbor = cells[c + stride]
                resistance = distance / kd[axis] + (widths[axis][ijk[axis] + 1] * 0.5
                                                   / neighbor["conductivity_diag"][axis])
                g = area / resistance
                for row, col, sign in [(c, c, 1), (c + stride, c + stride, 1),
                                       (c, c + stride, -1), (c + stride, c, -1)]:
                    matrix[row, col] += sign * g
            if axis == 2:
                if ijk[2] == 0:
                    g = area * kd[2] / distance
                    matrix[c, c] += g
                    rhs[c] += g * inp["boundary"]["bottom_T_K"]
                if ijk[2] == shape[2] - 1:
                    rhs[c] += area * inp["boundary"]["top_q_in_W_m2"]
    return np.linalg.solve(matrix, rhs).tolist()


def keyed(values):
    return {"cells": [{"id": i, "T_K": value} for i, value in enumerate(values)]}


def generate(out):
    layers = [{"thickness_m": h, "conductivity_W_mK": k}
              for h, k in [(0.001, 1), (0.002, 2), (0.003, 4)]]
    laminate = grid([0, 0.01], [0, 0.01], [0, 0.0005, 0.001, 0.002, 0.003, 0.0045, 0.006],
                    lambda i, j, k: [layers[k // 2]["conductivity_W_mK"]] * 3)
    laminate["layers"] = layers
    analytic = []
    for a, b in zip(laminate["z_m"], laminate["z_m"][1:]):
        z, start, resistance = (a + b) / 2, 0.0, 0.0
        for layer in layers:
            h = layer["thickness_m"]
            resistance += max(0, min(z - start, h)) / layer["conductivity_W_mK"]
            start += h
        analytic.append(300 + 100 * resistance)
    write(out, "laminate_1d/input.json", laminate)
    write(out, "laminate_1d/expected.json", keyed(analytic))
    hex_input = grid([0, 0.001, 0.003, 0.006], [0, 0.002, 0.005],
                     [0, 0.0005, 0.0015, 0.003, 0.005],
                     lambda i, j, k: [1 + 0.2*i + 0.3*j + 0.1*k,
                                      2 + 0.1*i + 0.4*j + 0.2*k,
                                      4 + 0.3*i + 0.2*j + 0.5*k])
    write(out, "hex_kdiag/input.json", hex_input)
    write(out, "hex_kdiag/expected.json", keyed(solve(hex_input)))
    fields = []
    for name, location, components, dtype, unit in [
        ("T", "point", 1, "float64", "K"), ("U", "point", 3, "float64", "m"),
        *[(n, "cell", 1, "int32", "1") for n in ("layer_id", "material_id", "cell_kind")],
        ("elem_gid", "cell", 1, "int64", "1"), ("volume", "cell", 1, "float64", "m3"),
        ("copper_fraction", "cell", 1, "float64", "1"),
        ("conductivity_diag", "cell", 3, "float64", "W/(m·K)"),
        ("T_cell", "cell", 1, "float64", "K")]:
        fields.append(dict(name=name, location=location, components=components, dtype=dtype, unit=unit))
    write(out, "manifest_fvm_solid/manifest.json", {
        "schema_version": "chipcae.results/1.0", "steps": [{"n": 0, "file": "step-0.vtkhdf"}],
        "partitions": {"per_step": [1], "solver_fixed_per_run": True},
        "mesh": {"nx0": 3, "ny0": 2, "L": 0, "padded_cells": 6, "nz": 4,
                 "mesh_sha256": hashlib.sha256(b"synthetic fvm-io/1.0 mesh").hexdigest()},
        "fields": fields,
        "summary": [{"name": name, "value": value, "unit": unit, "method": method, "step": 0}
                    for name, value, unit, method in [
                        ("warpage_pv", 1e-6, "m", "FVM top-face centroids; least-squares plane removal"),
                        ("T_max", 301.0, "K", "FVM cell centers"),
                        ("U_mag_max", 2e-6, "m", "FVM cell centers")]]})
    prepublication = json.loads((out / "manifest_fvm_solid/manifest.json").read_text())
    prepublication.update(steps=[], summary=[])
    prepublication["partitions"]["per_step"] = []
    write(out, "manifest_fvm_solid/prepublication.json", prepublication)
    mesh = {"axes": [[0, 1, 3], [0, 1], [0, 1, 2]],
            "attr_table": [{"layer": "copper_a"}, {"layer": "copper_b"}],
            "copper_layer_of_layer": [0, 1], "copper_phi": [0.2, 0.3, 0.4, 0.5]}
    boundaries = []
    # Outcomes are opaque contract data, independent of the checker classifier.
    for name, patch, code in (
            ("valid_mesh", {}, None),
            ("no_copper", {"copper_layer_of_layer": [], "copper_phi": []}, None),
            ("nz_zero", {"axes": [[0, 1, 3], [0, 1], [0]]}, "INPUT_INVALID"),
            ("empty_attr_table", {"attr_table": []}, "INPUT_INVALID"),
            ("copper_short", {"copper_phi": [0.2, 0.3, 0.4]}, "INPUT_INVALID"),
            ("copper_long", {"copper_phi": [0.2, 0.3, 0.4, 0.5, 0.6]}, "INPUT_INVALID")):
        inp = copy.deepcopy(mesh)
        inp.update(patch)
        boundaries.append({"name": name, "input": inp, "expected_code": code})
    write(out, "mesh_boundaries.json", boundaries)
    base = {"schema": "chipcae.case/1.0", "stack": {"ref": "stack.json"},
            "materials": {"ref": "materials.json"},
            "mesh": {"type": "aligned_cartesian_cut", "nx": 8, "ny": 8, "levels": 1, "max_ratio": 10},
            "physics": [{"id": "thermal", "solver": "fvm.conduction", "mode": "steady"},
                        {"id": "mech", "solver": "fvm.solid", "temp_from": "thermal", "T_ref": 300,
                         "nonlinear": {"geometric": False}}],
            "loads": [{"type": "fixed_temperature", "target": "face:zmin", "T": 300},
                      {"type": "heat_source", "target": "layer:heater", "power": 10},
                      {"type": "convection", "target": "face:zmax", "h": 10, "T_inf": 300}],
            "constraints": {"rigid": "3-2-1"}, "solver": {"linear": {"rtol": 1e-10}},
            "output": {"format": "vtkhdf", "warpage": {"standard": "JESD22-B112",
                       "remove_rigid": "best_fit_plane"}, "fields": [
                           "T", "U", "layer_id", "material_id", "cell_kind", "elem_gid", "volume"]}}
    outcomes = {}
    for name, pc in [("valid_fvm", None), ("valid_fvm_gamg", "gamg"), ("mixed_fem_fvm", None),
                     ("fvm_direct", "direct"), ("fvm_column_gmg", "column_gmg"),
                     ("solver_case", None)]:
        case, error = copy.deepcopy(base), None
        if pc:
            case["solver"]["linear"]["preconditioner"] = pc
        if name == "solver_case":
            case["physics"][0]["solver"] = "FVM.Conduction"
            error = {"code": "INPUT_INVALID", "message": "invalid or duplicate physics"}
        elif name == "mixed_fem_fvm":
            case["physics"][1]["solver"] = "fem.solid"
            error = {"code": "INPUT_INVALID", "message": "mixed fem/fvm physics not supported in case/1.0"}
        elif pc in ("direct", "column_gmg"):
            error = {"code": "INPUT_INVALID", "message": f"preconditioner {pc} not supported by fvm"}
        outcomes[name] = error
        write(out, f"cases/{name}.json", case)
    write(out, "expected.json", {"fields": fields, "cases": outcomes})
    source = Path(__file__).resolve().parent
    for script in ("gen.py", "check.py"):
        content = (source / script).read_bytes()
        (out / script).write_bytes(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try:
        args.out.mkdir(parents=True, exist_ok=True)
        generate(args.out)
    except (OSError, ImportError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
