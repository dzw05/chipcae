#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto a = part("A"), b = part("B");
        b.material = "other";
        for (bool reverse : {false, true}) {
            auto m = merge(base(),
                           reverse ? std::vector<PlacedPart>{b, a} : std::vector<PlacedPart>{a, b});
            check(defect(m, DefectReason::PART_OVERLAP), "different material overlap");
            std::string selected;
            for (auto it = m.stack.layers.back().regions.rbegin();
                 it != m.stack.layers.back().regions.rend();
                 ++it)
                if (covers(it->footprint, .003, .003)) {
                    selected = it->material;
                    break;
                }
            check(selected == (reverse ? a.material : b.material), "later covering region wins");
        }
        b.material = a.material;
        for (bool reverse : {false, true})
            check(!defect(merge(base(),
                                reverse ? std::vector<PlacedPart>{b, a}
                                        : std::vector<PlacedPart>{a, b}),
                          DefectReason::PART_OVERLAP),
                  "same material overlap between distinct parts is silent");
        a.slabs[0].sections = {rect(.019, .019)};
        check(defect(merge(base(), {a}), DefectReason::PART_OUTSIDE_OUTLINE), "outside outline");
        auto slight = part("slight");
        slight.slabs[0].sections = {rect(0, 0, 1, 1)};
        auto almost_inside = base();
        almost_inside.outline.box = {0, 0, std::nextafter(1.0, 0.0), 1};
        check(defect(merge(almost_inside, {slight}), DefectReason::PART_OUTSIDE_OUTLINE),
              "positive outside area below the former epsilon threshold is reported");
        // A bounding-box intersection alone must not count as polygon overlap.
        auto concave = part("concave");
        concave.slabs[0].sections = {{Ring{{{.002, .002},
                                            {.010, .002},
                                            {.010, .004},
                                            {.004, .004},
                                            {.004, .010},
                                            {.002, .010}}},
                                      {}}};
        auto probe = part("probe");
        probe.material = "probe-material";
        probe.slabs[0].sections = {rect(.006, .006, .002, .002)};
        check(!defect(merge(base(), {concave, probe}), DefectReason::PART_OVERLAP),
              "concave notch is empty");
        probe.slabs[0].sections = {rect(.003, .003, .002, .002)};
        check(defect(merge(base(), {concave, probe}), DefectReason::PART_OVERLAP),
              "concave polygon intersection detected");
        auto hollow = part("hollow");
        hollow.slabs[0].sections = {
            {rect(.002, .002, .012, .012).outer, {rect(.004, .004, .006, .006).outer}}};
        probe.slabs[0].sections = {rect(.005, .005, .002, .002)};
        check(!defect(merge(base(), {hollow, probe}), DefectReason::PART_OVERLAP),
              "hole interior has no material");
        auto polygon_base = base();
        polygon_base.outline.polygon = concave.slabs[0].sections[0].outer.pts;
        check(defect(merge(polygon_base, {probe}), DefectReason::PART_OUTSIDE_OUTLINE),
              "polygon outline uses actual area");
        auto seeded = base();
        seeded.layers[0].regions.push_back(
            {"seed", "different", "board", {{.002, .002, .006, .006}, {}}});
        check(
            defect(merge(seeded, {part("inside", .0005, .001)}, .001), DefectReason::PART_OVERLAP),
            "base inclusion overlap detected");
        seeded.layers[0].regions[0].material = "metal";
        check(
            !defect(merge(seeded, {part("inside", .0005, .001)}, .001), DefectReason::PART_OVERLAP),
            "same material base inclusion overlap is silent");
        auto tiny = part("tiny");
        tiny.material = "different";
        tiny.slabs[0].sections = {rect(.002, .002, 1e-9, 1e-9)};
        check(!defect(merge(base(), {part(), tiny}), DefectReason::PART_OVERLAP),
              "overlap below squared area tolerance is silent");
        auto plate = part("plate");
        plate.material = "plate-material";
        plate.slabs[0].sections = {rect(.002, .002, .012, .012)};
        auto threshold = options();
        threshold.tau_a = std::sqrt(.0001);
        auto fragmented = merge_parts(base(), {plate, hollow}, threshold);
        check(bool(fragmented) && defect(fragmented.value(), DefectReason::PART_OVERLAP),
              "sum bridge fragment areas before applying overlap threshold");
    });
}
