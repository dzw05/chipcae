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
    std::map<std::string, std::string> mapping;
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
        if (!mapping.emplace(std::move(name), std::move(id)).second)
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

    for (auto& layer : model.layers) {
        const std::string source =
            layer.material.empty() ? (layer.copper ? "copper" : "") : layer.material;
        if (source.empty())
            return fail("MATERIAL_MAPPING_MISSING: layer '" + layer.name +
                        "' has no ECAD material");
        const auto found = mapping.find(source);
        if (found == mapping.end())
            return fail("MATERIAL_MAPPING_MISSING: no mapping for ECAD material '" + source + "'");
        layer.material = found->second;
    }
    return true;
}
} // namespace chipcae::geometry::ecad
