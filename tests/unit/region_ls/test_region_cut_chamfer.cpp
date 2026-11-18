#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        const auto s = chamfer_stack();
        const auto m = registered_mesh();
        const auto g = geometry(s, m, "56 x 44 mm chamfer");
        require(g.planes.size() == 8, "chamfer emits eight planes");
        // Canonical start (0, .002), CCW: bottom-left chamfer comes first.
        // Analytic literals evaluated with 90-digit arithmetic from the stored
        // input vertices. Their binary rounding makes the upper-right edge
        // slightly different from an ideal 45-degree line; keep the 1e-15 limit.
        const double q = std::sqrt(.5);
        const std::array<std::array<double, 3>, 8> normals = {
            {{-q, -q, 0},
             {0, -1, 0},
             {.707106781186547217742161025430, -.707106781186547831059527698779, 0},
             {1, 0, 0},
             {.707106781186546297766111015403, .707106781186548751035577708804, 0},
             {0, 1, 0},
             {-.707106781186546604424794352079, .707106781186548444376894372130, 0},
             {-1, 0, 0}}};
        const std::array<double, 8> offsets = {-.001414213562373095078240922325,
                                               0,
                                               .038183766184073549326301269235,
                                               .056,
                                               .069296464556281642887553660395,
                                               .044,
                                               .029698484809835036508688202583,
                                               0};
        for (std::size_t k = 0; k < g.planes.size(); ++k) {
            const auto& p = g.planes[k];
            require(p.body == 17 && p.n[2] == 0, "registered body and exact zero z normal");
            if (k % 2 == 1) {
                require(p.n == normals[k] && p.d == offsets[k],
                        "axis edges use exact normals and grid offsets");
            } else {
                for (int axis = 0; axis < 3; ++axis)
                    require(std::abs(p.n[axis] - normals[k][axis]) <= 1e-15,
                            "analytic chamfer unit normal, absolute tolerance 1e-15");
                require(std::abs(p.d - offsets[k]) <= 1e-15 * .056,
                        "analytic slanted offset, absolute tolerance 1e-15 * .056 m");
            }
        }
        constexpr double exact_area = .002456; // (56*44 - 2*2^2) mm^2 in m^2.
        require(std::abs(clipped_area(s.outline.box, g) - exact_area) / exact_area <= 1e-12,
                "independently clipped chamfer area, relative tolerance 1e-12");
    });
}
