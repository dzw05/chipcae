"""Exercise the public C++ writer, then independently inspect HDF5 and VTK."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

class ResultsTest(unittest.TestCase):
    def command(self, run, prefix=None):
        command = [sys.argv[1], str(run), sys.argv[2]]
        if prefix is not None:
            command.append(str(prefix))
        if int(sys.argv[3]) > 1:
            command = [sys.argv[4], '-np', sys.argv[3]] + command
        return command

    def test_partitions_and_failures(self):
        import h5py
        import numpy as np
        ranks = int(sys.argv[3])
        for serial in (False, True):
            env = dict(os.environ)
            if serial:
                env['CHIPCAE_RESULTS_SERIAL'] = '1'
            for counts in ([r + 1 for r in range(ranks)],
                           [0] + [r + 1 for r in range(1, ranks)]):
                if not any(counts):
                    continue
                with self.subTest(serial=serial, counts=counts), tempfile.TemporaryDirectory() as tmp:
                    prefix = pathlib.Path(tmp) / 'partition'
                    expected_points, expected_conn, expected_gid = [], [], []
                    offsets = []
                    for rank, cells in enumerate(counts):
                        lines = [f'{8*cells} {cells}']
                        for cell in range(cells):
                            for x, y, z in ((0,0,0),(1,0,0),(1,1,0),(0,1,0),
                                            (0,0,1),(1,0,1),(1,1,1),(0,1,1)):
                                point = (rank*100+cell*2+x, y, z)
                                expected_points.append(point)
                                lines.append(' '.join(map(str, (*point, rank, cell, 0))))
                        for cell in range(cells):
                            conn = list(range(cell*8, cell*8+8))
                            expected_conn.extend(conn)
                            expected_gid.append(rank*100+cell)
                            lines.append(' '.join(map(str, conn+[rank, cell, cell%3, rank*100+cell])))
                        lines.append('0.001 0.002')
                        pathlib.Path(str(prefix)+f'.{rank}').write_text('\n'.join(lines)+'\n')
                        offsets.extend(range(0, cells*8+1, 8))
                    run = pathlib.Path(tmp) / 'run'
                    subprocess.run(self.command(run, prefix), env=env, check=True, timeout=25)
                    with h5py.File(run/'out/step-0.vtkhdf') as f:
                        g = f['VTKHDF/hex']
                        np.testing.assert_array_equal(g['NumberOfCells'], counts)
                        np.testing.assert_array_equal(g['NumberOfPoints'], np.array(counts)*8)
                        np.testing.assert_array_equal(g['NumberOfConnectivityIds'], np.array(counts)*8)
                        np.testing.assert_array_equal(g['Offsets'], offsets)
                        np.testing.assert_array_equal(g['Connectivity'], expected_conn)
                        np.testing.assert_array_equal(g['Points'], expected_points)
                        np.testing.assert_array_equal(g['CellData/elem_gid'], expected_gid)
                        np.testing.assert_array_equal(g['PointData/U'],
                            [[r, c, 0] for r, n in enumerate(counts) for c in range(n) for _ in range(8)])
            # Rank zero allocation failure and later-rank setup/write/flush/close
            # failures must terminate on every rank without publishing a step.
            faults = ['H5Fflush', 'H5Fclose']
            if serial or ranks == 1 or os.environ.get('CHIPCAE_RESULTS_TEST_PARALLEL') != '1':
                faults += ['gather_alloc']
            if not serial and ranks > 1 and os.environ.get('CHIPCAE_RESULTS_TEST_PARALLEL') == '1':
                faults += ['dataset_setup', 'H5Sselect_none', 'H5Dwrite', 'H5Dclose']
            for fault, target in [(f, r) for f in faults for r in range(ranks)]:
                if (serial or os.environ.get('CHIPCAE_RESULTS_TEST_PARALLEL') != '1') and fault in ('H5Fflush', 'H5Fclose') and target != 0:
                    continue
                with self.subTest(serial=serial, fault=fault), tempfile.TemporaryDirectory() as tmp:
                    run = pathlib.Path(tmp) / 'run'
                    fault_env = dict(env, CHIPCAE_RESULTS_FAIL=fault,
                                     CHIPCAE_RESULTS_FAIL_RANK=str(target))
                    result = subprocess.run(self.command(run), env=fault_env, timeout=25,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    if sys.argv[5].upper() not in ('ON','TRUE','YES','1'):
                        self.assertEqual(result.returncode, 0)
                        self.assertTrue((run/'manifest.json').exists())
                        continue
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((run/'out/step-0.vtkhdf').exists())
                    self.assertFalse((run/'manifest.json').exists())

    def test_cut(self):
        import h5py
        import numpy as np
        from collections import Counter
        with tempfile.TemporaryDirectory() as tmp:
            run=pathlib.Path(tmp)/'cut'
            subprocess.run(self.command(run,'cut'),check=True,timeout=25)
            with h5py.File(run/'out/step-0.vtkhdf') as f:
                g=f['VTKHDF/cut']; points=g['Points'][:]
                conn=g['FaceConnectivity'][:]; offsets=g['FaceOffsets'][:7]
                np.testing.assert_array_equal(g['Types'],[42])
                np.testing.assert_array_equal(g['PolyhedronToFaces'],range(6))
                self.assertEqual(offsets[-1],len(conn))
                self.assertEqual(g['Offsets'][1],len(g['Connectivity']))
                self.assertEqual(g['PolyhedronOffsets'][1],len(g['PolyhedronToFaces']))
                edges=Counter(); volume=0
                for a,b in zip(offsets[:-1],offsets[1:]):
                    ids=conn[a:b]; face=points[ids]
                    normal=np.cross(face[1]-face[0],face[2]-face[0])
                    self.assertGreater(np.dot(normal,face.mean(axis=0)-points.mean(axis=0)),0)
                    for x,y in zip(ids,np.roll(ids,-1)): edges[x,y]+=1
                    for i in range(1,len(face)-1):
                        volume+=np.dot(face[0],np.cross(face[i],face[i+1]))/6
                self.assertTrue(all(edges[y,x]==n for (x,y),n in edges.items()))
                self.assertGreater(volume,0)
                self.assertAlmostEqual(volume,1.,delta=1e-12)
            try:
                import vtk
            except ImportError:
                return
            reader=vtk.vtkHDFReader(); reader.SetUseCache(False); reader.SetFileName(str(run/'out/step-0.vtkhdf')); reader.UpdateInformation(); reader.GetFieldDataArraySelection().EnableArray('layer_names'); reader.GetFieldDataArraySelection().EnableArray('material_names'); reader.Update()
            cell=reader.GetOutputDataObject(0).GetPartition(1,0).GetCell(0)
            self.assertEqual(cell.GetNumberOfFaces(),6)
            volume=0
            for i in range(6):
                face=cell.GetFace(i)
                self.assertEqual(face.GetNumberOfPoints(),4)
                xyz=[np.array(face.GetPoints().GetPoint(j)) for j in range(4)]
                for j in (1,2): volume+=np.dot(xyz[0],np.cross(xyz[j],xyz[j+1]))/6
            self.assertAlmostEqual(volume,1.,delta=1e-12)

    def test_roundtrip(self):
        try:
            import h5py
            import numpy as np
        except ImportError:
            self.skipTest('h5py/numpy unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp) / 'run'
            command = [sys.argv[1], str(run), sys.argv[2]]
            ranks = int(sys.argv[3])
            if ranks > 1:
                command = [sys.argv[4], '-np', str(ranks)] + command
            subprocess.run(command, check=True)
            self.assertFalse((run / 'manifest.json.tmp').exists())
            m = json.loads((run / 'manifest.json').read_text())
            self.assertEqual(m['schema_version'], 'chipcae.results/1.0')
            self.assertEqual(m['units'], 'SI')
            self.assertEqual(len(m['blocks']), 4)
            self.assertEqual(m['partitions'], {'per_step':[ranks,ranks], 'solver_fixed_per_run':True})
            for field in m['fields']:
                self.assertEqual(field['dtype'], 'int64' if field['name']=='elem_gid' else
                                 'int32' if field['name'] in ('layer_id','material_id','cell_kind') else 'float64')
            self.assertEqual(m['summary'][0]['method'], 'top surface (z=max) Q2 nodes; least-squares plane w=a+bx+cy removed; PV=max-min residual')
            self.assertEqual([s['n'] for s in m['steps']], [0,1])
            self.assertEqual(len(m['summary']),6)
            self.assertEqual(next(s['value'] for s in m['summary'] if s['name']=='warpage_pv'),0.001)
            self.assertEqual({s['name'] for s in m['summary']}, {'warpage_pv','T_max','U_mag_max'})
            self.assertTrue(next(f for f in m['fields'] if f['name']=='sigma_vm')['reserved'])
            self.assertEqual(json.loads((run/'stackup.json').read_text()), json.loads(pathlib.Path(sys.argv[2]).read_text()))
            with h5py.File(run/m['steps'][0]['file']) as f:
                root=f['VTKHDF']
                self.assertEqual(root.attrs['Type'].decode().rstrip('\0'), 'PartitionedDataSetCollection')
                self.assertEqual(root.attrs.get_id('Type').dtype.kind, 'S')
                self.assertEqual(tuple(root.attrs['Version']), (2,8))
                self.assertEqual(len(root['Assembly']),3)
                self.assertEqual(list(root['Assembly']),['volume','boundary','lagrangian'])
                for path,block in [('volume/hex','hex'),('volume/cut','cut'),
                                   ('boundary/eb_surface','eb_surface'),('lagrangian/particles','particles')]:
                    link=root['Assembly'].get(path,getlink=True)
                    self.assertIsInstance(link,h5py.SoftLink)
                    self.assertEqual(link.path,'/VTKHDF/'+block)
                g=root['hex']
                np.testing.assert_array_equal(g['NumberOfPoints'], [8]+[0]*(ranks-1))
                np.testing.assert_array_equal(g['NumberOfCells'], [1]+[0]*(ranks-1))
                np.testing.assert_array_equal(g['Offsets'], [0,8]+[0]*(ranks-1))
                self.assertEqual(g['Points'].shape,(8,3))
                for name in ['Points','PointData/T','PointData/U']:
                    self.assertEqual(g[name].dtype,np.dtype('float64'))
                self.assertEqual(g['Connectivity'].dtype,np.dtype('int64'))
                self.assertEqual(g['Types'].dtype,np.dtype('uint8'))
                np.testing.assert_array_equal(g['Types'],[12])
                self.assertEqual(g['PointData/U'].shape,(8,3))
                self.assertEqual(g['PointData/T'].shape,(8,))
                np.testing.assert_allclose(g['PointData/T'],193.15)
                self.assertEqual(g['CellData/elem_gid'].dtype,np.dtype('int64'))
                for name in ['layer_id','material_id','cell_kind']:
                    self.assertEqual(g['CellData/'+name].dtype,np.dtype('int32'))
                    self.assertEqual(g['CellData/'+name].shape,(1,))
                self.assertNotIn('sigma_vm',g['CellData'])
                for name in ['cut','eb_surface','particles']:
                    b=root[name]
                    self.assertEqual(tuple(b.attrs['Version']), (2,8))
                    self.assertEqual(b['Points'].shape,(0,3))
                    self.assertEqual(b['Points'].dtype,np.dtype('float64'))
                    def check_topology_dtype(path,dataset):
                        if isinstance(dataset,h5py.Dataset) and not path.startswith(('FieldData/','PointData/','CellData/')):
                            dtype='float64' if path=='Points' else 'uint8' if path=='Types' else 'int64'
                            self.assertEqual(dataset.dtype,np.dtype(dtype),path)
                    b.visititems(check_topology_dtype)
                    np.testing.assert_array_equal(b['NumberOfPoints'], [0]*ranks)
                    self.assertEqual(b['PointData/U'].shape,(0,3))
                    for field, dtype in [('PointData/T','float64'), ('PointData/U','float64'),
                                         ('CellData/layer_id','int32'), ('CellData/material_id','int32'),
                                         ('CellData/cell_kind','int32'), ('CellData/elem_gid','int64')]:
                        self.assertEqual(b[field].dtype,np.dtype(dtype))
                        self.assertEqual(len(b[field]),0)
                    for t in ([b] if name=='cut' else [b[k] for k in ('Vertices','Lines','Polygons','Strips')]):
                        for count in ('NumberOfCells','NumberOfConnectivityIds'):
                            np.testing.assert_array_equal(t[count], [0]*ranks)
                        np.testing.assert_array_equal(t['Offsets'], [0]*ranks)
                        self.assertEqual(t['Connectivity'].dtype,np.dtype('int64'))
                        self.assertEqual(len(t['Connectivity']),0)
                    if name=='cut':
                        self.assertEqual(b['Types'].dtype,np.dtype('uint8'))
                        self.assertEqual(len(b['Types']),0)
                        for k in ('FaceOffsets','PolyhedronOffsets'):
                            np.testing.assert_array_equal(b[k],[0]*ranks)
                        for k in ('FaceConnectivity','PolyhedronToFaces'):
                            self.assertEqual(b[k].shape,(0,))
                        for k in ('NumberOfFaces','NumberOfFaceConnectivityIds','NumberOfPolyhedronToFaceIds'):
                            np.testing.assert_array_equal(b[k],[0]*ranks)
                    self.assertEqual(b['FieldData/layer_names'].asstr()[:].tolist(), ['substrate','die_attach','Si'])
                    self.assertEqual(root[name].attrs['Index'], ['hex','cut','eb_surface','particles'].index(name))
                    self.assertEqual(len(root[name]['NumberOfPoints']),ranks)
            try:
                import vtk
            except ImportError:
                print('VTK readback: SKIP (vtk unavailable)')
            else:
                reader=vtk.vtkHDFReader(); reader.SetUseCache(False); reader.SetFileName(str(run/'out/step-0.vtkhdf')); reader.UpdateInformation(); reader.GetFieldDataArraySelection().EnableArray('layer_names'); reader.GetFieldDataArraySelection().EnableArray('material_names');reader.Update()
                output=reader.GetOutputDataObject(0)
                self.assertIsNotNone(output)
                self.assertEqual(output.GetNumberOfPartitionedDataSets(),4)
                self.assertEqual(output.GetPartition(0,0).GetNumberOfPoints(),8)
                grid=output.GetPartition(0,0)
                self.assertEqual({grid.GetPointData().GetArrayName(i) for i in range(grid.GetPointData().GetNumberOfArrays())}, {'T','U'})
                for i,name in enumerate(('hex','cut','eb_surface','particles')):
                    self.assertEqual(output.GetMetaData(i).Get(vtk.vtkCompositeDataSet.NAME()),name)
                    self.assertEqual(output.GetPartitionedDataSet(i).GetNumberOfPartitions(),ranks)
                    grid=output.GetPartition(i,0)
                    self.assertEqual(grid.GetClassName(),'vtkUnstructuredGrid' if i<2 else 'vtkPolyData')
                    self.assertEqual(grid.GetFieldData().GetAbstractArray('layer_names').GetValue(0),'substrate')
                    if i:
                        self.assertEqual(grid.GetNumberOfPoints(),0)
                        self.assertEqual(grid.GetNumberOfCells(),0)
                        self.assertEqual(grid.GetPointData().GetArray('U').GetNumberOfComponents(),3)
                        self.assertEqual(grid.GetPointData().GetArray('U').GetNumberOfTuples(),0)
                        for k in ('layer_id','material_id','cell_kind','elem_gid'):
                            self.assertEqual(grid.GetCellData().GetArray(k).GetNumberOfTuples(),0)
                from vtk.util.numpy_support import vtk_to_numpy
                for block in range(4):
                    for rank in range(ranks):
                        grid=output.GetPartition(block,rank)
                        for name,dtype in [('T','float64'),('U','float64')]:
                            array=grid.GetPointData().GetArray(name)
                            self.assertEqual(vtk_to_numpy(array).dtype,np.dtype(dtype))
                            self.assertEqual(array.GetNumberOfTuples(),grid.GetNumberOfPoints())
                        for name,dtype in [('layer_id','int32'),('material_id','int32'),
                                           ('cell_kind','int32'),('elem_gid','int64')]:
                            array=grid.GetCellData().GetArray(name)
                            self.assertEqual(vtk_to_numpy(array).dtype,np.dtype(dtype))
                            self.assertEqual(array.GetNumberOfTuples(),grid.GetNumberOfCells())
                assembly=output.GetDataAssembly()
                # Reader represents soft-link leaves as dataset IDs on their parent.
                for name, ids in [('volume',(0,1)),('boundary',(2,)),('lagrangian',(3,))]:
                    node=assembly.GetFirstNodeByPath('/assembly/'+name)
                    self.assertGreaterEqual(node,0)
                    self.assertEqual(tuple(assembly.GetDataSetIndices(node)),ids)
                print('VTK readback: 4 blocks, 8 points, complete empty fields and names')

if __name__=='__main__':
    try:
        import h5py
    except ImportError:
        print('SKIP: h5py unavailable')
        sys.exit(77)
    unittest.main(argv=[sys.argv[0]])
