#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].regions = {box("bottom", 1, 1, 7, 2),
                               box("top", 1, 6, 7, 7),
                               box("left", 1, 2, 2, 6),
                               box("right", 6, 2, 7, 6)};
        auto m = grid(s, 2);
        std::size_t cavity = 0;
        for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
            double x = 0, y = 0;
            for (int v = 0; v < 8; ++v) {
                auto node = m.hex[8 * e + v];
                x += m.vertices[3 * node] / 8;
                y += m.vertices[3 * node + 1] / 8;
            }
            if (x > 2 && x < 6 && y > 2 && y < 6)
                ++cavity;
        }
        require(cavity == 0, "ring cavity must contain zero emitted cells");
        require(m.elem_gid.size() == 160, "ring must retain exactly covered cells");
        compact(m);
    });
}
