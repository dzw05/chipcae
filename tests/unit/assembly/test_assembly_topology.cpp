#include "assembly_test_support.hpp"

int main() {
    return run([] {
        const auto outer = rect(0, 0, .01, .01).outer;
        auto reject = [&](SlabSection section) {
            auto p = part("invalid");
            p.slabs[0].sections = {section};
            invalid_input(place(p.slabs, FramePlacement{}, base()));
            invalid_input(merge_parts(base(), {p}, options()));
        };
        reject({outer, {rect(.012, .002, .002, .002).outer}}); // Outside hole.
        reject({outer, {rect(.009, .002, .002, .002).outer}}); // Crossing outer.
        reject({outer, {rect(0, .002, .002, .002).outer}});    // Shared boundary.
        reject({outer,
                {rect(.002, .002, .004, .004).outer,
                 rect(.004, .004, .004, .004).outer}}); // Overlapping holes.
        reject({outer,
                {rect(.002, .002, .002, .002).outer,
                 rect(.002, .004, .002, .002).outer}}); // Touching holes.
        reject({outer,
                {rect(.002, .002, .006, .006).outer,
                 rect(.003, .003, .002, .002).outer}}); // Nested holes.
        Ring crossing{{{0, 0}, {.008, .008}, {0, .008}, {.006, 0}}};
        check(area(crossing.pts) != 0, "self crossing regression has nonzero signed area");
        reject({crossing, {}});
        reject({outer, {crossing}});
        Ring backtrack{{{0, 0}, {.008, 0}, {.004, 0}, {.004, .008}, {0, .008}}};
        reject({backtrack, {}});

        // Redundant closure, duplicate vertices and collinear forward edges remain valid.
        auto p = part("valid");
        p.slabs[0].sections = {{outer, {rect(.002, .002, .002, .002).outer}}};
        auto expected = merge(base(), {p});
        auto& pts = p.slabs[0].sections[0].outer.pts;
        pts.insert(pts.begin() + 1, {.005, 0});
        pts.insert(pts.begin(), pts.front());
        pts.push_back(pts.front());
        std::reverse(pts.begin(), pts.end());
        same_regions(expected, merge(base(), {p}));

        // Disjoint holes can touch after roundoff coalescing; no zero-height strip may escape.
        p.slabs[0].sections = {{outer,
                                {rect(.002, .002, .002, .002).outer,
                                 rect(.002, std::nextafter(.004, 1.), .002, .002).outer}}};
        auto touching = merge(base(), {p});
        check(touching.defects.empty(), "roundoff-touching holes remain supported");
        double material_area = 0;
        for (const auto& layer : touching.stack.layers)
            for (const auto& region : layer.regions) {
                const auto& box = region.footprint.box;
                material_area += (box[2] - box[0]) * (box[3] - box[1]);
            }
        near(material_area, .01 * .01 - 2 * .002 * .002, 1e-16);

        // A sliver below roundoff at its local extent collapses during normalization.
        p = part("collapsed");
        p.slabs[0].sections = {rect(0, 0, .01, 1e-18)};
        const auto& tiny = p.slabs[0].sections[0].outer.pts;
        check((tiny[1][0] - tiny[0][0]) * (tiny[2][1] - tiny[0][1]) > 0,
              "positive pre-normalization area");
        auto result = merge_parts(base(), {p}, options());
        check(bool(result), "collapsed section is reported as unsupported");
        check(result.value().defects.size() == 1, "one collapsed-section defect");
        const auto& d = result.value().defects.front();
        check(d.tier == Tier::C && d.reason == DefectReason::PART_NOT_PRISMATIC &&
                  d.defect_id == "collapsed/cut_unsupported#0" &&
                  d.details == "cut_unsupported: degenerate section",
              "deterministic degenerate diagnostic");
        check(result.value().stack.layers.size() == 1 &&
                  result.value().stack.layers.front().regions.empty(),
              "collapsed section emits no geometry");
    });
}
