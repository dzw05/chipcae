#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        auto left = box("left", 2, 2, 6, 6);
        left.body = "ring";
        left.footprint.polygon = {
            {2, 2}, {4.25, 2}, {4.25, 3}, {3, 3}, {3, 5}, {4.25, 5}, {4.25, 6}, {2, 6}};
        auto right = box("right", 2, 2, 6, 6);
        right.body = "ring";
        right.footprint.polygon = {
            {3.75, 2}, {6, 2}, {6, 6}, {3.75, 6}, {3.75, 5}, {5, 5}, {5, 3}, {3.75, 3}};
        s.layers[0].regions = {left, right, left};
        int n = 128;
        double h = 8. / n;
        auto m = samples(n);
        auto ls = levels(s, m);
        require(ls.size() == 1, "same body and duplicate regions union into one LS");
        for (std::size_t v = 0; v < m.node_gid.size(); ++v) {
            double x = m.vertices[3 * v], y = m.vertices[3 * v + 1];
            double exact = std::max(rect(x, y, 2, 2, 6, 6), -rect(x, y, 3, 3, 5, 5));
            if (std::abs(exact) <= 2 * h)
                require(std::abs(ls[0].phi[v] - exact) <= 1e-12, "ring band analytic phi");
            if (x == 4 && (y == 2.5 || y == 5.5))
                require(ls[0].phi[v] < 0, "bridge seam must be inside");
        }
        require(std::abs(area(ls[0].phi, n) - 12) / 12 <= 1e-3, "ring area relative error");
    });
}
