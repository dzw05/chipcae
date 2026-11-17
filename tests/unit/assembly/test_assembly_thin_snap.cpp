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
        auto displaced = part("shifted", .001 + 5e-6, .002);
        auto d = merge(base(), {displaced});
        check(d.defects.size() == 1 && d.defects[0].reason == DefectReason::THIN_LAYER_MERGED &&
                  d.defects[0].defect_id == "shifted/thin#0",
              "nonnumerical boundary displacement preserves source provenance");
        auto spanning = part("spanning", 0, .002);
        auto sliver = part("sliver", .0005, .000505);
        auto mixed = merge(base(), {spanning, sliver});
        bool spanning_thin = false;
        for (const auto& defect : mixed.defects)
            spanning_thin = spanning_thin || (defect.reason == DefectReason::THIN_LAYER_MERGED &&
                                              defect.defect_id == "spanning/thin#0");
        check(spanning_thin, "thin split retains each source's provenance");
        auto remote = merge(base(), {part("remote", .002, .002005)}, .001);
        check(remote.stack.layers.size() == 1 && remote.stack.layers[0].regions.empty(),
              "eliminated remote source cannot extend stack with phantom void");
    });
}
