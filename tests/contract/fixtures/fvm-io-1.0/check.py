#!/usr/bin/env python3
"""Acceptance tests for the synthetic fvm-io/1.0 package, not the product parser.

Exit status: 0 pass, 1 contract mismatch, 2 usage/I/O/dependency error.
Input format is described in the contract test README. No generator is imported:
reference temperatures are independently assembled from the fixture inputs.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
REQUIRED = {
    "gen.py", "check.py", "expected.json",
    "laminate_1d/input.json", "laminate_1d/expected.json",
    "hex_kdiag/input.json", "hex_kdiag/expected.json",
    "manifest_fvm_solid/manifest.json",
    *(f"cases/{name}.json" for name in (
        "valid_fvm", "valid_fvm_gamg", "mixed_fem_fvm", "fvm_direct", "fvm_column_gmg")),
}
CASE_ERRORS = {
    "valid_fvm": None,
    "valid_fvm_gamg": None,
    "mixed_fem_fvm": {"code": "INPUT_INVALID",
                      "message": "mixed fem/fvm physics not supported in case/1.0"},
    "fvm_direct": {"code": "INPUT_INVALID",
                   "message": "preconditioner direct not supported by fvm"},
    "fvm_column_gmg": {"code": "INPUT_INVALID",
                       "message": "preconditioner column_gmg not supported by fvm"},
}
FIELD_TYPES = {
    "T": ("point", 1, "float64"), "U": ("point", 3, "float64"),
    "layer_id": ("cell", 1, "int32"), "material_id": ("cell", 1, "int32"),
    "cell_kind": ("cell", 1, "int32"), "elem_gid": ("cell", 1, "int64"),
    "volume": ("cell", 1, "float64"), "copper_fraction": ("cell", 1, "float64"),
    "conductivity_diag": ("cell", 3, "float64"), "T_cell": ("cell", 1, "float64"),
}
SUMMARY = {
    "warpage_pv": ("m", "FVM top-face centroids; least-squares plane removal"),
    "T_max": ("K", "FVM cell centers"), "U_mag_max": ("m", "FVM cell centers"),
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def finite(value):
    require(type(value) in (int, float) and math.isfinite(value),
            f"expected finite number, got {value!r}")
    return value


def reject_constant(value):
    raise AssertionError(f"nonfinite JSON constant: {value}")


def read(path):
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=reject_constant)


def named(rows, key="name"):
    result = {}
    for row in rows:
        if key in ("id", "n"):
            require(type(row[key]) is int and row[key] >= 0, f"invalid {key}: {row[key]}")
        require(row[key] not in result, f"duplicate {key}: {row[key]}")
        result[row[key]] = row
    return result


def temperatures(document, count):
    cells = named(document["cells"], "id")
    require(set(cells) == set(range(count)), "missing/unexpected temperature cell IDs")
    return [finite(cells[c]["T_K"]) for c in range(count)]


def reference(inp):
    """Cell-centered TPFA with insulated lateral faces, bottom T and top inward q."""
    import numpy as np

    widths = [np.diff(inp[f"{axis}_m"]) for axis in "xyz"]
    require(all(len(w) > 0 and np.all(np.isfinite(w)) and np.all(w > 0)
                for w in widths), "axis coordinates must be finite and increasing")
    nx, ny, nz = shape = tuple(len(w) for w in widths)
    cells = named(inp["cells"], "id")
    count = nx * ny * nz
    require(set(cells) == set(range(count)), "missing/unexpected input cell IDs")
    matrix, rhs = np.zeros((count, count)), np.zeros(count)
    bottom = finite(inp["boundary"]["bottom_T_K"])
    flux = finite(inp["boundary"]["top_q_in_W_m2"])
    require(flux > 0 and inp["boundary"]["sides"] == "insulated", "boundary conditions")
    conductivities = []
    for c in range(count):
        ijk = (c % nx, (c // nx) % ny, c // (nx * ny))
        require(cells[c]["ijk"] == list(ijk), f"cell {c}: ordering c=((k*ny)+j)*nx+i")
        kd = cells[c]["conductivity_diag"]
        require(len(kd) == 3 and all(finite(k) > 0 for k in kd),
                f"cell {c}: conductivity must have three positive components")
        conductivities.append(kd)
    bottom_faces, top_areas = [], []
    for c in range(count):
        ijk = cells[c]["ijk"]
        for d, stride in enumerate((1, nx, nx * ny)):
            area = math.prod(widths[e][ijk[e]] for e in range(3) if e != d)
            half = widths[d][ijk[d]] / 2
            if ijk[d] + 1 < shape[d]:
                other = c + stride
                resistance = (half / conductivities[c][d]
                              + widths[d][ijk[d] + 1] / (2 * conductivities[other][d]))
                g = area / resistance
                matrix[c, c] += g
                matrix[other, other] += g
                matrix[c, other] -= g
                matrix[other, c] -= g
            if d == 2 and ijk[d] == 0:
                g = conductivities[c][d] * area / half
                matrix[c, c] += g
                rhs[c] += g * bottom
                bottom_faces.append((c, g))
            if d == 2 and ijk[d] == nz - 1:
                rhs[c] += flux * area
                top_areas.append(area)
    solved = np.linalg.solve(matrix, rhs)
    require(np.all(np.isfinite(solved)), "nonfinite reference temperature")
    q_in = flux * sum(top_areas)
    q_out = sum(g * (solved[c] - bottom) for c, g in bottom_faces)
    require(q_in > 0 and abs(q_in - q_out) / q_in <= 1e-9,
            f"energy conservation: Q_in={q_in}, Q_out={q_out}")
    return solved


def numerical(package):
    inp = read(package / "laminate_1d/input.json")
    layers = inp["layers"]
    require(len(layers) == 3, "laminate: exactly three layers")
    for layer, h, k in zip(layers, (0.001, 0.002, 0.003), (1, 2, 4)):
        require(abs(finite(layer["thickness_m"]) - h) <= 1e-16, "laminate thickness")
        require(abs(finite(layer["conductivity_W_mK"]) - k) <= 1e-16,
                "laminate conductivity")
    require(abs(inp["boundary"]["bottom_T_K"] - 300) <= 1e-14
            and abs(inp["boundary"]["top_q_in_W_m2"] - 100) <= 1e-14,
            "laminate: bottom 300 K, top inward flux 100 W/m2")
    require(len(inp["x_m"]) == 2 and len(inp["y_m"]) == 2, "laminate: one column")
    faces = (0, 0.0005, 0.001, 0.002, 0.003, 0.0045, 0.006)
    require(len(inp["z_m"]) == len(faces)
            and all(abs(finite(a) - b) <= 1e-16 for a, b in zip(inp["z_m"], faces)),
            "laminate: two equal cells per layer with aligned interfaces")
    computed = reference(inp)
    require(len(computed) == 6, "laminate: six cells")
    golden = temperatures(read(package / "laminate_1d/expected.json"), 6)
    literal = (300.025, 300.075, 300.125, 300.175, 300.21875, 300.25625)
    require(all(abs(actual - expected) <= 1e-10 for actual, expected in zip(golden, literal)),
            "laminate: stored temperatures differ from hand-derived values")
    centers, counts = set(), [0, 0, 0]
    for cell in inp["cells"]:
        kindex = cell["ijk"][2]
        z = (inp["z_m"][kindex] + inp["z_m"][kindex + 1]) / 2
        centers.add(z)
        resistance, start, layer_index = 0.0, 0.0, None
        for index, layer in enumerate(layers):
            h, k = layer["thickness_m"], layer["conductivity_W_mK"]
            resistance += max(0, min(z - start, h)) / k
            if start < z < start + h:
                layer_index = index
            start += h
        require(layer_index is not None, "laminate center outside layers")
        counts[layer_index] += 1
        k = layers[layer_index]["conductivity_W_mK"]
        require(all(abs(component - k) <= 1e-14 for component in cell["conductivity_diag"]),
                "laminate input conductivity disagrees with layer")
        analytic = 300 + 100 * resistance
        c = cell["id"]
        require(abs(computed[c] - analytic) <= 1e-10
                and abs(golden[c] - analytic) <= 1e-10,
                f"laminate cell {c}: reference/expected differs from analytic temperature")
    require(len(centers) == 6 and min(counts) >= 2, "laminate: six unique centers, two per layer")
    inp = read(package / "hex_kdiag/input.json")
    require([len(inp[f"{a}_m"]) - 1 for a in "xyz"] == [3, 2, 4], "hex: 3x2x4")
    for axis in "xyz":
        w = [b - a for a, b in zip(inp[f"{axis}_m"], inp[f"{axis}_m"][1:])]
        require(max(w) - min(w) > 1e-14, f"hex: nonuniform {axis} spacing")
    cells = named(inp["cells"], "id")
    require(set(cells) == set(range(24)), "hex: exactly 24 cells")
    require(any(max(c["conductivity_diag"]) - min(c["conductivity_diag"]) > 1e-14
                for c in cells.values()), "hex: anisotropic conductivity required")
    for stride in (1, 3, 6):
        require(any(abs(a - b) > 1e-14 for a, b in zip(
            cells[0]["conductivity_diag"], cells[stride]["conductivity_diag"])),
                "hex: conductivity must vary with i, j and k")
    computed = reference(inp)
    golden = temperatures(read(package / "hex_kdiag/expected.json"), 24)
    for c, (actual, expected) in enumerate(zip(computed, golden)):
        require(abs(expected) > 0 and abs(actual - expected) / abs(expected) <= 1e-12,
                f"hex cell {c}: recomputed temperature {actual} != expected {expected}")


def manifest(package, repo):
    doc = read(package / "manifest_fvm_solid/manifest.json")
    try:
        import jsonschema
    except ImportError:
        print("SKIP-SCHEMA: jsonschema unavailable; checking required structures")
    else:
        schema = read(repo / "schema/manifest-1.schema.json")
        jsonschema.validators.validator_for(schema)(schema).validate(doc)
    require(doc["schema_version"] == "chipcae.results/1.0", "manifest version")
    steps = doc["steps"]
    require(isinstance(steps, list) and steps, "completed output requires nonempty steps")
    named(steps, "n")
    for step in steps:
        require(type(step["n"]) is int and step["n"] >= 0
                and isinstance(step["file"], str) and step["file"], "step metadata")
    partitions = doc["partitions"]
    require(type(partitions["solver_fixed_per_run"]) is bool, "partition fixed flag")
    per_step = partitions["per_step"]
    require(len(per_step) == len(steps) and all(type(n) is int and n > 0 for n in per_step),
            "partition counts must match steps")
    if partitions["solver_fixed_per_run"]:
        require(len(set(per_step)) == 1, "fixed partitions changed")
    mesh = doc["mesh"]
    for key in ("nx0", "ny0", "L", "padded_cells", "nz"):
        require(type(mesh[key]) is int and mesh[key] >= 0, f"mesh.{key}")
    require(re.fullmatch(r"[0-9a-f]{64}", mesh["mesh_sha256"]), "mesh SHA-256")
    fields = named(doc["fields"])
    metadata = named(read(package / "expected.json")["fields"])
    required = set(FIELD_TYPES) - {"T_cell"}
    require(required <= fields.keys(), f"missing required fields: {required - fields.keys()}")
    active = {name for name, field in fields.items() if not field.get("reserved", False)}
    require(active in (required, required | {"T_cell"}), "unexpected active field names")
    require(set(metadata) == set(FIELD_TYPES), "expected field metadata incomplete")
    # Diagnostic unit strings await an architect ruling; the package must declare them.
    units = {"T": "K", "U": "m", "T_cell": "K", "volume": "m3",
             "layer_id": "1", "material_id": "1", "cell_kind": "1", "elem_gid": "1"}
    for name, spec in FIELD_TYPES.items():
        for field in ([metadata[name], fields[name]] if name in fields else [metadata[name]]):
            require(type(field["components"]) is int, f"{name}: integer component count")
            require(tuple(field[k] for k in ("location", "components", "dtype")) == spec,
                    f"{name}: location/components/dtype")
            require(isinstance(field["unit"], str) and field["unit"], f"{name}: unit required")
            require(field["unit"] == metadata[name]["unit"], f"{name}: unit mismatch")
            if name in units:
                require(field["unit"] == units[name], f"{name}: SI unit")
            require(not field.get("reserved", False), f"{name}: required field is reserved")
    for row in doc["summary"]:
        finite(row["value"])
    for step in steps:
        rows = named([s for s in doc["summary"] if s.get("step", step["n"]) == step["n"]])
        for name, (unit, method) in SUMMARY.items():
            require(name in rows and rows[name]["unit"] == unit
                    and rows[name]["method"] == method, f"summary {name}: unit/method")


def cases(package):
    outcomes = read(package / "expected.json")["cases"]
    require(outcomes == CASE_ERRORS, "case expected error code/message mismatch")
    for name, expected in CASE_ERRORS.items():
        doc = read(package / f"cases/{name}.json")
        require(doc["schema"] == "chipcae.case/1.0", f"{name}: case version")
        physics = named(doc["physics"], "id")
        require(set(physics) == {"thermal", "mech"}, f"{name}: thermal and mech required")
        solvers = {p["solver"] for p in physics.values()}
        require(solvers <= {"fem.conduction", "fem.solid", "fvm.conduction", "fvm.solid"},
                f"{name}: unknown solver")
        pc = doc["solver"]["linear"].get("preconditioner", "jacobi")
        families = {s.split(".")[0] for s in solvers}
        require(families == ({"fem", "fvm"} if name == "mixed_fem_fvm" else {"fvm"}),
                f"{name}: unexpected solver family")
        require(physics["thermal"]["solver"].endswith(".conduction")
                and physics["mech"]["solver"].endswith(".solid"), f"{name}: physics roles")
        actual = None
        if len(families) > 1:
            actual = CASE_ERRORS["mixed_fem_fvm"]
        elif pc in ("direct", "column_gmg"):
            actual = {"code": "INPUT_INVALID", "message": f"preconditioner {pc} not supported by fvm"}
        else:
            require(pc in ("jacobi", "gamg"), f"{name}: unsupported preconditioner")
        require(actual == expected, f"{name}: contract outcome {actual} != {expected}")
        if expected is None:
            require(solvers == {"fvm.conduction", "fvm.solid"}, f"{name}: FVM pair")
            require(pc == ("gamg" if name.endswith("gamg") else "jacobi"), f"{name}: preconditioner")


def validate(package, repo):
    missing = sorted(relative for relative in REQUIRED if not (package / relative).is_file())
    require(not missing, f"{package.name}: missing required fixture files: {', '.join(missing)}")
    for relative in sorted(REQUIRED):
        path = package / relative
        require("\r" not in path.read_bytes().decode("utf-8"), f"{relative}: LF newlines required")
    numerical(package)
    manifest(package, repo)
    cases(package)


def hashes(package):
    return {p.relative_to(package).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(package.rglob("*")) if p.is_file()}


def generator_acceptance(package):
    with tempfile.TemporaryDirectory(prefix="fvm-io-gen-") as temp:
        first, second = Path(temp) / "first", Path(temp) / "second"
        for out in (first, second, first):
            result = subprocess.run([sys.executable, str(package / "gen.py"), "--out", str(out)],
                                    capture_output=True, text=True)
            require(result.returncode == 0, f"gen.py --out: {result.stdout}\n{result.stderr}")
            require(hashes(out) == hashes(package), "generator file set/SHA-256 mismatch")


def mutation_acceptance(package, repo):
    """Exercise checker rejection and field-order/optional-field compatibility on copies."""
    mutations = (
        ("hex_kdiag/expected.json", lambda d: d["cells"][0].update(T_K=1.0), False),
        ("manifest_fvm_solid/manifest.json", lambda d: next(s for s in d["summary"]
            if s["name"] == "T_max").update(method="wrong"), False),
        ("manifest_fvm_solid/manifest.json", lambda d: d["fields"].remove(
            next(f for f in d["fields"] if f["name"] == "T")), False),
        ("expected.json", lambda d: d["cases"]["fvm_direct"].update(message="invalid preconditioner"), False),
        ("manifest_fvm_solid/manifest.json", lambda d: d["fields"].append(
            dict(d["fields"][0])), False),
        ("manifest_fvm_solid/manifest.json", lambda d: d["fields"].reverse(), True),
        ("manifest_fvm_solid/manifest.json", lambda d: next(s for s in d["summary"]
            if s["name"] == "T_max").update(value=301.234), True),
        ("manifest_fvm_solid/manifest.json", lambda d: d.update(
            fields=[f for f in d["fields"] if f["name"] != "T_cell"]), True),
    )
    for relative, mutate, accepted in mutations:
        with tempfile.TemporaryDirectory(prefix="fvm-io-mutate-") as temp:
            copy = Path(temp) / "package"
            shutil.copytree(package, copy)
            path = copy / relative
            doc = read(path)
            mutate(doc)
            path.write_text(json.dumps(doc, allow_nan=False) + "\n", encoding="utf-8")
            try:
                validate(copy, repo)
            except AssertionError:
                require(not accepted, f"compatible mutation rejected: {relative}")
            else:
                require(accepted, f"invalid mutation accepted: {relative}")


def optional_schema_acceptance(package, repo):
    """Simulate an unavailable optional dependency in a separate interpreter."""
    code = (
        "import runpy, sys; from pathlib import Path; "
        "sys.modules['jsonschema'] = None; "
        "checker = runpy.run_path(sys.argv[1]); "
        "checker['manifest'](Path(sys.argv[2]), Path(sys.argv[3]))"
    )
    with tempfile.TemporaryDirectory(prefix="fvm-io-no-schema-") as temp:
        copy = Path(temp) / "package"
        shutil.copytree(package, copy)
        for missing_T in (False, True):
            if missing_T:
                path = copy / "manifest_fvm_solid/manifest.json"
                doc = read(path)
                doc["fields"] = [f for f in doc["fields"] if f["name"] != "T"]
                path.write_text(json.dumps(doc, allow_nan=False) + "\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, "-c", code, str(copy / "check.py"), str(copy), str(repo)],
                capture_output=True, text=True)
            require("SKIP-SCHEMA" in result.stdout,
                    f"optional schema diagnostic missing: {result.stdout} {result.stderr}")
            require((result.returncode == 0) == (not missing_T),
                    f"without jsonschema: missing_T={missing_T}: {result.stdout} {result.stderr}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--handoff", type=Path, help="also compare the handoff package SHA-256 map")
    args = parser.parse_args()
    try:
        import numpy  # Required, unlike the optional jsonschema validator.
        validate(HERE, args.repo)
        generator_acceptance(HERE)
        mutation_acceptance(HERE, args.repo)
        optional_schema_acceptance(HERE, args.repo)
        if args.handoff is not None:
            require(hashes(args.handoff) == hashes(HERE), "repository/handoff SHA-256 mismatch")
    except (OSError, ImportError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("fvm-io/1.0 acceptance OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
