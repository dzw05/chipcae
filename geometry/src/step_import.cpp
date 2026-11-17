#include "chipcae/geometry/step_import.hpp"

#include "diagnostic_id.hpp"
#include "step_options.hpp"

#include <BOPAlgo_ArgumentAnalyzer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_ListIteratorOfListOfStatus.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepCheck_Shell.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Interface_Static.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <ShapeAnalysis_ShapeTolerance.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <StepBasic_ConversionBasedUnit.hxx>
#include <StepBasic_SiUnit.hxx>
#include <StepBasic_UncertaintyMeasureWithUnit.hxx>
#include <StepData_StepModel.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XSAlgo.hxx>
#include <XSAlgo_AlgoContainer.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>
#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace chipcae::geometry {
namespace detail {
struct StepShape {
    TopoDS_Shape shape;
    TopTools_IndexedMapOfShape faces;
    Handle(ShapeBuild_ReShape) history = new ShapeBuild_ReShape;
    Handle(TDocStd_Document) document;
    Handle(XSControl_WorkSession) session;
    std::vector<std::string> entities;
    std::vector<std::string> names;
};
} // namespace detail
namespace {
// OCCT 7.6 normally calls ShapeFix internally during STEP transfer. Override
// its documented exchange algorithm hook so detection sees the unhealed BRep.
class DetectionAlgorithms final : public XSAlgo_AlgoContainer {
public:
    TopoDS_Shape ProcessShape(const TopoDS_Shape& shape,
                              Standard_Real,
                              Standard_Real,
                              Standard_CString,
                              Standard_CString,
                              Handle(Standard_Transient) &,
                              const Message_ProgressRange&,
                              Standard_Boolean) const override {
        return shape;
    }
    Standard_Boolean CheckPCurve(const TopoDS_Edge&,
                                 const TopoDS_Face&,
                                 Standard_Real,
                                 Standard_Boolean) const override {
        // This hook normally repairs pcurves. BRepCheck will inspect them later.
        return true;
    }
};
class DetectionTransfer {
    Handle(XSAlgo_AlgoContainer) previous_ = XSAlgo::AlgoContainer();

public:
    DetectionTransfer() {
        Handle(DetectionAlgorithms) algorithms = new DetectionAlgorithms;
        algorithms->SetToolContainer(previous_->ToolContainer());
        XSAlgo::SetAlgoContainer(algorithms);
    }
    ~DetectionTransfer() { XSAlgo::SetAlgoContainer(previous_); }
    DetectionTransfer(const DetectionTransfer&) = delete;
    DetectionTransfer& operator=(const DetectionTransfer&) = delete;
};
std::array<Real, 6> bounds(const TopoDS_Shape& shape) {
    Bnd_Box b;
    // Do not use triangulations or topology tolerances to inflate SI boxes.
    try {
        BRepBndLib::AddOptimal(shape, b, false, false);
    } catch (const Standard_Failure&) {
        // Missing surfaces/pcurves can invalidate exact bounds. Preserve a
        // location from surviving vertices so the dirty shape can be reported.
        b.SetVoid();
        for (TopExp_Explorer it(shape, TopAbs_VERTEX); it.More(); it.Next())
            b.Add(BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
    }
    if (b.IsVoid())
        return {};
    std::array<Real, 6> result;
    b.Get(result[0], result[1], result[2], result[3], result[4], result[5]);
    for (auto x : result)
        if (!std::isfinite(x))
            return {};
    return result;
}
Real extent(const TopoDS_Shape& shape) {
    auto b = bounds(shape);
    return std::hypot(b[3] - b[0], b[4] - b[1], b[5] - b[2]);
}
Real length(const TopoDS_Shape& shape) {
    GProp_GProps p;
    BRepGProp::LinearProperties(shape, p);
    return p.Mass();
}
Real face_width(const TopoDS_Shape& shape) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(shape, p);
    return std::sqrt(std::abs(p.Mass()));
}
std::array<Real, 3> center(const TopoDS_Shape& shape) {
    const auto b = bounds(shape);
    return {(b[0] + b[3]) / 2, (b[1] + b[4]) / 2, (b[2] + b[5]) / 2};
}
std::string label_name(const TDF_Label& label) {
    Handle(TDataStd_Name) name;
    if (!label.FindAttribute(TDataStd_Name::GetID(), name))
        return "unnamed";
    // OCCT converts Unicode names directly to UTF-8.
    const auto& text = name->Get();
    std::string utf8(static_cast<std::size_t>(text.LengthOfCString()) + 1, '\0');
    char* ptr = utf8.data();
    const auto count = text.ToUTF8CString(ptr);
    utf8.resize(static_cast<std::size_t>(count));
    return utf8;
}
bool contains(const TopoDS_Shape& ancestor, const TopoDS_Shape& shape) {
    if (ancestor.IsSame(shape))
        return true;
    for (TopExp_Explorer it(ancestor, shape.ShapeType()); it.More(); it.Next())
        if (it.Current().IsSame(shape))
            return true;
    return false;
}
Real unit_scale(const StepBasic_Unit& unit, int depth = 0) {
    if (depth > 8)
        return 1;
    auto named = unit.NamedUnit();
    auto si = Handle(StepBasic_SiUnit)::DownCast(named);
    if (!si.IsNull()) {
        if (!si->HasPrefix())
            return 1;
        constexpr int powers[] = {18, 15, 12, 9, 6, 3, 2, 1, -1, -2, -3, -6, -9, -12, -15, -18};
        return std::pow(10.0, powers[static_cast<int>(si->Prefix())]);
    }
    auto conversion = Handle(StepBasic_ConversionBasedUnit)::DownCast(named);
    if (!conversion.IsNull() && !conversion->ConversionFactor().IsNull()) {
        auto factor = conversion->ConversionFactor();
        return factor->ValueComponent() * unit_scale(factor->UnitComponent(), depth + 1);
    }
    return 1;
}
std::string status_name(BRepCheck_Status status) {
    switch (status) {
    case BRepCheck_NoError:
        return "BRepCheck_NoError";
    case BRepCheck_InvalidPointOnCurve:
        return "BRepCheck_InvalidPointOnCurve";
    case BRepCheck_InvalidPointOnCurveOnSurface:
        return "BRepCheck_InvalidPointOnCurveOnSurface";
    case BRepCheck_InvalidPointOnSurface:
        return "BRepCheck_InvalidPointOnSurface";
    case BRepCheck_No3DCurve:
        return "BRepCheck_No3DCurve";
    case BRepCheck_Multiple3DCurve:
        return "BRepCheck_Multiple3DCurve";
    case BRepCheck_Invalid3DCurve:
        return "BRepCheck_Invalid3DCurve";
    case BRepCheck_NoCurveOnSurface:
        return "BRepCheck_NoCurveOnSurface";
    case BRepCheck_InvalidCurveOnSurface:
        return "BRepCheck_InvalidCurveOnSurface";
    case BRepCheck_InvalidCurveOnClosedSurface:
        return "BRepCheck_InvalidCurveOnClosedSurface";
    case BRepCheck_InvalidSameRangeFlag:
        return "BRepCheck_InvalidSameRangeFlag";
    case BRepCheck_InvalidSameParameterFlag:
        return "BRepCheck_InvalidSameParameterFlag";
    case BRepCheck_InvalidDegeneratedFlag:
        return "BRepCheck_InvalidDegeneratedFlag";
    case BRepCheck_FreeEdge:
        return "BRepCheck_FreeEdge";
    case BRepCheck_InvalidMultiConnexity:
        return "BRepCheck_InvalidMultiConnexity";
    case BRepCheck_InvalidRange:
        return "BRepCheck_InvalidRange";
    case BRepCheck_EmptyWire:
        return "BRepCheck_EmptyWire";
    case BRepCheck_RedundantEdge:
        return "BRepCheck_RedundantEdge";
    case BRepCheck_SelfIntersectingWire:
        return "BRepCheck_SelfIntersectingWire";
    case BRepCheck_NoSurface:
        return "BRepCheck_NoSurface";
    case BRepCheck_InvalidWire:
        return "BRepCheck_InvalidWire";
    case BRepCheck_RedundantWire:
        return "BRepCheck_RedundantWire";
    case BRepCheck_IntersectingWires:
        return "BRepCheck_IntersectingWires";
    case BRepCheck_InvalidImbricationOfWires:
        return "BRepCheck_InvalidImbricationOfWires";
    case BRepCheck_EmptyShell:
        return "BRepCheck_EmptyShell";
    case BRepCheck_RedundantFace:
        return "BRepCheck_RedundantFace";
    case BRepCheck_InvalidImbricationOfShells:
        return "BRepCheck_InvalidImbricationOfShells";
    case BRepCheck_UnorientableShape:
        return "BRepCheck_UnorientableShape";
    case BRepCheck_NotClosed:
        return "BRepCheck_NotClosed";
    case BRepCheck_NotConnected:
        return "BRepCheck_NotConnected";
    case BRepCheck_SubshapeNotInShape:
        return "BRepCheck_SubshapeNotInShape";
    case BRepCheck_BadOrientation:
        return "BRepCheck_BadOrientation";
    case BRepCheck_BadOrientationOfSubshape:
        return "BRepCheck_BadOrientationOfSubshape";
    case BRepCheck_InvalidPolygonOnTriangulation:
        return "BRepCheck_InvalidPolygonOnTriangulation";
    case BRepCheck_InvalidToleranceValue:
        return "BRepCheck_InvalidToleranceValue";
    case BRepCheck_EnclosedRegion:
        return "BRepCheck_EnclosedRegion";
    case BRepCheck_CheckFail:
        return "BRepCheck_CheckFail";
    }
    return "BRepCheck_Unknown_" + std::to_string(static_cast<int>(status));
}
std::string status_name(BOPAlgo_CheckStatus status) {
    switch (status) {
    case BOPAlgo_CheckUnknown:
        return "BOPAlgo_CheckUnknown";
    case BOPAlgo_BadType:
        return "BOPAlgo_BadType";
    case BOPAlgo_SelfIntersect:
        return "BOPAlgo_SelfIntersect";
    case BOPAlgo_TooSmallEdge:
        return "BOPAlgo_TooSmallEdge";
    case BOPAlgo_NonRecoverableFace:
        return "BOPAlgo_NonRecoverableFace";
    case BOPAlgo_IncompatibilityOfVertex:
        return "BOPAlgo_IncompatibilityOfVertex";
    case BOPAlgo_IncompatibilityOfEdge:
        return "BOPAlgo_IncompatibilityOfEdge";
    case BOPAlgo_IncompatibilityOfFace:
        return "BOPAlgo_IncompatibilityOfFace";
    case BOPAlgo_OperationAborted:
        return "BOPAlgo_OperationAborted";
    case BOPAlgo_GeomAbs_C0:
        return "BOPAlgo_GeomAbs_C0";
    case BOPAlgo_InvalidCurveOnSurface:
        return "BOPAlgo_InvalidCurveOnSurface";
    case BOPAlgo_NotValid:
        return "BOPAlgo_NotValid";
    }
    return "BOPAlgo_Unknown_" + std::to_string(static_cast<int>(status));
}
template <class Callback>
void nearby_pairs(const std::vector<TopoDS_Shape>& shapes, Real inflation, Callback callback) {
    struct Entry {
        std::size_t index;
        std::array<Real, 6> box;
    };
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < shapes.size(); ++i)
        entries.push_back({i, bounds(shapes[i])});
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.box[0] < b.box[0];
    });
    for (std::size_t i = 0; i < entries.size(); ++i)
        for (std::size_t j = i + 1;
             j < entries.size() && entries[j].box[0] <= entries[i].box[3] + 2 * inflation;
             ++j) {
            bool overlap = true;
            for (int axis = 1; axis < 3; ++axis)
                overlap = overlap &&
                          entries[i].box[axis] <= entries[j].box[axis + 3] + 2 * inflation &&
                          entries[j].box[axis] <= entries[i].box[axis + 3] + 2 * inflation;
            if (overlap)
                callback(entries[i].index, entries[j].index);
        }
}
class ReadPrecision {
    int mode_ = Interface_Static::IVal("read.precision.mode");
    int maxmode_ = Interface_Static::IVal("read.maxprecision.mode");
    double val_ = Interface_Static::RVal("read.precision.val");
    double maxval_ = Interface_Static::RVal("read.maxprecision.val");

public:
    explicit ReadPrecision(const ImportTolerances& t) {
        Interface_Static::SetIVal("read.precision.mode", 1);
        Interface_Static::SetRVal("read.precision.val", t.eps_weld);
        Interface_Static::SetIVal("read.maxprecision.mode", 1);
        Interface_Static::SetRVal("read.maxprecision.val", t.tau_A);
    }
    ~ReadPrecision() {
        Interface_Static::SetIVal("read.precision.mode", mode_);
        Interface_Static::SetRVal("read.precision.val", val_);
        Interface_Static::SetIVal("read.maxprecision.mode", maxmode_);
        Interface_Static::SetRVal("read.maxprecision.val", maxval_);
    }
};
struct Inspector {
    std::unordered_set<std::string> ids;
    std::unordered_set<Index> duplicate_reported;
    detail::StepShape& source;
    StepImportResult& result;
    const StepImportOptions& options;
    Index face_of(const TopoDS_Shape& shape) const {
        if (shape.ShapeType() == TopAbs_FACE)
            return source.faces.FindIndex(shape);
        for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
            const auto index = source.faces.FindIndex(it.Current());
            if (index)
                return index;
        }
        for (int i = 1; i <= source.faces.Extent(); ++i)
            for (TopExp_Explorer it(source.faces(i), shape.ShapeType()); it.More(); it.Next())
                if (it.Current().IsSame(shape))
                    return i;
        return 0;
    }
    void add(StepDefectKind kind,
             const TopoDS_Shape& shape,
             Real measured,
             std::array<Real, 3> where,
             Index face = 0,
             std::string status = {}) {
        if (!face)
            face = face_of(shape);
        StepDefect d;
        d.kind = kind;
        d.status = std::move(status);
        d.measured = measured;
        d.where = where;
        d.bbox = bounds(shape);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            d.bbox[axis] = std::min(d.bbox[axis], where[axis]);
            d.bbox[axis + 3] = std::max(d.bbox[axis + 3], where[axis]);
        }
        d.face_index = face;
        if (face) {
            d.step_entity = source.entities.at(static_cast<std::size_t>(face - 1));
            d.body_name = source.names.at(static_cast<std::size_t>(face - 1));
        }
        switch (kind) {
        case StepDefectKind::Gap:
        case StepDefectKind::SmallEdge:
        case StepDefectKind::SmallFace:
        case StepDefectKind::SliverFace:
        case StepDefectKind::ToleranceExceeded:
            d.tier = classify(measured, options.tolerances);
            break;
        case StepDefectKind::DuplicateFace:
        case StepDefectKind::WireSelfIntersection:
            d.tier = Tier::A;
            break;
        case StepDefectKind::FlippedFace:
        case StepDefectKind::SolidOverlap:
        case StepDefectKind::InvalidGeometry:
            d.tier = Tier::B;
            break;
        default:
            d.tier = Tier::C;
            break;
        }
        if (kind == StepDefectKind::DuplicateFace)
            duplicate_reported.insert(face);
        d.defect_id = detail::diagnostic_id(d, options.tolerances.eps_weld);
        if (ids.insert(d.defect_id + d.status).second)
            result.defects.push_back(std::move(d));
    }
    bool duplicate_faces(const TopoDS_Shape& a, const TopoDS_Shape& b) {
        const auto ba = bounds(a), bb = bounds(b);
        for (std::size_t k = 0; k < 6; ++k)
            if (std::abs(ba[k] - bb[k]) > options.tolerances.eps_weld)
                return false;
        BRepAlgoAPI_Common common(a, b);
        if (!common.IsDone()) {
            add(StepDefectKind::InvalidGeometry,
                a,
                extent(a),
                center(a),
                0,
                "Duplicate-face boolean failed");
            return false;
        }
        const auto wa = face_width(a), wb = face_width(b);
        const auto wc = face_width(common.Shape());
        return wa > 0 && std::abs(wa - wb) <= options.tolerances.eps_weld &&
               std::abs(wa - wc) <= options.tolerances.eps_weld;
    }
    void inspect() {
        BRepCheck_Analyzer check(source.shape, true);
        TopTools_IndexedMapOfShape shapes;
        TopExp::MapShapes(source.shape, shapes);
        for (int i = 1; i <= shapes.Extent(); ++i) {
            const auto& s = shapes(i);
            const auto r = check.Result(s);
            if (r.IsNull())
                continue;
            const auto report_statuses = [&](const TopoDS_Shape& affected,
                                             const BRepCheck_ListOfStatus& statuses) {
                for (BRepCheck_ListIteratorOfListOfStatus it(statuses); it.More(); it.Next()) {
                    switch (it.Value()) {
                    case BRepCheck_NoError:
                        break;
                    case BRepCheck_IntersectingWires:
                    case BRepCheck_SelfIntersectingWire:
                        add(StepDefectKind::WireSelfIntersection,
                            affected,
                            extent(affected),
                            center(affected));
                        break;
                    case BRepCheck_RedundantFace:
                        add(StepDefectKind::DuplicateFace,
                            affected,
                            extent(affected),
                            center(affected));
                        break;
                    case BRepCheck_UnorientableShape:
                    case BRepCheck_BadOrientation:
                    case BRepCheck_BadOrientationOfSubshape:
                        add(StepDefectKind::FlippedFace,
                            affected,
                            extent(affected),
                            center(affected));
                        break;
                    case BRepCheck_InvalidMultiConnexity:
                        // Edge usage is evaluated per solid below, never over an assembly.
                        break;
                    case BRepCheck_FreeEdge:
                    case BRepCheck_NotClosed:
                        if (affected.ShapeType() == TopAbs_SHELL)
                            add(StepDefectKind::OpenShell,
                                affected,
                                extent(affected),
                                center(affected));
                        break;
                    default:
                        add(StepDefectKind::InvalidGeometry,
                            affected,
                            extent(affected),
                            center(affected),
                            0,
                            status_name(it.Value()));
                        break;
                    }
                }
            };
            report_statuses(s, r->Status());
            for (r->InitContextIterator(); r->MoreShapeInContext(); r->NextShapeInContext())
                report_statuses(r->ContextualShape(), r->StatusOnShape());
        }
        for (TopExp_Explorer it(source.shape, TopAbs_SHELL); it.More(); it.Next()) {
            BRepCheck_Shell shell(TopoDS::Shell(it.Current()));
            if (shell.Closed() != BRepCheck_NoError)
                add(StepDefectKind::OpenShell,
                    it.Current(),
                    extent(it.Current()),
                    center(it.Current()));
        }
        // Analyze each body independently; inter-body contacts belong to the pair pass.
        std::vector<TopoDS_Shape> bodies;
        for (TopExp_Explorer it(source.shape, TopAbs_SOLID); it.More(); it.Next())
            bodies.push_back(it.Current());
        for (TopExp_Explorer it(source.shape, TopAbs_SHELL, TopAbs_SOLID); it.More(); it.Next())
            bodies.push_back(it.Current());
        for (TopExp_Explorer it(source.shape, TopAbs_FACE, TopAbs_SHELL); it.More(); it.Next())
            bodies.push_back(it.Current());
        for (const auto& body : bodies) {
            BOPAlgo_ArgumentAnalyzer args;
            args.SetShape1(body);
            args.SelfInterMode() = true;
            args.SmallEdgeMode() = true;
            args.Perform();
            if (args.HasErrors())
                add(StepDefectKind::InvalidGeometry,
                    body,
                    extent(body),
                    center(body),
                    0,
                    "ArgumentAnalyzer failure");
            for (const auto& failure : args.GetCheckResult()) {
                const auto& faulty = failure.GetFaultyShapes1();
                if (failure.GetCheckStatus() == BOPAlgo_TooSmallEdge) {
                    for (const auto& shape : faulty)
                        add(StepDefectKind::SmallEdge, shape, length(shape), center(shape));
                    continue;
                }
                bool wire = false;
                if (failure.GetCheckStatus() == BOPAlgo_SelfIntersect && !faulty.IsEmpty())
                    for (TopExp_Explorer it(body, TopAbs_WIRE); it.More(); it.Next()) {
                        bool all = true;
                        for (const auto& shape : faulty)
                            all = all && contains(it.Current(), shape);
                        if (all) {
                            add(StepDefectKind::WireSelfIntersection,
                                it.Current(),
                                extent(it.Current()),
                                center(it.Current()));
                            wire = true;
                        }
                    }
                if (!wire) {
                    const auto kind = failure.GetCheckStatus() == BOPAlgo_SelfIntersect
                                          ? StepDefectKind::FaceSelfIntersection
                                          : StepDefectKind::InvalidGeometry;
                    if (faulty.IsEmpty())
                        add(kind,
                            body,
                            extent(body),
                            center(body),
                            0,
                            status_name(failure.GetCheckStatus()));
                    for (const auto& shape : faulty)
                        add(kind,
                            shape,
                            extent(shape),
                            center(shape),
                            0,
                            status_name(failure.GetCheckStatus()));
                }
            }
            TopTools_IndexedDataMapOfShapeListOfShape usage;
            if (body.ShapeType() == TopAbs_SOLID) {
                TopExp::MapShapesAndUniqueAncestors(body, TopAbs_EDGE, TopAbs_FACE, usage);
                for (int i = 1; i <= usage.Extent(); ++i)
                    if (usage.FindFromIndex(i).Extent() > 2)
                        add(StepDefectKind::NonManifoldEdge,
                            usage.FindKey(i),
                            length(usage.FindKey(i)),
                            center(usage.FindKey(i)));
            }
        }
        TopTools_IndexedDataMapOfShapeListOfShape edges;
        TopExp::MapShapesAndUniqueAncestors(source.shape, TopAbs_EDGE, TopAbs_FACE, edges);
        std::vector<int> boundary;
        for (int i = 1; i <= edges.Extent(); ++i) {
            const auto& e = edges.FindKey(i);
            if (BRep_Tool::Degenerated(TopoDS::Edge(e)))
                continue;
            const auto& faces = edges.FindFromIndex(i);
            if (faces.Extent() == 1 &&
                !BRep_Tool::IsClosed(TopoDS::Edge(e), TopoDS::Face(faces.First()))) {
                boundary.push_back(i);
            }
            if (length(e) < options.tolerances.eps_weld)
                add(StepDefectKind::SmallEdge, e, length(e), center(e));
        }
        // Nearby free boundaries on different faces are candidate gaps. Locate at
        // the midpoint of closest points, measured as their exact separation.
        std::vector<TopoDS_Shape> free_edges;
        std::unordered_set<int> paired;
        for (auto index : boundary)
            free_edges.push_back(edges.FindKey(index));
        nearby_pairs(free_edges, options.tolerances.tau_B, [&](std::size_t i, std::size_t j) {
            const auto a = boundary[i], b = boundary[j];
            if (edges.FindFromIndex(a).First().IsSame(edges.FindFromIndex(b).First()))
                return;
            BRepExtrema_DistShapeShape distance(edges.FindKey(a), edges.FindKey(b));
            if (!distance.IsDone() || distance.NbSolution() == 0)
                return;
            const auto gap = distance.Value();
            if (gap > options.tolerances.tau_B)
                return;
            // A whole opposing edge must be close; endpoint-only proximity is
            // not evidence of a missing seam.
            const auto ca = center(edges.FindKey(a)), cb = center(edges.FindKey(b));
            if (std::hypot(ca[0] - cb[0], ca[1] - cb[1], ca[2] - cb[2]) > options.tolerances.tau_B)
                return;
            BRepAdaptor_Curve curve(TopoDS::Edge(edges.FindKey(a)));
            const auto midpoint = curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2);
            const auto vertex = BRepBuilderAPI_MakeVertex(midpoint).Vertex();
            BRepExtrema_DistShapeShape local(vertex, edges.FindKey(b));
            if (!local.IsDone() || local.NbSolution() == 0 ||
                local.Value() > options.tolerances.tau_B)
                return;
            const auto opposite = local.PointOnShape2(1);
            std::array<Real, 3> where{(midpoint.X() + opposite.X()) / 2,
                                      (midpoint.Y() + opposite.Y()) / 2,
                                      (midpoint.Z() + opposite.Z()) / 2};
            paired.insert(a);
            paired.insert(b);
            add(StepDefectKind::Gap,
                edges.FindKey(a),
                local.Value(),
                where,
                source.faces.FindIndex(edges.FindFromIndex(a).First()));
        });
        for (auto index : boundary)
            if (!paired.count(index))
                add(StepDefectKind::OpenEdge,
                    edges.FindKey(index),
                    length(edges.FindKey(index)),
                    center(edges.FindKey(index)));
        std::vector<TopoDS_Shape> faces;
        for (int i = 1; i <= source.faces.Extent(); ++i)
            faces.push_back(source.faces(i));
        nearby_pairs(faces, options.tolerances.tau_B, [&](std::size_t i, std::size_t j) {
            if (duplicate_reported.count(static_cast<Index>(i + 1)) ||
                duplicate_reported.count(static_cast<Index>(j + 1)))
                return;
            if (duplicate_faces(faces[i], faces[j]))
                add(StepDefectKind::DuplicateFace,
                    faces[i],
                    face_width(faces[i]),
                    center(faces[i]));
        });
        TopTools_IndexedMapOfShape solids;
        TopExp::MapShapes(source.shape, TopAbs_SOLID, solids);
        std::vector<TopoDS_Shape> solid_shapes;
        for (int i = 1; i <= solids.Extent(); ++i)
            solid_shapes.push_back(solids(i));
        nearby_pairs(solid_shapes, options.tolerances.tau_B, [&](std::size_t i, std::size_t j) {
            // Exact common volume also catches complete containment.
            BRepAlgoAPI_Common common(solid_shapes[i], solid_shapes[j]);
            if (!common.IsDone()) {
                add(StepDefectKind::InvalidGeometry,
                    solid_shapes[i],
                    extent(solid_shapes[i]),
                    center(solid_shapes[i]),
                    0,
                    "Boolean common failed");
                return;
            }
            GProp_GProps volume;
            BRepGProp::VolumeProperties(common.Shape(), volume);
            if (volume.Mass() > 0)
                add(StepDefectKind::SolidOverlap,
                    solid_shapes[i],
                    std::cbrt(volume.Mass()),
                    center(common.Shape()));
        });
    }
};
} // namespace
bool step_import_available() noexcept {
    return true;
}
StepImportResult import_step(const std::filesystem::path& path,
                             const StepImportOptions& options) try {
    const auto& t = options.tolerances;
    detail::validate_step_options(path, options);
    // OCCT's exchange configuration uses process-global state.
    static std::mutex exchange_mutex;
    const std::lock_guard lock(exchange_mutex);
    STEPCAFControl_Reader reader;
    const ReadPrecision read_precision(t);
    reader.SetNameMode(true);
    if (reader.ReadFile(path.string().c_str()) != IFSelect_RetDone)
        throw std::runtime_error("cannot read STEP: " + path.string());
    // OCCT expresses system units as millimetres per unit. Transfer directly
    // into metres, once, preserving transfer-reader face identity.
    reader.ChangeReader().SetSystemLengthUnit(1000.0);
    auto source = std::make_shared<detail::StepShape>();
    source->document = new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::SetLengthUnit(source->document, 1.0);
    {
        const DetectionTransfer detection_only;
        if (!reader.Transfer(source->document))
            throw std::runtime_error("cannot transfer STEP");
    }
    source->session = reader.Reader().WS();
    auto tool = XCAFDoc_DocumentTool::ShapeTool(source->document->Main());
    TDF_LabelSequence roots;
    tool->GetFreeShapes(roots);
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (int i = 1; i <= roots.Length(); ++i)
        builder.Add(compound, tool->GetShape(roots(i)));
    source->shape = compound;
    TopExp::MapShapes(source->shape, TopAbs_FACE, source->faces);
    if (source->faces.IsEmpty())
        throw std::runtime_error("STEP contains no faces");
    StepImportResult result;
    result.bbox = bounds(source->shape);
    source->entities.resize(static_cast<std::size_t>(source->faces.Extent()));
    source->names.resize(source->entities.size());
    const auto transfer = source->session->TransferReader();
    const auto model = reader.Reader().StepModel();
    for (int i = 1; i <= source->faces.Extent(); ++i) {
        const auto& face = source->faces(i);
        auto entity = transfer->EntityFromShapeResult(face, 1);
        if (entity.IsNull())
            entity = transfer->EntityFromShapeResult(face.Located(TopLoc_Location()), 1);
        if (entity.IsNull())
            entity = transfer->EntityFromShapeResult(face, 3);
        if (!entity.IsNull()) {
            const auto id = model->IdentLabel(entity);
            if (id > 0)
                source->entities[static_cast<std::size_t>(i - 1)] = "#" + std::to_string(id);
        }
        // Explicit identity history: E4 does not replace faces. E5 can compose
        // its replacement history with this preserved original topology.
        source->history->Replace(face, face);
    }
    std::function<void(TDF_Label, std::string, TopLoc_Location)> visit;
    visit = [&](TDF_Label label, std::string prefix, TopLoc_Location parent) {
        const auto path_name =
            prefix.empty() ? label_name(label) : prefix + "/" + label_name(label);
        auto definition = label;
        auto location = parent;
        if (tool->IsReference(label)) {
            tool->GetReferredShape(label, definition);
            location = parent * tool->GetLocation(label);
        }
        TDF_LabelSequence children;
        if (tool->GetComponents(definition, children)) {
            for (int i = 1; i <= children.Length(); ++i)
                visit(children(i), path_name, location);
        } else {
            result.body_names.push_back(path_name);
            auto shape = tool->GetShape(definition).Moved(location);
            for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
                const auto index = source->faces.FindIndex(it.Current());
                if (index)
                    source->names[static_cast<std::size_t>(index - 1)] = path_name;
            }
        }
    };
    for (int i = 1; i <= roots.Length(); ++i)
        visit(roots(i), "", TopLoc_Location());
    Inspector inspector{{}, {}, *source, result, options};
    try {
        for (int i = 1; i <= source->faces.Extent(); ++i)
            if (source->entities[static_cast<std::size_t>(i - 1)].empty())
                inspector.add(StepDefectKind::InvalidGeometry,
                              source->faces(i),
                              0,
                              center(source->faces(i)),
                              i,
                              "Original STEP face attribution unavailable");
        for (int i = 1; i <= model->NbEntities(); ++i) {
            auto uncertainty =
                Handle(StepBasic_UncertaintyMeasureWithUnit)::DownCast(model->Value(i));
            if (uncertainty.IsNull())
                continue;
            const auto value =
                uncertainty->ValueComponent() * unit_scale(uncertainty->UnitComponent());
            if (value > t.tau_A) {
                // The declaration applies to every transferred face in its representation.
                for (int face = 1; face <= source->faces.Extent(); ++face)
                    inspector.add(StepDefectKind::ToleranceExceeded,
                                  source->faces(face),
                                  value,
                                  center(source->faces(face)),
                                  face,
                                  "STEP uncertainty #" +
                                      std::to_string(model->IdentLabel(uncertainty)));
            }
        }
        ShapeAnalysis_ShapeTolerance tolerance_scan;
        auto excessive = tolerance_scan.OverTolerance(source->shape, t.tau_A);
        for (int i = 1; i <= excessive->Length(); ++i) {
            const auto& shape = excessive->Value(i);
            inspector.add(StepDefectKind::ToleranceExceeded,
                          shape,
                          tolerance_scan.Tolerance(shape, 1),
                          center(shape));
        }
        inspector.inspect();
    } catch (const Standard_Failure& failure) {
        inspector.add(StepDefectKind::InvalidGeometry,
                      source->shape,
                      0,
                      center(source->shape),
                      0,
                      failure.GetMessageString());
    } catch (const std::runtime_error& failure) {
        inspector.add(StepDefectKind::InvalidGeometry,
                      source->shape,
                      0,
                      center(source->shape),
                      0,
                      failure.what());
    }
    result.source = std::move(source);
    return result;
} catch (const Standard_Failure& failure) {
    throw std::runtime_error(std::string("OCCT STEP inspection failed: ") +
                             failure.GetMessageString());
}
} // namespace chipcae::geometry
