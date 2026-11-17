"""Generate small, repeatable D02 fixtures; binaries belong outside git."""
import argparse
import hashlib
import json
from pathlib import Path
import h5py
import numpy as np
import vtkhdf_h5 as W
from geom import CORNERS, FACES, interpolate, analytic


def atomic_json(path, value):
    path = Path(path)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(json.dumps(value, indent=2) + '\n')
    tmp.replace(path)


def part(step, kind=0, empty=False, poly=False, old=False):
    p = CORNERS.copy()
    if poly:  # Cube clipped at x=0.625, exact volume 0.625.
        p[:, 0] *= .625
    t, u = interpolate(p, step)
    # Affine geometry motion; the stored solution is evaluated in parent coordinates.
    points = p * (1 + .1*step) + [0, 0, .02*step]
    n = 0 if empty else 1
    cd = {('layer' if old else 'layer_id'): np.full(n, 1, np.int32),
          ('material' if old else 'material_id'): np.full(n, 1, np.int32),
          'cell_kind': np.full(n, kind, np.int32),
          'elem_gid': np.full(n, 2**53 + 17 + kind, np.int64)}
    q = dict(points=points[:8*n], types=np.full(n, 42 if poly else 12, np.uint8),
             conn=np.arange(8*n, dtype=np.int64), offsets=[0,8] if n else [0],
             PointData={'T':t[:8*n], 'U':u[:8*n]}, CellData=cd)
    if poly:
        q.update(face_conn=np.array(FACES).ravel() if n else [],
                 face_offsets=np.arange(7)*4 if n else [0],
                 p2f=np.arange(6) if n else [], p_offsets=[0,6] if n else [0])
    return q


def pdpart(step, empty=False, particles=False):
    p = part(step, empty=empty)
    if particles:
        p['points'] = p['points'][:1]
        p['PointData'] = {k:v[:1] for k,v in p['PointData'].items()}
        p['verts'] = ([0], [0,1]) if not empty else ([], [0])
    else:
        p['points'] = p['points'][[4,5,6,7]] if not empty else p['points']
        p['PointData'] = {k:v[[4,5,6,7]] if not empty else v for k,v in p['PointData'].items()}
        p['polys'] = ([0,1,2,3], [0,4]) if not empty else ([], [0])
    return p


def generate_run(path, variant='default'):
    path = Path(path)
    (path/'out').mkdir(parents=True, exist_ok=True)
    old = variant == 'old_names'
    s1 = variant == 's1_compat'
    version = (2,9) if s1 else (2,8)
    names = ['hex', 'cut_poly' if s1 else 'cut',
             'embedded_boundary' if s1 else 'eb_surface', 'particles']
    manifest = dict(schema_version='chipcae.results/1.0', units='SI', stackup='stackup.json',
                    generation=0, complete=False, event_log_format=1, steps=[], fields=[],
                    blocks=[dict(index=i,name=n) for i,n in enumerate(names)], summary=[],
                    partitions={'per_step':[], 'solver_fixed_per_run':variant != 'variable_parts'})
    for name,loc,c,unit,dt in [('T','point',1,'K','float64'),('U','point',3,'m','float64'),
                              ('layer' if old else 'layer_id','cell',1,'1','int32'),
                              ('material' if old else 'material_id','cell',1,'1','int32'),
                              ('cell_kind','cell',1,'1','int32'),('elem_gid','cell',1,'1','int64'),
                              ('sigma_vm','cell',1,'Pa','float64')]:
        f = dict(name=name,location=loc,components=c,unit=unit,dtype=dt)
        if name == 'sigma_vm': f['reserved'] = True
        manifest['fields'].append(f)
    # A real stack fixture, kept verbatim rather than synthesizing another schema.
    source = Path(__file__).resolve().parents[2]/'schema/examples/fcbga_demo.stack.json'
    (path/'stackup.json').write_bytes(source.read_bytes())
    for step in range(3):
        count = 3 if variant != 'variable_parts' or step == 0 else 2
        blocks = []
        for i,name in enumerate(names):
            parts = []
            for rank in range(count):
                empty = rank == count-1
                if i < 2:
                    q = part(step, kind=(2 if rank == 1 else i), empty=empty,
                             poly=i == 1 and variant != 'hex_fallback', old=old)
                else:
                    q = pdpart(step, empty=empty or i == 3, particles=i == 3)
                    if old:
                        for a,b in [('layer_id','layer'),('material_id','material')]:
                            q['CellData'][b] = q['CellData'].pop(a)
                parts.append(q)
            blocks.append((name, 'UnstructuredGrid' if i<2 else 'PolyData', parts, version))
        target = path/f'out/step-{step}.vtkhdf'
        tmp = target.with_suffix('.vtkhdf.tmp')
        W.write_pdc(tmp, blocks, {'volume':names[:2], 'boundary':[names[2]], 'lagrangian':[names[3]]}, version)
        with h5py.File(tmp, 'r+') as f:
            for name in names:
                fd = f['VTKHDF/'+name].create_group('FieldData')
                fd['layer_names'] = np.array([b'substrate', b'bump_eq', b'die_layer'])
                fd['material_names'] = np.array([b'organic_eq', b'SAC305_ball_eq', b'underfill', b'silicon'])
        tmp.replace(target)
        checksum = hashlib.sha256(target.read_bytes()).hexdigest()
        manifest['steps'].append(dict(n=step,time=step*.5,file=f'out/step-{step}.vtkhdf',complete=True,checksum=checksum))
        manifest['partitions']['per_step'].append(count)
        # Full Q2 top surface has extrema at corners and centre, plane removal is symmetric.
        top = np.array([[x,y,1] for x in (0,.5,1) for y in (0,.5,1)])
        t,u = analytic(top,step)
        plane = np.column_stack((top[:,:2], np.ones(9)))
        residual = u[:,2] - plane @ np.linalg.lstsq(plane,u[:,2],rcond=None)[0]
        for name,value,unit,method in [('warpage_pv',np.ptp(residual),'m','full Q2 top nodes, least-squares plane removed'),
                                       ('T_max',t.max(),'K','analytic Q2 maximum'),
                                       ('U_mag_max',np.linalg.norm(u,axis=1).max(),'m','analytic Q2 maximum')]:
            manifest['summary'].append(dict(name=name,value=float(value),unit=unit,step=step,method=method))
        manifest['generation'] += 1
        manifest['complete'] = step == 2
        published = {k:v for k,v in manifest.items() if not (s1 and k == 'partitions')}
        atomic_json(path/'manifest.json', published)
    return manifest


def temporal(path):
    parts = []
    for step in range(3):
        for rank in range(2):
            q = part(step,kind=1,poly=True)
            # Two neighbouring cubes share points, but each owns six outward faces.
            p = np.vstack((CORNERS, [[2,0,0],[2,1,0],[2,0,1],[2,1,1]]))
            cells = [np.arange(8), np.array([1,8,9,2,5,10,11,6])]
            q['points'] = p*(1+.1*step)+[rank*3,0,.02*step]
            q['types'] = np.array([42,42],np.uint8)
            q['conn'] = np.concatenate(cells)
            q['offsets'] = [0,8,16]
            q['face_conn'] = np.concatenate([c[np.array(FACES).ravel()] for c in cells])
            q['face_offsets'] = np.arange(13)*4
            q['p2f'] = np.arange(12)
            q['p_offsets'] = [0,6,12]
            t,u = analytic(p,step)
            q['PointData'] = {'T':t,'U':u}
            q['CellData'] = {k:np.concatenate((v,v)) for k,v in q['CellData'].items()}
            parts.append(q)
    with h5py.File(path,'w') as f:
        g = f.create_group('VTKHDF')
        W._attrs(g,'UnstructuredGrid',(2,8))
        W.write_ug(g,parts,(2,8))
        s = g.create_group('Steps'); s.attrs['NSteps'] = 3
        s['Values'] = [0.,.5,1.]
        s['NumberOfParts'] = np.array([2,2,2],np.int64)
        for key,n in [('PartOffsets',2),('PointOffsets',24),('CellOffsets',4),
                      ('ConnectivityIdOffsets',32),('FaceConnectivityOffsets',96),
                      ('FaceOffsetsOffsets',24),('PolyhedronToFaceIdOffsets',24)]:
            s[key] = np.arange(3,dtype=np.int64)*n


def reject_negative(path):
    """Preflight before handing any file to vtkHDFReader (which may crash)."""
    with h5py.File(path) as f:
        def check(name,obj):
            if name.endswith('PolyhedronToFaces') and np.any(obj[:] < 0):
                raise ValueError('unsupported negative PolyhedronToFaces reference')
        f.visititems(check)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    for variant in ['default','hex_fallback','old_names','s1_compat','variable_parts']:
        generate_run(args.out/variant,variant)
    temporal(args.out/'temporal_ug.vtkhdf')
    import shutil
    bad = args.out/'reject_negative.vtkhdf'
    shutil.copyfile(args.out/'default/out/step-0.vtkhdf',bad)
    with h5py.File(bad,'r+') as f: f['VTKHDF/cut/PolyhedronToFaces'][0] = -1
    from mock_solver import fixtures, run
    fixtures(args.out/'events')
    run(args.out/'default')
    print(f'D02 fixtures: {args.out}')

if __name__ == '__main__': main()
