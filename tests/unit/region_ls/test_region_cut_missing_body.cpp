#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        auto s = stack();
        auto r = square("fallback-region", 2, 2, 6, 6);
        r.body = "MissingBody";
        s.layers[0].regions = {r};
        HexMeshArrays m;
        error(s, m, "INPUT_INVALID", "MissingBody: body not in attr_table");
        s.layers[0].regions[0].body.clear();
        error(s, m, "INPUT_INVALID", "fallback-region: body not in attr_table");
        m.attr_table.bodies = {{"Fallback-region", 3}};
        error(s, m, "INPUT_INVALID", "fallback-region: body not in attr_table");
        s.layers[0].regions[0].body = "MissingBody";
        m.attr_table.bodies = {{"missingbody", 3}, {"fallback-region", 91}};
        error(s, m, "INPUT_INVALID", "MissingBody: body not in attr_table");
        // The empty-body fallback is also exercised on a successful lookup.
        s.layers[0].regions[0].body.clear();
        auto g = geometry(s, m, "registered fallback-region");
        require(g.planes.size() == 4, "fallback region produces a rectangle");
        for (const auto& p : g.planes)
            require(p.body == 91, "empty body resolves to the case-sensitive region name");
    });
}
