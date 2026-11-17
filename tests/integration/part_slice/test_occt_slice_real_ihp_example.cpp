#include "occt_fixture.hpp"

#include <set>
using namespace slice_test;
int main() {
    const auto path = demo("IHP_NATIVE_CONTACT_CONVERSION_EXAMPLE.step");
    if (path.empty())
        return 77;
    return run([&] {
        const auto input = import_step(path);
        require(input.body_names.size() == 10, "IHP imports ten bodies");
        const std::set<DefectReason> allowed{DefectReason::THIN_LAYER_MERGED,
                                             DefectReason::PART_UNMAPPED,
                                             DefectReason::PART_NOT_PRISMATIC,
                                             DefectReason::PART_TILTED,
                                             DefectReason::PART_OVERLAP,
                                             DefectReason::PART_OUTSIDE_OUTLINE,
                                             DefectReason::PART_ATTACH_INVALID,
                                             DefectReason::OCCT_UNAVAILABLE};
        bool valid = true;
        std::cout << "body | slabs | highest tier | reasons\n";
        for (const auto& name : input.body_names) {
            try {
                const auto r = slice_prismatic(input, name, options());
                bool c = false, reasons_valid = true;
                std::cout << name << " | " << r.slabs.size() << " | "
                          << (tier(r) == Tier::C   ? "C"
                              : tier(r) == Tier::B ? "B"
                                                   : "A")
                          << " | ";
                for (const auto& d : r.defects) {
                    std::cout << to_string(d.reason) << ' ';
                    if (d.tier == Tier::C) {
                        c = true;
                        reasons_valid &= allowed.contains(d.reason);
                    }
                }
                std::cout << std::endl;
                valid &= (c && reasons_valid) || (!c && !r.slabs.empty());
            } catch (const std::exception& e) {
                std::cerr << name << " | exception | " << e.what() << std::endl;
                valid = false;
            }
        }
        require(valid,
                "each IHP body returns without exception and satisfies contract "
                "classification");
    });
}
