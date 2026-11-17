"""S2 cut geometry, empty MPI rank, fields, VTK and legacy fallback."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
try:
    from check_cut_rules import check, NAMES
    import h5py
    import numpy as np
except ImportError:
    print('SKIP: h5py/numpy unavailable')
    sys.exit(77)


class CutResultsTest(unittest.TestCase):
    def test_s2(self):
        ranks = int(sys.argv[3])
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp)/'cut'
            fallback = pathlib.Path(tmp)/'fallback'
            prefix = [] if ranks == 1 else [sys.argv[4], '-np', str(ranks)]
            command = prefix + [sys.argv[1], str(run), sys.argv[2]]
            subprocess.run(command,check=True)
            subprocess.run(prefix+[sys.argv[1],str(fallback),sys.argv[2],'--cut-as-hex'],check=True)
            path = run/'out/step-0.vtkhdf'
            manifest = json.loads((run/'manifest.json').read_text())
            self.assertEqual([b['name'] for b in manifest['blocks']],NAMES)
            self.assertEqual(next(s['value'] for s in manifest['summary'] if s['name']=='T_max'),308)
            check(path)
            with h5py.File(path) as f:
                root = f['VTKHDF']; g = root['cut']
                expected_counts = [1] if ranks == 1 else [0,1]
                np.testing.assert_array_equal(g['NumberOfCells'],expected_counts)
                np.testing.assert_array_equal(g['NumberOfPoints'],[6] if ranks==1 else [0,6])
                np.testing.assert_array_equal(g['Offsets'],[0,6] if ranks==1 else [0,0,6])
                np.testing.assert_array_equal(g['NumberOfFaces'],[5] if ranks==1 else [0,5])
                np.testing.assert_array_equal(g['NumberOfFaceConnectivityIds'],[18] if ranks==1 else [0,18])
                np.testing.assert_array_equal(g['NumberOfPolyhedronToFaceIds'],[5] if ranks==1 else [0,5])
                np.testing.assert_array_equal(g['Types'],[42])
                np.testing.assert_allclose(g['CellData/volume'],[.5],rtol=1e-12,atol=0)
                for name in NAMES[:2]:
                    b = root[name]; P = b['Points'][:]
                    np.testing.assert_allclose(b['PointData/T'],300+P[:,0]+2*P[:,1]+3*P[:,2])
                    np.testing.assert_allclose(b['PointData/U'],np.tile([.001,.002,.003],(len(P),1)))
                np.testing.assert_array_equal(root['hex/NumberOfCells'],[1] if ranks==1 else [1,0])
            with h5py.File(fallback/'out/step-0.vtkhdf') as f:
                root = f['VTKHDF']; g = root['hex']
                np.testing.assert_array_equal(root['cut/NumberOfCells'],[0]*ranks)
                fixture = json.loads((pathlib.Path(__file__).parent/'fixtures/p1_legacy_hex.json').read_text())
                for name, value in fixture['datasets'].items():
                    # Partition counts and local IDs differ in serial and MPI.
                    if name.startswith('NumberOf') or name in ['Connectivity','Offsets']:
                        continue
                    np.testing.assert_array_equal(g[name],value,err_msg=name)
                np.testing.assert_array_equal(g['NumberOfCells'],[2] if ranks==1 else [1,1])
                np.testing.assert_array_equal(g['Connectivity'],list(range(16)) if ranks==1 else list(range(8))*2)
                np.testing.assert_array_equal(g['Offsets'],[0,8,16] if ranks==1 else [0,8,0,8])
            try:
                import vtk
            except ImportError:
                print('VTK readback SKIP: install vtk==9.7.2')
            else:
                reader = vtk.vtkHDFReader(); reader.SetFileName(str(path)); reader.Update()
                output = reader.GetOutputDataObject(0)
                self.assertEqual(output.GetNumberOfPartitionedDataSets(),4)
                for i,name in enumerate(NAMES):
                    self.assertEqual(output.GetMetaData(i).Get(vtk.vtkCompositeDataSet.NAME()),name)
                    self.assertEqual(output.GetPartitionedDataSet(i).GetNumberOfPartitions(),ranks)
                    for rank in range(ranks):
                        self.assertIsNotNone(output.GetPartition(i,rank))
                grid = output.GetPartition(1,0 if ranks==1 else 1)
                self.assertEqual(grid.GetNumberOfCells(),1)
                self.assertEqual(grid.GetCellType(0),42)
                self.assertEqual(grid.GetCell(0).GetNumberOfFaces(),5)
                vf = vtk.vtkCellSizeFilter(); vf.SetInputData(grid); vf.Update()
                volume = vf.GetOutput().GetCellData().GetArray('Volume').GetValue(0)
                self.assertLessEqual(abs(volume-.5)/.5,1e-12)
                if ranks==2:
                    self.assertEqual(output.GetPartition(1,0).GetNumberOfCells(),0)
                print(f'VTK {vtk.vtkVersion.GetVTKVersion()} volume={volume:.17g}')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
