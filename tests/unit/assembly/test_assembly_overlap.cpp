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
        check(!defect(merge(base(), {a, b}), DefectReason::PART_OVERLAP),
              "same material no overlap defect");
        a.slabs[0].sections = {rect(.019, .019)};
        check(defect(merge(base(), {a}), DefectReason::PART_OUTSIDE_OUTLINE), "outside outline");
    });
}
