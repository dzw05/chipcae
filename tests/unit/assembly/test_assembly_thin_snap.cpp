#include "assembly_test_support.hpp"
int main() {
    return run([] {
        for (bool bond : {false, true})
            for (double t : {5e-6, 5e-8}) {
                auto p = part(bond ? "lid.bond" : "lid", .001, .001 + t);
                auto r = merge_parts(base(), {p}, options());
                check(bool(r), "thin merge succeeds");
                check(r.value().welded_interfaces > 0, "thin interface aligned");
                bool found = false;
                for (auto& d : r.value().defects)
                    if (d.reason == DefectReason::THIN_LAYER_MERGED) {
                        found = true;
                        check(d.tier == Tier::B && d.defect_id == p.id + "/thin#0",
                              "thin provenance");
                        check(!d.details.empty(), "thin thickness details");
                        const std::regex number(
                            R"([-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)");
                        bool original = false;
                        bool resulting = false;
                        size_t count = 0;
                        for (std::sregex_iterator it(d.details.begin(), d.details.end(), number),
                             end;
                             it != end;
                             ++it) {
                            double v = std::stod(it->str());
                            ++count;
                            original = original || std::abs(v - t) < 1e-12;
                            resulting = resulting || std::abs(v) < 1e-12;
                        }
                        check(original && resulting && count >= 2,
                              "details include original and resulting thicknesses");
                    }
                check(found == (bond || t >= options().eps_weld),
                      "bond always reports thin; sub weld ordinary only counts");
                double z = 0;
                for (auto& l : r.value().stack.layers) {
                    check(l.thickness > 0, "positive snapped layers");
                    z += l.thickness;
                }
                near(z, .001);
            }
    });
}
