// Synthetic OCCT geometry only; no supplier STEP data is checked in.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
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
#include <cmath>
#include <filesystem>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4)
        return 2;
    const std::filesystem::path dir(argv[1]);
    const std::string mode(argv[2]);
    if (argc == 4 && (mode != "ring_chamfer_lid" || std::string(argv[3]) != "rot"))
        return 2;
    const auto write = [&](const char* name, const TopoDS_Shape& shape) {
        STEPControl_Writer writer;
        if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone)
            return false;
        return writer.Write((dir / name).string().c_str()) == IFSelect_RetDone;
    };
    if (mode == "ring_chamfer_lid") {
        const auto outer = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 40, 40, .5).Shape();
        const auto inner = BRepPrimAPI_MakeBox(gp_Pnt(5, 5, -1), 30, 30, 2.5).Shape();
        TopoDS_Shape ring = BRepAlgoAPI_Cut(outer, inner).Shape();
        if (argc == 4) {
            gp_Trsf rotation;
            rotation.SetRotation(gp_Ax1(gp_Pnt(20, 20, 0), gp_Dir(0, 0, 1)),
                                 15 * std::acos(-1.) / 180);
            ring = BRepBuilderAPI_Transform(ring, rotation, true).Shape();
        }
        BRepBuilderAPI_MakePolygon polygon;
        for (const auto& point : {gp_Pnt(2, 0, 0),
                                  gp_Pnt(54, 0, 0),
                                  gp_Pnt(56, 2, 0),
                                  gp_Pnt(56, 42, 0),
                                  gp_Pnt(54, 44, 0),
                                  gp_Pnt(2, 44, 0),
                                  gp_Pnt(0, 42, 0),
                                  gp_Pnt(0, 2, 0)})
            polygon.Add(point);
        polygon.Close();
        const auto lid =
            BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(polygon.Wire()).Face(), gp_Vec(0, 0, .5))
                .Shape();
        return write("stiffener_ring.step", ring) && write("lid_chamfer.step", lid) ? 0 : 1;
    }
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
        // The wedge tapers along local Y. Align it with global Z for slicing.
        const gp_Ax2 axes(gp_Pnt(0, 1, 0), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0));
        if (!write("lid.step", BRepPrimAPI_MakeWedge(axes, 1, .5, 1, 1 - .5 * tau_b).Shape()))
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
