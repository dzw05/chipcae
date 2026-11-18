#include "assembly_test_support.hpp"
int main() {
    return run([] {
        for (bool bond : {false, true})
            for (double t : {5e-6, 5e-8}) {
                auto p = part(bond ? "lid.bond" : "lid", .001, .001 + t);
                auto r = merge_parts(base(), {p}, options());
                check(bool(r), "thin merge succeeds");
                check(r.value().welded_interfaces == 0,
                      "collapsed slab contributes no weld endpoints");
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
                check(found, "every supported thin interval reports material loss");
                double z = 0;
                for (auto& l : r.value().stack.layers) {
                    check(l.thickness > 0, "positive snapped layers");
                    z += l.thickness;
                }
                near(z, .001);
            }
        auto interior = merge(base(), {part("interior", .0005, .000505)}, .001);
        check(interior.stack.layers.size() == 1 && interior.stack.layers[0].name == "Core" &&
                  interior.stack.layers[0].thickness == .001 &&
                  interior.stack.layers[0].regions.empty(),
              "collapsed interior source cannot split base");
        auto subweld = merge(base(), {part("subweld", .0005, .00050005)}, .001);
        check(subweld.welded_interfaces == 0 && subweld.defects.size() == 1 &&
                  subweld.defects[0].tier == Tier::B &&
                  subweld.defects[0].reason == DefectReason::THIN_LAYER_MERGED &&
                  subweld.defects[0].defect_id == "subweld/thin#0",
              "collapsed sub-weld slab reports thin loss without a phantom weld");
        check(subweld.stack.layers.size() == 1 && subweld.stack.layers[0].regions.empty(),
              "collapsed sub-weld slab does not split or populate the base");
        auto remote = merge(base(), {part("remote", .002, .002005)}, .001);
        check(remote.stack.layers.size() == 1 && remote.stack.layers[0].regions.empty(),
              "eliminated remote source cannot extend stack with phantom void");
    });
}
