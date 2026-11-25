#include "occt_fixture.hpp"

#include <map>
using namespace slice_test;
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--check-demo-env") {
        return run([&] {
            require(demo_env("CHIPCAE_DEMO_BULK_STEP") == std::filesystem::path(argv[2]),
                    "CHIPCAE_DEMO_BULK_STEP resolves only configured regular files");
        });
    }
    const auto path = demo_env("CHIPCAE_DEMO_BULK_STEP");
    if (path.empty())
        return 77;
    return run([&] {
        const auto input = import_step(path);
        require(input.body_names.size() == 4, "BULK imports four bodies");
        // Independent STEP reader provides the original planar bounds in metres.
        STEPCAFControl_Reader reader;
        require(reader.ReadFile(path.string().c_str()) == IFSelect_RetDone, "read reference STEP");
        reader.ChangeReader().SetSystemLengthUnit(1000.0);
        Handle(TDocStd_Document) doc = new TDocStd_Document("BinXCAF");
        XCAFDoc_DocumentTool::SetLengthUnit(doc, 1.0);
        require(reader.Transfer(doc), "transfer reference STEP");
        const auto tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        TDF_LabelSequence roots;
        tool->GetFreeShapes(roots);
        std::map<std::string, std::array<double, 2>> bounds;
        const auto visit = [&](const auto& self,
                               const TDF_Label& label,
                               const std::string& prefix,
                               const TopLoc_Location& parent) -> void {
            Handle(TDataStd_Name) name;
            require(label.FindAttribute(TDataStd_Name::GetID(), name), "reference body name");
            const std::string text = TCollection_AsciiString(name->Get()).ToCString();
            const std::string full = prefix.empty() ? text : prefix + "/" + text;
            TDF_Label referred = label;
            auto location = parent;
            if (tool->IsReference(label)) {
                tool->GetReferredShape(label, referred);
                location = parent * tool->GetLocation(label);
            }
            TDF_LabelSequence children;
            if (tool->GetComponents(referred, children)) {
                for (int i = 1; i <= children.Length(); ++i)
                    self(self, children.Value(i), full, location);
                return;
            }
            Bnd_Box box;
            BRepBndLib::AddOptimal(tool->GetShape(referred).Moved(location), box, false, false);
            double x0, y0, z0, x1, y1, z1;
            box.Get(x0, y0, z0, x1, y1, z1);
            bounds.emplace(full, std::array<double, 2>{z0, z1});
        };
        for (int i = 1; i <= roots.Length(); ++i)
            visit(visit, roots.Value(i), "", TopLoc_Location());
        bool valid = true;
        for (const auto& name : input.body_names) {
            try {
                const auto s = single(slice_prismatic(input, name, options()));
                require(s.sections[0].holes.empty(), "BULK rectangle has no holes");
                rectangle(s.sections[0].outer, 1e-9);
                require(bounds.contains(name), "reference body matches imported body");
                near(s.z0, bounds.at(name)[0], 1e-9, "BULK bottom matches STEP plane");
                near(s.z1, bounds.at(name)[1], 1e-9, "BULK top matches STEP plane");
            } catch (const std::exception& e) {
                std::cerr << name << ": " << e.what() << '\n';
                valid = false;
            }
        }
        require(valid, "all four BULK bodies slice successfully");
    });
}
