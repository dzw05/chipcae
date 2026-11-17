"""Minimal VTKHDF writer using h5py only (simulates the solver kernel; no VTK dependency)."""
import h5py, numpy as np
h5py.get_config().track_order = True
IDT = np.int64

def _ds(g, name, arr, dtype=None):
    a = np.asarray(arr, dtype=dtype)
    g.create_dataset(name, data=a)

def _attrs(g, typ, version, index=None):
    g.attrs['Version'] = np.array(version, dtype=np.int64)
    g.attrs.create('Type', np.bytes_(typ))  # fixed-length ASCII
    if index is not None: g.attrs['Index'] = np.int64(index)

def _data_groups(g, parts, key, names):
    grp = g.create_group(key)
    for nm in names:
        arrs = [np.asarray(p[key][nm]) for p in parts]
        _ds(grp, nm, np.concatenate(arrs) if arrs else np.zeros(0))

def write_ug(g, parts, version):
    """parts: list of dicts: points (n,3), types (m,), conn, offsets(m+1),
    optional poly: face_conn, face_offsets, p2f, p_offsets; CellData, PointData dicts"""
    _ds(g, 'NumberOfPoints', [len(p['points']) for p in parts], IDT)
    _ds(g, 'NumberOfCells', [len(p['types']) for p in parts], IDT)
    _ds(g, 'NumberOfConnectivityIds', [len(p['conn']) for p in parts], IDT)
    _ds(g, 'Points', np.concatenate([np.asarray(p['points'], float).reshape(-1, 3) for p in parts]))
    _ds(g, 'Types', np.concatenate([np.asarray(p['types'], np.uint8) for p in parts]), np.uint8)
    _ds(g, 'Connectivity', np.concatenate([np.asarray(p['conn'], IDT) for p in parts]), IDT)
    _ds(g, 'Offsets', np.concatenate([np.asarray(p['offsets'], IDT) for p in parts]), IDT)
    if any('face_conn' in p for p in parts):
        _ds(g, 'NumberOfFaces', [len(p['face_offsets']) - 1 for p in parts], IDT)
        _ds(g, 'NumberOfFaceConnectivityIds', [len(p['face_conn']) for p in parts], IDT)
        _ds(g, 'NumberOfPolyhedronToFaceIds', [len(p['p2f']) for p in parts], IDT)
        _ds(g, 'FaceConnectivity', np.concatenate([np.asarray(p['face_conn'], IDT) for p in parts]), IDT)
        _ds(g, 'FaceOffsets', np.concatenate([np.asarray(p['face_offsets'], IDT) for p in parts]), IDT)
        _ds(g, 'PolyhedronToFaces', np.concatenate([np.asarray(p['p2f'], IDT) for p in parts]), IDT)
        _ds(g, 'PolyhedronOffsets', np.concatenate([np.asarray(p['p_offsets'], IDT) for p in parts]), IDT)
    _data_groups(g, parts, 'CellData', parts[0]['CellData'].keys())
    _data_groups(g, parts, 'PointData', parts[0]['PointData'].keys())

def write_pd(g, parts):
    """parts: points, verts:(conn,offsets), polys:(conn,offsets), CellData (verts then polys), PointData"""
    _ds(g, 'NumberOfPoints', [len(p['points']) for p in parts], IDT)
    _ds(g, 'Points', np.concatenate([np.asarray(p['points'], float).reshape(-1, 3) for p in parts]))
    for topo, key in [('Vertices', 'verts'), ('Lines', None), ('Polygons', 'polys'), ('Strips', None)]:
        tg = g.create_group(topo)
        cs, os_ = [], []
        for p in parts:
            if key and key in p: c, o = p[key]
            else: c, o = np.zeros(0, IDT), np.zeros(1, IDT)
            cs.append(np.asarray(c, IDT)); os_.append(np.asarray(o, IDT))
        _ds(tg, 'NumberOfCells', [len(o) - 1 for o in os_], IDT)
        _ds(tg, 'NumberOfConnectivityIds', [len(c) for c in cs], IDT)
        _ds(tg, 'Connectivity', np.concatenate(cs), IDT)
        _ds(tg, 'Offsets', np.concatenate(os_), IDT)
    _data_groups(g, parts, 'CellData', parts[0]['CellData'].keys())
    _data_groups(g, parts, 'PointData', parts[0]['PointData'].keys())

def write_pdc(fname, blocks, assembly, version=(2, 8), time_value=None):
    """blocks: list of (name, 'UnstructuredGrid'|'PolyData', parts, block_version)
    assembly: dict node -> list of block names (one level)"""
    with h5py.File(fname, 'w', track_order=True) as f:
        root = f.create_group('VTKHDF', track_order=True)
        _attrs(root, 'PartitionedDataSetCollection', version)
        for idx, (name, typ, parts, bver) in enumerate(blocks):
            g = root.create_group(name, track_order=True)
            _attrs(g, typ, bver, idx)
            (write_ug if typ == 'UnstructuredGrid' else write_pd)(*( (g, parts, bver) if typ == 'UnstructuredGrid' else (g, parts)))
        asm = root.create_group('Assembly', track_order=True)
        for node, names in assembly.items():
            ng = asm.create_group(node, track_order=True)
            for nm in names:
                ng[nm] = h5py.SoftLink('/VTKHDF/' + nm)
