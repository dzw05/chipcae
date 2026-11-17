#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].regions = {square("missing", 2, 2, 6, 6)};
        auto m = samples(4);
        bool caught = false;
        try {
            (void)region_level_sets(s, m, {});
        } catch (const std::invalid_argument& e) {
            caught = true;
            require(std::string(e.what()) ==
                        "region_level_sets: body 'missing' not in attr_table.bodies",
                    "exact unregistered-body diagnostic");
        }
        require(caught, "unregistered polygon body must throw invalid_argument");
    });
}
