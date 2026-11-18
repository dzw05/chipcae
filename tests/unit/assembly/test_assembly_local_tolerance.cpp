#include "assembly_test_support.hpp"

int main() {
    return run([] {
        // A closer source interface must not steal an endpoint from the base.
        for (bool above : {false, true}) {
            const double start = above ? .001009 : .000991;
            auto neighbor = part("neighbor", above ? .001012 : .000988, .002);
            auto subject = part("subject", start, .002);
            for (bool reverse : {false, true}) {
                auto m = merge(base(),
                               reverse ? std::vector<PlacedPart>{subject, neighbor}
                                       : std::vector<PlacedPart>{neighbor, subject});
                double z = 0;
                bool found = false;
                for (const auto& l : m.stack.layers) {
                    for (const auto& r : l.regions)
                        if (r.body == "subject") {
                            if (!found)
                                check(z == .001, "subject starts at qualifying base boundary");
                            found = true;
                        }
                    z += l.thickness;
                }
                check(found, "aligned subject retains material");
            }
        }
        // Small valid rectangles and convex cuts survive large translations.
        for (bool triangle : {false, true}) {
            auto b = base();
            b.outline.box = {1e9 - 1, 1e9 - 1, 1e9 + 1, 1e9 + 1};
            auto p = part("remote");
            auto section = rect(1e9, 1e9, 1e-6, 1e-6);
            if (triangle)
                section.outer.pts.pop_back();
            p.slabs[0].sections = {section};
            auto m = merge_parts(b, {p}, options());
            check(bool(m) && m.value().defects.empty(), "remote small feature remains supported");
            const auto& regions = m.value().stack.layers.back().regions;
            check(regions.size() == 1, "remote small feature emits one region");
            check(regions[0].footprint.box == std::array{1e9, 1e9, 1e9 + 1e-6, 1e9 + 1e-6},
                  "remote small feature retains its bounds");
            check(regions[0].footprint.polygon.size() == (triangle ? 3 : 0),
                  "remote cut classification is preserved");
        }
        // The former degeneracy fixture is a valid square at its own local scale.
        {
            auto b = base();
            b.outline.box = {0, 0, 2, 2};
            auto p = part("tiny");
            p.slabs[0].sections = {rect(1, 1, 1e-15, 1e-15)};
            auto m = merge_parts(b, {p}, options());
            check(bool(m) && m.value().defects.empty(), "tiny translated square is valid");
            const auto& regions = m.value().stack.layers.back().regions;
            check(regions.size() == 1 && regions[0].footprint.polygon.empty() &&
                      regions[0].footprint.box == std::array{1., 1., 1. + 1e-15, 1. + 1e-15},
                  "tiny translated square retains its exact bounds");
        }
        // Distinct convex cuts below the former absolute threshold stay distinct.
        // Cyclic order and normalization roundoff still describe the same cut.
        for (double scale : {1e-6, .05, 1000.}) {
            auto b = base();
            b.outline.box = {-scale, -scale, 2 * scale, 2 * scale};
            SlabSection section{Ring{{{0, 0}, {scale, 0}, {0, scale}}}, {}};
            auto p = part("cuts");
            p.slabs[0].sections = {section};
            p.slabs.push_back({.002, .003, {section}});
            auto& repeated = p.slabs.back().sections[0].outer.pts;
            repeated[1][0] = std::nextafter(scale, 0.);
            std::reverse(repeated.begin(), repeated.end());
            std::rotate(repeated.begin(), repeated.begin() + 1, repeated.end());
            auto equivalent = merge_parts(b, {p}, options());
            check(bool(equivalent) && equivalent.value().defects.empty(),
                  "equivalent cuts use local scale tolerance");
            repeated = section.outer.pts;
            repeated[1][0] -= scale * 1e-10;
            auto distinct = merge_parts(b, {p}, options());
            check(bool(distinct), "distinct cut produces a defect, not an input error");
            check(distinct.value().defects.size() == 1 &&
                      distinct.value().defects[0].reason == DefectReason::PART_NOT_PRISMATIC &&
                      distinct.value().defects[0].defect_id == "cuts/cut_unsupported#1" &&
                      distinct.value().defects[0].details ==
                          "cut_unsupported: multiple distinct cut sections",
                  "distinct cuts cannot be hidden by an absolute coordinate tolerance");
        }
    });
}
