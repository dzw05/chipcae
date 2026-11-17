#include "occt_fixture.hpp"

#include <BRep_Builder.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TopoDS_Compound.hxx>

using namespace slice_test;
int main() {
    return run([] {
        // One STEP entity contains a frame, an island in its cavity, and a
        // disconnected rectangle. Nesting must attach the hole to the frame.
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        builder.Add(compound,
                    BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(40, 40, 0.5).Shape(),
                                    BRepPrimAPI_MakeBox(gp_Pnt(5, 5, -1), 30, 30, 3).Shape())
                        .Shape());
        builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(10, 10, 0), 5, 5, 0.5).Shape());
        builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(50, 0, 0), 10, 20, 0.5).Shape());
        // Disable XCAF assembly expansion so this is one named entity with
        // disconnected topology, rather than three separately selected bodies.
        Handle(TDocStd_Document) doc = new TDocStd_Document("BinXCAF");
        const auto tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        const auto label = tool->AddShape(compound, false);
        TDataStd_Name::Set(label, "components");
        const auto path = std::filesystem::temp_directory_path() /
                          ("chipcae-components-" + std::to_string(getpid()) + ".step");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        } cleanup{path};
        STEPCAFControl_Writer writer;
        Interface_Static::SetCVal("write.step.unit", "MM");
        require(writer.Transfer(doc, STEPControl_AsIs), "component transfer");
        require(writer.Write(path.string().c_str()) == IFSelect_RetDone, "component write");
        const auto input =
            import_step(path, {compute_tolerances({0.06, 0.0005, 0.001, 0}, {}).value()});
        require(input.body_names.size() == 1, "one named compound");
        const auto result = slice(input);
        same(result, slice(input));
        require(tier(result) == Tier::A, "disconnected prisms are tier A");
        require(result.slabs.size() == 1, "components share one height interval");
        const auto& slab = result.slabs.front();
        require(slab.sections.size() == 3, "all disconnected components retained");
        std::size_t holes = 0;
        for (const auto& section : slab.sections) {
            rectangle(section.outer, 1e-12);
            holes += section.holes.size();
            if (!section.holes.empty()) {
                require(section.holes.size() == 1, "frame has one cavity");
                near(area(section.outer), 0.0016, 1e-15, "hole belongs to frame");
            }
        }
        require(holes == 1, "one hole overall");
        near(area(slab), 0.000925, 0.000925 * 1e-12, "net component area");

        const auto other =
            tool->AddShape(BRepPrimAPI_MakeBox(gp_Pnt(100, 0, 2), 10, 20, 1).Shape(), false);
        TDataStd_Name::Set(other, "other");
        TopoDS_Compound separated;
        builder.MakeCompound(separated);
        builder.Add(separated, BRepPrimAPI_MakeBox(gp_Pnt(200, 0, 2), 10, 20, 1).Shape());
        builder.Add(separated, BRepPrimAPI_MakeBox(gp_Pnt(200, 0, 4), 10, 20, 1).Shape());
        TDataStd_Name::Set(tool->AddShape(separated, false), "gaps");
        STEPCAFControl_Writer multi_writer;
        require(multi_writer.Transfer(doc, STEPControl_AsIs), "multi body transfer");
        require(multi_writer.Write(path.string().c_str()) == IFSelect_RetDone, "multi body write");
        const auto multi =
            import_step(path, {compute_tolerances({0.12, 0.0005, 0.001, 0}, {}).value()});
        require(multi.body_names.size() == 3, "three independent named bodies");
        for (const auto& name : multi.body_names) {
            const auto selected = slice_prismatic(multi, name, options());
            same(selected, slice_prismatic(multi, name, options()));
            if (name == input.body_names.front()) {
                same(result, selected);
            } else if (name == "gaps") {
                require(tier(selected) == Tier::A, "separated solids tier A");
                require(selected.slabs.size() == 2, "equal sections cannot merge across empty gap");
                near(selected.slabs[0].z0, 0.002, 1e-12, "first interval bottom");
                near(selected.slabs[0].z1, 0.003, 1e-12, "first interval top");
                near(selected.slabs[1].z0, 0.004, 1e-12, "second interval bottom");
                near(selected.slabs[1].z1, 0.005, 1e-12, "second interval top");
            } else {
                require(name == "other", "expected selected body name");
                const auto box = single(selected);
                rectangle(box.sections.front().outer, 1e-12);
                near(box.z0, 0.002, 1e-12, "selected body bottom");
                near(box.z1, 0.003, 1e-12, "selected body top");
                near(area(box), 0.0002, 1e-15, "selected body area");
            }
        }
        // A vertical sheet ending at the midpoint must not add its boundary
        // edge to the box contour. Exercise contact from either side.
        for (const bool above : {false, true}) {
            TopoDS_Compound contact;
            builder.MakeCompound(contact);
            builder.Add(contact, BRepPrimAPI_MakeBox(40, 30, 0.5).Shape());
            BRepBuilderAPI_MakePolygon wire;
            const double end = above ? 0.5 : 0.0;
            wire.Add(gp_Pnt(50, 0, end));
            wire.Add(gp_Pnt(50, 10, end));
            wire.Add(gp_Pnt(50, 10, 0.25));
            wire.Add(gp_Pnt(50, 0, 0.25));
            wire.Close();
            builder.Add(contact, BRepBuilderAPI_MakeFace(wire.Wire()).Face());
            Handle(TDocStd_Document) contact_doc = new TDocStd_Document("BinXCAF");
            const auto contact_tool = XCAFDoc_DocumentTool::ShapeTool(contact_doc->Main());
            TDataStd_Name::Set(contact_tool->AddShape(contact, false), "contact");
            STEPCAFControl_Writer contact_writer;
            require(contact_writer.Transfer(contact_doc, STEPControl_AsIs), "contact transfer");
            require(contact_writer.Write(path.string().c_str()) == IFSelect_RetDone,
                    "contact write");
            const auto touching =
                import_step(path, {compute_tolerances({0.06, 0.0005, 0.001, 0}, {}).value()});
            require(touching.body_names.size() == 1, "one contact body");
            auto midpoint_options = options();
            midpoint_options.tau_a = 0.00025;
            midpoint_options.tau_b = 0.00025;
            const auto sliced = slice_prismatic(touching, "contact", midpoint_options);
            const auto slab = single(sliced);
            rectangle(slab.sections.front().outer, 1e-12);
            near(area(slab), 0.0012, 1e-15, "endpoint contact contributes no section edge");
            same(sliced, slice_prismatic(touching, "contact", midpoint_options));
        }
    });
}
