#include "outline_contains.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Ring = std::vector<std::array<double, 2>>;

// Original pipeline.cpp xy loop: the compatibility oracle is deliberately independent.
bool legacy_contains(const Ring& ring, double x, double y) {
    double sign = 0;
    for (std::size_t j = 0; j < ring.size(); ++j) {
        auto a = ring[j], b = ring[(j + 1) % ring.size()];
        double v = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
        if (std::abs(v) < 1e-14)
            continue;
        if (sign * v < 0)
            return false;
        sign = v;
    }
    return true;
}
} // namespace

int main() {
    using chipcae::solve::detail::outline_contains;
    int failures = 0;
    auto check = [&](bool ok, const std::string& context) {
        if (!ok) {
            ++failures;
            std::cerr << "FAIL " << context << '\n';
        }
    };
    const Ring octagon{{.003, 0},
                       {.017, 0},
                       {.02, .003},
                       {.02, .017},
                       {.017, .02},
                       {.003, .02},
                       {0, .017},
                       {0, .003}};
    std::mt19937 random(0x198224U);
    // Specify the integer-to-double mapping as well as the seed for reproducibility.
    for (int i = 0; i < 1000; ++i) {
        const double x = -.005 + .03 * static_cast<double>(random()) / 4294967296.;
        const double y = -.005 + .03 * static_cast<double>(random()) / 4294967296.;
        check(outline_contains(octagon, x, y) == legacy_contains(octagon, x, y),
              "convex octagon legacy oracle point " + std::to_string(i));
    }
    // Preserve the old strict cross-product skip even just outside a convex edge.
    for (double y : {-1e-14, -1e-11, 0., 1e-14})
        check(outline_contains(octagon, .01, y) == legacy_contains(octagon, .01, y),
              "convex near-boundary arithmetic");
    check(outline_contains({}, 12, -9), "empty outline contains every point");

    const Ring l{{0, 0}, {.02, 0}, {.02, .0137}, {.0113, .0137}, {.0113, .02}, {0, .02}};
    for (int variant = 0; variant < 16; ++variant) {
        auto ring = l;
        if (variant & 1)
            std::reverse(ring.begin(), ring.end());
        if (variant & 2)
            ring.insert(ring.begin() + 1, ring.front()); // Zero-length segment.
        if (variant & 4)
            ring.push_back(ring.front());
        if (variant & 8) {
            Ring subdivided;
            for (std::size_t j = 0; j < ring.size(); ++j) {
                const auto a = ring[j], b = ring[(j + 1) % ring.size()];
                subdivided.push_back(a);
                subdivided.push_back({(a[0] + b[0]) / 2, (a[1] + b[1]) / 2});
            }
            ring = std::move(subdivided);
        }
        const std::string label = "L variant " + std::to_string(variant) + ": ";
        for (const auto& [point, expected] :
             std::vector<std::pair<std::array<double, 2>, bool>>{{{.015, .017}, false},
                                                                 {{.005, .017}, true},
                                                                 {{.017, .005}, true},
                                                                 {{.0113, .0137}, true},
                                                                 {{.0113, .0168}, true},
                                                                 {{0, 0}, true},
                                                                 {{-.001, .01}, false},
                                                                 {{.0113 + 1e-14, .0168}, true},
                                                                 {{.0113 + 1e-12, .0168}, false},
                                                                 {{.0113 - 1e-12, .0168}, true}}) {
            std::ostringstream context;
            context << std::setprecision(17) << label << "point (" << point[0] << "," << point[1]
                    << ") expected " << expected;
            check(outline_contains(ring, point[0], point[1]) == expected, context.str());
        }
    }
    Ring collinear{{0, 0}, {.01, 0}, {.02, 0}, {.02, .02}, {0, .02}, {0, .02}, {0, 0}};
    for (const auto& p : std::vector<std::array<double, 2>>{{.01, 0}, {.01, .01}, {.021, .01}})
        check(outline_contains(collinear, p[0], p[1]) == legacy_contains(collinear, p[0], p[1]),
              "convex collinear and duplicate vertices preserve legacy behavior");
    auto collinear_l = l;
    collinear_l.insert(collinear_l.begin() + 1, {.01, 0});
    check(outline_contains(collinear_l, .005, .017), "nonconvex collinear edge arm");
    check(!outline_contains(collinear_l, .015, .017), "nonconvex collinear edge notch");
    std::cout << "1000 convex oracle points and containment boundary cases; " << failures
              << " failures\n";
    return failures == 0 ? 0 : 1;
}
