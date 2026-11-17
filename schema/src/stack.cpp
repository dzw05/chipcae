#include "chipcae/schema/stack.hpp"

#include "sha256.hpp"

#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <set>
#include <sstream>
#include <type_traits>
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
template <class UInt>
bool read_le(std::istream& in, UInt& value) {
    static_assert(std::is_unsigned_v<UInt>);
    value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        const int byte = in.get();
        if (byte == std::char_traits<char>::eof())
            return false;
        value |= static_cast<UInt>(static_cast<unsigned char>(byte)) << (8 * i);
    }
    return true;
}
template <class UInt>
void write_le(std::ostream& out, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i)
        out.put(static_cast<char>((value >> (8 * i)) & 0xff));
}
bool read_i64(std::istream& in, std::int64_t& value) {
    std::uint64_t bits;
    if (!read_le(in, bits))
        return false;
    value = std::bit_cast<std::int64_t>(bits);
    return true;
}
void write_i64(std::ostream& out, std::int64_t value) {
    write_le(out, std::bit_cast<std::uint64_t>(value));
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
        std::set<std::string> names, refs, external_paths;
        if (j.contains("external_refs"))
            for (const auto& r : j.at("external_refs").array_range())
                external_paths.insert(r.at("path").as<std::string>());
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
            if (l.contains("copper")) {
                const auto& c = l.at("copper");
                CopperSpec copper;
                copper.metal = c.at("metal").as<std::string>();
                if (c.contains("fraction"))
                    copper.fraction = c.at("fraction").as<Real>();
                if (c.contains("polygons")) {
                    CopperPolygons polygons{c.at("polygons").at("file").as<std::string>(),
                                            c.at("polygons").at("layer").as<std::string>()};
                    if (!external_paths.contains(polygons.file))
                        return Error{path + "/copper/polygons/file",
                                     "polyset file must be listed in external_refs"};
                    copper.polygons = std::move(polygons);
                }
                if (v.kind != "Patterned")
                    return Error{path + "/copper", "copper requires kind Patterned"};
                if (v.material.empty())
                    return Error{path + "/material", "copper layer requires a base material"};
                if (l.contains("regions"))
                    return Error{path + "/regions", "copper layer cannot have regions"};
                v.copper = std::move(copper);
            }
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

Expected<Polyset> read_polyset(const std::filesystem::path& file) {
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return Error{"$", "cannot open polyset"};
        char magic[8];
        in.read(magic, 8);
        if (!in || std::string(magic, 8) != "CCPOLY01")
            return Error{"$/magic", "invalid polyset magic"};
        std::uint64_t dbu_bits;
        std::uint32_t layer_count;
        if (!read_le(in, dbu_bits) || !read_le(in, layer_count))
            return Error{"$", "truncated polyset header"};
        Polyset result;
        result.dbu_m = std::bit_cast<double>(dbu_bits);
        if (!std::isfinite(result.dbu_m) || !(result.dbu_m > 0) || layer_count > 1000000)
            return Error{"$/header", "invalid dbu or layer count"};
        result.layers.reserve(layer_count);
        for (std::uint32_t i = 0; i < layer_count; ++i) {
            std::uint32_t name_size, ring_count;
            if (!read_le(in, name_size) || name_size > 1048576)
                return Error{"$/layers", "invalid layer name length"};
            PolysetLayer layer;
            layer.name.resize(name_size);
            in.read(layer.name.data(), static_cast<std::streamsize>(name_size));
            if (!in || !read_le(in, ring_count) || ring_count > 10000000)
                return Error{"$/layers", "truncated layer or invalid ring count"};
            layer.ring_offsets.resize(static_cast<std::size_t>(ring_count) + 1);
            for (std::size_t offset_index = 0; offset_index < layer.ring_offsets.size();
                 ++offset_index) {
                auto& offset = layer.ring_offsets[offset_index];
                if (!read_i64(in, offset) || offset < 0 ||
                    (offset_index > 0 && offset < layer.ring_offsets[offset_index - 1]))
                    return Error{"$/layers/ring_offsets", "invalid ring offsets"};
            }
            if (layer.ring_offsets.front() != 0)
                return Error{"$/layers/ring_offsets", "first offset must be zero"};
            for (std::size_t ring = 1; ring < layer.ring_offsets.size(); ++ring)
                if (layer.ring_offsets[ring] - layer.ring_offsets[ring - 1] < 3)
                    return Error{"$/layers/ring_offsets", "rings require at least three points"};
            const auto points = static_cast<std::uint64_t>(layer.ring_offsets.back());
            if (points > 100000000)
                return Error{"$/layers/coordinates", "coordinate count too large"};
            layer.coordinates.resize(static_cast<std::size_t>(points) * 2);
            for (auto& coordinate : layer.coordinates)
                if (!read_i64(in, coordinate))
                    return Error{"$/layers/coordinates", "truncated coordinate array"};
            result.layers.push_back(std::move(layer));
        }
        if (in.peek() != std::char_traits<char>::eof())
            return Error{"$", "trailing polyset data"};
        return result;
    } catch (const std::exception& e) {
        return Error{"$", e.what()};
    }
}

Expected<bool> write_polyset(const std::filesystem::path& file, const Polyset& polyset) {
    try {
        if (!std::isfinite(polyset.dbu_m) || !(polyset.dbu_m > 0) ||
            polyset.layers.size() > UINT32_MAX)
            return Error{"$/header", "invalid dbu or layer count"};
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
            return Error{"$", "cannot create polyset"};
        out.write("CCPOLY01", 8);
        write_le(out, std::bit_cast<std::uint64_t>(polyset.dbu_m));
        write_le(out, static_cast<std::uint32_t>(polyset.layers.size()));
        for (const auto& layer : polyset.layers) {
            if (layer.name.size() > UINT32_MAX || layer.ring_offsets.empty() ||
                layer.ring_offsets.size() > UINT32_MAX || layer.coordinates.size() % 2 != 0 ||
                layer.ring_offsets.front() != 0 || layer.ring_offsets.back() < 0 ||
                static_cast<std::uint64_t>(layer.ring_offsets.back()) * 2 !=
                    layer.coordinates.size())
                return Error{"$/layers", "invalid layer arrays"};
            for (std::size_t i = 1; i < layer.ring_offsets.size(); ++i)
                if (layer.ring_offsets[i] < layer.ring_offsets[i - 1] ||
                    layer.ring_offsets[i] - layer.ring_offsets[i - 1] < 3)
                    return Error{"$/layers/ring_offsets", "rings require at least three points"};
            write_le(out, static_cast<std::uint32_t>(layer.name.size()));
            out.write(layer.name.data(), static_cast<std::streamsize>(layer.name.size()));
            write_le(out, static_cast<std::uint32_t>(layer.ring_offsets.size() - 1));
            for (auto offset : layer.ring_offsets)
                write_i64(out, offset);
            for (auto coordinate : layer.coordinates)
                write_i64(out, coordinate);
        }
        if (!out)
            return Error{"$", "failed writing polyset"};
        return true;
    } catch (const std::exception& e) {
        return Error{"$", e.what()};
    }
}
} // namespace chipcae::mesh
