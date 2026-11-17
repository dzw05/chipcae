#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Build and run the WP-2 trilayer CalculiX reference (C3D20). Results are REFERENCE, not measured."""
from __future__ import annotations

import json
import os
import platform
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
# This repo-local script imports the shared QoI implementation from the checkout.
sys.path.insert(0, str(REPO))
from tools.vv.calculix_trilayer.contract import validate_reference  # noqa: E402
from tools.vv.warpage_qoi import interp_grid, qoi_surface  # noqa: E402

L = 1.0
THICK = [0.05, 0.01, 0.04]
H = sum(THICK)
E = [25e9, 3e9, 130e9]
NU = [0.2, 0.35, 0.28]
ALPHA = [15e-6, 50e-6, 2.6e-6]
DT = -100.0
T0 = 400.0
T1 = 300.0
Z_PER_LAYER = [2, 2, 2]  # same for coarse and fine
MESHES = {
    "coarse": {"nx": 8, "ny": 8},
    "fine": {"nx": 16, "ny": 16},
}
PROTOTYPE = 4.269532769e-3
CCX = os.environ.get("CHIPCAE_CCX", "ccx")


def node_id(i, j, k, nx, ny, nz):
    """Lexicographic id, 1-based, of a tensor-product Lagrange-quadratic point.

    There are (2*nx+1, 2*ny+1, 2*nz+1) nodes.
    """
    mx, my = 2 * nx + 1, 2 * ny + 1
    return 1 + i + mx * (j + my * k)


def build_mesh(nx, ny):
    nz = sum(Z_PER_LAYER)
    xs = np.linspace(0.0, L, 2 * nx + 1)
    ys = np.linspace(0.0, L, 2 * ny + 1)
    z_planes = [0.0]
    for t, n in zip(THICK, Z_PER_LAYER):
        dz = t / n
        for _ in range(n):
            z_planes.append(z_planes[-1] + dz)
    # quadratic nodes sit mid-way, so z node coordinates are half-steps
    zs = []
    for p in range(len(z_planes) - 1):
        zs.append(z_planes[p])
        zs.append(0.5 * (z_planes[p] + z_planes[p + 1]))
    zs.append(z_planes[-1])
    zs = np.array(zs)
    assert len(zs) == 2 * nz + 1

    coords = {}
    for k, z in enumerate(zs):
        for j, y in enumerate(ys):
            for i, x in enumerate(xs):
                coords[node_id(i, j, k, nx, ny, nz)] = (x, y, z)

    elements = []  # (eid, mid, n20)
    eid = 1
    z_elem = 0
    for li, nze in enumerate(Z_PER_LAYER):
        for _ in range(nze):
            k0 = 2 * z_elem  # corner node k index
            for je in range(ny):
                for ie in range(nx):
                    i0, j0 = 2 * ie, 2 * je
                    def N(ii, jj, kk):
                        return node_id(ii, jj, kk, nx, ny, nz)
                    # corners 1-8
                    n1 = N(i0, j0, k0)
                    n2 = N(i0 + 2, j0, k0)
                    n3 = N(i0 + 2, j0 + 2, k0)
                    n4 = N(i0, j0 + 2, k0)
                    n5 = N(i0, j0, k0 + 2)
                    n6 = N(i0 + 2, j0, k0 + 2)
                    n7 = N(i0 + 2, j0 + 2, k0 + 2)
                    n8 = N(i0, j0 + 2, k0 + 2)
                    # midsides
                    n9 = N(i0 + 1, j0, k0)
                    n10 = N(i0 + 2, j0 + 1, k0)
                    n11 = N(i0 + 1, j0 + 2, k0)
                    n12 = N(i0, j0 + 1, k0)
                    n13 = N(i0 + 1, j0, k0 + 2)
                    n14 = N(i0 + 2, j0 + 1, k0 + 2)
                    n15 = N(i0 + 1, j0 + 2, k0 + 2)
                    n16 = N(i0, j0 + 1, k0 + 2)
                    n17 = N(i0, j0, k0 + 1)
                    n18 = N(i0 + 2, j0, k0 + 1)
                    n19 = N(i0 + 2, j0 + 2, k0 + 1)
                    n20 = N(i0, j0 + 2, k0 + 1)
                    n20s = (n1, n2, n3, n4, n5, n6, n7, n8, n9, n10, n11, n12,
                            n13, n14, n15, n16, n17, n18, n19, n20)
                    elements.append((eid, li + 1, n20s))
                    eid += 1
            z_elem += 1
    return coords, elements, nz


def write_inp(path: Path, name: str, nx, ny):
    coords, elements, nz = build_mesh(nx, ny)
    lines = ["*HEADING", f" chipcae WP-2 trilayer {name} C3D20 REFERENCE (not measured)", "*NODE"]
    for nid in sorted(coords):
        x, y, z = coords[nid]
        lines.append(f"{nid}, {x:.12g}, {y:.12g}, {z:.12g}")
    for mid in (1, 2, 3):
        lines.append(f"*ELEMENT, TYPE=C3D20, ELSET=LAY{mid}")
        for eid, m, nodes in elements:
            if m != mid:
                continue
            a = ",".join(str(n) for n in nodes[:10])
            b = ",".join(str(n) for n in nodes[10:])
            lines.append(f"{eid}, {a},")
            lines.append(f"{b}")
    # bottom corner nodes for 3-2-1 (z=0)
    n_xmin_ymin = node_id(0, 0, 0, nx, ny, nz)
    n_xmax_ymin = node_id(2 * nx, 0, 0, nx, ny, nz)
    n_xmin_ymax = node_id(0, 2 * ny, 0, nx, ny, nz)
    lines += [
        f"*NSET, NSET=C321_3",
        f"{n_xmin_ymin}",
        f"*NSET, NSET=C321_2",
        f"{n_xmax_ymin}",
        f"*NSET, NSET=C321_1",
        f"{n_xmin_ymax}",
    ]
    # top surface (all z=H nodes, including midsides)
    top = [nid for nid, (_, _, z) in coords.items() if z >= H - 1e-12 * H]
    lines.append("*NSET, NSET=TOP")
    for i in range(0, len(top), 16):
        lines.append(",".join(str(n) for n in top[i:i + 16]))
    lines.append("*NSET, NSET=NALL, GENERATE")
    lines.append(f"1, {len(coords)}")
    for mid, (e, nu, al) in enumerate(zip(E, NU, ALPHA), start=1):
        lines += [
            f"*MATERIAL, NAME=MAT{mid}",
            "*ELASTIC",
            f"{e:.8g}, {nu:.8g}",
            "*EXPANSION",
            f"{al:.8g}",
            f"*SOLID SECTION, ELSET=LAY{mid}, MATERIAL=MAT{mid}",
        ]
    lines += [
        "*BOUNDARY",
        "C321_3, 1, 3",  # ux uy uz at xmin/ymin/z=0
        "C321_2, 2, 3",  # uy uz at xmax/ymin/z=0
        "C321_1, 3, 3",  # uz at xmin/ymax/z=0
        "*INITIAL CONDITIONS, TYPE=TEMPERATURE",
        f"NALL, {T0:.8g}",
        "*STEP",
        "*STATIC",
        "*TEMPERATURE",
        f"NALL, {T1:.8g}",
        "*NODE FILE, NSET=TOP",
        "U",
        "*END STEP",
    ]
    path.write_text("\n".join(lines) + "\n")


def parse_frd(path: Path):
    """Read ASCII FRD (1X,I2,I10,3E12.5) nodes and DISP; see check_frd_parser.py."""
    nodes = {}  # nid -> (x,y,z)
    disp = {}
    text = path.read_text(errors="replace")
    mode = None
    for line in text.splitlines():
        key = line.lstrip()
        if key.startswith("-1") and mode in {"coords", "disp"}:
            nid = int(line[3:13])
            a = float(line[13:25])
            b = float(line[25:37])
            c = float(line[37:49])
            if mode == "coords":
                nodes[nid] = (a, b, c)
            else:
                disp[nid] = (a, b, c)
        elif key.startswith("-3"):
            mode = None
        elif line.startswith("    2C"):
            mode = "coords"
        elif key.startswith("-4") and "DISP" in line:
            mode = "disp"
    return nodes, disp


def w_bfp_from_frd(frd: Path) -> float:
    nodes, disp = parse_frd(frd)
    if not disp:
        raise RuntimeError(f"no DISP records parsed from {frd}")
    rows = []
    for nid, (_ux, _uy, uz) in disp.items():
        if nid not in nodes:
            continue
        x, y, z = nodes[nid]
        # top surface only (generator requested NSET=TOP, but keep the geometric filter)
        if z < H - 1e-12 * H:
            continue
        rows.append((x * 1000.0, y * 1000.0, uz * 1000.0))  # mm
    if not rows:
        raise RuntimeError("no top-surface nodes in DISP")
    P = np.array(rows, dtype=float)
    # P for interp_grid is x,y,z,ux,uy,uz — z unused, ux uy unused
    Pall = np.column_stack([P[:, 0], P[:, 1], np.zeros(len(P)), np.zeros(len(P)), np.zeros(len(P)), P[:, 2]])
    xs = ys = np.linspace(0.0, 1000.0, 81)
    X, Y, W = interp_grid(Pall, xs, ys)
    q = qoi_surface(X, Y, W)
    return q["W_bfp"] / 1000.0  # back to metres


def ccx_version() -> str:
    r = subprocess.run([CCX, "-v"], capture_output=True, text=True)
    out = (r.stdout or r.stderr or "").strip() or f"exit {r.returncode}"
    return out.splitlines()[0][:120]


def run_mesh(workdir: Path, name: str):
    nx, ny = MESHES[name]["nx"], MESHES[name]["ny"]
    inp = workdir / f"{name}.inp"
    write_inp(inp, name, nx, ny)
    # ccx expects the jobname without extension, and CWD = workdir
    job = name
    r = subprocess.run([CCX, job], cwd=workdir, capture_output=True, text=True)
    (workdir / f"{name}.ccx.stdout").write_text(r.stdout)
    (workdir / f"{name}.ccx.stderr").write_text(r.stderr)
    if r.returncode != 0:
        raise RuntimeError(f"ccx {name} failed: {r.stderr[-2000:]}\n{r.stdout[-2000:]}")
    frd = workdir / f"{name}.frd"
    if not frd.exists():
        raise RuntimeError(f"missing {frd}")
    w = w_bfp_from_frd(frd)
    return {
        "name": name,
        "n_elem_xyz": [nx, ny, sum(Z_PER_LAYER)],
        "n_elem_z_per_layer": list(Z_PER_LAYER),
        "W_bfp_m": w,
    }


def main():
    here = Path(__file__).resolve().parent
    work = here / "work"
    work.mkdir(exist_ok=True)
    results = [run_mesh(work, name) for name in ("coarse", "fine")]
    selected = next(r for r in results if r["name"] == "fine")
    w = selected["W_bfp_m"]
    abs_delta = abs(w - PROTOTYPE)
    rel = abs_delta / PROTOTYPE
    ver = ccx_version()
    payload = {
        "kind": "REFERENCE",
        "label": "参考值（原型计算值）对照用的独立 CalculiX 结果，不是实测，不是解析解",
        "W_bfp_m": w,
        "element_type": "C3D20",
        "calculix_version": ver,
        "host": f"box {platform.system()} {platform.machine()}",
        "geometry": {
            "L_m": L,
            "thicknesses_m": THICK,
            "E_Pa": E,
            "nu": NU,
            "alpha_1_per_K": ALPHA,
            "dT_K": DT,
        },
        "meshes": results,
        "selected_mesh": "fine",
        "prototype_W_bfp_m": PROTOTYPE,
        "abs_delta_m": abs_delta,
        "rel_delta": rel,
        "provenance": {
            "generator": "tools/vv/calculix_trilayer/generate.py",
            "label": "参考值（原型计算值）对照用的独立 CalculiX 结果，不是实测，不是解析解",
        },
    }
    validate_reference(payload)
    out = here / "REFERENCE.json"
    out.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
