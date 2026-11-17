"""Rules 1–8 from REPORT_A; face/volume checks adapted from check2_reference.py.
The copied reference is a sample generator/diagnostic tied to out/expected.json,
so this adapter accepts arbitrary ResultsWriter output and never creates files.
"""
import collections
import h5py
import numpy as np

NAMES = ['hex', 'cut', 'eb_surface', 'particles']


def check(path):
    with h5py.File(path) as f:
        root = f['VTKHDF']
        assert tuple(root.attrs['Version']) == (2, 8)
        print('rule 1 PASS')
        for name in NAMES:
            g = root[name]
            assert g['Points'].dtype == np.dtype('float64') and g['Points'].shape[1] == 3
            if name in NAMES[:2]:
                assert g['Types'].dtype == np.dtype('uint8')
                for key in ['Connectivity', 'Offsets', 'NumberOfCells', 'NumberOfPoints', 'NumberOfConnectivityIds']:
                    assert g[key].dtype == np.dtype('int64')
        print('rule 2 PASS')
        for name in NAMES[:2]:
            g = root[name]
            start = 0
            for nc, nconn in zip(g['NumberOfCells'][:], g['NumberOfConnectivityIds'][:]):
                off = g['Offsets'][start:start+nc+1]
                assert off[0] == 0 and off[-1] == nconn and np.all(np.diff(off) >= 0)
                start += nc+1
            assert start == len(g['Offsets'])
        print('rule 3 PASS')
        g = root['cut']
        for key, count in [('FaceOffsets','NumberOfFaces'), ('PolyhedronOffsets','NumberOfCells')]:
            assert len(g[key]) == sum(g[count][:]) + len(g[count])
        for name in NAMES[2:]:
            for topo in ['Vertices','Lines','Polygons','Strips']:
                assert np.all(root[name][topo]['Offsets'][:] == 0)
        print('rule 4 PASS')
        # Rank-local offsets and point/face IDs restart independently.
        ps = fs = cs = fcs = pfs = 0
        max_volume_error = 0.
        for rank, nc in enumerate(g['NumberOfCells'][:]):
            npnt = int(g['NumberOfPoints'][rank]); nf = int(g['NumberOfFaces'][rank])
            P = g['Points'][ps:ps+npnt]
            fo = g['FaceOffsets'][fs+rank:fs+rank+nf+1]
            po = g['PolyhedronOffsets'][cs+rank:cs+rank+nc+1]
            fc = g['FaceConnectivity'][fcs:fcs+int(g['NumberOfFaceConnectivityIds'][rank])]
            pf = g['PolyhedronToFaces'][pfs:pfs+int(g['NumberOfPolyhedronToFaceIds'][rank])]
            for i in range(nc):
                faces = [fc[fo[j]:fo[j+1]].tolist() for j in pf[po[i]:po[i+1]]]
                ids = list(dict.fromkeys(v for face in faces for v in face))
                center = P[ids].mean(axis=0); edges = collections.Counter(); volume = 0.
                for face in faces:
                    assert len(face) >= 3 and len(set(face)) == len(face)
                    q = P[face]; area = np.zeros(3)
                    for j in range(1,len(face)-1):
                        area += np.cross(q[j]-q[0],q[j+1]-q[0])/2
                        volume += np.dot(q[0]-center,np.cross(q[j]-center,q[j+1]-center))/6
                    assert np.linalg.norm(area) > 0 and np.dot(area,q[0]-center) > 0
                    edges.update(zip(face,face[1:]+face[:1]))
                assert all(n == 1 and edges[(b,a)] == 1 for (a,b),n in edges.items())
                expected = g['CellData/volume'][cs+i]
                max_volume_error = max(max_volume_error, abs(volume-expected))
                assert volume > 0 and abs(volume-expected) <= 1e-12
            ps += npnt; fs += nf; cs += nc
            fcs += int(g['NumberOfFaceConnectivityIds'][rank]); pfs += int(g['NumberOfPolyhedronToFaceIds'][rank])
        print(f'rule 5 PASS (max divergence error {max_volume_error:.3g})')
        for key in ['FaceConnectivity','FaceOffsets','PolyhedronToFaces','PolyhedronOffsets',
                    'NumberOfFaces','NumberOfFaceConnectivityIds','NumberOfPolyhedronToFaceIds']:
            assert g[key].dtype == np.dtype('int64') and np.all(g[key][:] >= 0)
        assert sum(g['NumberOfFaceConnectivityIds'][:]) == len(g['FaceConnectivity'])
        assert sum(g['NumberOfPolyhedronToFaceIds'][:]) == len(g['PolyhedronToFaces'])
        print('rule 6 PASS')
        for name in NAMES[:2]:
            b = root[name]; npnt = sum(b['NumberOfPoints'][:]); nc = sum(b['NumberOfCells'][:])
            for field in b['PointData']:
                assert len(b['PointData'][field]) == npnt
            for field in ['layer_id','material_id','cell_kind']:
                assert b['CellData'][field].dtype == np.dtype('int32') and len(b['CellData'][field]) == nc
            if 'elem_gid' in b['CellData']:
                assert b['CellData/elem_gid'].dtype == np.dtype('int64')
        assert g['CellData/volume'].dtype == np.dtype('float64')
        print('rule 7 PASS')
        for i,name in enumerate(NAMES):
            assert root[name].attrs['Index'] == i
            category = 'volume' if i<2 else 'boundary' if i==2 else 'lagrangian'
            assert isinstance(root['Assembly'][category].get(name,getlink=True),h5py.SoftLink)
        print('rule 8 PASS')
