"""Independent HDF5, analytic, wire and subprocess checks for D02."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings
import zlib
import h5py
import numpy as np

TOOLS = Path(__file__).resolve().parents[1]/'tools'
sys.path.insert(0,str(TOOLS))
import make_samples as S
import mock_solver as M
EXPECTED = json.loads((Path(__file__).parent/'data/expected.json').read_text())


class Samples(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        subprocess.run([sys.executable,str(TOOLS/'make_samples.py'),'--out',str(cls.root)],check=True)

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def test_structure_fields_and_q2(self):
        for step in range(3):
            with h5py.File(self.root/f'default/out/step-{step}.vtkhdf') as f:
                root = f['VTKHDF']
                np.testing.assert_array_equal(root.attrs['Version'],EXPECTED['version'])
                self.assertEqual(list(root)[:4],EXPECTED['blocks'])
                for index,name in enumerate(EXPECTED['blocks']):
                    g = root[name]
                    self.assertEqual(g.attrs['Index'],index)
                    np.testing.assert_array_equal(g.attrs['Version'],[2,8])
                    self.assertEqual(g['NumberOfPoints'].dtype,np.dtype('int64'))
                    self.assertEqual(len(g['NumberOfPoints']),3)
                    self.assertEqual(g['NumberOfPoints'][-1],0)
                    if name == 'particles': self.assertEqual(g['NumberOfPoints'][:].sum(),0)
                    p = (g['Points'][:] - [0,0,.02*step])/(1+.1*step)
                    x,y,z = p.T
                    np.testing.assert_allclose(g['PointData/T'][:],193.15+7*step+3*x*x+2*y*z+z*z,atol=1e-12)
                    np.testing.assert_allclose(g['PointData/U'][:],np.column_stack((1e-4*x*z,2e-4*y*z,(step+1)*1e-3*(x*x+2*y*y))),atol=1e-16)
                    for field in ('layer_id','material_id','cell_kind'):
                        self.assertEqual(g['CellData/'+field].dtype,np.dtype('int32'))
                    ids = g['CellData/elem_gid'][:]
                    self.assertEqual(ids.dtype,np.dtype('int64'))
                    if len(ids): self.assertGreater(ids.min(),2**53)
                    self.assertNotIn('sigma_vm',g['CellData'])
                    for k in ('T','U'): self.assertEqual(g['PointData/'+k].dtype,np.dtype('float64'))
                    if name in ('hex','cut'):
                        self.assertEqual(g['Types'].dtype,np.dtype('uint8'))
                        for key in ('Offsets','Connectivity','NumberOfCells','NumberOfConnectivityIds'):
                            self.assertEqual(g[key].dtype,np.dtype('int64'))
                        np.testing.assert_array_equal(g['Offsets'][:],[0,8,0,8,0])
                        np.testing.assert_array_equal(g['Connectivity'][:],list(range(8))*2)
                        self.assertIn(2,g['CellData/cell_kind'][:])  # default hidden void case
                    else:
                        for topo in ('Vertices','Lines','Polygons','Strips'):
                            self.assertEqual(g[topo+'/Offsets'][-1],0)
                            self.assertEqual(g[topo+'/Offsets'].dtype,np.dtype('int64'))
                self.assertEqual(root['Assembly/volume/hex'].name,'/VTKHDF/Assembly/volume/hex')

    def test_outward_closed_per_cell(self):
        for step in range(3):
            with h5py.File(self.root/f'default/out/step-{step}.vtkhdf') as f:
                g = f['VTKHDF/cut']
                self.assertTrue(np.all(g['Types'][:] == 42))
                for key in ('FaceConnectivity','FaceOffsets','PolyhedronToFaces','PolyhedronOffsets','NumberOfFaces','NumberOfFaceConnectivityIds','NumberOfPolyhedronToFaceIds'):
                    self.assertEqual(g[key].dtype,np.dtype('int64'))
                np.testing.assert_array_equal(g['FaceOffsets'][:],list(range(0,25,4))*2+[0])
                np.testing.assert_array_equal(g['PolyhedronOffsets'][:],[0,6,0,6,0])
                for rank in range(2):
                    points = g['Points'][rank*8:(rank+1)*8]
                    fc = g['FaceConnectivity'][rank*24:(rank+1)*24]
                    fo = g['FaceOffsets'][rank*7:(rank+1)*7]
                    faces = [fc[fo[i]:fo[i+1]] for i in range(6)]
                    edges = []; v = 0.
                    for face in faces:
                        p = points[face]; normal = np.cross(p[1]-p[0],p[2]-p[0])
                        self.assertGreater(np.dot(normal,p.mean(0)-points.mean(0)),0)
                        edges.extend(zip(face,np.roll(face,-1)))
                        for j in range(1,len(p)-1):
                            v += np.dot(p[0],np.cross(p[j],p[j+1]))/6
                    for a,b in edges: self.assertEqual(edges.count((b,a)),1)
                    self.assertAlmostEqual(v,EXPECTED['cut_volume']*(1+.1*step)**3,places=12)

    def test_manifest_summary(self):
        m = json.loads((self.root/'default/manifest.json').read_text())
        self.assertEqual(m['schema_version'],'chipcae.results/1.0')
        self.assertEqual(m['units'],'SI'); self.assertTrue((self.root/'default'/m['stackup']).is_file())
        self.assertEqual(m['generation'],3); self.assertTrue(m['complete'])
        self.assertEqual(m['partitions']['per_step'],[3,3,3])
        self.assertEqual([x['name'] for x in m['fields']][-1],'sigma_vm')
        self.assertTrue(m['fields'][-1]['reserved'])
        for item in m['fields']:
            self.assertIn('location',item); self.assertIn('components',item)
            self.assertNotIn('association',item)
        for step in m['steps']:
            p = self.root/'default'/step['file']
            self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(),step['checksum'])
            self.assertTrue(step['complete']); self.assertNotIn('step',step)
        # Independent full-Q2 summary using a dense symmetric top grid, not output vertices.
        x,y = np.meshgrid(np.linspace(0,1,101),np.linspace(0,1,101))
        for n in range(3):
            w = (n+1)*1e-3*(x*x+2*y*y)
            a = np.column_stack((x.ravel(),y.ravel(),np.ones(x.size)))
            r = w.ravel()-a@np.linalg.lstsq(a,w.ravel(),rcond=None)[0]
            expected = {'warpage_pv':np.ptp(r),'T_max':199.15+7*n,
                        'U_mag_max':np.sqrt(1e-8+4e-8+9e-6*(n+1)**2)}
            for s in [s for s in m['summary'] if s['step']==n]:
                self.assertAlmostEqual(s['value'],expected.pop(s['name']),places=12)
                self.assertIn(s['unit'],('K','m'))
            self.assertFalse(expected)
        self.assertFalse(list(self.root.rglob('*.tmp')))

    def test_variants(self):
        with h5py.File(self.root/'hex_fallback/out/step-0.vtkhdf') as f:
            g=f['VTKHDF/cut']; self.assertTrue(np.all(g['Types'][:] == 12))
            self.assertEqual(g['CellData/cell_kind'][0],1)
            self.assertNotIn('PolyhedronToFaces',g)
        with h5py.File(self.root/'old_names/out/step-0.vtkhdf') as f:
            self.assertIn('layer',f['VTKHDF/hex/CellData'])
            self.assertNotIn('layer_id',f['VTKHDF/hex/CellData'])
        with h5py.File(self.root/'s1_compat/out/step-0.vtkhdf') as f:
            np.testing.assert_array_equal(f['VTKHDF'].attrs['Version'],[2,9])
            self.assertIn('cut_poly',f['VTKHDF']); self.assertIn('embedded_boundary',f['VTKHDF'])
        m=json.loads((self.root/'s1_compat/manifest.json').read_text())
        self.assertNotIn('partitions',m)
        m=json.loads((self.root/'variable_parts/manifest.json').read_text())
        self.assertEqual(m['partitions']['per_step'],[3,2,2])

    def test_negative_isolated_rejection(self):
        command = 'import sys; sys.path.insert(0,sys.argv[1]); from make_samples import reject_negative; reject_negative(sys.argv[2])'
        r=subprocess.run([sys.executable,'-c',command,str(TOOLS),str(self.root/'reject_negative.vtkhdf')],capture_output=True,text=True)
        self.assertNotEqual(r.returncode,0); self.assertIn('unsupported negative',r.stderr)

    def test_temporal_offsets(self):
        with h5py.File(self.root/'temporal_ug.vtkhdf') as f:
            for key,value in EXPECTED['temporal_offsets'].items():
                np.testing.assert_array_equal(f['VTKHDF/Steps/'+key][:],value)

    def test_caelog_fixtures(self):
        path=self.root/'events'
        metadata=json.loads((path/'fixture.json').read_text())
        for name in ('bad-crc','half'):
            events,offset=M.read_log(path/(name+'.caelog'))
            self.assertEqual([e['seq'] for e in events],[1])
            self.assertEqual(offset,metadata['valid_first_record_bytes'])
        # Polling must resume after an incomplete record is eventually finished.
        growing = self.root/'growing.caelog'
        growing.write_bytes((path/'half.caelog').read_bytes())
        self.assertEqual(len(M.read_log(growing)[0]),1)
        with growing.open('ab') as f: f.write((path/'valid.caelog').read_bytes()[-7:])
        self.assertEqual(len(M.read_log(growing)[0]),2)
        events,offset=M.read_log(path/'valid.caelog',from_seq=2)
        self.assertEqual([e['seq'] for e in events],[2]); self.assertEqual(offset,metadata['valid_bytes'])
        data=(path/'valid.caelog').read_bytes()
        self.assertEqual(data[:8],b'CAELOG1\0')
        n,crc=struct.unpack_from('<II',data,8)
        self.assertEqual(crc,zlib.crc32(data[16:16+n]))
        self.assertEqual(data[16:16+n],struct.pack('<QqBBQd',1,1700000000000000001,21,1,0,0.))
        with self.assertRaisesRegex(ValueError,'unsupported event_log_format'): M.read_log(path/'valid.caelog',2)

    def test_control_rules_and_stop(self):
        path=self.root/'control-test'; path.mkdir()
        c=M.ControlReader(path/'control.jsonl')
        self.assertEqual(c.poll(),[])
        c.path.write_text('{"cmd_seq":"7","action":"STOP"}')
        with warnings.catch_warnings(record=True) as ws:
            self.assertEqual(c.poll(),[]); self.assertFalse(ws)
        with c.path.open('a') as f: f.write('\nno json\n{"cmd_seq":"99","action":"BOGUS"}\n{"cmdSeq":"8","action":"STOP"}\n{"cmd_seq":"8","action":"STOP"}\n')
        with warnings.catch_warnings(record=True) as ws:
            commands=c.poll(); self.assertEqual(len(ws),2)
        self.assertEqual([x['cmd_seq'] for x in commands],[7,8]); self.assertEqual(c.seq,8)
        self.assertEqual(c.poll(),[])
        with c.path.open('a') as f:
            f.write('{"cmd_seq":"100","action":"STOP","issued_at":"bad"}\n')
            f.write('{"cmd_seq":"9","action":"STOP","unknown":1}\n')
            f.write('{"cmd_seq":"9","action":"STOP","issuedAt":"2026-11-17T00:00:00Z"}\n')
        with warnings.catch_warnings(record=True) as ws:
            commands=c.poll(); self.assertEqual(len(ws),2)
        self.assertEqual([x['cmd_seq'] for x in commands],[9])
        # Entire run over a generated manifest exercises OutputAvailable as well.
        run=self.root/'default'
        (run/'control.jsonl').write_text('{"cmd_seq":"23","client_request_id":"stop-test","action":"STOP"}\n')
        M.run(run)
        events=[json.loads(x) for x in (run/'mock-events.jsonl').read_text().splitlines()]
        ack=next(e['control_ack'] for e in events if 'control_ack' in e)
        self.assertEqual(ack['cmd_seq'],'23'); self.assertEqual(ack['client_request_id'],'stop-test')
        self.assertEqual(ack['status'],'APPLIED'); self.assertEqual(ack['effective_step'],1)
        self.assertEqual(events[-1]['exit']['reason'],'user_stop')
        for name in ('state','progress','residual','residual_batch','log_line','output_available','exit'):
            self.assertTrue(any(name in e for e in events),name)
        wire,_=M.read_log(run/'events.caelog')
        self.assertTrue(any('output_available' in e for e in wire))

    def test_vtk_readback(self):
        # Use the existing read-only VTK environment when system Python lacks vtk.
        python = sys.executable if importlib.util.find_spec('vtk') else '/workspace/proto-vtkhdf/venv/bin/python'
        if not Path(python).exists(): self.skipTest('optional vtk environment unavailable')
        subprocess.run([python,str(Path(__file__).with_name('vtk_readback.py')),str(self.root)],check=True,timeout=90)

if __name__ == '__main__': unittest.main()
