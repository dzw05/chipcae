# D04 result viewer

`ResultView` is a Qt6 widget with an embedded QVTK view. The caller owns a
`ResultSession` that must outlive the view and calls `loadStep(session, n)`;
all loading stays in ResultSession. `ResultControls(view, session)` provides
step/layer selection, four block toggles, void visibility, display displacement
scale, clip origin/normal, manifest summaries, picked elem_gid, and PNG export.
Create controls after loading a step so the session has its layer name tables.
Set QVTKOpenGLNativeWidget's default surface format before QApplication.

The view copies session data, combines partitions by block, filters cells, clips
in original SI coordinates, then displays `x + scale * U`. Arrays remain in SI.
Kind 1 has a gold wire overlay; kind 2 is hidden by default and retained in raw
data. Layer ids start at 1, with 0 selecting all layers. Legacy block aliases
and D03 layer/material aliases are supported. Missing T/U displays a plain mesh.
`pickedId(block, filteredCell)` and `pick(x, y)` return the original int64 gid as
text, or 未知. VTK mouse coordinates have their origin at bottom left; interactive
left clicks emit `elementPicked` and update the controls.

`summary(officialName)` reads only current-step ResultSession summary entries,
including their units, and returns unavailable when missing. It never computes
headline warpage values from rendered geometry.

`tetrahedralize` validates convex, planar, closed 3D cells, or returns a clear
error. It inserts an interior vertex average, interpolates point fields, orients
face triangles outward, and copies cell fields onto a tetrahedral fan. It checks
fan volume against an independently anchored divergence sum. `quantify` uses
this fan before clipping, splits resulting tetra/wedge primitives, and integrates
by tetra determinants. Point field integration uses the mean of vertex values;
cell fields are constant per tetra. For vector arrays the integral is component
0. Non-convex cells require constrained tetrahedralization and are rejected.
Surface/particle display clipping uses VTK directly; quantitative volume rejects
non-volume input. `error()` reports display clip failures.

`capture(path)` temporarily attaches the renderer to an offscreen render window
selected by VTK's caller-configured backend and writes an 800x600 PNG. This also
works without showing the Qt widget. Leave DISPLAY and VTK_DEFAULT_OPENGL_WINDOW
to the caller; no test overrides DISPLAY.
