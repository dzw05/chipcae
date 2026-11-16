#include "chipcae/schema/stack.hpp"

#include "sha256.hpp"

#include <cctype>
#include <fstream>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <set>
#include <sstream>
namespace chipcae::mesh {
namespace {
using J = jsoncons::json;
Footprint footprint(const J& j) {
    Footprint f;
    for (std::size_t i = 0; i < 4; ++i)
        f.box[i] = j.at("box").at(i).as<Real>();
    if (j.contains("polygon"))
        for (const auto& p : j.at("polygon").array_range())
            f.polygon.push_back({p.at(0).as<Real>(), p.at(1).as<Real>()});
    return f;
}
} // namespace
Expected<StackModel> load_stack(const std::filesystem::path& file,
                                const std::filesystem::path& schema) {
    try {
        std::ifstream input(file), specification(schema);
        if (!input || !specification)
            return Error{"$", "cannot open instance or schema"};
        J j = J::parse(input), s = J::parse(specification);
        auto validator = jsoncons::jsonschema::make_json_schema(s);
        Error failure;
        validator.validate(j, [&](const jsoncons::jsonschema::validation_message& m) {
            if (failure.reason.empty())
                failure = Error{"$" + m.instance_location().string(), m.message()};
            return jsoncons::jsonschema::walk_result::advance;
        });
        if (!failure.reason.empty())
            return failure;
        StackModel model;
        model.id = j.at("id").as<std::string>();
        model.outline = footprint(j.at("outline"));
        if (!(model.outline.box[2] > model.outline.box[0] &&
              model.outline.box[3] > model.outline.box[1]))
            return Error{"$/outline/box", "positive rectangle required"};
        model.symmetry = j.at("symmetry").as<std::string>();
        if (model.symmetry != "none")
            return Error{"$/symmetry", "symmetry other than none is not implemented"};
        model.package_type = j.at("package_type").as<std::string>();
        auto warn_solder = [&](std::string material) {
            auto original = material;
            for (auto& c : material)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (material.find("sac") != std::string::npos ||
                material.find("solder") != std::string::npos)
                model.warnings.push_back("solder material name: " + original +
                                         "; verify equivalent properties");
        };
        std::set<std::string> names, refs;
        auto unique = [&](const std::string& name) { return names.insert(name).second; };
        auto inside = [&](const Footprint& f) {
            const auto& b = model.outline.box;
            if (!(f.box[2] > f.box[0] && f.box[3] > f.box[1]))
                return false;
            if (f.box[0] < b[0] || f.box[1] < b[1] || f.box[2] > b[2] || f.box[3] > b[3])
                return false;
            for (auto p : f.polygon)
                if (p[0] < b[0] || p[1] < b[1] || p[0] > b[2] || p[1] > b[3])
                    return false;
            return true;
        };
        for (std::size_t i = 0; i < j.at("layers").size(); ++i) {
            const auto& l = j.at("layers").at(i);
            std::string path = "$/layers/" + std::to_string(i);
            Layer v;
            v.name = l.at("name").as<std::string>();
            v.kind = l.at("kind").as<std::string>();
            if (v.name == "void")
                return Error{path + "/name", "void is a reserved name"};
            if (!unique(v.name))
                return Error{path + "/name", "duplicate name"};
            refs.insert("layer:" + v.name);
            v.thickness = l.at("thickness").as<Real>();
            v.grading = l.at("grading").as<Real>();
            v.n_elem_min = l.at("n_elem_min").as<Local>();
            if (l.contains("material"))
                v.material = l.at("material").as<std::string>();
            v.body = l.get_value_or<std::string>("body", v.name);
            if (v.body == "void")
                return Error{path + "/body", "void is a reserved name"};
            refs.insert("body:" + v.body);
            if (v.kind == "Homogenized" && !v.material.ends_with("_eq"))
                return Error{path + "/material", "homogenized material must end with _eq"};
            warn_solder(v.material);
            if (l.contains("regions"))
                for (const auto& r : l.at("regions").array_range()) {
                    Inclusion a;
                    a.name = r.at("name").as<std::string>();
                    a.material = r.at("material").as<std::string>();
                    a.body = r.get_value_or<std::string>("body", a.name);
                    a.footprint = footprint(r.at("footprint"));
                    if (a.name == "void")
                        return Error{path + "/regions/" + a.name, "void is a reserved name"};
                    if (a.body == "void")
                        return Error{path + "/regions/" + a.name + "/body",
                                     "void is a reserved name"};
                    if (!unique(a.name))
                        return Error{path + "/regions", "duplicate name"};
                    if (!inside(a.footprint))
                        return Error{path + "/regions/" + a.name + "/footprint", "outside outline"};
                    if (v.kind == "Homogenized" && !a.material.ends_with("_eq"))
                        return Error{path + "/regions/" + a.name,
                                     "homogenized material must end with _eq"};
                    warn_solder(a.material);
                    refs.insert("region:" + a.name);
                    refs.insert("body:" + a.body);
                    v.regions.push_back(a);
                }
            model.layers.push_back(v);
        }
        if (j.contains("activation_groups"))
            for (const auto& g : j.at("activation_groups").array_range()) {
                auto name = g.at("name").as<std::string>();
                if (name == "void")
                    return Error{"$/activation_groups/" + name, "void is a reserved name"};
                if (!unique(name))
                    return Error{"$/activation_groups/" + name, "duplicate name"};
                for (const auto& m : g.at("members").array_range()) {
                    auto ref = m.as<std::string>();
                    bool exists = refs.contains(ref);
                    if (ref.starts_with("interface:")) {
                        auto p = ref.find('|');
                        exists = p != std::string::npos &&
                                 refs.contains("layer:" + ref.substr(10, p - 10)) &&
                                 refs.contains("layer:" + ref.substr(p + 1));
                    }
                    if (!exists)
                        return Error{"$/activation_groups/" + name + "/members",
                                     "missing target: " + ref};
                    model.activation_groups[name].push_back(ref);
                }
            }
        if (j.contains("external_refs"))
            for (const auto& r : j.at("external_refs").array_range()) {
                auto relative = std::filesystem::path(r.at("path").as<std::string>());
                auto root = std::filesystem::weakly_canonical(file.parent_path());
                auto target = std::filesystem::weakly_canonical(root / relative);
                auto rel = target.lexically_relative(root);
                if (relative.is_absolute() || rel.empty() || *rel.begin() == "..")
                    return Error{"$/external_refs/path", "path escapes case root"};
                std::ifstream bytes(target, std::ios::binary);
                if (!bytes)
                    return Error{"$/external_refs/path", "file missing"};
                std::string data((std::istreambuf_iterator<char>(bytes)), {});
                auto hash = r.at("sha256").as<std::string>();
                for (auto& c : hash)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (sha256(data) != hash)
                    return Error{"$/external_refs/sha256", "digest mismatch"};
            }
        return model;
    } catch (const std::exception& e) {
        return Error{"$", e.what()};
    }
}
} // namespace chipcae::mesh
