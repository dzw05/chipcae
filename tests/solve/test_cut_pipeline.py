"""Public cut CLI, coupled heat, rejected inputs, VTK 2.8 and MPI ownership."""
import copy
import struct
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import h5py
import numpy as np
import vtk

exe, mpirun, source = sys.argv[1:]
error_codes = {name: int(code) for code, name in re.findall(
    r'code:\s*(\d+),\s*name:\s*(\w+)',
    (Path(source) / 'protocol/errors.yaml').read_text())}
assert {'HIERARCHY_INVALID', 'INPUT_INVALID'} <= error_codes.keys(), error_codes
acceptance_failures = []
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
            assert result.returncode == error_code, (name, result.returncode, error_code, result.stderr)
            assert error in result.stderr, result.stderr
            assert not list(out.glob('out/*.vtkhdf')), list(out.rglob('*.vtkhdf'))
            return
        assert result.returncode == 0, (name, ranks, result.returncode, result.stderr)
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
                    if cells:
                        ids = group['CellData/layer_id'][:]
                        assert set(ids) <= set(range(1, len(stack['layers'])+1))
                        if len(stack['layers']) == 2 and name != 'particles':
                            gids = group['CellData/elem_gid'][:]
                            np.testing.assert_array_equal(ids, 1 + (gids % 4)//2)
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
        print(out.name, summary, 'mesh_sha256', manifest['mesh']['mesh_sha256'],
              'summary_bits', {quantity: struct.pack('>d', float(summary[quantity])).hex()
                               for quantity in ('T_max', 'warpage_pv')}, flush=True)
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
    layered = copy.deepcopy(thermal)
    layered['loads'] = [{'type':'fixed_temperature','target':'face:zmin','T':300},
                        {'type':'fixed_temperature','target':'face:zmax','T':400}]
    run('layered-heat',layered)
    with h5py.File(root/'layered-heat/out/step-0.vtkhdf') as file:
        for block in ['hex','cut','eb_surface']:
            z = file['VTKHDF/'+block+'/Points'][:,2]
            temperature = file['VTKHDF/'+block+'/PointData/T'][:]
            resistance = np.minimum(z,.001)+np.maximum(z-.001,0)/10
            error = np.max(abs(temperature-(300+100*resistance/.0011)))
            print(block,'layered conduction max error K',error,flush=True)
            assert error <= 1e-8
    interlayer = copy.deepcopy(layered)
    interlayer['loads'].append({'type':'convection','target':'layer:base','h':1000,'T_inf':350})
    # Nonconstant solution exercises Robin terms on trimmed original interlayer faces.
    robin = run('interlayer-convection', interlayer)
    assert 300 < robin['T_max'] <= 400
    with h5py.File(root/'interlayer-convection/out/step-0.vtkhdf') as f:
        t = f['VTKHDF/cut/PointData/T'][:]
        assert np.ptp(t) > 10
    thermal['loads'] = [{'type':'fixed_temperature','target':'boundary:embedded','T':350}]
    run('fixed-temperature',thermal,error='Nitsche is not implemented')
    fixed = copy.deepcopy(original)
    fixed['constraints'] = {'fixed':[{'target':'boundary:embedded'}]}
    run('fixed-displacement',fixed,error='Nitsche is not implemented')
    # These acceptance cases catch a missing general-cutter call and convex-only
    # summary sampling. Collect failures so every shape/rank/diagnostic executes.
    def acceptance(name, action):
        try:
            return action()
        except (AssertionError, KeyError, ValueError, subprocess.TimeoutExpired) as error:
            acceptance_failures.append(f'{name}: {error}')
            print(f'FAIL {name}: {error}', flush=True)
        finally:
            (root/'stack.json').write_text(json.dumps(stack))

    def output_volume(name):
        total = 0.0
        seen = set()
        with h5py.File(root/name/'out/step-0.vtkhdf') as file:
            # Output contains owned cells. Check GIDs across both solid blocks
            # rather than summing replicated meshes or EB/particle measures.
            for block in ('hex', 'cut'):
                grid = file['VTKHDF/'+block]
                gids = grid['CellData/elem_gid'][:]
                assert len(set(map(int, gids))) == len(gids), (name, block, 'duplicate GID')
                assert not seen.intersection(map(int, gids)), (name, 'duplicate solid cell')
                seen.update(map(int, gids))
                if 'vtkGhostType' in grid['CellData']:
                    assert np.all(grid['CellData/vtkGhostType'][:] == 0), name
                if block == 'cut':
                    volumes = grid['CellData/volume'][:]
                    assert len(volumes) == len(gids)
                    total += float(np.sum(volumes))
                else:
                    # VTKHDF connectivity uses indices local to each partition.
                    point_start = cell_start = conn_start = 0
                    for npart, ncell, nconn in zip(grid['NumberOfPoints'][:],
                                                  grid['NumberOfCells'][:],
                                                  grid['NumberOfConnectivityIds'][:]):
                        npart, ncell, nconn = int(npart), int(ncell), int(nconn)
                        assert nconn == 8*ncell
                        points = grid['Points'][point_start:point_start+npart]
                        conn = grid['Connectivity'][conn_start:conn_start+nconn].reshape(-1, 8)
                        if ncell:
                            corners = points[conn]
                            total += float(np.sum(np.prod(np.ptp(corners, axis=1), axis=1)))
                        point_start += npart
                        cell_start += ncell
                        conn_start += nconn
                    assert cell_start == len(gids)
        return total

    outline_case = copy.deepcopy(layered)
    thickness = sum(layer['thickness'] for layer in stack['layers']
                    if layer['kind'].lower() != 'void')
    # Exercise the volume reader on existing convex np1/np2 output as well.
    polygon = np.asarray(stack['outline']['polygon'])
    convex_area = abs(np.sum(polygon[:, 0]*np.roll(polygon[:, 1], -1)
                             - polygon[:, 1]*np.roll(polygon[:, 0], -1)))/2
    for name in ('one', 'two'):
        volume = output_volume(name)
        assert abs(volume/(convex_area*thickness)-1) <= 1e-10, (name, volume)
    shapes = {
        'V': ([[0,0],[.0064,0],[.0103,.0087],[.0142,0],[.02,0],[.02,.02],[0,.02]],
              0.00036607),
        'L': ([[0,0],[.02,0],[.02,.0137],[.0113,.0137],[.0113,.02],[0,.02]],
              3.4519e-4),
    }
    for shape, (ring, area) in shapes.items():
        results = {}
        for ranks in (1, 2):
            name = f'nonconvex-{shape}-np{ranks}'
            def positive(name=name, ranks=ranks, ring=ring, area=area):
                data = copy.deepcopy(stack)
                data['outline']['polygon'] = ring
                (root/'stack.json').write_text(json.dumps(data))
                summary = run(name, outline_case, ranks=ranks)
                volume = output_volume(name)
                error = abs(volume/(area*thickness)-1)
                assert error <= 1e-10, (name, volume, area*thickness, error)
                for quantity in ('T_max', 'warpage_pv'):
                    assert np.isfinite(summary[quantity]), (name, summary)
                assert summary['warpage_pv'] > 0, (name, summary)
                print(name, 'volume relative error', error, flush=True)
                return summary
            summary = acceptance(name, positive)
            if summary is not None:
                results[ranks] = summary
        if len(results) == 2:
            for quantity in ('T_max', 'warpage_pv'):
                def compare(quantity=quantity):
                    one, two = results[1][quantity], results[2][quantity]
                    assert one != 0, (shape, quantity, one)
                    difference = abs(two-one)/abs(one)
                    print(shape, quantity, 'MPI relative difference', difference, flush=True)
                    assert difference <= 1e-9, (shape, quantity, difference)
                acceptance(f'{shape} MPI {quantity}', compare)

    def l_robin():
        data = copy.deepcopy(stack)
        data['outline']['polygon'] = shapes['L'][0]
        (root/'stack.json').write_text(json.dumps(data))
        robin_case = copy.deepcopy(outline_case)
        robin_case['loads'].append({'type':'convection', 'target':'boundary:embedded',
                                    'h':1000, 'T_inf':350})
        summary = run('nonconvex-L-robin', robin_case)
        assert 300 < summary['T_max'] <= 400, summary
    acceptance('L embedded Robin', l_robin)

    def l_with_region_planes():
        # Probe the combination without requiring the material-cut guard to
        # accept regions; this combination is outside the acceptance contract.
        data = copy.deepcopy(stack)
        data['outline']['polygon'] = shapes['L'][0]
        lid = data['layers'][-1]
        material = lid.pop('material')
        lid['regions'] = [{'name':'lid-region', 'body':'lid', 'material':material,
                           'footprint':copy.deepcopy(stack['outline'])}]
        try:
            (root/'stack.json').write_text(json.dumps(data))
            (root/'case.json').write_text(json.dumps(outline_case))
            (root/'profile.json').write_text(json.dumps({
                'schema':'chipcae.run_profile/1.0', 'ranks':1, 'backend':'cpu', 'gpus':0}))
            out = root/'nonconvex-L-region-planes'
            result = subprocess.run(
                [exe, '--case', str(root/'case.json'), '--profile', str(root/'profile.json'),
                 '--out', str(out)], capture_output=True, text=True, timeout=240)
            print('L with nonempty region planes: exit', result.returncode,
                  'stdout', result.stdout, 'stderr', result.stderr, flush=True)
            if result.returncode == 0:
                manifest = json.loads((out/'manifest.json').read_text())
                print('L with nonempty region planes: summary', manifest['summary'], flush=True)
        except subprocess.TimeoutExpired as error:
            print('L with nonempty region planes: timeout', error, flush=True)
        finally:
            (root/'stack.json').write_text(json.dumps(stack))
    l_with_region_planes()

    for name, ring, diagnostic in (
        ('bow-tie', [[0,0],[.02,.02],[.02,0],[0,.02]], 'self-intersecting outline'),
        ('narrow-R', [[0,0],[.0105,0],[.0105,.0043],[.0043,.0043],
                      [.0043,.0157],[.0157,.0157],[.0157,.0043],[.0115,.0043],
                      [.0115,0],[.02,0],[.02,.02],[0,.02]],
         'disconnected solid in one cell'),
    ):
        def rejected(name=name, ring=ring, diagnostic=diagnostic):
            data = copy.deepcopy(stack)
            data['outline']['polygon'] = ring
            (root/'stack.json').write_text(json.dumps(data))
            invalid_case = copy.deepcopy(outline_case)
            invalid_case['mesh'].update(nx=8, ny=8, levels=1)
            run(name, invalid_case, error=diagnostic, error_code=13)
        acceptance(name, rejected)
    # Padded non-power-of-two grid exercises actual-cell 3-2-1 selection.
    padded = copy.deepcopy(original)
    padded['mesh'].update(nx=7,ny=5,levels=2)
    run('padded',padded)

    tiny = copy.deepcopy(stack)
    tiny['outline']['polygon'] = [[0,0],[.02,0],[.02,.00001],[0,.00001]]
    (root/'stack.json').write_text(json.dumps(tiny))
    run('insufficient-constraints',error='six DOFs',error_code=14)

    (root/'stack.json').write_text(json.dumps(stack))
    # Public fcbga package with an external, hash-checked outline sidecar.
    package = json.loads((Path(source)/'schema/examples/fcbga_demo.stack.json').read_text())
    ring = [[0,0],[20000,0],[20000,15000],[15000,20000],[0,20000]]
    binary = b'CCPOLY01' + struct.pack('<dII',1e-6,1,7) + b'outline'
    binary += struct.pack('<Iqq',1,0,len(ring))
    binary += b''.join(struct.pack('<qq',*v) for v in ring)
    (root/'outline.polyset').write_bytes(binary)
    package['external_refs'] = [{'path':'outline.polyset','sha256':hashlib.sha256(binary).hexdigest()}]
    (root/'stack.json').write_text(json.dumps(package))
    # Freeze illustrative temperature tables at their first sample for the
    # constant-property solve backend; keep all package material mappings.
    library = json.loads((Path(source)/'schema/examples/fcbga_demo.materials.json').read_text())
    for material in library['materials']:
        for section in ['elastic','thermal_expansion']:
            table = material[section]['table']
            table['temperatures'] = table['temperatures'][:1]
            table['columns'] = {key:values[:1] for key,values in table['columns'].items()}
            table['interpolation'] = 'constant'
            table['extrapolation'] = 'clamp'
    (root/'materials.json').write_text(json.dumps(library))
    demo = json.loads((Path(source)/'schema/examples/fcbga_demo.case.json').read_text())
    demo['stack'] = {'ref':'stack.json'}
    demo['materials'] = {'ref':'materials.json'}
    demo["mesh"].update(nx=4,ny=4,levels=1,max_ratio=100)
    demo['solver'] = {'linear':{'preconditioner':'direct'}}
    demo['output']['fields'] = ['T','U','layer_id','material_id','cell_kind','elem_gid']
    stack = package
    run('fcbga-polyset-one',demo)
    run('fcbga-polyset-two',demo,ranks=2)

    # Minimal silicon fixture isolates mesh validation from physics and cuts.
    stack = dict(id='digest', unit='m', outline=dict(box=[0, 0, 1, 1]),
                 symmetry='none', package_type='FCBGA', layers=[
                     dict(name='solid', kind='Explicit', material='silicon',
                          thickness=.1, n_elem_min=1, grading=1)])
    (root / 'stack.json').write_text(json.dumps(stack))
    table = lambda columns: dict(temperatures=[300], columns=columns,
                                 interpolation='constant', extrapolation='clamp')
    material = dict(
        id='silicon', schema_version='1', unit_system='SI', model='thermo_elastic',
        density=2330, elastic=dict(symmetry='isotropic',
                                  table=table({'E': [130e9], 'nu': [.3]})),
        thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                               stress_free_temperature=300,
                               table=table({'alpha': [2.6e-6]})),
        provenance=dict(source_type='illustrative', confidence='illustrative'))
    (root / 'materials.json').write_text(json.dumps(
        {'schema': 'chipcae.material_library/1', 'materials': [material]}))
    case = {
        'schema': 'chipcae.case/1.0', 'stack': {'ref': 'stack.json'},
        'materials': {'ref': 'materials.json'},
        'mesh': {'type': 'aligned_cartesian_cut', 'nx': 4, 'ny': 2, 'levels': 1,
                 'min_elems_per_layer': 1, 'max_ratio': 10},
        'physics': [{'id': 'mech', 'solver': 'fem.solid'}],
        'loads': [{'type': 'uniform_temperature', 'T_ref': 300, 'T_target': 300}],
        'constraints': {'rigid': '3-2-1'},
        'solver': {'linear': {'preconditioner': 'direct'}},
        'output': {'format': 'vtkhdf'}}
    for name, mesh_options, ranks, error_name, stderr_text in [
        ('hierarchy-invalid', {'levels': 32}, 1, 'HIERARCHY_INVALID', 'HIERARCHY_INVALID'),
        ('hierarchy-levels-ranks', {'nx': 2, 'ny': 2, 'levels': 2}, 2,
         'HIERARCHY_INVALID', 'hierarchy/levels'),
        ('solve_mesh_error_input_invalid', {'cell_budget': 1}, 1, 'INPUT_INVALID', 'cell_budget'),
    ]:
        invalid_case = copy.deepcopy(case)
        invalid_case['mesh'].update(mesh_options)
        try:
            run(name, invalid_case, ranks=ranks, error=stderr_text,
                error_code=error_codes[error_name])
            print(f'{name}: exit={error_codes[error_name]}, stderr contains {stderr_text}', flush=True)
        except (AssertionError, KeyError, subprocess.TimeoutExpired) as error:
            acceptance_failures.append(f'{name}: {error}')
    assert not acceptance_failures, '\n'.join(acceptance_failures)
