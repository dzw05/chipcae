#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace chipcae::geometry::ecad {
namespace {
// Matches material/src/schema.cpp:295-302.
bool valid_material_id(std::string_view id) {
    if (id.empty())
        return false;
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}
// Matches schema/src/stack.cpp:149-150 for Homogenized materials.
bool missing_homogenized_suffix(std::string_view material) {
    return !material.ends_with("_eq");
}
bool parse_string(const std::string& s, std::size_t& p, std::string& out) {
    if (p >= s.size() || s[p++] != '"')
        return false;
    out.clear();
    while (p < s.size()) {
        const char c = s[p++];
        if (c == '"')
            return true;
        if (c == '\\') {
            if (p == s.size())
                return false;
            const char escaped = s[p++];
            if (escaped != '"' && escaped != '\\' && escaped != '/')
                return false;
            out.push_back(escaped);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            return false;
        } else {
            out.push_back(c);
        }
    }
    return false;
}
void whitespace(const std::string& s, std::size_t& p) {
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
        ++p;
}
struct MaterialMap {
    std::map<std::string, std::string> mapping, bases;
};
bool read_material_map(const std::filesystem::path& file, MaterialMap& map, std::string* error) {
    auto fail = [&](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };
    std::ifstream input(file);
    if (!input)
        return fail("MATERIAL_MAPPING_MISSING: cannot open material map");
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    std::size_t p = 0;
    whitespace(text, p);
    if (p == text.size() || text[p++] != '{')
        return fail("invalid material map: expected JSON object");
    auto& mapping = map.mapping;
    auto& bases = map.bases;
    whitespace(text, p);
    while (p < text.size() && text[p] != '}') {
        std::string name, id;
        if (!parse_string(text, p, name))
            return fail("invalid material map: expected string key");
        whitespace(text, p);
        if (p == text.size() || text[p++] != ':')
            return fail("invalid material map: expected ':'");
        whitespace(text, p);
        if (!parse_string(text, p, id) || name.empty() || id.empty())
            return fail("invalid material map: material names and IDs must be nonempty strings");
        // Reserved base keys never participate in source-material lookup. Repeated
        // reserved keys use the last value; ordinary duplicate keys remain errors.
        if (name == "copper_base" || name.starts_with("copper_base@"))
            bases.insert_or_assign(std::move(name), std::move(id));
        else if (!mapping.emplace(std::move(name), std::move(id)).second)
            return fail("invalid material map: duplicate material name");
        whitespace(text, p);
        if (p < text.size() && text[p] == ',') {
            ++p;
            whitespace(text, p);
        } else {
            break;
        }
    }
    if (p == text.size() || text[p++] != '}')
        return fail("invalid material map: expected '}'");
    whitespace(text, p);
    if (p != text.size())
        return fail("invalid material map: trailing data");

    return true;
}
std::string folded(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
    return s;
}
std::string quoted(const std::vector<std::string>& keys) {
    std::string result;
    for (const auto& key : keys) {
        if (!result.empty())
            result += ", ";
        result += "\"" + key + "\"";
    }
    return result;
}
struct MaterialLookup {
    const std::string* id = nullptr;
    std::vector<std::string> tried, conflicts;
};
MaterialLookup
lookup_material(const EcadLayer& layer, const MaterialMap& map, bool copper_fallback) {
    MaterialLookup result;
    const auto& mapping = map.mapping;
    auto found = mapping.end();
    if (!layer.material.empty()) {
        result.tried.push_back(layer.material);
        found = mapping.find(layer.material);
        if (found == mapping.end()) {
            const auto source = folded(layer.material);
            for (auto it = mapping.begin(); it != mapping.end(); ++it)
                if (folded(it->first) == source) {
                    result.conflicts.push_back(it->first);
                    found = it;
                }
            if (result.conflicts.size() != 1)
                found = mapping.end();
        }
    }
    if (found == mapping.end()) {
        result.tried.push_back("layer:" + layer.name);
        found = mapping.find(result.tried.back());
    }
    if (found == mapping.end() && copper_fallback) {
        result.tried.push_back("copper");
        found = mapping.find(result.tried.back());
    }
    if (found != mapping.end())
        result.id = &found->second;
    return result;
}
} // namespace

bool restore_solder_masks(EcadModel& model,
                          const std::filesystem::path& material_map,
                          std::string* error) {
    MaterialMap map;
    if (!read_material_map(material_map, map, error))
        return false;

    std::unordered_map<std::string, std::size_t> existing;
    for (std::size_t i = 0; i < model.layers.size(); ++i)
        existing.emplace(model.layers[i].name, i);
    std::unordered_set<std::string> restored;
    for (const auto& mask : model.omitted_masks)
        if (!existing.contains(mask.layer.name) && std::isfinite(mask.layer.thickness_m) &&
            mask.layer.thickness_m > 0 && lookup_material(mask.layer, map, false).id)
            restored.insert("SolderMask:" + mask.layer.name);
    if (restored.empty())
        return true;

    // The reader emits an omission record for every captured mask. It remains
    // until restoration, including when a preexisting layer has the same name.
    std::unordered_set<std::string> pending;
    for (const auto& entry : model.report.unsupported)
        pending.insert(entry.element);
    // Include masks restored by earlier calls so original capture positions remain
    // meaningful when a later map selects additional masks, including equal slots.
    struct Placement {
        const OmittedMask* mask;
        std::size_t existing;
    };
    std::vector<Placement> placements;
    std::vector<bool> placed(model.layers.size(), false);
    std::unordered_set<std::string> seen;
    for (const auto& mask : model.omitted_masks) {
        const auto found = existing.find(mask.layer.name);
        const auto element = "SolderMask:" + mask.layer.name;
        const bool previously_restored =
            found != existing.end() && !pending.contains(element) &&
            !model.layers[found->second].copper &&
            folded(model.layers[found->second].function) == "SOLDERMASK";
        if ((previously_restored || restored.contains(element)) &&
            seen.insert(mask.layer.name).second) {
            const auto index = found == existing.end() ? model.layers.size() : found->second;
            placements.push_back({&mask, index});
            if (index < placed.size())
                placed[index] = true;
        }
    }
    // Merge in insertion-slot order, retaining capture order for equal slots.
    std::stable_sort(placements.begin(), placements.end(), [](const auto& a, const auto& b) {
        return a.mask->position < b.mask->position;
    });
    std::vector<EcadLayer> layers;
    layers.reserve(model.layers.size() + restored.size());
    std::vector<std::size_t> remap(model.layers.size());
    auto append_mask = [&](const Placement& entry) {
        if (entry.existing < model.layers.size()) {
            remap[entry.existing] = layers.size();
            layers.push_back(std::move(model.layers[entry.existing]));
        } else {
            layers.push_back(entry.mask->layer);
            layers.back().copper = false;
            layers.back().polygons.clear();
        }
    };
    std::size_t position = 0, next = 0;
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        if (placed[i])
            continue;
        while (next < placements.size() && placements[next].mask->position <= position)
            append_mask(placements[next++]);
        remap[i] = layers.size();
        layers.push_back(std::move(model.layers[i]));
        ++position;
    }
    while (next < placements.size())
        append_mask(placements[next++]);
    model.layers = std::move(layers);
    // Insertion changes only indices of existing via data, never its geometry,
    // fractions, crossed layers or diagnostics.
    for (auto& via : model.vias)
        for (auto& index : via.dielectric_layers)
            if (index < remap.size())
                index = remap[index];
    for (auto& cell : model.via_fractions)
        if (cell.layer < remap.size())
            cell.layer = remap[cell.layer];
    std::erase_if(model.report.unsupported,
                  [&](const auto& entry) { return restored.contains(entry.element); });
    return true;
}

bool apply_material_map(EcadModel& model, const std::filesystem::path& file, std::string* error) {
    auto fail = [&](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };
    MaterialMap map;
    if (!read_material_map(file, map, error))
        return false;
    const auto& bases = map.bases;
    std::vector<std::string> layer_errors(model.layers.size());
    std::vector<bool> mapped(model.layers.size(), false);
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        auto& layer = model.layers[i];
        const auto lookup = lookup_material(layer, map, layer.copper);
        if (lookup.id) {
            (layer.copper ? layer.metal : layer.material) = *lookup.id;
            mapped[i] = true;
            continue;
        }
        std::string entry = layer.name + " (tried: " + quoted(lookup.tried);
        if (lookup.conflicts.size() > 1)
            entry += "; case-conflict: " + quoted(lookup.conflicts);
        layer_errors[i] = entry + ")";
    }
    // Resolve bases only after every dielectric has its mapped ID. Scan original
    // indices, including failed dielectrics; ties select the larger (lower) index.
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        auto& layer = model.layers[i];
        if (!layer.copper)
            continue;
        layer.material.clear();
        std::vector<std::string> tried{"copper_base@" + layer.name};
        auto base = bases.find(tried.back());
        if (base != bases.end()) {
            layer.material = base->second;
            continue;
        }
        std::size_t nearest = model.layers.size(), distance = model.layers.size();
        for (std::size_t j = 0; j < model.layers.size(); ++j) {
            if (model.layers[j].copper)
                continue;
            const auto d = i > j ? i - j : j - i;
            if (d <= distance) {
                nearest = j;
                distance = d;
            }
        }
        if (nearest != model.layers.size()) {
            tried.push_back("layer:" + model.layers[nearest].name);
            if (mapped[nearest]) {
                layer.material = model.layers[nearest].material;
                continue;
            }
        }
        // A failed neighbour never supplies its source name. Global fallback can
        // supply the base, but that neighbour still makes the entire map fail.
        tried.push_back("copper_base");
        base = bases.find(tried.back());
        if (base != bases.end())
            layer.material = base->second;
        else if (layer_errors[i].empty())
            layer_errors[i] = layer.name + " (tried: " + quoted(tried) + ")";
    }
    std::vector<std::string> missing;
    for (auto& entry : layer_errors)
        if (!entry.empty())
            missing.push_back(std::move(entry));
    if (!missing.empty()) {
        std::string message =
            "MATERIAL_MAPPING_MISSING: " + std::to_string(missing.size()) + " layer(s) unmapped: ";
        for (std::size_t i = 0; i < missing.size(); ++i)
            message += (i ? "; " : "") + missing[i];
        return fail(message);
    }
    return true;
}
bool validate_mapped_targets(const EcadModel& model, std::string* error) {
    auto fail = [&](const EcadLayer& layer,
                    std::string_view field,
                    const std::string& id,
                    std::string_view reason) {
        if (error)
            *error = "layer '" + layer.name + "' " + std::string(field) + " '" + id +
                     "': " + std::string(reason);
        return false;
    };
    constexpr std::string_view id_rule = "must match ^[a-z0-9_-]+$ (lowercase letters, digits, '_' "
                                         "or '-'; no dots, e.g. cu, fr4_eq)";
    for (const auto& layer : model.layers) {
        if ((!layer.copper || !layer.material.empty()) && !valid_material_id(layer.material))
            return fail(layer, "material", layer.material, id_rule);
        if (layer.copper && !layer.metal.empty() && !valid_material_id(layer.metal))
            return fail(layer, "metal", layer.metal, id_rule);
    }
    for (const auto& layer : model.layers)
        if (!layer.copper && missing_homogenized_suffix(layer.material))
            return fail(
                layer, "material", layer.material, "homogenized material must end with _eq");
    return true;
}
} // namespace chipcae::geometry::ecad
