"""Exercise the public C++ writer, then independently inspect HDF5 and VTK."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

class ResultsTest(unittest.TestCase):
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
            self.assertEqual([s['n'] for s in m['steps']], [0,1])
            self.assertEqual(len(m['summary']),6)
            self.assertEqual(next(s['value'] for s in m['summary'] if s['name']=='warpage_pv'),0.001)
            self.assertEqual({s['name'] for s in m['summary']}, {'warpage_pv','T_max','U_abs_max'})
            self.assertTrue(next(f for f in m['fields'] if f['name']=='sigma_vm')['reserved'])
            self.assertEqual(json.loads((run/'stackup.json').read_text()), json.loads(pathlib.Path(sys.argv[2]).read_text()))
            with h5py.File(run/m['steps'][0]['file']) as f:
                root=f['VTKHDF']
                self.assertEqual(root.attrs['Type'].decode().rstrip('\0'), 'PartitionedDataSetCollection')
                self.assertGreaterEqual(tuple(root.attrs['Version']), (2,9))
                self.assertEqual(len(root['Assembly']),4)
                self.assertEqual(list(root['Assembly']),['hex','cut_poly','embedded_boundary','particles'])
                self.assertIsInstance(root['Assembly'].get('hex', getlink=True),h5py.SoftLink)
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
                for name in ['cut_poly','embedded_boundary','particles']:
                    self.assertEqual(root[name]['Points'].shape,(0,3))
                    self.assertEqual(root[name].attrs['Index'], ['hex','cut_poly','embedded_boundary','particles'].index(name))
                    self.assertEqual(len(root[name]['NumberOfPoints']),ranks)
            try:
                import vtk
            except ImportError:
                print('VTK readback: SKIP (vtk unavailable)')
            else:
                reader=vtk.vtkHDFReader(); reader.SetFileName(str(run/'out/step-0.vtkhdf'));reader.Update()
                output=reader.GetOutputDataObject(0)
                self.assertIsNotNone(output)
                self.assertEqual(output.GetNumberOfPartitionedDataSets(),4)
                self.assertEqual(output.GetPartition(0,0).GetNumberOfPoints(),8)
                grid=output.GetPartition(0,0)
                self.assertEqual({grid.GetPointData().GetArrayName(i) for i in range(grid.GetPointData().GetNumberOfArrays())}, {'T','U'})
                print('VTK readback: 4 blocks, 8 points')

if __name__=='__main__':
    try:
        import h5py
    except ImportError:
        print('SKIP: h5py unavailable')
        sys.exit(77)
    unittest.main(argv=[sys.argv[0]])
