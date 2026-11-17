#include "slice_test.hpp"
using namespace slice_test;
int main() {
#ifdef CHIPCAE_ENABLE_OCCT
    return 77;
#else
    return run([] {
        const auto r = slice_prismatic({}, "any body", options());
        require(r.slabs.empty() && r.defects.size() == 1, "no OCCT: empty slabs and one defect");
        require(r.defects[0].tier == Tier::C, "no OCCT: tier C");
        require(std::string(to_string(r.defects[0].reason)) == "OCCT_UNAVAILABLE",
                "no OCCT reason");
    });
#endif
}
