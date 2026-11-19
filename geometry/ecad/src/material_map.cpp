#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace chipcae::geometry::ecad {
namespace {
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
} // namespace

bool apply_material_map(EcadModel& model, const std::filesystem::path& file, std::string* error) {
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
    std::map<std::string, std::string> mapping, bases;
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

    auto folded = [](std::string s) {
        for (char& c : s)
            if (c >= 'a' && c <= 'z')
                c -= 'a' - 'A';
        return s;
    };
    auto quoted = [](const std::vector<std::string>& keys) {
        std::string result;
        for (const auto& key : keys) {
            if (!result.empty())
                result += ", ";
            result += "\"" + key + "\"";
        }
        return result;
    };
    std::vector<std::string> layer_errors(model.layers.size());
    std::vector<bool> mapped(model.layers.size(), false);
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        auto& layer = model.layers[i];
        std::vector<std::string> tried, conflicts;
        auto found = mapping.end();
        if (!layer.material.empty()) {
            tried.push_back(layer.material);
            found = mapping.find(layer.material);
            if (found == mapping.end()) {
                for (auto it = mapping.begin(); it != mapping.end(); ++it)
                    if (folded(it->first) == folded(layer.material)) {
                        conflicts.push_back(it->first);
                        found = it;
                    }
                if (conflicts.size() != 1)
                    found = mapping.end();
            }
        }
        if (found == mapping.end()) {
            tried.push_back("layer:" + layer.name);
            found = mapping.find(tried.back());
        }
        if (found == mapping.end() && layer.copper) {
            tried.push_back("copper");
            found = mapping.find(tried.back());
        }
        if (found != mapping.end()) {
            (layer.copper ? layer.metal : layer.material) = found->second;
            mapped[i] = true;
            continue;
        }
        std::string entry = layer.name + " (tried: " + quoted(tried);
        if (conflicts.size() > 1)
            entry += "; case-conflict: " + quoted(conflicts);
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
} // namespace chipcae::geometry::ecad
