#include "chipcae/mesh/background/activation.hpp"

#include <algorithm>
#include <set>
namespace chipcae::mesh {
std::uint64_t mask_hash(ActivationMask mask) {
    std::sort(mask.begin(), mask.end());
    mask.erase(std::unique(mask.begin(), mask.end()), mask.end());
    std::uint64_t h = 14695981039346656037ull;
    for (auto a : mask)
        for (unsigned b = 0; b < 4; ++b) {
            h ^= (static_cast<std::uint32_t>(a) >> (8 * b)) & 255;
            h *= 1099511628211ull;
        }
    return h;
}
Expected<ActivationResolution> resolve_activation(const StackModel& s,
                                                  const HexMeshArrays& m,
                                                  const cut::CutLayerArrays* cut,
                                                  const std::vector<std::string>& refs) {
    // Cut subregion mapping is completed in K1-3 without introducing a solver dependency.
    if (cut)
        return Error{"activation", "cut mapping requires cut-layer resolver"};
    ActivationResolution out;
    std::set<Local> attrs, faces;
    std::vector<std::string> expanded;
    for (const auto& ref : refs) {
        if (ref.starts_with("group:")) {
            auto it = s.activation_groups.find(ref.substr(6));
            if (it == s.activation_groups.end())
                return Error{ref, "unknown group"};
            for (const auto& member : it->second) {
                if (member.starts_with("group:"))
                    return Error{ref, "nested group"};
                expanded.push_back(member);
            }
        } else
            expanded.push_back(ref);
    }
    for (const auto& ref : expanded) {
        if (ref.starts_with("faceset:") || ref.starts_with("interface:")) {
            auto it = m.face_sets.find(ref);
            if (it == m.face_sets.end())
                return Error{ref, "unknown face set"};
            faces.insert(it->second.begin(), it->second.end());
            continue;
        }
        bool found = false;
        auto star = ref.find('*');
        for (const auto& [name, list] : m.attr_table.references) {
            bool match = name == ref;
            if (star != std::string::npos)
                match = name.starts_with(ref.substr(0, star)) &&
                        name.ends_with(ref.substr(star + 1)) && name.size() + 1 >= ref.size();
            if (match) {
                found = true;
                attrs.insert(list.begin(), list.end());
            }
        }
        if (!found || (star != std::string::npos && attrs.empty()))
            return Error{ref, "unknown target or empty wildcard"};
    }
    out.cells.attributes.assign(attrs.begin(), attrs.end());
    out.faces.bdr_attributes.assign(faces.begin(), faces.end());
    std::set<BodyId> bodies;
    for (auto a : attrs)
        bodies.insert(m.attr_table.entries[static_cast<std::size_t>(a - 1)].body);
    out.cells.bodies.assign(bodies.begin(), bodies.end());
    for (std::size_t e = 0; e < m.attribute.size(); ++e)
        if (attrs.contains(m.attribute[e])) {
            out.cells.elems.push_back(static_cast<Local>(e));
            auto n0 = m.hex[8 * e], n6 = m.hex[8 * e + 6];
            Real v = 1;
            for (int a = 0; a < 3; ++a)
                v *= m.vertices[3 * n6 + a] - m.vertices[3 * n0 + a];
            out.cells.volume += v;
        }
    out.cells.n_elem_global = static_cast<Index>(out.cells.elems.size());
    return out;
}
Expected<bool> check_no_reactivation(const StackModel& s,
                                     const HexMeshArrays& m,
                                     const std::vector<ActivationStep>& steps) {
    std::set<Local> retired;
    for (const auto& step : steps) {
        auto a = resolve_activation(s, m, nullptr, step.activate);
        if (!a)
            return a.error();
        auto d = resolve_activation(s, m, nullptr, step.deactivate);
        if (!d)
            return d.error();
        for (auto id : a.value().cells.attributes)
            if (retired.contains(id))
                return Error{"steps", "reactivation of retired attribute " + std::to_string(id)};
        retired.insert(d.value().cells.attributes.begin(), d.value().cells.attributes.end());
    }
    return true;
}
} // namespace chipcae::mesh
