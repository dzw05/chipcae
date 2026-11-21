#include "outline_contains.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <utility>
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
// Independent oracle for the original absolute-threshold convexity scan.
bool legacy_is_convex(const Ring& ring) {
    std::size_t n = ring.size();
    while (n > 1 && ring[n - 1] == ring.front())
        --n;
    for (std::size_t j = 0; j < n; ++j) {
        const auto a = ring[j], b = ring[(j + 1) % n];
        double sign = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const double v =
                (b[0] - a[0]) * (ring[k][1] - a[1]) - (b[1] - a[1]) * (ring[k][0] - a[0]);
            if (std::abs(v) < 1e-14)
                continue;
            if (sign * v < 0)
                return false;
            sign = v;
        }
    }
    return true;
}
} // namespace

int main() {
    using chipcae::solve::detail::outline_contains;
    using chipcae::solve::detail::outline_is_convex;
    int failures = 0;
    auto check = [&](bool ok, const std::string& context) {
        if (!ok) {
            ++failures;
            std::cerr << "FAIL " << context << '\n';
        }
    };
    // Guard original fixtures against a classification change between the two thresholds.
    auto compatibility = [&](const Ring& ring, const std::string& name) {
        check(outline_is_convex(ring) == legacy_is_convex(ring), name + " legacy classification");
        auto lo = ring.front(), hi = ring.front();
        std::size_t n = ring.size();
        while (n > 1 && ring[n - 1] == ring.front())
            --n;
        for (const auto& p : ring)
            for (std::size_t axis = 0; axis < 2; ++axis) {
                lo[axis] = std::min(lo[axis], p[axis]);
                hi[axis] = std::max(hi[axis], p[axis]);
            }
        const double diagonal = std::hypot(hi[0] - lo[0], hi[1] - lo[1]);
        const double threshold = 1e-12 * diagonal * diagonal;
        double minimum = std::numeric_limits<double>::infinity();
        for (std::size_t j = 0; j < n; ++j) {
            const auto a = ring[j], b = ring[(j + 1) % n];
            for (std::size_t k = 0; k < n; ++k) {
                const double v = std::abs((b[0] - a[0]) * (ring[k][1] - a[1]) -
                                          (b[1] - a[1]) * (ring[k][0] - a[0]));
                if (v > 0) {
                    minimum = std::min(minimum, v);
                    check(!(v >= threshold && v < 1e-14), name + " threshold-sensitive cross");
                }
            }
        }
        std::cout << std::setprecision(17) << "compatibility " << name
                  << " convex=" << outline_is_convex(ring) << " min_nonzero_cross=" << minimum
                  << " relative_threshold=" << threshold << '\n';
    };
    const Ring octagon{{.003, 0},
                       {.017, 0},
                       {.02, .003},
                       {.02, .017},
                       {.017, .02},
                       {.003, .02},
                       {0, .017},
                       {0, .003}};
    compatibility(octagon, "octagon");
    const bool octagon_convex = outline_is_convex(octagon);
    std::mt19937 random(0x198224U);
    // Specify the integer-to-double mapping as well as the seed for reproducibility.
    for (int i = 0; i < 1000; ++i) {
        const double x = -.005 + .03 * static_cast<double>(random()) / 4294967296.;
        const double y = -.005 + .03 * static_cast<double>(random()) / 4294967296.;
        check(outline_contains(octagon, octagon_convex, x, y) == outline_contains(octagon, x, y),
              "octagon two-step point " + std::to_string(i));
        check(outline_contains(octagon, x, y) == legacy_contains(octagon, x, y),
              "convex octagon legacy oracle point " + std::to_string(i));
    }
    // Preserve the old strict cross-product skip even just outside a convex edge.
    for (double y : {-1e-14, -1e-11, 0., 1e-14})
        check(outline_contains(octagon, .01, y) == legacy_contains(octagon, .01, y),
              "convex near-boundary arithmetic");
    check(outline_contains({}, 12, -9), "empty outline contains every point");

    check(outline_is_convex({}), "empty outline is convex");
    for (bool convex : {false, true})
        check(outline_contains({}, convex, 12, -9), "empty two-step outline contains every point");
    const Ring coincident{{.01, .01}, {.01, .01}, {.01, .01}};
    check(outline_is_convex(coincident) == legacy_is_convex(coincident),
          "coincident classification preserves legacy behavior");

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
        compatibility(ring, label);
        const bool convex = outline_is_convex(ring);
        for (const auto& [point, expected] :
             std::vector<std::pair<std::array<double, 2>, bool>>{{{.015, .017}, false},
                                                                 {{.005, .017}, true},
                                                                 {{.017, .005}, true},
                                                                 {{.0113, .0137}, true},
                                                                 {{.0113, .0168}, true},
                                                                 {{.005, .0137}, true},
                                                                 {{.015, .0137}, true},
                                                                 {{.005, .02}, true},
                                                                 {{.015, .02}, false},
                                                                 {{.025, .0137}, false},
                                                                 {{.0113, .025}, false},
                                                                 {{0, 0}, true},
                                                                 {{-.001, .01}, false},
                                                                 {{.0113 + 1e-14, .0168}, true},
                                                                 {{.0113 + 1e-12, .0168}, false},
                                                                 {{.0113 - 1e-12, .0168}, true}}) {
            std::ostringstream context;
            context << std::setprecision(17) << label << "point (" << point[0] << "," << point[1]
                    << ") expected " << expected;
            check(outline_contains(ring, point[0], point[1]) == expected, context.str());
            check(outline_contains(ring, convex, point[0], point[1]) ==
                      outline_contains(ring, point[0], point[1]),
                  context.str() + " two-step");
        }
    }
    // The L box diagonal is sqrt(2)*.02: the boundary tolerance is
    // sqrt(2)*2e-14, scaled with the ring. A fixed absolute tolerance would
    // misclassify these points. The outer corner also requires distance to
    // the finite segment endpoint rather than distance to its supporting line.
    for (double scale : {.1, 1., 1000.}) {
        auto ring = l;
        for (auto& p : ring) {
            p[0] *= scale;
            p[1] *= scale;
        }
        for (const auto& [point, expected] : std::vector<std::pair<std::array<double, 2>, bool>>{
                 {{.0113 + 2e-14, .0168}, true},
                 {{.0113 + 4e-14, .0168}, false},
                 {{.02 + 1.5e-14, .0137 + 1.5e-14}, true},
                 {{.02 + 2.5e-14, .0137 + 2.5e-14}, false}}) {
            std::ostringstream context;
            context << std::setprecision(17) << "L boundary scale " << scale << " point ("
                    << point[0] * scale << "," << point[1] * scale << ") expected " << expected;
            check(outline_contains(ring, point[0] * scale, point[1] * scale) == expected,
                  context.str());
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
    const Ring v{{0, 0}, {.0064, 0}, {.0103, .0087}, {.0142, 0}, {.02, 0}, {.02, .02}, {0, .02}};
    compatibility(collinear, "collinear convex");
    compatibility(collinear_l, "collinear L");
    compatibility(v, "pipeline V");
    compatibility(l, "pipeline L");
    compatibility({{0, 0}, {.02, 0}, {.02, .015}, {.015, .02}, {0, .02}}, "fcbga outline");
    // examples/s2_solve/stack.json:11, corner offsets .002 (test octagon uses .003).
    compatibility({{.002, 0},
                   {.018, 0},
                   {.02, .002},
                   {.02, .018},
                   {.018, .02},
                   {.002, .02},
                   {0, .018},
                   {0, .002}},
                  "base stack octagon");
    check(legacy_is_convex(octagon), "legacy original octagon is convex");
    check(!legacy_is_convex(l), "legacy original L is nonconvex");
    check(!legacy_is_convex(v), "legacy original V is nonconvex");
    // The old absolute classification threshold loses the notch at tiny scales.
    // Keep convex containment's legacy behavior, including its tiny-scale limit.
    for (double scale : {1e-6, 1e-3, .1, 1., 1e3}) {
        for (int reversed = 0; reversed < 2; ++reversed) {
            for (const auto& [name, original] : std::vector<std::pair<std::string, Ring>>{
                     {"L", l}, {"V", v}, {"octagon", octagon}}) {
                auto ring = original;
                for (auto& p : ring) {
                    p[0] *= scale;
                    p[1] *= scale;
                }
                if (reversed)
                    std::reverse(ring.begin(), ring.end());
                std::ostringstream label;
                label << name << " scale " << scale << " reversed " << reversed;
                check(outline_is_convex(ring) == (name == "octagon"),
                      label.str() + " scale-relative classification");
                if (name == "octagon") {
                    for (double x : {.01, .025})
                        check(outline_contains(ring, x * scale, .01 * scale) ==
                                  legacy_contains(ring, x * scale, .01 * scale),
                              label.str() + " legacy containment");
                } else {
                    const auto arm = name == "L" ? std::array<double, 2>{.005, .017}
                                                 : std::array<double, 2>{.003, .004};
                    const auto notch = name == "L" ? std::array<double, 2>{.015, .017}
                                                   : std::array<double, 2>{.0103, .003};
                    check(outline_contains(ring, arm[0] * scale, arm[1] * scale),
                          label.str() + " arm must be inside");
                    check(!outline_contains(ring, notch[0] * scale, notch[1] * scale),
                          label.str() + " notch must be outside");
                }
            }
        }
    }
    const bool l_convex = outline_is_convex(l);
    std::size_t consumed = 0;
    auto time_queries = [&](bool hoisted) {
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 100000; ++i) {
            const double x = .025 * static_cast<double>(i % 997) / 997.;
            const double y = .025 * static_cast<double>(i % 991) / 991.;
            consumed += hoisted ? outline_contains(l, l_convex, x, y) : outline_contains(l, x, y);
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
            .count();
    };
    const double per_call = time_queries(false), hoisted = time_queries(true);
    std::cout << "实测 outline_contains 1e5 queries: per-call-convexity=" << per_call
              << " hoisted=" << hoisted << " consumed=" << consumed << '\n';
    std::cout << "1000 convex oracle points and containment boundary cases; " << failures
              << " failures\n";
    return failures == 0 ? 0 : 1;
}
