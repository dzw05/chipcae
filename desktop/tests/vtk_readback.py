"""VTK process isolation: never feed the negative-reference fixture to VTK."""
import sys
from pathlib import Path
import vtk
root = Path(sys.argv[1])
r = vtk.vtkHDFReader()
for variant in ('default','hex_fallback','variable_parts','s1_compat'):
    first = None
    for step in (0,1,2,0):
        # VTK 9.7.2 caches mesh/fields across same-sized PDC files. Recreate on file switch.
        r = vtk.vtkHDFReader()
        r.SetFileName(str(root/variant/f'out/step-{step}.vtkhdf')); r.Update()
        o = r.GetOutputDataObject(0)
        assert o.GetNumberOfPartitionedDataSets() == 4
        g = o.GetPartitionedDataSet(0).GetPartition(0)
        value = (g.GetPoint(0),g.GetPointData().GetArray('T').GetValue(0))
        assert g.GetNumberOfCells() == 1
        assert g.GetPointData().GetArray('U').GetNumberOfComponents() == 3
        if step == 0:
            if first is None: first=value
            else: assert value == first
        else: assert value != first
        expected = 3 if variant != 'variable_parts' or step == 0 else 2
        for i in range(4):
            p = o.GetPartitionedDataSet(i)
            assert p.GetNumberOfPartitions() == expected
            empty = p.GetPartition(expected-1)
            assert empty is None or empty.GetNumberOfCells() == 0
        cut = o.GetPartitionedDataSet(1).GetPartition(0)
        cell = cut.GetCell(0)
        assert cell.GetCellType() == (12 if variant == 'hex_fallback' else 42)
        assert cell.GetNumberOfFaces() == 6
r = vtk.vtkHDFReader()
r.SetFileName(str(root/'temporal_ug.vtkhdf')); r.UpdateInformation()
first=None
for t in (0.,.5,1.,0.):
    r.UpdateTimeStep(t)
    o=r.GetOutputDataObject(0)
    g=o.GetPartition(0) if hasattr(o,'GetPartition') else o
    value=(g.GetPoint(0),g.GetPointData().GetArray('T').GetValue(0))
    assert g.GetNumberOfCells() == 2
    if t == 0:
        if first is None: first=value
        else: assert value == first
    else: assert value != first
print('VTK',vtk.vtkVersion.GetVTKVersion(),'PDC file switching and standalone UG temporal readback PASS')
