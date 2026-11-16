#include "chipcae/mesh/background/activation.hpp"

#include "../../cut/include/chipcae/mesh/cut/cut_layer.hpp"

#include <algorithm>
#include <set>
namespace chipcae::mesh {
std::uint64_t mask_hash(std::vector<Index> mask) {
    std::sort(mask.begin(), mask.end());
    mask.erase(std::unique(mask.begin(), mask.end()), mask.end());
    std::uint64_t h = 14695981039346656037ull;
    for (auto a : mask)
        for (unsigned b = 0; b < 8; ++b) {
            h ^= (static_cast<std::uint64_t>(a) >> (8 * b)) & 255;
            h *= 1099511628211ull;
        }
    return h;
}
std::vector<Index> activated_gids(const HexMeshArrays& m, const ActivationMask& mask) {
    std::vector<Index> ids;
    for (std::size_t e = 0; e < m.attribute.size(); ++e)
        if (std::find(mask.begin(), mask.end(), m.attribute[e]) != mask.end())
            ids.push_back(m.elem_gid[e]);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}
std::string canonical_interface(std::string a, std::string b) {
    if (a == "void" || (b != "void" && b < a))
        std::swap(a, b);
    return "interface:" + a + "|" + b;
}
Expected<std::vector<BodyId>> resolve_interface_side(const HexMeshArrays& m,
                                                     const std::string& ref) {
    if (ref == "void")
        return std::vector<BodyId>{};
    auto name = ref.find(':') == std::string::npos ? "layer:" + ref : ref;
    if (!name.starts_with("layer:") && !name.starts_with("region:") && !name.starts_with("body:"))
        return Error{ref, "invalid interface side"};
    std::set<BodyId> ids;
    auto it = m.attr_table.references.find(name);
    if (it == m.attr_table.references.end())
        return Error{ref, "unknown interface side"};
    for (auto a : it->second) {
        const auto& r = m.attr_table.entries[a - 1];
        if (!name.starts_with("layer:") || r.region.empty())
            ids.insert(r.body);
    }
    if (ids.empty())
        return Error{ref, "empty interface side"};
    return std::vector<BodyId>(ids.begin(), ids.end());
}
Expected<ActivationResolution> resolve_volume_ref(const StackModel& s,
                                                  const HexMeshArrays& m,
                                                  const cut::CutLayerArrays* cut,
                                                  const std::vector<std::string>& refs) {
    ActivationResolution out;
    for (const auto& ref : refs) {
        if (!out.cells.ref.empty())
            out.cells.ref += ",";
        out.cells.ref += ref;
    }
    out.faces.ref = out.cells.ref;
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
            if (ref.starts_with("interface:")) {
                auto sep = ref.find('|');
                if (sep == std::string::npos)
                    return Error{ref, "two interface sides required"};
                auto a = resolve_interface_side(m, ref.substr(10, sep - 10)),
                     b = resolve_interface_side(m, ref.substr(sep + 1));
                if (!a)
                    return a.error();
                if (!b)
                    return b.error();
                if (a.value().empty() && b.value().empty())
                    return Error{ref, "void|void is invalid"};
                for (auto id : a.value())
                    if (std::find(b.value().begin(), b.value().end(), id) != b.value().end())
                        return Error{ref, "overlapping interface body sets"};
                bool found = false;
                auto names = [&](const auto& ids) {
                    std::vector<std::string> n;
                    if (ids.empty())
                        n.push_back("void");
                    for (const auto& [name, id] : m.attr_table.bodies)
                        if (std::find(ids.begin(), ids.end(), id) != ids.end())
                            n.push_back("body:" + name);
                    return n;
                };
                for (const auto& an : names(a.value()))
                    for (const auto& bn : names(b.value())) {
                        auto it = m.face_sets.find(canonical_interface(an, bn));
                        if (cut) {
                            auto ci = cut->face_sets.find(canonical_interface(an, bn));
                            if (ci != cut->face_sets.end()) {
                                found = true;
                                faces.insert(ci->second.begin(), ci->second.end());
                            }
                        }
                        if (it != m.face_sets.end()) {
                            found = true;
                            faces.insert(it->second.begin(), it->second.end());
                        }
                    }
                if (!found)
                    return Error{ref, "no shared patches generated"};
            } else {
                auto it = m.face_sets.find(ref);
                if (it == m.face_sets.end())
                    return Error{ref, "unknown face set"};
                faces.insert(it->second.begin(), it->second.end());
            }
            continue;
        }
        if (!ref.starts_with("layer:") && !ref.starts_with("region:") && !ref.starts_with("body:"))
            return Error{ref, "unsupported reference prefix"};
        std::set<Local> matched;
        bool found = false;
        auto star = ref.find('*');
        for (const auto& [name, list] : m.attr_table.references) {
            bool match = name == ref;
            if (star != std::string::npos)
                match = name.starts_with(ref.substr(0, star)) &&
                        name.ends_with(ref.substr(star + 1)) && name.size() + 1 >= ref.size();
            if (match) {
                found = true;
                matched.insert(list.begin(), list.end());
            }
        }
        if (!found || matched.empty())
            return Error{ref, "unknown target or empty wildcard"};
        attrs.insert(matched.begin(), matched.end());
    }
    out.cells.attributes.assign(attrs.begin(), attrs.end());
    out.faces.bdr_attributes.assign(faces.begin(), faces.end());
    if (cut)
        for (std::size_t sub = 0; sub < cut->sub_attribute.size(); ++sub)
            for (Local q = cut->bq_off[sub]; q < cut->bq_off[sub + 1]; ++q)
                if (faces.contains(cut->bq_bdr_attr[q])) {
                    out.faces.eb_subs.push_back(static_cast<Local>(sub));
                    break;
                }
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
    if (cut)
        for (std::size_t i = 0; i < cut->sub_attribute.size(); ++i)
            if (attrs.contains(cut->sub_attribute[i])) {
                out.cells.cut_subs.push_back(static_cast<Local>(i));
                auto e = cut->cut_elem[i];
                Real v = 1;
                for (int k = 0; k < 3; ++k)
                    v *= m.vertices[3 * m.hex[e * 8 + 6] + k] - m.vertices[3 * m.hex[e * 8] + k];
                out.cells.volume -= v * (1 - cut->sub_volfrac[i]);
            }
    out.cells.n_elem_global = static_cast<Index>(out.cells.elems.size());
    return out;
}
Expected<ActivationResolution> resolve_activation(const StackModel& s,
                                                  const HexMeshArrays& m,
                                                  const cut::CutLayerArrays* c,
                                                  const std::vector<std::string>& refs) {
    return resolve_volume_ref(s, m, c, refs);
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
            if (retired.contains(id) || std::find(d.value().cells.attributes.begin(),
                                                  d.value().cells.attributes.end(),
                                                  id) != d.value().cells.attributes.end())
                return Error{"steps", "reactivation of retired attribute " + std::to_string(id)};
        retired.insert(d.value().cells.attributes.begin(), d.value().cells.attributes.end());
    }
    return true;
}
} // namespace chipcae::mesh
