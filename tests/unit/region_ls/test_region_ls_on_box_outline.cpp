#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].regions = {square("lid", 0, 0, 8, 8)};
        auto m = samples(32);
        auto ls = levels(s, m);
        require(ls.size() == 1, "outline polygon LS");
        for (std::size_t n = 0; n < m.node_gid.size(); ++n) {
            double x = m.vertices[3 * n], y = m.vertices[3 * n + 1];
            double exact = rect(x, y, 0, 0, 8, 8);
            require(std::abs(ls[0].phi[n] - exact) <= 1e-12, "outline analytic distance");
            if (x == 0 || x == 8 || y == 0 || y == 8)
                require(ls[0].phi[n] <= 0, "coincident edges classified inside");
        }
    });
}
