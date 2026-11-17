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
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((run/'out/step-0.vtkhdf').exists())
                    self.assertFalse((run/'manifest.json').exists())

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
            self.assertEqual({s['name'] for s in m['summary']}, {'warpage_pv','T_max','U_mag_max'})
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
