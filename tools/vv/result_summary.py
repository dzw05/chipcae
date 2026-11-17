#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Validate and summarize the first step of a chipcae results directory.

Duplicate coordinates use the first stored displacement/temperature. Temperature
fields must cover all nonempty blocks or be absent from all of them. Numeric
field types accept either byte order, retaining the required kind and width.
"""
import argparse
import json
from pathlib import Path
import sys

import h5py
import numpy as np

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
    values = block[path][...]
    _require(values.ndim == 1 and values.dtype.kind in 'iu'
             and np.all(values >= 0), f"{block.name}/{path}: invalid partition counts")
    return sum(int(v) for v in values)


def _array(block, path, shape, dtype):
    values = block[path][...]
    _require(values.shape == shape, f"{block.name}/{path}: expected shape {shape}")
    _require(values.dtype.newbyteorder('=') == np.dtype(dtype),
             f"{block.name}/{path}: expected {dtype}")
    if values.dtype.kind == 'f':
        _require(np.all(np.isfinite(values)), f"{block.name}/{path}: nonfinite values")
    return values


def _version(block):
    _require(isinstance(block, h5py.Group), "expected VTKHDF group")
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
        points = (_array(block, 'Points', (n, 3), 'float64')
                  if n or 'Points' in block else np.empty((0, 3)))
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
        if name == 'hex':
            counts = _counts(fields.get('cell_kind', np.array([], dtype='int32')))
            blocks[name] = dict(points=n, cells=c, cell_kind_counts=counts)
        else:
            types = block['Types'][...] if c or 'Types' in block else np.array([], dtype='uint8')
            _require(types.shape == (c,) and types.dtype.kind in 'iu', 'cut/Types: invalid cell types')
            blocks[name] = dict(points=n, cells=c, type_counts=_counts(types))
        data.append((points, u, t))
    eb = root.get('eb_surface')
    if eb is None:
        blocks['eb_surface'] = dict(points=0, polygons=0)
    else:
        _version(eb)
        blocks['eb_surface'] = dict(points=_count(eb, 'NumberOfPoints'),
                                    polygons=_count(eb, 'Polygons/NumberOfCells'))
    names = []
    for field in ('layer_names', 'material_names'):
        path = 'FieldData/' + field
        # The writer repeats these global tables in each block.
        source = next((root[b][path] for b in ('hex', 'cut') if path in root[b]), None)
        if source is None:
            names.append([])
        else:
            _require(source.ndim == 1, f"{path}: expected a string vector")
            names.append(source.asstr()[...].tolist())
    return blocks, data, names


def _summarize(run_dir, top_rel_tol):
    _require(np.isfinite(top_rel_tol) and top_rel_tol >= 0, 'top_rel_tol must be finite and nonnegative')
    try:
        content = (run_dir / 'manifest.json').read_text(encoding='utf-8')
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
             and isinstance(steps[0].get('file'), str), 'expected steps[0].file string')
    step_file = steps[0]['file']
    summary = {}
    pv = None
    entries = manifest.get('summary', [])
    _require(isinstance(entries, list), 'summary must be a list')
    for entry in entries:
        _require(isinstance(entry, dict) and isinstance(entry.get('name'), str)
                 and 'value' in entry, 'invalid summary entry')
        name, value = entry['name'], entry['value']
        _require(name not in summary, f'duplicate summary name: {name}')
        _require(isinstance(value, (int, float)) and not isinstance(value, bool)
                 and np.isfinite(value), f'{name}: expected finite numeric value')
        summary[name] = value
        if name == 'warpage_pv':
            _require(entry.get('unit') == 'm', 'warpage_pv unit must be m')
            pv = value
    mesh = manifest.get('mesh', {})
    _require(isinstance(mesh, dict), 'mesh must be an object')
    path = Path(step_file)
    if not path.is_absolute():
        path = run_dir / path
    try:
        step = h5py.File(path, 'r')
    except OSError as exc:
        raise ResultSummaryError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc
    with step as f:
        _require('VTKHDF' in f and isinstance(f['VTKHDF'], h5py.Group),
                 '/VTKHDF: required group missing or invalid')
        blocks, data, names = _read(f['VTKHDF'])
    points = np.concatenate([d[0] for d in data])
    u = np.concatenate([d[1] for d in data])
    top = np.array([], dtype=int)
    if len(points):
        z = points[:, 2]
        indices = np.flatnonzero(z >= z.max() - top_rel_tol * (z.max() - z.min()))
        _, first = np.unique(points[indices], axis=0, return_index=True)
        top = indices[np.sort(first)]
    w = None
    if len(top) >= 3:
        _, residual = best_fit_plane(points[top, 0], points[top, 1], u[top, 2])
        w = float(np.ptp(residual))
    temperature = None
    if any(len(p) and t is not None for p, _, t in data):
        _require(all(d[2] is not None for d in data if len(d[0])),
                 "partial T coverage: temperature required for every nonempty block")
        # Mirror the point concatenation block for block. Missing T is valid
        # only for empty geometry here, so it contributes a zero-length slice.
        all_t = np.concatenate([t if t is not None else np.empty(0, dtype='float64')
                                for _, _, t in data])
        # Keep temperatures paired with the original point indices selected
        # above; duplicate coordinates retain their first stored value.
        tt = all_t[top]
        temperature = dict(min=float(all_t.min()), max=float(all_t.max()),
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
