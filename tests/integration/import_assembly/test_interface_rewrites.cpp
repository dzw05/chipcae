#include "assembly_seam_support.hpp"

int main() {
    using chipcae::importer::interface_rewrite_targets;
    const auto targets = interface_rewrite_targets({"L#0", "L#1"}, {"U#0", "U#1"});
    return targets != std::vector<std::string>{"interface:L#0|U#0",
                                               "interface:L#0|U#1",
                                               "interface:L#1|U#0",
                                               "interface:L#1|U#1"} ||
           !interface_rewrite_targets({}, {"U"}).empty();
}
