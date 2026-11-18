#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        b.layers[0].thickness = .001;
        for (int i = 0; i < 2; ++i) {
            auto l = b.layers[0];
            l.name = "Thin" + std::to_string(i);
            l.thickness = 4e-6;
            b.layers.push_back(l);
        }
        auto p = part("ring", 0, .001008);
        p.slabs[0].sections = {ell()};
        auto bond = part("ring.bond", .001, .001004);
        auto r = merge_parts(b, {p, bond}, options());
        check(bool(r), "thin interval merge succeeds");
        std::vector<std::string> ids;
        for (auto& d : r.value().defects)
            if (d.reason == DefectReason::THIN_LAYER_MERGED) {
                check(d.tier == Tier::B, "thin tier B");
                ids.push_back(d.defect_id);
                const std::regex number(R"([-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)");
                bool original = false;
                size_t numbers = 0;
                for (std::sregex_iterator i(d.details.begin(), d.details.end(), number), end;
                     i != end;
                     ++i) {
                    ++numbers;
                    original = original || std::abs(std::stod(i->str()) - 4e-6) < 1e-12;
                }
                check(original && numbers >= 2, "details include original and resulting thickness");
            }
        check(ids == std::vector<std::string>{"ring/thin#1", "ring/thin#2", "ring.bond/thin#0"},
              "pre-alignment interval IDs independent of boxes and defect count");
    });
}
