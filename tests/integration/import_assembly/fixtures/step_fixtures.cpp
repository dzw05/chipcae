// Synthetic OCCT geometry only; no supplier STEP data is checked in.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeWedge.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <filesystem>
#include <gp_Pnt.hxx>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    const std::filesystem::path dir(argv[1]);
    const std::string mode(argv[2]);
    const auto write = [&](const char* name, const TopoDS_Shape& shape) {
        STEPControl_Writer writer;
        if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone)
            return false;
        return writer.Write((dir / name).string().c_str()) == IFSelect_RetDone;
    };
    // STEP writer uses millimetres; the import contract normalizes to metres.
    // Board t_min=35um, eps_weld=min(1e-6*L,.005*t_min)=11.1803398875nm.
    const double eps =
        .005 * .035 < 1e-6 * 11.180339887498949 ? .005 * .035 : 1e-6 * 11.180339887498949;
    const double delta = (eps + .035) / 2;
    if (!write("lid.step", BRepPrimAPI_MakeBox(gp_Pnt(1, 1, 0), 1, 1, .5).Shape()))
        return 1;
    if (!write("second.step", BRepPrimAPI_MakeBox(gp_Pnt(4, 1, 0), 1, 1, .5 + delta).Shape()))
        return 1;
    auto outer = BRepPrimAPI_MakeBox(gp_Pnt(.5, .5, 0), 8, 4, .2).Shape();
    auto inner = BRepPrimAPI_MakeBox(gp_Pnt(1, 1, -.1), 7, 3, .4).Shape();
    if (!write("stiffener.step", BRepAlgoAPI_Cut(outer, inner).Shape()))
        return 1;
    if (mode == "draft") {
        // 0.5*tau_B over a side wall: tau_B=min(1e-4*L,.1*t_min).
        const double tau_b = .0035 < .001118033988749895 ? .0035 : .001118033988749895;
        if (!write("lid.step", BRepPrimAPI_MakeWedge(1, 1, .5, 1 - .5 * tau_b).Shape()))
            return 1;
    }
    Handle(TDocStd_Document) document;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", document);
    auto tool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    auto mapped = tool->AddShape(BRepPrimAPI_MakeBox(gp_Pnt(1, 1, 0), 1, 1, .5).Shape(), false);
    TDataStd_Name::Set(mapped, mode == "display" ? "NOT_CAE_GEOMETRY" : "lid");
    auto unmapped = tool->AddShape(BRepPrimAPI_MakeBox(gp_Pnt(4, 1, 0), 1, 1, .5).Shape(), false);
    TDataStd_Name::Set(unmapped, "unmapped");
    STEPCAFControl_Writer bulk;
    bulk.SetNameMode(true);
    if (!bulk.Transfer(document, STEPControl_AsIs) ||
        bulk.Write((dir / "bulk.step").string().c_str()) != IFSelect_RetDone)
        return 1;
    return 0;
}
