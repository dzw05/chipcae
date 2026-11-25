#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Validate and summarize a selected step of a chipcae results directory.

By default select the first manifest step; an integer selects its n, and "last"
selects the maximum n. Summaries include legacy entries without a step tag and
entries tagged with the selected n, in manifest encounter order.

Duplicate coordinates use the first stored displacement and temperature
(hex before cut, then partition/storage order), even if copies differ.
Temperature is all-or-nothing across hex and cut, including zero-point blocks.
Partial coverage is an input error. Each T array must match its block's point
count. When both blocks omit T, the temperature field is null.
Numeric field types accept either byte order, retaining the required kind and
width.
Collinear top geometry fits along its centered principal axis (constant fit
when x/y have rank zero), using the shared plane fitter for residuals.
"""
import argparse
from fractions import Fraction
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
    _require(path in block, f"{block.name}/{path}: required field missing")
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


def _polyhedron_chunk(local_points, ids, fo, refs, po, first, last, references):
    """Geometry for one bounded cell chunk, in stored face/triangle order.

    Offsets and IDs have already been validated. Structured keys avoid encoding
    (cell, point) in an integer whose product could overflow.
    """
    count = last - first
    faces = refs[int(po[first]):int(po[last])]
    face_cells = np.repeat(np.arange(count), np.diff(po[first:last + 1]))
    starts = fo[faces]
    lengths = fo[faces + 1] - starts
    occurrences = (np.repeat(starts - (np.cumsum(lengths) - lengths), lengths)
                   + np.arange(int(lengths.sum())))
    pairs = np.empty(len(occurrences), dtype=[('cell', 'int64'), ('point', 'int64')])
    pairs['cell'] = np.repeat(face_cells, lengths)
    pairs['point'] = ids[occurrences]
    pairs = pairs[np.lexsort((pairs['point'], pairs['cell']))]
    unique = np.empty(len(pairs), dtype=bool)
    unique[0] = True
    unique[1:] = pairs[1:] != pairs[:-1]
    pairs = pairs[unique]
    vertex_cells = pairs['cell']
    vertices = local_points[pairs['point']]
    vertex_counts = np.bincount(vertex_cells, minlength=count)
    vertex_starts = np.cumsum(vertex_counts) - vertex_counts

    with np.errstate(all='ignore'):
        # mean(axis=0) of an (N, 3) vertex array sums each coordinate in
        # ascending point-ID order. add.at preserves that order per cell,
        # including repeated IDs having been removed above.
        centers = np.zeros((count, 3))
        np.add.at(centers, vertex_cells, vertices)
        centers /= vertex_counts[:, None]
        spans = (np.maximum.reduceat(vertices, vertex_starts, axis=0)
                 - np.minimum.reduceat(vertices, vertex_starts, axis=0))
        bounds = np.prod(spans, axis=1)

        triangle_counts = lengths - 2
        triangle_cells = np.repeat(face_cells, triangle_counts)
        origins = np.repeat(starts, triangle_counts)
        offsets = (np.arange(int(triangle_counts.sum()))
                   - np.repeat(np.cumsum(triangle_counts) - triangle_counts,
                               triangle_counts) + 1)
        center = centers[triangle_cells]
        a = local_points[ids[origins]] - center
        b = local_points[ids[origins + offsets]] - center
        c = local_points[ids[origins + offsets + 1]] - center
        crosses = np.cross(b, c)
        # Batched length-three dot, rather than an einsum/reduction with a
        # different contraction order. Divide before the ordered cell sum.
        contributions = (a[:, None, :] @ crosses[:, :, None]).ravel() / 6.
        result = np.zeros(count)
        np.add.at(result, triangle_cells, contributions)

        # Conservative roundoff guard for cross/dot and accumulated sums.
        # Include the uncancelled products, not just signed contributions:
        # near-zero volumes of nonclosed/degenerate cells can cancel heavily.
        products = (np.abs(b[:, [1, 2, 0]] * c[:, [2, 0, 1]])
                    + np.abs(b[:, [2, 0, 1]] * c[:, [1, 2, 0]]))
        magnitude = np.sum(np.abs(a) * products, axis=1) / 6.
        absolute_sum = np.zeros(count)
        np.add.at(absolute_sum, triangle_cells, magnitude)
        terms = np.bincount(triangle_cells, minlength=count)
        error = 64 * (terms + 1) * (np.finfo('float64').eps * absolute_sum
                                   + np.finfo('float64').tiny)
        ambiguous = (~np.isfinite(result) | ~np.isfinite(error)
                     | (np.abs(result) <= error)
                     | (error >= 1e-12 * np.maximum(np.abs(result), bounds)))
        if references is not None:
            tolerance = 1e-9 * np.maximum(np.abs(references), bounds)
            # The comparison also rounds: subtracting a much larger reference
            # can turn a tiny volume error into an entire ULP of the reference.
            # Cover both subtractions and tolerance rounding at operand scale,
            # rather than testing the threshold distance against volume error alone.
            comparison_scale = np.maximum(np.maximum(np.abs(result), np.abs(references)),
                                          tolerance)
            comparison_error = error + 8 * np.finfo('float64').eps * comparison_scale
            ambiguous |= (~np.isfinite(comparison_error)
                          | (np.abs(np.abs(result - references) - tolerance)
                             <= comparison_error))

        # The vector result need not make an arbitrarily close threshold
        # decision. Recompute ambiguous cells with the historical arithmetic;
        # this also preserves its overflow/underflow classification.
        for cell in np.flatnonzero(ambiguous):
            source = first + int(cell)
            rings = [ids[int(fo[face]):int(fo[face + 1])]
                     for face in refs[int(po[source]):int(po[source + 1])]]
            vertices = local_points[np.unique(np.concatenate(rings))]
            center = vertices.mean(axis=0)
            volume = np.float64(0.)
            for ring in rings:
                coords = local_points[ring] - center
                for i in range(1, len(coords) - 1):
                    volume += np.dot(coords[0], np.cross(coords[i], coords[i + 1])) / 6.
            result[cell] = volume
            bounds[cell] = np.prod(vertices.max(axis=0) - vertices.min(axis=0))
    return result, bounds


def _polyhedra(block):
    """Validate partition-local, exclusively owned faces and signed cell volumes."""
    paths = ('FaceConnectivity', 'FaceOffsets', 'PolyhedronToFaces',
             'PolyhedronOffsets', 'NumberOfFaces', 'NumberOfFaceConnectivityIds',
             'NumberOfPolyhedronToFaceIds')
    if not any(path in block for path in paths):
        return
    for path in paths:
        _require(path in block, f'{block.name}/{path}: incomplete polyhedron topology')

    pc = block['NumberOfPoints'][...]
    cc = block['NumberOfCells'][...]
    totals, counts = [], []
    for path in paths[4:]:
        totals.append(_count(block, path))
        counts.append(_array(block, path, (len(pc),), 'int64'))
    nf, nfc, npf = counts
    face_ids = _array(block, 'FaceConnectivity', (totals[1],), 'int64')
    face_offsets = _array(block, 'FaceOffsets', (totals[0] + len(pc),), 'int64')
    poly_faces = _array(block, 'PolyhedronToFaces', (totals[2],), 'int64')
    total_cells = sum(int(c) for c in cc)
    poly_offsets = _array(block, 'PolyhedronOffsets', (total_cells + len(pc),), 'int64')
    points = _array(block, 'Points', (sum(int(p) for p in pc), 3), 'float64')
    volumes = (_array(block, 'CellData/volume', (total_cells,), 'float64')
               if 'CellData/volume' in block else None)

    point_cursor = cell_cursor = face_offset_cursor = poly_offset_cursor = 0
    face_id_cursor = face_ref_cursor = 0
    for rank, allocation in enumerate(zip(pc, cc, nf, nfc, npf)):
        point_count, cells, faces, connections, references = map(int, allocation)
        fo = face_offsets[face_offset_cursor:face_offset_cursor + faces + 1]
        po = poly_offsets[poly_offset_cursor:poly_offset_cursor + cells + 1]
        ids = face_ids[face_id_cursor:face_id_cursor + connections]
        refs = poly_faces[face_ref_cursor:face_ref_cursor + references]
        local_points = points[point_cursor:point_cursor + point_count]
        # Validate endpoints and ordering before subtracting offsets or indexing.
        for path, offsets, terminal, minimum in (
                ('FaceOffsets', fo, connections, 3),
                ('PolyhedronOffsets', po, references, 4)):
            _require(offsets[0] == 0 and offsets[-1] == terminal
                     and np.all(offsets[1:] >= offsets[:-1]),
                     f'{block.name}/{path}: partition {rank} invalid offsets')
            _require(np.all(offsets[1:] - offsets[:-1] >= minimum),
                     f'{block.name}/{path}: partition {rank} '
                     f'each list requires at least {minimum} IDs')
        _require(np.all((ids >= 0) & (ids < point_count)),
                 f'{block.name}/FaceConnectivity: partition {rank} point ID out of bounds')
        _require(np.all((refs >= 0) & (refs < faces)),
                 f'{block.name}/PolyhedronToFaces: partition {rank} face ID out of bounds')
        _require(references == faces
                 and np.array_equal(np.sort(refs), np.arange(faces, dtype='int64')),
                 f'{block.name}/PolyhedronToFaces: partition {rank} '
                 'each face must be referenced exactly once')

        for first in range(0, cells, 1024):
            last = min(first + 1024, cells)
            stored = (volumes[cell_cursor + first:cell_cursor + last]
                      if volumes is not None else None)
            computed, bounds = _polyhedron_chunk(
                local_points, ids, fo, refs, po, first, last, stored)
            nonpositive = ~np.isfinite(computed) | (computed <= 0)
            mismatch = np.zeros(last - first, dtype=bool)
            with np.errstate(all='ignore'):
                if stored is not None:
                    tolerance = 1e-9 * np.maximum(np.abs(stored), bounds)
                    difference = np.abs(computed - stored)
                    mismatch = ~((stored > 0) & np.isfinite(bounds)
                                 & np.isfinite(tolerance) & np.isfinite(difference)
                                 & (difference <= tolerance))
            failures = np.flatnonzero(nonpositive | mismatch)
            if not len(failures):
                continue
            index = int(failures[0])
            cell = first + index
            volume = computed[index]
            _require(not nonpositive[index],
                     f'{block.name}/PolyhedronToFaces: partition {rank} cell {cell} '
                     f'nonpositive polyhedron volume (computed={volume})')
            if volumes is not None:
                reference = stored[index]
                bbox_volume = bounds[index]
                _require(not mismatch[index],
                         f'{block.name}/CellData/volume: volume mismatch, '
                         f'partition {rank} cell {cell} computed={volume} '
                         f'reference={reference} bbox_volume={bbox_volume}')
        point_cursor += point_count
        cell_cursor += cells
        face_offset_cursor += faces + 1
        poly_offset_cursor += cells + 1
        face_id_cursor += connections
        face_ref_cursor += references


def _version(block):
    _require(isinstance(block, h5py.Group), f"{block.name}: expected VTKHDF group")
    _require(np.array_equal(block.attrs.get('Version'), [2, 8]),
             f"{block.name}: expected Version [2, 8]")


def _type(block, expected):
    message = f'{block.name.lstrip("/")}: expected scalar fixed-byte ASCII Type {expected}'
    _require('Type' in block.attrs, message)
    attr = block.attrs.get_id('Type')
    _require(attr.shape == () and attr.dtype.kind == 'S', message)
    try:
        value = bytes(block.attrs['Type']).rstrip(b'\0').decode('ascii')
    except UnicodeDecodeError:
        _require(False, message)
    _require(value == expected, message)


def _index(block, expected):
    message = f'{block.name.lstrip("/")}: expected scalar integer Index {expected}'
    _require('Index' in block.attrs, message)
    _require(block.attrs.get_id('Index').shape == (), message)
    value = block.attrs['Index']
    _require(isinstance(value, (int, np.integer))
             and not isinstance(value, (bool, np.bool_)), message)
    _require(int(value) == expected, message)


def _optional_type_index(block, expected_index):
    # Legacy optional blocks may omit the pair, but partial metadata is invalid.
    has_type, has_index = 'Type' in block.attrs, 'Index' in block.attrs
    if not has_type and not has_index:
        return
    _require(has_type and has_index,
             f'{block.name.lstrip("/")}: incomplete Type/Index')
    _type(block, 'PolyData')
    _index(block, expected_index)


def _counts(values):
    keys, counts = np.unique(values, return_counts=True)
    return {str(int(k)): int(n) for k, n in zip(keys, counts)}


def _assembly(root):
    if 'Assembly' not in root:
        return
    assembly = root.get('Assembly')
    prefix = 'VTKHDF/Assembly'
    _require(isinstance(assembly, h5py.Group), f'{prefix}: expected group')
    categories = ('volume', 'boundary', 'lagrangian')
    members = set(assembly)
    for category in categories:
        _require(category in members, f'{prefix}: missing category {category}')
    for name in sorted(members - set(categories)):
        _require(False, f'{prefix}: unexpected member {name}')
    for category in categories:
        _require(isinstance(assembly.get(category), h5py.Group),
                 f'{prefix}/{category}: expected group')
    for path, target in (
            ('volume/hex', '/VTKHDF/hex'),
            ('volume/cut', '/VTKHDF/cut'),
            ('boundary/eb_surface', '/VTKHDF/eb_surface'),
            ('lagrangian/particles', '/VTKHDF/particles')):
        link = assembly.get(path, getlink=True)
        _require(isinstance(link, h5py.SoftLink) and not isinstance(link, h5py.ExternalLink),
                 f'{prefix}/{path}: expected soft link')
        _require(link.path == target, f'{prefix}/{path}: expected {target}')
        _require(isinstance(root.file.get(link.path), h5py.Group),
                 f'{prefix}/{path}: dangling soft link')


def _read(root):
    _version(root)
    _type(root, 'PartitionedDataSetCollection')
    _assembly(root)
    blocks, data, temperatures = {}, [], []
    for index, name in enumerate(('hex', 'cut')):
        _require(name in root and isinstance(root[name], h5py.Group),
                 f'{root.name}/{name}: required group missing or invalid')
        block = root[name]
        _version(block)
        _type(block, 'UnstructuredGrid')
        _index(block, index)
        n = _count(block, 'NumberOfPoints')
        c = _count(block, 'NumberOfCells')
        _require(block['NumberOfPoints'].shape == block['NumberOfCells'].shape,
                 f"{name}: partition count mismatch")
        points = _array(block, 'Points', (n, 3), 'float64')
        u = (_array(block, 'PointData/U', (n, 3), 'float64')
             if n or 'PointData/U' in block else np.empty((0, 3)))
        if 'PointData/T' in block:
            temperatures.append(_array(block, 'PointData/T', (n,), 'float64'))
        fields = {}
        for field in ('layer_id', 'material_id', 'cell_kind', 'elem_gid'):
            path = 'CellData/' + field
            if c or path in block:
                fields[field] = _array(block, path, (c,),
                                       'int64' if field == 'elem_gid' else 'int32')
        if 'CellData/copper_fraction' in block:
            _array(block, 'CellData/copper_fraction', (c,), 'float64')
        if 'CellData/conductivity_diag' in block:
            _array(block, 'CellData/conductivity_diag', (c, 3), 'float64')
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
        if name == 'cut':
            _polyhedra(block)
        data.append((points, u))
    _require(len(temperatures) in (0, len(data)),
             'PointData/T must be present in both hex and cut or absent from both')
    eb = root.get('eb_surface')
    if eb is None:
        blocks['eb_surface'] = dict(points=0, polygons=0)
    else:
        _version(eb)
        _optional_type_index(eb, 2)
        n = _count(eb, 'NumberOfPoints')
        c = _count(eb, 'Polygons/NumberOfCells')
        _require(eb['NumberOfPoints'].shape == eb['Polygons/NumberOfCells'].shape,
                 'eb_surface: partition count mismatch')
        _array(eb, 'Points', (n, 3), 'float64')
        if 'CellData/copper_fraction' in eb:
            _array(eb, 'CellData/copper_fraction', (c,), 'float64')
        if 'CellData/conductivity_diag' in eb:
            _array(eb, 'CellData/conductivity_diag', (c, 3), 'float64')
        _partitions(eb, 'Polygons/NumberOfCells')
        blocks['eb_surface'] = dict(points=n, polygons=c)
    particles = root.get('particles')
    if particles is not None:
        _optional_type_index(particles, 3)
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
    return blocks, data, temperatures, names


def _require_finite(value, path=''):
    """Check computed floats in output order without converting JSON integers."""
    if isinstance(value, dict):
        for key, item in value.items():
            _require_finite(item, f'{path}.{key}' if path else key)
    elif isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            _require_finite(item, f'{path}[{index}]')
    elif isinstance(value, float):
        _require(np.isfinite(value), f'{path}: non-finite computed value')


def _summarize(run_dir, top_rel_tol, step):
    _require(np.isfinite(top_rel_tol) and top_rel_tol >= 0, 'top_rel_tol must be finite and nonnegative')
    _require(step is None
             or (isinstance(step, int) and not isinstance(step, bool) and step >= 0)
             or (isinstance(step, str) and step == 'last'),
             'step must be a nonnegative integer or "last"')
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
    _require(isinstance(manifest, dict), 'manifest must be an object')
    _require(manifest.get('schema_version') == 'chipcae.results/1.0',
             'expected manifest schema chipcae.results/1.0')
    steps = manifest.get('steps')
    _require(isinstance(steps, list) and len(steps) > 0 and isinstance(steps[0], dict)
             and isinstance(steps[0].get('file'), str) and steps[0]['file'],
             'expected nonempty steps[0].file string')
    step_indices = {}
    for index, record in enumerate(steps):
        _require(isinstance(record, dict), f'steps[{index}]: expected object')
        _require(isinstance(record.get('file'), str) and record['file'],
                 f'expected nonempty steps[{index}].file string')
        n = record.get('n')
        _require(isinstance(n, int) and not isinstance(n, bool) and n >= 0,
                 f'steps[{index}].n: expected nonnegative integer')
        _require(n not in step_indices, f'steps[{index}].n: duplicate step n: {n}')
        step_indices[n] = index
    selected_index = 0
    if step == 'last':
        selected_index = step_indices[max(step_indices)]
    elif step is not None:
        if step not in step_indices:
            raise ResultSummaryError(12, 'INPUT_REFERENCE_MISSING', f'step {step}: not found')
        selected_index = step_indices[step]
    selected = steps[selected_index]
    step_file = selected.get('file')
    _require(isinstance(step_file, str) and step_file,
             f'expected nonempty steps[{selected_index}].file string')
    summary = {}
    pv = None
    entries = manifest.get('summary', [])
    _require(isinstance(entries, list), 'summary must be a list')
    for index, entry in enumerate(entries):
        _require(isinstance(entry, dict) and isinstance(entry.get('name'), str)
                 and entry['name'] and 'value' in entry, 'invalid summary entry')
        name, value = entry['name'], entry['value']
        _require(not isinstance(value, bool) and isinstance(value, (int, float))
                 and (isinstance(value, int) or np.isfinite(value)),
                 f'summary[{index}].value ({name}): expected finite numeric value')
        if name == 'warpage_pv':
            _require(entry.get('unit') == 'm',
                     f'summary[{index}].unit (warpage_pv): expected "m"')
        if 'step' in entry:
            owner = entry['step']
            _require(isinstance(owner, int) and not isinstance(owner, bool) and owner >= 0,
                     f'summary[{index}].step: expected nonnegative integer')
            if owner != selected['n']:
                continue
        _require(name not in summary, f'summary[{index}].name: duplicate summary name: {name}')
        summary[name] = value
        if name == 'warpage_pv':
            pv = value
    mesh = manifest.get('mesh', {})
    _require(isinstance(mesh, dict), 'mesh must be an object')
    for key in ('nx0', 'ny0', 'L', 'padded_cells', 'nz'):
        if key in mesh:
            value = mesh[key]
            _require(isinstance(value, int) and not isinstance(value, bool),
                     f'mesh.{key}: expected integer')
    if 'mesh_sha256' in mesh:
        _require(isinstance(mesh['mesh_sha256'], str), 'mesh.mesh_sha256: expected string')
    _require('partitions' in manifest, 'partitions: required object')
    partitions = manifest['partitions']
    _require(isinstance(partitions, dict), 'partitions: expected object')
    per_step = partitions.get('per_step')
    _require(isinstance(per_step, list), 'partitions.per_step: expected array')
    _require(len(per_step) == len(steps),
             f'partitions.per_step: expected length {len(steps)}')
    for index, value in enumerate(per_step):
        _require(isinstance(value, int) and not isinstance(value, bool) and value >= 1,
                 f'partitions.per_step[{index}]: expected integer >= 1')
    fixed = partitions.get('solver_fixed_per_run')
    _require(isinstance(fixed, bool), 'partitions.solver_fixed_per_run: expected boolean')
    if fixed:
        _require(all(value == per_step[0] for value in per_step),
                 'partitions.per_step: expected identical values')
    path = Path(step_file)
    if not path.is_absolute():
        path = run_dir / path
    try:
        step = h5py.File(path, 'r')
    except (OSError, RuntimeError, EOFError) as exc:
        raise ResultSummaryError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc
    with step as f:
        _require('VTKHDF' in f and isinstance(f['VTKHDF'], h5py.Group),
                 '/VTKHDF: required group missing or invalid')
        blocks, data, temperatures, names = _read(f['VTKHDF'])
        if per_step is not None:
            expected = per_step[selected_index]
            count_paths = ['hex/NumberOfCells', 'cut/NumberOfCells']
            if 'eb_surface' in f['VTKHDF']:
                count_paths.append('eb_surface/Polygons/NumberOfCells')
            for count_path in count_paths:
                actual = f['VTKHDF'][count_path].shape[0]
                _require(actual == expected,
                         f'partitions.per_step[{selected_index}]: partition count mismatch '
                         f'at /VTKHDF/{count_path}: expected {expected}, got {actual}')
    points = np.concatenate([d[0] for d in data])
    u = np.concatenate([d[1] for d in data])
    top = np.array([], dtype=int)
    if len(points):
        z = points[:, 2]
        indices = np.flatnonzero(z >= z.max() - top_rel_tol * (z.max() - z.min()))
        _, first = np.unique(points[indices], axis=0, return_index=True)
        top = indices[first]
    w = None
    if len(top) >= 3:
        with np.errstate(over='ignore', invalid='ignore'):
            xy = points[top, :2]
            centered = xy - xy[0]
            if np.linalg.matrix_rank(centered) < 2:
                # span{along, 1} equals span{x, y, 1} on a line, so residuals agree.
                # Plane slopes are not unique on a line; its fitted values are.
                # Remove the redundant direction explicitly, retaining the shared
                # least-squares API and avoiding large absolute coordinate offsets.
                _, _, axes = np.linalg.svd(centered, full_matrices=False)
                along = centered @ axes[0]
                _, residual = best_fit_plane(along, np.zeros_like(along), u[top, 2])
            else:
                _, residual = best_fit_plane(xy[:, 0], xy[:, 1], u[top, 2])
            w = float(np.ptp(residual))
    temperature = None
    if temperatures:
        all_t = np.concatenate(temperatures)
        tt = all_t[top]
        with np.errstate(over='ignore', invalid='ignore'):
            temperature = dict(min=float(all_t.min()) if len(all_t) else None,
                               max=float(all_t.max()) if len(all_t) else None,
                               top_min=float(tt.min()) if len(tt) else None,
                               top_max=float(tt.max()) if len(tt) else None,
                               top_mean=float(tt.mean()) if len(tt) else None)
    rel_diff = None
    # Leave non-finite w for the final traversal so earlier output fields win.
    if w is not None and np.isfinite(w) and pv not in (None, 0):
        # JSON integers may exceed float range. Convert only the final ratio,
        # preserving the signed denominator required by the summary contract.
        rel_diff = float(abs(Fraction(w) - pv) / pv) if isinstance(pv, int) else abs(w-pv)/pv
    result = dict(schema=SCHEMA, step_file=step_file, mesh_sha256=mesh.get('mesh_sha256'),
                  blocks=blocks, layer_names=names[0], material_names=names[1],
                  temperature=temperature,
                  warpage=dict(manifest_warpage_pv_m=pv, w_bfp_m=w,
                               rel_diff=rel_diff,
                               n_top_points=len(top)), manifest_summary=summary)
    _require_finite(result)
    return result


def summarize(run_dir: Path, top_rel_tol: float = 1e-12, step: int | str | None = None) -> dict:
    """Return the frozen summary, raising protocol errors for invalid input."""
    try:
        return _summarize(Path(run_dir), top_rel_tol, step)
    except ResultSummaryError:
        raise
    except (OSError, RuntimeError, EOFError, ValueError, TypeError, KeyError, IndexError, AttributeError,
            OverflowError, np.linalg.LinAlgError) as exc:
        raise ResultSummaryError(14, 'INPUT_INVALID', str(exc)) from exc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_dir', type=Path)
    parser.add_argument('-o', type=Path)
    parser.add_argument('--top-rel-tol', type=float, default=1e-12)
    parser.add_argument('--step', help='select a nonnegative step n or "last" (default: first step)')
    args = parser.parse_args(argv)
    try:
        step = args.step
        if step is not None and step != 'last':
            _require(step.isascii() and step.isdecimal(),
                     'step must be a nonnegative decimal integer or "last"')
            try:
                step = int(step)
            except ValueError as exc:
                raise ResultSummaryError(14, 'INPUT_INVALID', str(exc)) from exc
        result = summarize(args.run_dir, args.top_rel_tol, step)
        content = json.dumps(result, indent=2, sort_keys=False, ensure_ascii=False, allow_nan=False) + '\n'
        if args.o is None:
            sys.stdout.write(content)
        else:
            args.o.write_text(content, encoding='utf-8')
    except (ResultSummaryError, OSError, ValueError) as exc:
        name = exc.name if isinstance(exc, ResultSummaryError) else 'INPUT_INVALID'
        reason = ' '.join(str(exc).splitlines())
        print(f'result_summary: {name}: {reason}', file=sys.stderr)
        return exc.code if isinstance(exc, ResultSummaryError) else 14
    return 0


if __name__ == '__main__':
    sys.exit(main())
