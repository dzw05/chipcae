#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Validate and summarize the first step of a chipcae results directory.

Duplicate top coordinates must have identical displacement and available
temperature values. Temperature uses blocks providing T; absent samples have
null statistics (including top_* when no global top point provides T). The whole
temperature field is null only when no block provides T. Numeric
field types accept either byte order, retaining the required kind and width.
"""
import argparse
import json
from pathlib import Path
import sys

import h5py
import numpy as np

# Direct CLI execution needs the repository root to import the shared VV helper.
if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.vv.warpage_qoi import best_fit_plane

SCHEMA = "chipcae.vv.result_summary/0.1"


class ResultSummaryError(Exception):
    def __init__(self, code: int, name: str, message: str):
        super().__init__(message)
        self.code = code
        self.name = name


def _require(condition, message):
    if not condition:
        raise ResultSummaryError(14, "INPUT_INVALID", message)


def _count(block, path):
    source = block[path]
    _require(isinstance(source, h5py.Dataset)
             and source.ndim == 1
             and source.dtype.newbyteorder('=') == np.dtype('int64'),
             f"{block.name}/{path}: expected int64 partition counts")
    values = source[...]
    _require(values.ndim == 1
             and np.all(values >= 0), f"{block.name}/{path}: invalid partition counts")
    return sum(int(v) for v in values)


def _array(block, path, shape, dtype):
    _require(path in block, f"{block.name}/{path}: required field missing")
    source = block[path]
    _require(isinstance(source, h5py.Dataset) and source.shape == shape, f"{block.name}/{path}: expected shape {shape}")
    values = source[...]
    _require(values.dtype.newbyteorder('=') == np.dtype(dtype),
             f"{block.name}/{path}: expected {dtype}")
    if values.dtype.kind == 'f':
        _require(np.all(np.isfinite(values)), f"{block.name}/{path}: nonfinite values")
    return values


def _partitions(block, cells_path='NumberOfCells'):
    """Check rank-local topology against the allocations used by all fields.

    Points and fields share concatenated tuple slices, with no independent
    per-field partition metadata. Topology provides the independent check:
    offsets restart at zero and connectivity IDs are local to each point slice.
    Minimal field-only inputs can omit topology altogether.
    """
    topology = block if cells_path == 'NumberOfCells' else block['Polygons']
    paths = ('NumberOfConnectivityIds', 'Offsets', 'Connectivity')
    if not any(path in topology for path in paths):
        return
    _require(all(path in topology for path in paths),
             f'{topology.name}: incomplete partition topology')
    total = _count(topology, 'NumberOfConnectivityIds')
    pc = block['NumberOfPoints'][...]
    cc = block[cells_path][...]
    nc = topology['NumberOfConnectivityIds'][...]
    _require(nc.shape == pc.shape, f'{topology.name}: partition count mismatch')
    offsets = _array(topology, 'Offsets', (sum(int(c) + 1 for c in cc),), 'int64')
    ids = _array(topology, 'Connectivity', (total,), 'int64')
    oi = ci = 0
    for rank, (points, cells, connections) in enumerate(zip(pc, cc, nc)):
        points, cells, connections = int(points), int(cells), int(connections)
        off = offsets[oi:oi + cells + 1]
        local_ids = ids[ci:ci + connections]
        _require(off[0] == 0 and off[-1] == connections
                 and np.all(off[1:] >= off[:-1])
                 and np.all((local_ids >= 0) & (local_ids < points)),
                 f'{topology.name}: partition {rank} geometry/field allocation mismatch')
        oi += cells + 1
        ci += connections


def _version(block):
    _require(isinstance(block, h5py.Group), f"{block.name}: expected VTKHDF group")
    _require(np.array_equal(block.attrs.get('Version'), [2, 8]),
             f"{block.name}: expected Version [2, 8]")


def _counts(values):
    keys, counts = np.unique(values, return_counts=True)
    return {str(int(k)): int(n) for k, n in zip(keys, counts)}


def _read(root):
    _version(root)
    blocks, data = {}, []
    for name in ('hex', 'cut'):
        _require(name in root and isinstance(root[name], h5py.Group),
                 f'{root.name}/{name}: required group missing or invalid')
        block = root[name]
        _version(block)
        n = _count(block, 'NumberOfPoints')
        c = _count(block, 'NumberOfCells')
        _require(block['NumberOfPoints'].shape == block['NumberOfCells'].shape,
                 f"{name}: partition count mismatch")
        points = _array(block, 'Points', (n, 3), 'float64')
        u = (_array(block, 'PointData/U', (n, 3), 'float64')
             if n or 'PointData/U' in block else np.empty((0, 3)))
        t = (_array(block, 'PointData/T', (n,), 'float64')
             if 'PointData/T' in block else None)
        fields = {}
        for field in ('layer_id', 'material_id', 'cell_kind', 'elem_gid'):
            path = 'CellData/' + field
            if c or path in block:
                fields[field] = _array(block, path, (c,),
                                       'int64' if field == 'elem_gid' else 'int32')
        types = _array(block, 'Types', (c,), 'uint8')
        expected_type = 12 if name == 'hex' else 42
        _require(np.all(types == expected_type),
                 f'{block.name}/Types: expected cell type {expected_type}')
        if name == 'hex':
            counts = _counts(fields.get('cell_kind', np.array([], dtype='int32')))
            blocks[name] = dict(points=n, cells=c, cell_kind_counts=counts)
        else:
            blocks[name] = dict(points=n, cells=c, type_counts=_counts(types))
        _partitions(block)
        data.append((points, u, t))
    eb = root.get('eb_surface')
    if eb is None:
        blocks['eb_surface'] = dict(points=0, polygons=0)
    else:
        _version(eb)
        n = _count(eb, 'NumberOfPoints')
        c = _count(eb, 'Polygons/NumberOfCells')
        _require(eb['NumberOfPoints'].shape == eb['Polygons/NumberOfCells'].shape,
                 'eb_surface: partition count mismatch')
        _array(eb, 'Points', (n, 3), 'float64')
        _partitions(eb, 'Polygons/NumberOfCells')
        blocks['eb_surface'] = dict(points=n, polygons=c)
    names = []
    for field in ('layer_names', 'material_names'):
        path = 'FieldData/' + field
        # The writer repeats these global tables in each block.
        table = None
        for block in ('hex', 'cut'):
            if path not in root[block]:
                continue
            source = root[block][path]
            _require(isinstance(source, h5py.Dataset) and source.ndim == 1
                     and h5py.check_string_dtype(source.dtype) is not None,
                     f"{source.name}: expected a string vector")
            current = source.asstr()[...].tolist()
            _require(table is None or current == table,
                     f"{source.name}: conflicting {field} tables in hex and cut")
            table = current
        names.append(table if table is not None else [])
    return blocks, data, names


def _summarize(run_dir, top_rel_tol):
    _require(np.isfinite(top_rel_tol) and top_rel_tol >= 0, 'top_rel_tol must be finite and nonnegative')
    try:
        content = (run_dir / 'manifest.json').read_text(encoding='utf-8')
    except UnicodeDecodeError as exc:
        raise ResultSummaryError(14, 'INPUT_INVALID', 'manifest.json: invalid UTF-8') from exc
    except OSError as exc:
        raise ResultSummaryError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc
    try:
        manifest = json.loads(content)
    except json.JSONDecodeError as exc:
        raise ResultSummaryError(14, 'INPUT_INVALID', f'manifest.json: {exc}') from exc
    _require(isinstance(manifest, dict) and manifest.get('schema_version') == 'chipcae.results/1.0',
             'expected manifest schema chipcae.results/1.0')
    steps = manifest.get('steps')
    _require(isinstance(steps, list) and len(steps) > 0 and isinstance(steps[0], dict)
             and isinstance(steps[0].get('file'), str) and steps[0]['file'],
             'expected nonempty steps[0].file string')
    step_file = steps[0]['file']
    summary = {}
    pv = None
    entries = manifest.get('summary', [])
    _require(isinstance(entries, list), 'summary must be a list')
    for index, entry in enumerate(entries):
        _require(isinstance(entry, dict) and isinstance(entry.get('name'), str)
                 and entry['name'] and 'value' in entry, 'invalid summary entry')
        name, value = entry['name'], entry['value']
        _require(name not in summary, f'summary[{index}].name: duplicate summary name: {name}')
        _require(isinstance(value, (int, float)) and not isinstance(value, bool)
                 and (isinstance(value, int) or np.isfinite(value)),
                 f'summary[{index}].value ({name}): expected finite numeric value')
        summary[name] = value
        if name == 'warpage_pv':
            _require(entry.get('unit') == 'm',
                     f'summary[{index}].unit (warpage_pv): expected "m"')
            pv = value
    mesh = manifest.get('mesh', {})
    _require(isinstance(mesh, dict), 'mesh must be an object')
    path = Path(step_file)
    if not path.is_absolute():
        path = run_dir / path
    try:
        step = h5py.File(path, 'r')
    except (OSError, RuntimeError, EOFError) as exc:
        raise ResultSummaryError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc
    try:
        with step as f:
            _require('VTKHDF' in f and isinstance(f['VTKHDF'], h5py.Group),
                     '/VTKHDF: required group missing or invalid')
            blocks, data, names = _read(f['VTKHDF'])
    except (OSError, RuntimeError, EOFError) as exc:
        raise ResultSummaryError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc
    points = np.concatenate([d[0] for d in data])
    u = np.concatenate([d[1] for d in data])
    all_t = np.full(len(points), np.nan, dtype='float64')
    offset = 0
    for p, _, t in data:
        if t is not None:
            all_t[offset:offset + len(p)] = t
        offset += len(p)
    top = np.array([], dtype=int)
    tt = np.array([], dtype='float64')
    if len(points):
        z = points[:, 2]
        indices = np.flatnonzero(z >= z.max() - top_rel_tol * (z.max() - z.min()))
        _, first, inverse = np.unique(points[indices], axis=0, return_index=True,
                                      return_inverse=True)
        _require(np.all(u[indices] == u[indices[first[inverse]]]),
                 'duplicate top coordinates: conflicting U values')
        top = indices[np.sort(first)]
        available = indices[np.isfinite(all_t[indices])]
        if len(available):
            _, first_t, inverse_t = np.unique(points[available], axis=0,
                                              return_index=True, return_inverse=True)
            _require(np.all(all_t[available] == all_t[available[first_t[inverse_t]]]),
                     'duplicate top coordinates: conflicting T values')
            tt = all_t[available[np.sort(first_t)]]
    w = None
    if len(top) >= 3:
        _, residual = best_fit_plane(points[top, 0], points[top, 1], u[top, 2])
        w = float(np.ptp(residual))
    temperature = None
    if any(t is not None for _, _, t in data):
        available_t = all_t[np.isfinite(all_t)]
        temperature = dict(min=float(available_t.min()) if len(available_t) else None,
                           max=float(available_t.max()) if len(available_t) else None,
                           top_min=float(tt.min()) if len(tt) else None,
                           top_max=float(tt.max()) if len(tt) else None,
                           top_mean=float(tt.mean()) if len(tt) else None)
    return dict(schema=SCHEMA, step_file=step_file, mesh_sha256=mesh.get('mesh_sha256'),
                blocks=blocks, layer_names=names[0], material_names=names[1],
                temperature=temperature,
                warpage=dict(manifest_warpage_pv_m=pv, w_bfp_m=w,
                             rel_diff=abs(w-pv)/pv if w is not None and pv not in (None, 0) else None,
                             n_top_points=len(top)), manifest_summary=summary)


def summarize(run_dir: Path, top_rel_tol: float = 1e-12) -> dict:
    """Return the frozen summary, raising protocol errors for invalid input."""
    try:
        return _summarize(Path(run_dir), top_rel_tol)
    except ResultSummaryError:
        raise
    except (OSError, ValueError, TypeError, KeyError, IndexError, AttributeError,
            OverflowError, np.linalg.LinAlgError) as exc:
        raise ResultSummaryError(14, 'INPUT_INVALID', str(exc)) from exc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_dir', type=Path)
    parser.add_argument('-o', type=Path)
    parser.add_argument('--top-rel-tol', type=float, default=1e-12)
    args = parser.parse_args(argv)
    try:
        result = summarize(args.run_dir, args.top_rel_tol)
        content = json.dumps(result, indent=2, sort_keys=False, ensure_ascii=False) + '\n'
        if args.o is None:
            sys.stdout.write(content)
        else:
            args.o.write_text(content, encoding='utf-8')
    except (ResultSummaryError, OSError) as exc:
        name = exc.name if isinstance(exc, ResultSummaryError) else 'INPUT_INVALID'
        reason = ' '.join(str(exc).splitlines())
        print(f'result_summary: {name}: {reason}', file=sys.stderr)
        return exc.code if isinstance(exc, ResultSummaryError) else 14
    return 0


if __name__ == '__main__':
    sys.exit(main())
