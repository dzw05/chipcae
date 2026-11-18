#include "region_cut_support.hpp"

#include <map>
using namespace sp5b;
namespace {
// Analytic inequalities for the shared octagonal footprint, independent of phi
// and of the region_cut_planes implementation.
bool in_footprint(double x, double y) {
    return x >= 2 && x <= 6 && y >= 2 && y <= 6 && x + y >= 5 && x + y <= 11 && x - y <= 3 &&
           y - x <= 3;
}
using Ownership = std::vector<std::pair<chipcae::Index, BodyId>>;
Ownership ownership(const HexMeshArrays& m) {
    Ownership result;
    std::array<std::size_t, 2> count{};
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
        const auto& attr = m.attr_table.entries.at(static_cast<std::size_t>(m.attribute[e] - 1));
        const double z = center(m, e)[2];
        require((z > 0 && z < 1) || (z > 1 && z < 2), "retained cell lies inside one layer");
        const bool lower = z < 1;
        const auto expected = m.attr_table.bodies.at(lower ? "lower-body" : "upper-body");
        require(attr.body == expected && attr.layer == (lower ? "L#0" : "L#1"),
                "each retained cell belongs to its physical layer's body");
        for (int v = 0; v < 8; ++v) {
            const double vertex_z = m.vertices[3 * m.hex[8 * e + v] + 2];
            require(lower ? vertex_z >= 0 && vertex_z <= 1 : vertex_z >= 1 && vertex_z <= 2,
                    "retained cell never crosses the shared layer interface");
        }
        ++count[lower ? 0 : 1];
        result.emplace_back(m.elem_gid[e], attr.body);
    }
    require(count[0] > 0 && count[1] > 0, "both touching bodies retain cells");
    return result;
}
} // namespace
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].name = "L#0";
        auto lower = square("lower-region", 2, 2, 6, 6);
        lower.body = "lower-body";
        lower.footprint.polygon = {{2, 3}, {3, 2}, {5, 2}, {6, 3}, {6, 5}, {5, 6}, {3, 6}, {2, 5}};
        s.layers[0].regions = {lower};
        auto upper = s.layers[0];
        upper.name = "L#1";
        upper.regions[0].name = "upper-region";
        upper.regions[0].body = "upper-body";
        s.layers.push_back(upper);
        auto with_ls = grid(s, 8);
        auto without_ls = with_ls;
        const auto region_phi = region_level_sets(s, with_ls, {});
        require(region_phi.size() == 2, "two bodies have separate region phi entries");
        require(region_phi[0].body == with_ls.attr_table.bodies.at("lower-body") &&
                    region_phi[1].body == with_ls.attr_table.bodies.at("upper-body"),
                "region phi entries use the registered, ordered body IDs");
        std::size_t interface_samples = 0;
        std::array<std::size_t, 2> interior_samples{};
        for (const auto& ls : region_phi)
            require(ls.phi.size() == with_ls.node_gid.size(), "region phi covers all mesh nodes");
        for (std::size_t n = 0; n < with_ls.node_gid.size(); ++n) {
            const double x = with_ls.vertices[3 * n], y = with_ls.vertices[3 * n + 1],
                         z = with_ls.vertices[3 * n + 2];
            if (!in_footprint(x, y))
                continue;
            if (z == 1) {
                ++interface_samples;
                require(region_phi[0].phi[n] <= 0 && region_phi[1].phi[n] <= 0,
                        "physical shared footprint interface is closed in both body entries");
            }
            for (const auto& ls : region_phi) {
                const bool lower_body = ls.body == with_ls.attr_table.bodies.at("lower-body");
                const bool other_interior = lower_body ? z > 1 && z < 2 : z > 0 && z < 1;
                if (other_interior) {
                    ++interior_samples[lower_body ? 0 : 1];
                    require(std::isinf(ls.phi[n]) && ls.phi[n] > 0,
                            "region phi is positive infinity strictly inside the other layer");
                }
            }
        }
        require(interface_samples > 0 && interior_samples[0] > 0 && interior_samples[1] > 0,
                "shared-interface and both layer-interior checks have actual samples");
        const auto planes_ls = geometry(s, with_ls, "LS-first plane extraction");
        const auto planes_plain = geometry(s, without_ls, "plane extraction without region phi");
        require(planes_ls.planes.size() == 16, "both eight-edge bodies emit nonempty geometry");
        exact_planes(planes_ls, planes_plain);
        CutOptions options;
        auto cut_ls = build_cut_layer(with_ls, planes_ls, options);
        auto cut_plain = build_cut_layer(without_ls, planes_plain, options);
        require(bool(cut_ls) && bool(cut_plain), "both real plane-cut pipelines succeed");
        const auto& a = cut_ls.value();
        const auto& b = cut_plain.value();
        require(!a.cut_elem.empty() && !a.sub_attribute.empty(),
                "slanted edges exercise actual cuts");
        require(with_ls.elem_gid == without_ls.elem_gid,
                "retained GIDs are independent of region phi");
        require(ownership(with_ls) == ownership(without_ls),
                "identical (elem_gid, BodyId) ownership");
        require(a.cut_elem == b.cut_elem && a.cut_elem_gid == b.cut_elem_gid &&
                    a.sub_off == b.sub_off && a.sub_attribute == b.sub_attribute &&
                    with_ls.attribute == without_ls.attribute,
                "cut and subattribute mappings are independent of region phi");
        require(a.level_sets.size() == 2, "cutter emits plane-generated level sets");
        for (const auto& ls : a.level_sets) {
            require(ls.phi.size() == with_ls.node_gid.size(),
                    "cut phi follows retained mesh nodes");
            bool checked_other_layer = false;
            const bool lower_body = ls.body == with_ls.attr_table.bodies.at("lower-body");
            for (std::size_t n = 0; n < ls.phi.size(); ++n) {
                const double z = with_ls.vertices[3 * n + 2];
                if (lower_body ? z > 1 && z < 2 : z > 0 && z < 1) {
                    checked_other_layer = true;
                    require(
                        std::isfinite(ls.phi[n]),
                        "plane-generated phi is not overwritten by layer-restricted region phi");
                }
            }
            require(checked_other_layer, "plane-phi preservation checks sample the other layer");
        }
    });
}
