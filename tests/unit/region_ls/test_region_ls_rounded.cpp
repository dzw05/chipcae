#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        auto r = box("rounded", 2, 2, 6, 6);
        constexpr double pi = 3.14159265358979323846;
        constexpr int segments = 4096;
        for (int corner = 0; corner < 4; ++corner) {
            double cx = (corner == 0 || corner == 3) ? 5 : 3, cy = corner < 2 ? 5 : 3;
            for (int i = 0; i < segments; ++i) {
                double t = (corner * .5 + double(i) / segments * .5) * pi;
                r.footprint.polygon.push_back({cx + std::cos(t), cy + std::sin(t)});
            }
        }
        s.layers[0].regions = {r};
        double exact = 12 + pi;
        std::array<double, 2> errors{};
        int k = 0;
        for (int n : {64, 128}) {
            auto m = samples(n);
            auto ls = levels(s, m);
            errors[k++] = std::abs(area(ls[0].phi, n) - exact) / exact;
        }
        double order = std::log(errors[0] / errors[1]) / std::log(2.);
        std::cout << "measured rounded errors " << errors[0] << " " << errors[1] << " order "
                  << order << '\n';
        require(order >= 1.8, "rounded area convergence order >=1.8");
        require(errors[1] <= 1e-3, "rounded fine area error <=1e-3");
    });
}
