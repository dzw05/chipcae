#include "stack_writer.hpp"

#include "../../schema/src/sha256.hpp"

#include <fstream>
#include <iterator>
#include <set>
#if defined(CHIPCAE_ENABLE_JSONCONS)
#include <jsoncons/json.hpp>
namespace chipcae::importer {
mesh::Expected<bool> write_stack(const mesh::StackModel& model, const std::filesystem::path& path) {
    using J = jsoncons::json;
    try {
        auto footprint = [](const mesh::Footprint& f) {
            J j;
            j["box"] = f.box;
            if (!f.polygon.empty())
                j["polygon"] = f.polygon;
            return j;
        };
        J j;
        j["id"] = model.id;
        j["unit"] = model.unit;
        j["symmetry"] = model.symmetry;
        j["package_type"] = model.package_type;
        j["outline"] = footprint(model.outline);
        j["layers"] = J::array();
        J refs = J::array();
        std::set<std::string> seen;
        for (const auto& l : model.layers) {
            J v;
            v["name"] = l.name;
            v["kind"] = l.kind;
            v["body"] = l.body;
            v["thickness"] = l.thickness;
            v["grading"] = l.grading;
            v["n_elem_min"] = l.n_elem_min;
            if (!l.material.empty())
                v["material"] = l.material;
            if (!l.regions.empty()) {
                v["regions"] = J::array();
                for (const auto& r : l.regions) {
                    J a;
                    a["name"] = r.name;
                    a["body"] = r.body;
                    if (!r.material.empty())
                        a["material"] = r.material;
                    a["footprint"] = footprint(r.footprint);
                    v["regions"].push_back(a);
                }
            }
            if (l.copper) {
                J c;
                c["metal"] = l.copper->metal;
                if (l.copper->fraction)
                    c["fraction"] = *l.copper->fraction;
                if (l.copper->polygons) {
                    const auto& p = *l.copper->polygons;
                    c["polygons"]["file"] = p.file;
                    c["polygons"]["layer"] = p.layer;
                    if (seen.insert(p.file).second) {
                        const auto resolved = path.parent_path() / std::filesystem::path(p.file);
                        std::ifstream in(resolved, std::ios::binary);
                        if (!in)
                            return mesh::Error{"$/external_refs",
                                               "INPUT_INVALID: missing sidecar " + p.file};
                        std::string bytes{std::istreambuf_iterator<char>(in), {}};
                        J ref;
                        ref["path"] = p.file;
                        ref["sha256"] = mesh::sha256(bytes);
                        refs.push_back(ref);
                    }
                }
                v["copper"] = c;
            }
            j["layers"].push_back(v);
        }
        if (!refs.empty())
            j["external_refs"] = refs;
        if (!model.activation_groups.empty()) {
            j["activation_groups"] = J::array();
            for (const auto& [name, members] : model.activation_groups) {
                J g;
                g["name"] = name;
                g["members"] = members;
                j["activation_groups"].push_back(g);
            }
        }
        std::ofstream out(path);
        out << j << '\n';
        out.flush();
        if (!out)
            return mesh::Error{"$", "INPUT_INVALID: cannot write stack"};
        return true;
    } catch (const std::exception& e) {
        return mesh::Error{"$", e.what()};
    }
}
} // namespace chipcae::importer
#else
namespace chipcae::importer {
mesh::Expected<bool> write_stack(const mesh::StackModel&, const std::filesystem::path&) {
    return mesh::Error{"$", "JSON support disabled"};
}
} // namespace chipcae::importer
#endif
