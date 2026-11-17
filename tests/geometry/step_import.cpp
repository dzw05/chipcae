#include "../../geometry/src/diagnostic_id.hpp"
#include "chipcae/geometry/geometry.hpp"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Writer.hxx>
#include <StepData_StepModel.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XSAlgo.hxx>
#include <XSAlgo_AlgoContainer.hxx>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <gp_Pln.hxx>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace chipcae::geometry;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void write(const TopoDS_Shape& shape, const std::filesystem::path& path, const char* unit = "M") {
    STEPControl_Writer writer;
    Interface_Static::SetCVal("write.step.unit", unit);
    writer.Model()->SetLocalLengthUnit(1.0);
    writer.Model()->SetWriteLengthUnit(std::string(unit) == "M" ? 1000.0 : 1.0);
    // Source coordinates are millimetres in OCCT's writer. The writer converts
    // them to the requested STEP representation unit.
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone,
            "fixture transfer failed");
    require(writer.Write(path.string().c_str()) == IFSelect_RetDone, "fixture write failed");
}
void units(const std::filesystem::path& dir) {
    const auto box = BRepPrimAPI_MakeBox(gp_Pnt(3, 5, 7), 20, 30, 40).Shape();
    write(box, dir / "mm.step", "MM");
    write(box, dir / "m.step", "M");
    const auto algorithms = XSAlgo::AlgoContainer();
    auto mm = import_step(dir / "mm.step"), m = import_step(dir / "m.step");
    require(algorithms == XSAlgo::AlgoContainer(), "exchange hooks were not restored");
    for (std::size_t i = 0; i < 6; ++i)
        require(std::abs(mm.bbox[i] - m.bbox[i]) <=
                    1e-12 * std::max(std::abs(mm.bbox[i]), std::abs(m.bbox[i])),
                "mm/m mismatch");
    require(std::abs(m.bbox[3] - 0.023) < 1e-14, "box not in metres");
    auto geom = Geometry::from_step(dir / "m.step");
    require(geom.step() && geom.step()->source, "geometry lost source ownership");
    bool rejected = false;
    try {
        (void)geom.signed_distance(0, 0, 0);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "STEP SDF must explicitly reject unsupported evaluation");
    auto invalid = StepImportOptions{};
    invalid.h_min = 0;
    rejected = false;
    try {
        (void)import_step(dir / "m.step", invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "invalid h_min accepted");
}
void gap(const std::filesystem::path& dir) {
    // Two planar rectangles with a 20 micrometre seam at x=10.01 mm.
    BRep_Builder b;
    TopoDS_Compound pair;
    b.MakeCompound(pair);
    gp_Pln plane(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    b.Add(pair, BRepBuilderAPI_MakeFace(plane, 0, 10, 0, 10).Face());
    b.Add(pair, BRepBuilderAPI_MakeFace(plane, 10.02, 20, 0, 10).Face());
    write(pair, dir / "gap.step");
    auto result = import_step(dir / "gap.step");
    const auto found = std::find_if(result.defects.begin(),
                                    result.defects.end(),
                                    [](const Defect& d) { return d.kind == DefectKind::Gap; });
    require(found != result.defects.end(), "gap not detected");
    require(std::count_if(result.defects.begin(),
                          result.defects.end(),
                          [](const Defect& d) { return d.kind == DefectKind::Gap; }) == 1,
            "expected exactly one gap");
    require(!found->body_name.empty(), "gap body name missing");
    require(found->defect_id.size() == 16, "wrong ID length");
    require(std::abs(found->measured - 20e-6) < 1e-12, "gap size wrong");
    require(std::hypot(found->where[0] - 0.01001, found->where[1] - 0.005, found->where[2]) <=
                StepImportOptions{}.tolerances.tau_B,
            "gap location outside tauB");
    std::ifstream file(dir / "gap.step");
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    require(!found->step_entity.empty(), "missing original STEP entity");
    // The first ADVANCED_FACE is the first rectangle, whose right boundary
    // generated this gap. Check the actual file reference, not just its syntax.
    const auto face = text.find("ADVANCED_FACE");
    const auto hash = text.rfind('#', face);
    const auto equal = text.find(' ', hash);
    const auto equals = text.find('=', hash);
    const auto end = std::min(equal, equals);
    require(found->step_entity == text.substr(hash, end - hash), "wrong original face reference");
    require(found->face_index == 1, "unstable face index");
    require(found->action.empty(), "detection performed repair");
    const auto again = import_step(dir / "gap.step");
    const auto repeated = std::find_if(again.defects.begin(),
                                       again.defects.end(),
                                       [](const Defect& d) { return d.kind == DefectKind::Gap; });
    require(repeated != again.defects.end() && repeated->defect_id == found->defect_id,
            "unstable defect ID");
}
void clean(const std::filesystem::path& dir) {
    // Authored enclosure: lid plus a rectangular stiffening ring below it.
    const auto lid = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 8), 40, 30, 2).Shape();
    const auto outer = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 40, 30, 6).Shape();
    const auto inner = BRepPrimAPI_MakeBox(gp_Pnt(2, 2, -1), 36, 26, 8).Shape();
    const auto ring = BRepAlgoAPI_Cut(outer, inner).Shape();
    TopoDS_Compound assembly;
    BRep_Builder b;
    b.MakeCompound(assembly);
    b.Add(assembly, lid);
    b.Add(assembly, ring);
    Handle(TDocStd_Document) doc = new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::SetLengthUnit(doc, 0.001);
    auto tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const auto root = tool->AddShape(assembly, true);
    TDataStd_Name::Set(root, "enclosure");
    TDF_LabelSequence parts;
    tool->GetComponents(root, parts);
    require(parts.Length() == 2, "fixture assembly components");
    TDataStd_Name::Set(parts(1), "lid");
    TDataStd_Name::Set(parts(2), "ring");
    STEPCAFControl_Writer writer;
    Interface_Static::SetCVal("write.step.unit", "M");
    writer.ChangeWriter().Model()->SetLocalLengthUnit(1.0);
    writer.ChangeWriter().Model()->SetWriteLengthUnit(1000.0);
    require(writer.Transfer(doc), "named assembly transfer failed");
    require(writer.Write((dir / "clean.step").string().c_str()) == IFSelect_RetDone,
            "named assembly write failed");
    auto result = import_step(dir / "clean.step");
    for (const auto& d : result.defects)
        std::cerr << "unexpected defect " << static_cast<int>(d.kind) << ' ' << d.step_entity
                  << '\n';
    require(result.defects.empty(), "clean enclosure has defects");
    require(std::find(result.body_names.begin(), result.body_names.end(), "enclosure/lid") !=
                result.body_names.end(),
            "XCAF assembly path missing");
    // Also exercise complete solid containment, which has no crossing faces.
    b.Add(assembly, BRepPrimAPI_MakeBox(gp_Pnt(4, 4, 8.5), 1, 1, 1).Shape());
    write(assembly, dir / "overlap.step");
    result = import_step(dir / "overlap.step");
    require(std::any_of(result.defects.begin(),
                        result.defects.end(),
                        [](const Defect& d) {
                            return d.kind == DefectKind::SolidOverlap && d.measured > 0;
                        }),
            "contained solid overlap missed");
    // Crossing faces exercise ArgumentAnalyzer's inter-solid interference.
    TopoDS_Compound crossing;
    b.MakeCompound(crossing);
    b.Add(crossing, BRepPrimAPI_MakeBox(10, 10, 10).Shape());
    b.Add(crossing, BRepPrimAPI_MakeBox(gp_Pnt(5, 5, 5), 10, 10, 10).Shape());
    write(crossing, dir / "crossing.step");
    result = import_step(dir / "crossing.step");
    require(std::any_of(result.defects.begin(),
                        result.defects.end(),
                        [](const Defect& d) {
                            return d.kind == DefectKind::SolidOverlap &&
                                   std::abs(d.measured - 0.005) < 1e-12;
                        }),
            "crossing solid overlap missing or measured incorrectly");
    require(
        std::none_of(result.defects.begin(),
                     result.defects.end(),
                     [](const Defect& d) { return d.kind == DefectKind::WireSelfIntersection; }),
        "solid interference mislabeled as a wire failure");
    TopoDS_Compound duplicate;
    b.MakeCompound(duplicate);
    gp_Pln plane(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    b.Add(duplicate, BRepBuilderAPI_MakeFace(plane, 0, 10, 0, 10).Face());
    b.Add(duplicate, BRepBuilderAPI_MakeFace(plane, 0, 10, 0, 10).Face());
    write(duplicate, dir / "duplicate.step");
    result = import_step(dir / "duplicate.step");
    require(std::any_of(result.defects.begin(),
                        result.defects.end(),
                        [](const Defect& d) { return d.kind == DefectKind::DuplicateFace; }),
            "duplicate faces missed");
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected test selector");
        require(detail::sha256_prefix("") == "e3b0c44298fc1c14", "SHA-256 empty vector");
        require(detail::sha256_prefix("abc") == "ba7816bf8f01cfea", "SHA-256 abc vector");
        require(detail::sha256_prefix(std::string(1000, 'a')) == "41edece42d63e8d9",
                "SHA-256 multiblock vector");
        auto dir = std::filesystem::current_path() / (std::string("step-fixture-") + argv[1]);
        std::filesystem::create_directories(dir);
        if (std::string(argv[1]) == "units")
            units(dir);
        else if (std::string(argv[1]) == "gap")
            gap(dir);
        else
            clean(dir);
        std::filesystem::remove_all(dir);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
