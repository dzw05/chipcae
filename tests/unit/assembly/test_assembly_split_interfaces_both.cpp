#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        b.layers[0].name = "A";
        b.layers[0].body = "";
        auto upper = b.layers[0];
        upper.name = "B";
        b.layers.push_back(upper);
        b.layers[0].regions.push_back({"R", "seed", "", {{0, 0, .001, .001}, {}}});
        b.activation_groups["g"] = {
            "unknown:x", "layer:A", "region:R", "interface:A|B", "interface:B|A"};
        auto m = merge(b, {part("span", .0005, .0015)}, .002);
        check(m.stack.layers.size() == 4, "both original layers split");
        const std::vector<std::string> refs = {"unknown:x",
                                               "layer:A#0",
                                               "layer:A#1",
                                               "region:R#0",
                                               "region:R#1",
                                               "interface:A#1|B#0",
                                               "interface:B#0|A#1"};
        check(m.stack.activation_groups.at("g") == refs,
              "expand refs and preserve interface spelling");
        for (size_t i = 0; i < 4; ++i) {
            auto& l = m.stack.layers[i];
            std::string original = i < 2 ? "A" : "B";
            check(l.name == original + "#" + std::to_string(i % 2) && l.body == original,
                  "split effective layer body");
            if (i < 2)
                check(l.regions[0].name == "R#" + std::to_string(i) && l.regions[0].body == "R",
                      "split region effective body");
        }
        check(m.stack.layers[1].regions.back().name == "span#0#0#0" &&
                  m.stack.layers[2].regions.back().name == "span#0#0#1",
              "cross-layer unique names");
    });
}
