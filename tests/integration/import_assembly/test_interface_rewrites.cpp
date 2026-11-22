#include "assembly_seam_support.hpp"

#include <iostream>

int main() {
    using chipcae::importer::interface_rewrite_targets;
    struct Case {
        std::vector<std::string> lower, upper, expected;
    };
    const std::vector<Case> cases{{{"L#0", "L#1"}, {"U#0", "U#1"}, {"interface:L#1|U#0"}},
                                  {{"L#0", "L#1"}, {"U"}, {"interface:L#1|U"}},
                                  {{"L"}, {"U#0", "U#1"}, {"interface:L|U#0"}},
                                  {{"L"}, {"U"}, {"interface:L|U"}},
                                  {{}, {"U"}, {}},
                                  {{"L"}, {}, {}}};
    bool failed = false;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        const auto actual = interface_rewrite_targets(c.lower, c.upper);
        if (actual != c.expected) {
            std::cerr << "interface case " << i << ": expected";
            for (const auto& target : c.expected)
                std::cerr << ' ' << target;
            std::cerr << ", got";
            for (const auto& target : actual)
                std::cerr << ' ' << target;
            std::cerr << '\n';
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
