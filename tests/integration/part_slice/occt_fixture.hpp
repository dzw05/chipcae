#pragma once
#include "slice_test.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Bnd_Box.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <StepData_StepModel.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gp_Ax1.hxx>
#include <unistd.h>
namespace slice_test {
// Fixture coordinates are millimetres; the public importer returns metres.
inline StepImportResult fixture(const TopoDS_Shape& shape) {
    static unsigned serial = 0;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("chipcae-sp2-" + std::to_string(getpid()) + "-" + std::to_string(serial++) + ".step");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{path};
    STEPControl_Writer writer;
    Interface_Static::SetCVal("write.step.unit", "MM");
    writer.Model()->SetLocalLengthUnit(1.0);
    writer.Model()->SetWriteLengthUnit(1.0);
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone, "fixture transfer");
    require(writer.Write(path.string().c_str()) == IFSelect_RetDone, "fixture write");
    auto result = import_step(path, {compute_tolerances({0.04, 0.0005, 0.001, 0}, {}).value()});
    require(result.source && result.body_names.size() == 1, "fixture imports one named body");
    return result;
}
inline SliceResult slice(const StepImportResult& input) {
    return slice_prismatic(input, input.body_names.at(0), options());
}
inline Slab single(const SliceResult& r) {
    require(r.slabs.size() == 1, "exactly one merged slab");
    require(r.slabs[0].sections.size() == 1, "one connected section");
    require(tier(r) == Tier::A, "prismatic part is tier A");
    return r.slabs[0];
}
inline std::filesystem::path demo(const char* name) {
    const char* dir = std::getenv("CHIPCAE_DEMO_STEP_DIR");
    if (!dir)
        return {};
    const auto path = std::filesystem::path(dir) / name;
    return std::filesystem::is_regular_file(path) ? path : std::filesystem::path{};
}
inline TopoDS_Wire square(double inset, double z) {
    BRepBuilderAPI_MakePolygon p;
    p.Add(gp_Pnt(inset, inset, z));
    p.Add(gp_Pnt(40 - inset, inset, z));
    p.Add(gp_Pnt(40 - inset, 40 - inset, z));
    p.Add(gp_Pnt(inset, 40 - inset, z));
    p.Close();
    return p.Wire();
}
} // namespace slice_test
