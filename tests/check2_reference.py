import os
os.environ.setdefault('VTK_SMP_MAX_THREADS','2')
os.environ.setdefault('OMP_NUM_THREADS','1')
import vtk, h5py, numpy as np, json, sys, subprocess, resource
from vtk.util.numpy_support import vtk_to_numpy as vn
from scipy.spatial import ConvexHull
import vtkhdf_h5 as W, geom
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def read(fn):
 r=vtk.vtkHDFReader();r.SetFileName(fn);r.Update();return r,r.GetOutput()
def leaves(o):
 if o.IsA('vtkDataSet'):return [o]
 it=o.NewIterator();it.InitTraversal();a=[]
 while not it.IsDoneWithTraversal():
  d=it.GetCurrentDataObject()
  if d and d.IsA('vtkDataSet'):a.append(d)
  it.GoToNextItem()
 return a
def append(ds):
 a=vtk.vtkAppendFilter()
 for d in ds:a.AddInputData(d)
 a.Update();o=vtk.vtkUnstructuredGrid();o.DeepCopy(a.GetOutput());return o
def volume(d):
 f=vtk.vtkCellSizeFilter();f.SetInputData(d);f.Update();return vn(f.GetOutput().GetCellData().GetArray('Volume')).copy()
def faces(c):return [[c.GetFace(j).GetPointId(k) for k in range(c.GetFace(j).GetNumberOfPoints())] for j in range(c.GetNumberOfFaces())]
def ugpart(P,fs,shared=False):
 conn=[];offs=[0];fc=[];fo=[0];pf=[];po=[0];cache={}
 for ff in fs:
  conn.extend(dict.fromkeys(x for f in ff for x in f));offs.append(len(conn))
  for f in ff:
   key=tuple(sorted(f))
   if shared and key in cache:pf.append(-cache[key]-1)
   else:
    cache[key]=len(fo)-1;pf.append(len(fo)-1);fc.extend(f);fo.append(len(fc))
  po.append(len(pf))
 return dict(points=np.array(P),types=[42]*len(fs),conn=conn,offsets=offs,face_conn=fc,face_offsets=fo,p2f=pf,p_offsets=po,PointData={},CellData={})
def single(fn,p,ver=(2,8)):
 assert not os.path.exists(fn)
 with h5py.File(fn,'w') as f:
  g=f.create_group('VTKHDF');W._attrs(g,'UnstructuredGrid',ver);W.write_ug(g,[p],ver)
def cubes():
 P=[];idx={};fs=[]
 for x in [0,1]:
  q=np.array([[0,0,0],[1,0,0],[1,1,0],[0,1,0],[0,0,1],[1,0,1],[1,1,1],[0,1,1]])+[x,0,0];ids=[]
  for p in q:
   k=tuple(p)
   if k not in idx:idx[k]=len(P);P.append(p)
   ids.append(idx[k])
  fs.append([[ids[i] for i in f] for f in [[0,3,2,1],[4,5,6,7],[0,1,5,4],[1,2,6,5],[2,3,7,6],[3,0,4,7]]])
 return P,fs
if len(sys.argv)>1 and sys.argv[1]=='probe':
 r,o=read(sys.argv[2]);ds=leaves(o);print('updated',flush=True)
 for d in ds:
  for i in range(d.GetNumberOfCells()):c=d.GetCell(i);assert c.GetNumberOfFaces()>0
 print(json.dumps({'cells':sum(d.GetNumberOfCells() for d in ds),'volumes':[volume(d).tolist() for d in ds]}));sys.exit()

def main():
 R={'versions':{'vtk':vtk.vtkVersion.GetVTKVersionFull(),'h5py':h5py.__version__,'HDF5':h5py.version.hdf5_version},'tests':{}}
 E=json.load(open('out/expected.json'))
 for st in [0,1]:
  r,o=read(f'out/step-{st}.vtkhdf');b=o.GetPartitionedDataSet(1);vs=[];ds=[];hulls=[];diagn=[];outbad=0;outsidebad=0
  for rank in range(b.GetNumberOfPartitions()):
   d=b.GetPartition(rank)
   if d is None:continue
   P=vn(d.GetPoints().GetData()) if d.GetNumberOfPoints() else np.empty((0,3));v=volume(d);vs.extend(v.tolist())
   for i in range(d.GetNumberOfCells()):
    c=d.GetCell(i);ff=faces(c);dv=geom.polyhedron_volume(P,ff);ds.append(dv);ids=[c.GetPointId(k) for k in range(c.GetNumberOfPoints())];hv=ConvexHull(P[ids]).volume;hulls.append(hv)
    edges={};col=0;mina=1.;nonplan=0.;normals=[];centers=[]
    for f in ff:
     q=P[f];av=geom.poly_area_vec(q);area=np.linalg.norm(av);mina=min(mina,area);n=av/area;center=q.mean(0);normals.append(n);centers.append(center);nonplan=max(nonplan,float(np.abs((q-center)@n).max()))
     if np.dot(n,center-P[ids].mean(0))<=0:outbad+=1
     if c.IsInside(center+n*1e-7,1e-10):outsidebad+=1
     for a,bb in zip(f,f[1:]+f[:1]):edges[(a,bb)]=edges.get((a,bb),0)+1
     for j in range(len(q)):
      if np.linalg.norm(np.cross(q[j]-q[j-1],q[(j+1)%len(q)]-q[j]))<1e-13:col+=1
    unpaired=sum(n!=edges.get((bb,a),0) for (a,bb),n in edges.items())
    diagn.append(dict(rank=rank,cell=i,vtk=float(v[i]),divergence=dv,hull=hv,relative_error=abs(v[i]-dv)/dv,collinear=col,unpaired=unpaired,min_face_area=mina,nonplanarity=nonplan))
  ev=np.concatenate(E['steps'][st]['cut_volumes']);a=np.array(vs);dv=np.array(ds)
  R['tests'][f'step{st}']={'status':'FAIL' if np.max(abs(a-ev))>1e-10 else 'PASS','cells':len(a),'vtk_mismatch_count':int(np.sum(abs(a-ev)>1e-9*E['h']**3)),'vtk_max_relative_error':float(np.max(abs(a-ev)/ev)),'vtk_sum':float(a.sum()),'expected_sum':float(ev.sum()),'divergence_max_error':float(np.max(abs(dv-ev))),'hull_max_error':float(np.max(abs(np.array(hulls)-ev))),'outward_bad':outbad,'normal_shift_inside':outsidebad,'diagnostics':diagn}
 P,fs=cubes()
 for shared,ver in [(False,(2,8)),(True,(2,9))]:
  fn=f'out/minimal_{ver[1]}.vtkhdf';single(fn,ugpart(P,fs,shared),ver)
 for name,fn in [('minimal28','out/minimal_8.vtkhdf'),('minimal29','out/minimal_9.vtkhdf'),('original29','out/step-2.vtkhdf')]:
  p=subprocess.run([sys.executable,__file__,'probe',fn],capture_output=True,text=True,timeout=40);R['tests'][name]={'status':'PASS' if p.returncode==0 else 'FAIL','returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr[-3000:]}
 json.dump(R,open('out/check2_partial.json','x'),indent=2)
 print(json.dumps({k:{a:b for a,b in v.items() if a!='diagnostics'} for k,v in R['tests'].items()},indent=2))
if __name__=='__main__':main()
