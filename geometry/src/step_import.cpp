#include "chipcae/geometry/step_import.hpp"

#include "diagnostic_id.hpp"

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
#include <STEPCAFControl_Reader.hxx>
#include <ShapeBuild_ReShape.hxx>
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
#include <stdexcept>

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
    BRepBndLib::AddOptimal(shape, b, false, false);
    if (b.IsVoid())
        throw std::runtime_error("STEP shape has no finite bounding box");
    std::array<Real, 6> result;
    b.Get(result[0], result[1], result[2], result[3], result[4], result[5]);
    for (auto x : result)
        if (!std::isfinite(x))
            throw std::runtime_error("STEP has unbounded geometry");
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
struct Inspector {
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
    void add(DefectKind kind,
             const TopoDS_Shape& shape,
             Real measured,
             std::array<Real, 3> where,
             Index face = 0) {
        if (!face)
            face = face_of(shape);
        Defect d;
        d.kind = kind;
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
        d.tier = classify(measured, options.tolerances);
        d.defect_id = detail::diagnostic_id(d, options.tolerances.eps_weld);
        if (std::none_of(result.defects.begin(), result.defects.end(), [&](const Defect& old) {
                return old.defect_id == d.defect_id;
            }))
            result.defects.push_back(std::move(d));
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
            const auto report_statuses = [&](const BRepCheck_ListOfStatus& statuses) {
                for (BRepCheck_ListIteratorOfListOfStatus it(statuses); it.More(); it.Next()) {
                    switch (it.Value()) {
                    case BRepCheck_NoError:
                        break;
                    case BRepCheck_IntersectingWires:
                    case BRepCheck_SelfIntersectingWire:
                        add(DefectKind::WireSelfIntersection, s, extent(s), center(s));
                        break;
                    case BRepCheck_RedundantFace:
                        add(DefectKind::DuplicateFace, s, extent(s), center(s));
                        break;
                    case BRepCheck_UnorientableShape:
                    case BRepCheck_BadOrientation:
                    case BRepCheck_BadOrientationOfSubshape:
                        add(DefectKind::FlippedFace, s, extent(s), center(s));
                        break;
                    case BRepCheck_InvalidMultiConnexity:
                        add(DefectKind::NonManifoldEdge, s, extent(s), center(s));
                        break;
                    case BRepCheck_FreeEdge:
                    case BRepCheck_NotClosed:
                        add(s.ShapeType() == TopAbs_SHELL ? DefectKind::OpenShell
                                                          : DefectKind::OpenEdge,
                            s,
                            extent(s),
                            center(s));
                        break;
                    default:
                        // Do not silently label an unsupported validity failure as a
                        // different physical defect or return a clean report.
                        throw std::runtime_error("unmapped STEP validity status: " +
                                                 std::to_string(static_cast<int>(it.Value())));
                    }
                }
            };
            report_statuses(r->Status());
            for (r->InitContextIterator(); r->MoreShapeInContext(); r->NextShapeInContext())
                report_statuses(r->StatusOnShape());
        }
        for (TopExp_Explorer it(source.shape, TopAbs_SHELL); it.More(); it.Next()) {
            BRepCheck_Shell shell(TopoDS::Shell(it.Current()));
            if (shell.Closed() != BRepCheck_NoError)
                add(DefectKind::OpenShell,
                    it.Current(),
                    extent(it.Current()),
                    center(it.Current()));
        }
        BOPAlgo_ArgumentAnalyzer args;
        args.SetShape1(source.shape);
        args.SelfInterMode() = true;
        args.SmallEdgeMode() = true;
        args.Perform();
        for (const auto& failure : args.GetCheckResult()) {
            for (const auto& s : failure.GetFaultyShapes1()) {
                if (s.ShapeType() == TopAbs_VERTEX)
                    continue;
                auto kind = failure.GetCheckStatus() == BOPAlgo_TooSmallEdge
                                ? DefectKind::SmallEdge
                                : DefectKind::WireSelfIntersection;
                if (failure.GetCheckStatus() != BOPAlgo_TooSmallEdge &&
                    failure.GetCheckStatus() != BOPAlgo_SelfIntersect)
                    continue;
                add(kind, s, kind == DefectKind::SmallEdge ? length(s) : extent(s), center(s));
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
            if (faces.Extent() > 2)
                add(DefectKind::NonManifoldEdge, e, length(e), center(e));
            if (faces.Extent() == 1 &&
                !BRep_Tool::IsClosed(TopoDS::Edge(e), TopoDS::Face(faces.First()))) {
                boundary.push_back(i);
                add(DefectKind::OpenEdge, e, length(e), center(e));
            }
            if (length(e) < options.tolerances.eps_weld)
                add(DefectKind::SmallEdge, e, length(e), center(e));
        }
        // Nearby free boundaries on different faces are candidate gaps. Locate at
        // the midpoint of closest points, measured as their exact separation.
        for (std::size_t i = 0; i < boundary.size(); ++i)
            for (std::size_t j = i + 1; j < boundary.size(); ++j) {
                const auto a = boundary[i], b = boundary[j];
                if (edges.FindFromIndex(a).First().IsSame(edges.FindFromIndex(b).First()))
                    continue;
                BRepExtrema_DistShapeShape distance(edges.FindKey(a), edges.FindKey(b));
                if (!distance.IsDone() || distance.NbSolution() == 0)
                    continue;
                const auto gap = distance.Value();
                if (gap <= options.tolerances.eps_weld || gap > options.tolerances.tau_B)
                    continue;
                // A whole opposing edge must be close; endpoint-only proximity is
                // not evidence of a missing seam.
                const auto ca = center(edges.FindKey(a)), cb = center(edges.FindKey(b));
                if (std::hypot(ca[0] - cb[0], ca[1] - cb[1], ca[2] - cb[2]) >
                    options.tolerances.tau_B)
                    continue;
                BRepAdaptor_Curve curve(TopoDS::Edge(edges.FindKey(a)));
                const auto midpoint =
                    curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2);
                const auto vertex = BRepBuilderAPI_MakeVertex(midpoint).Vertex();
                BRepExtrema_DistShapeShape local(vertex, edges.FindKey(b));
                if (!local.IsDone() || local.NbSolution() == 0 ||
                    local.Value() <= options.tolerances.eps_weld ||
                    local.Value() > options.tolerances.tau_B)
                    continue;
                const auto opposite = local.PointOnShape2(1);
                std::array<Real, 3> where{(midpoint.X() + opposite.X()) / 2,
                                          (midpoint.Y() + opposite.Y()) / 2,
                                          (midpoint.Z() + opposite.Z()) / 2};
                add(DefectKind::Gap,
                    edges.FindKey(a),
                    local.Value(),
                    where,
                    source.faces.FindIndex(edges.FindFromIndex(a).First()));
            }
        for (int i = 1; i <= source.faces.Extent(); ++i) {
            const auto& f = source.faces(i);
            const auto width = face_width(f);
            if (width < options.tolerances.eps_weld)
                add(DefectKind::SmallFace, f, width, center(f));
            const auto perimeter = length(f);
            if (perimeter > 0 && width > 0 &&
                width * width / perimeter < options.tolerances.eps_weld)
                add(DefectKind::SliverFace, f, width * width / perimeter, center(f));
        }
        for (int i = 1; i <= source.faces.Extent(); ++i)
            for (int j = i + 1; j <= source.faces.Extent(); ++j) {
                const auto& a = source.faces(i);
                const auto& b = source.faces(j);
                const auto ba = bounds(a), bb = bounds(b);
                bool same_box = true;
                for (std::size_t k = 0; k < 6; ++k)
                    same_box = same_box && std::abs(ba[k] - bb[k]) <= options.tolerances.eps_weld;
                if (!same_box)
                    continue;
                BRepAlgoAPI_Common common(a, b);
                if (!common.IsDone())
                    throw std::runtime_error("STEP duplicate-face inspection failed");
                const auto wa = face_width(a), wb = face_width(b);
                const auto wc = face_width(common.Shape());
                if (wa > 0 && std::abs(wa - wb) <= options.tolerances.eps_weld &&
                    std::abs(wa - wc) <= options.tolerances.eps_weld)
                    add(DefectKind::DuplicateFace, a, wa, center(a));
            }
        TopTools_IndexedMapOfShape solids;
        TopExp::MapShapes(source.shape, TopAbs_SOLID, solids);
        for (int i = 1; i <= solids.Extent(); ++i)
            for (int j = i + 1; j <= solids.Extent(); ++j) {
                // Exact common volume also catches complete containment.
                BRepAlgoAPI_Common common(solids(i), solids(j));
                if (!common.IsDone())
                    throw std::runtime_error("STEP overlap inspection failed");
                GProp_GProps volume;
                BRepGProp::VolumeProperties(common.Shape(), volume);
                if (volume.Mass() > 0)
                    add(DefectKind::SolidOverlap,
                        solids(i),
                        std::cbrt(volume.Mass()),
                        center(common.Shape()));
            }
    }
};
} // namespace
bool step_import_available() noexcept {
    return true;
}
StepImportResult import_step(const std::filesystem::path& path,
                             const StepImportOptions& options) try {
    const auto& t = options.tolerances;
    if (path.empty() || !std::isfinite(options.h_min) || options.h_min <= 0 ||
        !std::isfinite(t.eps_weld) || !std::isfinite(t.tau_A) || !std::isfinite(t.tau_B) ||
        t.eps_weld <= 0 || t.tau_A < t.eps_weld || t.tau_B < t.tau_A)
        throw std::invalid_argument("invalid STEP path or inspection scales");
    // OCCT's exchange configuration uses process-global state.
    static std::mutex exchange_mutex;
    const std::lock_guard lock(exchange_mutex);
    STEPCAFControl_Reader reader;
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
        if (source->entities[static_cast<std::size_t>(i - 1)].empty())
            throw std::runtime_error("cannot attribute original STEP face " + std::to_string(i));
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
    Inspector{*source, result, options}.inspect();
    result.source = std::move(source);
    return result;
} catch (const Standard_Failure& failure) {
    throw std::runtime_error(std::string("OCCT STEP inspection failed: ") +
                             failure.GetMessageString());
}
} // namespace chipcae::geometry
