"""Public cut CLI, coupled heat, rejected inputs, VTK 2.8 and MPI ownership."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import h5py
import numpy as np
import vtk

exe, mpirun, source = sys.argv[1:]
names = ['hex', 'cut', 'eb_surface', 'particles']
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    for file in (Path(source)/'examples/s2_solve').glob('*.json'):
        shutil.copy(file, root/file.name)
    original = json.loads((root/'case.json').read_text())
    stack = json.loads((root/'stack.json').read_text())
    def run(name, case=None, ranks=1, fallback=False, error=None, error_code=13):
        (root/'case.json').write_text(json.dumps(case or original))
        (root/'profile.json').write_text(json.dumps({'schema':'chipcae.run_profile/1.0', 'ranks':ranks,'backend':'cpu','gpus':0}))
        out = root/name
        command = [exe, '--case', str(root/'case.json'), '--profile', str(root/'profile.json'), '--out', str(out)]
        if fallback: command += ['--cut-output', 'hex']
        if ranks > 1: command = [mpirun, '-np', str(ranks), *command]
        result = subprocess.run(command, capture_output=True, text=True, timeout=240)
        if error:
            assert result.returncode == error_code, result.stderr
            assert error in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        manifest = json.loads((out/'manifest.json').read_text())
        assert [b['name'] for b in manifest['blocks']] == names
        summary = {s['name']: s['value'] for s in manifest['summary']}
        assert 'least-squares plane' in manifest['summary'][0]['method']
        path = out/'out/step-0.vtkhdf'
        with h5py.File(path) as file:
            r = file['VTKHDF']
            for name in names:
                group = r[name]
                np.testing.assert_array_equal(group.attrs['Version'], [2,8])
                assert group['Points'].shape[1] == 3
                if name != 'particles':
                    points = len(group['Points'])
                    if points:
                        assert group['PointData/T'].shape == (points,)
                        assert group['PointData/U'].shape == (points,3)
                    cells = int(sum(group['NumberOfCells'][:])) if name != 'eb_surface' else int(sum(group['Polygons/NumberOfCells'][:]))
                    for field in ['layer_id','material_id','cell_kind','elem_gid']:
                        assert len(group['CellData/'+field]) == cells
            for category, block in [('volume','hex'),('volume','cut'),('boundary','eb_surface'),('lagrangian','particles')]:
                assert r['Assembly/'+category].get(block, getlink=True).path == '/VTKHDF/'+block
            assert len(r['eb_surface/Polygons/Connectivity']) > 0
            if fallback:
                assert len(r['cut/Types']) == 0
                assert set(r['hex/Types'][:]) == {12}
                assert 1 in r['hex/CellData/cell_kind'][:]
            else:
                assert set(r['cut/Types'][:]) == {42}
                assert set(r['hex/CellData/cell_kind'][:]) == {0}
        reader = vtk.vtkHDFReader(); reader.SetFileName(str(path)); reader.Update()
        data = reader.GetOutputDataObject(0)
        assert data.GetNumberOfPartitionedDataSets() == 4
        for i,name in enumerate(names):
            assert data.GetMetaData(i).Get(vtk.vtkCompositeDataSet.NAME()) == name
            assert data.GetPartitionedDataSet(i).GetNumberOfPartitions() == ranks
            for rank in range(ranks):
                assert data.GetPartition(i,rank) is not None
        assert sum(data.GetPartition(2,i).GetNumberOfCells() for i in range(ranks)) > 0
        print(out.name, summary, flush=True)
        return summary
    one = run('one')
    with h5py.File(root/'one/out/step-0.vtkhdf') as file:
        points = file['VTKHDF/hex/Points'][:]
        displacement = file['VTKHDF/hex/PointData/U'][:]
        mask = (abs(points[:,2])<1e-12) & (abs(points[:,0]-.01)<=.00500001) & (abs(points[:,1]-.01)<=.00500001)
        x,y = points[mask,0]-.01,points[mask,1]-.01
        fit = np.linalg.lstsq(np.array([x*0+1,x,y,x*x,x*y,y*y]).T,displacement[mask,2],rcond=None)[0]
        ratio = 25/110
        analytic = 6*(5e-6-15e-6)*(-100)*4/(.002*(12+(1+ratio)*(1+1/ratio)))
        error = abs(-(fit[3]+fit[5])/analytic-1)
        print('cut bilayer Timoshenko center-curvature relative error',error,flush=True)
        assert error <= .01
    two = run('two', ranks=2)
    difference = abs(two['warpage_pv']/one['warpage_pv']-1)
    assert difference <= 1e-8, difference
    fallback = run('fallback',fallback=True)
    assert fallback == one
    # Heat first, natural embedded convection; constant T is exact on any cut.
    thermal = copy.deepcopy(original)
    thermal['physics'].insert(0, {'id':'thermal','solver':'fem.conduction','mode':'steady'})
    thermal['physics'][1].update(temp_from='thermal', T_ref=300)
    thermal['loads'] = [{'type':'convection','target':'boundary:embedded','h':10,'T_inf':350}]
    result = run('embedded-convection',thermal)
    assert abs(result['T_max']-350) < 1e-8
    thermal['loads'] = [{'type':'fixed_temperature','target':'boundary:embedded','T':350}]
    run('fixed-temperature',thermal,error='Nitsche is not implemented')
    fixed = copy.deepcopy(original)
    fixed['constraints'] = {'fixed':[{'target':'boundary:embedded'}]}
    run('fixed-displacement',fixed,error='Nitsche is not implemented')
    invalid = copy.deepcopy(stack)
    invalid['outline']['polygon'] = [[0,0],[.02,0],[.01,.01],[.02,.02],[0,.02]]
    (root/'stack.json').write_text(json.dumps(invalid))
    run('nonconvex',error='non-convex')
    (root/'stack.json').write_text(json.dumps(stack))
    # Padded non-power-of-two grid exercises actual-cell 3-2-1 selection.
    padded = copy.deepcopy(original)
    padded['mesh'].update(nx=7,ny=5,levels=2)
    run('padded',padded)

    tiny = copy.deepcopy(stack)
    tiny['outline']['polygon'] = [[0,0],[.02,0],[.02,.00001],[0,.00001]]
    (root/'stack.json').write_text(json.dumps(tiny))
    run('insufficient-constraints',error='six DOFs',error_code=14)
