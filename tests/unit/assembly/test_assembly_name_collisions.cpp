#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        invalid_input(merge_parts(b, {part("Core")}, options()), "part id collides with Core");
        b.layers[0].regions.push_back({"lid.bond", "glue", "", {{0, 0, .001, .001}, {}}});
        invalid_input(merge_parts(b, {part()}, options()), "part id collides with lid.bond");
        b = base();
        auto l = b.layers[0];
        l.name = "Core#0";
        b.layers.push_back(l);
        invalid_input(merge_parts(b, {part("split", .0005, .001)}, options()),
                      "split name collides with Core#0");
        invalid_input(merge_parts(base(), {part(), part()}, options()), "duplicate part id lid");
        for (bool region : {false, true}) {
            b = base();
            if (region)
                b.layers[0].regions.push_back({"R", "seed", "lid", {{0, 0, .001, .001}, {}}});
            else
                b.layers[0].body = "lid";
            invalid_input(merge_parts(b, {part()}, options()), "part id collides with lid");
        }
        b = base();
        b.activation_groups["lid"] = {"layer:lid"};
        b.layers[0].copper = CopperSpec{"lid", .2, {}};
        check(bool(merge_parts(b, {part()}, options())),
              "collision scan excludes groups and copper");
    });
}
