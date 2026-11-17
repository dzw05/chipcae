# E4 STEP import and inspection

`import_step(path, options)` returns SI bounds, defects, XCAF body paths and
shared immutable ownership of the original topology, document, transfer session
and `ShapeBuild_ReShape` identity history. Public headers contain no OCCT types.
`Geometry::from_step` owns this result; copying Geometry shares its source.
`Geometry::empty` retains its placeholder distance of `1`. STEP signed distance and STL
import explicitly throw until implemented; a STEP report does not imply an SDF.
Without OCCT, `step_import_available()` is false and import throws runtime_error.
No protocol error codes were added.

The threshold policy's former `Defect` is now `ThresholdDefect`; its fields and
assessment behavior are unchanged. This is a source-incompatible public API change:
downstream threshold users naming `geometry::Defect` must migrate to
`geometry::ThresholdDefect`. `diagnostics.hpp` provides the brief's
`Defect` and includes E3's `Tier`, `Real` and `Index` without OCCT.

## Source identity and units

XCAF document units are explicitly one metre before transfer. OCCT transfers
file units directly into metres; no later scaling or topology replacement is
performed. Bounds use exact geometry without triangulations or tolerance
inflation. The document, original faces and source transfer session outlive the
reader. Face indices are **one-based**, in `TopExp::MapShapes` order over the
free-root compound in XCAF root order. They are stable across repeated reads of
the same file and options, not across rewrites by a different STEP exporter.
Each face is reverse-mapped by `XSControl_TransferReader::EntityFromShapeResult`
and `StepData_StepModel::IdentLabel`; located instances fall back to their
unlocated definition. Assembly instance paths distinguish reused definitions.
E5 must compose its repair history with this identity history and retain these
original attributes; it must not remap repaired faces to fresh entity numbers.

IDs use an internal SHA-256 implementation with no additional library.
The canonical UTF-8 key is decimal enum value, original `#entity`, one-based
face index and three rounded SI coordinates divided by eps_weld, separated by
`|`, without whitespace. Rounding uses `std::round` (ties away from zero),
classic locale and fixed decimal integers, with negative zero normalized to zero.
When present, the original status is appended to distinguish validity failures at
the same location. The first 16 lowercase hex digits
are reported. IDs intentionally have 64 displayed bits as specified in E4.

## Inspection policy

Callers provide finite positive `h_min` from the intended mesh and ordered
positive weld/A/B tolerances from E3. Defaults are explicit in the header.
E4 uses exact BRep geometry, not tessellation, so no incremental mesh is made.
Any future tessellation must use absolute chordal deflection <= 0.1*h_min.
OCCT 7.6 normally invokes ShapeFix internally during transfer. A scoped
`XSAlgo_AlgoContainer` overrides `ProcessShape` with identity and disables the
repairing `CheckPCurve` hook. The previous container is restored on success and
exceptions. A mutex serializes this library's exchange calls; applications
using OCCT exchange directly must serialize those calls with their imports.
No ShapeFix, sewing, or healing is invoked. `action` stays empty.

`BRepCheck_Analyzer` reports orientation, duplicate topology, open boundaries
and intersecting wires. Other validity statuses become `InvalidGeometry`, with
the original symbolic BRepCheck name in `status`. ArgumentAnalyzer findings
outside a common wire become `FaceSelfIntersection` or `InvalidGeometry`.
Dirty geometry returns located diagnostics for E5 to write before deciding
whether to reject. Read and transfer failures still throw.

Non-manifold edge usage is counted independently within each solid. Shared
faces and edges between distinct solids are multi-body interfaces.
ArgumentAnalyzer runs per body; sorted bounding boxes inflated by tau_B filter
candidate face, free-edge, and solid pairs before exact checks. Duplicate
faces already reported by BRepCheck are excluded from repeat exact checks.
Exact common volume detects solid overlap, including full containment.
Temporary boolean results never replace the source geometry.

Callers must supply `compute_tolerances(ImportScales{L, t_min, h_xy, dbu_m},
ThresholdFile{})`; zero/default options throw `invalid_argument`. The reader
sets `read.precision.mode=1`, `read.precision.val=eps_weld`,
`read.maxprecision.mode=1`, and `read.maxprecision.val=tau_A` under the exchange
mutex. A scope guard restores all four settings on success and failure.
The original file uncertainty (converted using its STEP unit) and maximum
vertex/edge/face tolerances from `ShapeAnalysis_ShapeTolerance` are reported
as `ToleranceExceeded` when above tau_A: B through tau_B, C above tau_B.

Measurements, all in metres:

| Kind | Measured quantity |
| --- | --- |
| Gap | Local separation at a free edge parameter midpoint to the opposing free edge |
| OpenEdge, NonManifoldEdge, SmallEdge | Curve length (validity aggregates use bounding diagonal) |
| SmallFace, DuplicateFace | Square root of area (topological aggregate duplicates use bounding diagonal) |
| SliverFace | Area divided by perimeter |
| SolidOverlap | Cube root of common volume |
| FlippedFace, WireSelfIntersection, OpenShell | Bounding diagonal of the affected topology |

Gaps are candidate missing seams: both exact separation and boundary-box centre
distance must be at most tau_B. Location is halfway between an actual edge parameter midpoint and its
closest point on the opposing edge, making the authored straight seam midpoint
repeatable. Boundary endpoints alone do not imply a gap. This is not a general
surface reconstruction algorithm: unequal or curved seams may require E5 or
future inspection policy. Paired boundaries merge into Gap; unpaired boundaries remain OpenEdge.
Other locations are affected topology box centres; overlap uses the common
region centre. Bboxes enclose the implicated source shape and reported location. Only Gap, SmallEdge, SmallFace, SliverFace and ToleranceExceeded use
E3 `classify(measured, tolerances)`. NonManifoldEdge, FaceSelfIntersection,
OpenShell and unpaired OpenEdge are C; FlippedFace, SolidOverlap and
InvalidGeometry are B; DuplicateFace and WireSelfIntersection are A.
Measurements retain their physical values regardless of fixed grades.
E5 must verify repaired wire tolerances and downgrade when above tau_A.
Grades do not authorize repair.
`NearLayerInterface` needs layer context and `UnitSuspect` needs a caller's
physical scale expectations, neither of which can be inferred reliably from a
STEP file alone. E4 does not invent these findings.

## Tests and CI

All STEP fixtures are authored at test runtime using OCCT primitives and writers.
They cover equivalent mm/metre boxes against known SI dimensions, a 20 um seam
with the exact first ADVANCED_FACE file reference and tau_B location check,
repeatable IDs, named enclosure/lid/ring paths, clean topology, and contained
and crossing solid overlap, including rejection of wire misclassification.
Review regressions cover bow-tie faces, corrupt cylindrical pcurves, fixed
wire/overlap/duplicate/open-boundary grades, per-solid non-manifold usage,
file uncertainty above tau_B, precision-state restoration, and default option
rejection. A 30x30 array plus one overlapping box must import within 60 seconds
and report exactly one B-tier overlap. SHA-256 has empty, abc and multiblock known-answer checks.
The standalone header compiles without OCCT headers or definitions, even in
an OCCT-enabled build. Its tierA timeout is 60 seconds.

Only nightly full-dependency CI enables OCCT. Ubuntu noble shared development
and runtime packages are pinned at observed version 7.6.3+dfsg1-7.1build1 in
deps.lock, already hashed by dependency and compiler caches. The nightly change
filter covers geometry and these tests; `mesh|occt` explicitly selects both
suites. PR CI keeps OCCT disabled and runs the standalone header in build-test.
