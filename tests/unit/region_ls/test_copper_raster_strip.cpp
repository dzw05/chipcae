#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/copper_map.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <utility>
#include <vector>

namespace {
using chipcae::Real;
using chipcae::mesh::CopperPath;
using chipcae::mesh::level0_dense;
using chipcae::mesh::rasterize_copper;
using Nm = std::int64_t;
using NmPoint = std::array<Nm, 2>;
using NmPath = std::vector<NmPoint>;
using Point = std::array<long double, 2>;
using Polygon = std::vector<Point>;
struct Rectangle {
    Nm left, bottom, right, top;
};
struct Fixture {
    std::vector<CopperPath> paths;
    std::vector<Rectangle> rectangles;
    std::vector<NmPath> convex_parts;
};

CopperPath metres(const NmPath& points) {
    CopperPath path;
    path.reserve(points.size());
    for (const auto& p : points)
        path.push_back({static_cast<Real>(p[0]) / 1e9, static_cast<Real>(p[1]) / 1e9});
    return path;
}

NmPath rectangle_path(const Rectangle& r) {
    return {{r.left, r.bottom}, {r.right, r.bottom}, {r.right, r.top}, {r.left, r.top}};
}

Fixture rect_fixture() {
    Fixture f;
    for (Nm a = 0; a < 70; ++a)
        for (Nm b = 0; b < 140; ++b) {
            const Nm x = 1'100'000 + 330'000 * a, y = 1'300'000 + 340'000 * b;
            const Rectangle r{x, y, x + 201'000, y + 157'000};
            f.paths.push_back(metres(rectangle_path(r)));
            f.rectangles.push_back(r);
        }
    // One CCW staircase ring; the reference instead uses 10,000 disjoint rectangles.
    NmPath stair{{26'000'000, 1'000'000}, {49'000'000, 1'000'000}};
    for (Nm k = 9'999; k >= 0; --k) {
        const Nm x = 26'000'000 + 2'300 * k;
        const Nm top = k % 2 == 0 ? 49'000'000 : 48'800'000;
        stair.push_back({x + 2'300, top});
        stair.push_back({x, top});
        f.rectangles.push_back({x, 1'000'000, x + 2'300, top});
    }
    f.paths.push_back(metres(stair));
    return f;
}

Fixture slanted_fixture() {
    Fixture f;
    for (Nm a = 0; a < 40; ++a)
        for (Nm b = 0; b < 50; ++b) {
            const Nm x = 1'200'000 + 600'000 * a, y = 1'200'000 + 950'000 * b;
            NmPath diamond{{x, y - 250'000}, {x + 250'000, y}, {x, y + 250'000}, {x - 250'000, y}};
            f.paths.push_back(metres(diamond));
            f.convex_parts.push_back(std::move(diamond));
        }
    const auto height = [](Nm k) { return k % 2 == 0 ? Nm{49'000'000} : Nm{47'300'000}; };
    NmPath saw{{26'000'000, 1'000'000}, {49'000'000, 1'000'000}};
    for (Nm k = 4'000; k >= 0; --k)
        saw.push_back({26'000'000 + 5'750 * k, height(k)});
    f.paths.push_back(metres(saw));
    // Independent convex decomposition of the 4,000 slanted top-edge segments.
    for (Nm k = 0; k < 4'000; ++k) {
        const Nm x = 26'000'000 + 5'750 * k;
        f.convex_parts.push_back(
            {{x, 1'000'000}, {x + 5'750, 1'000'000}, {x + 5'750, height(k + 1)}, {x, height(k)}});
    }
    return f;
}

std::vector<Real> uniform_axis(std::size_t n) {
    std::vector<Real> axis(n + 1);
    for (std::size_t i = 0; i <= n; ++i)
        axis[i] = 0.05 * static_cast<Real>(i) / static_cast<Real>(n);
    return axis;
}

std::vector<Real> graded_axis(std::size_t n) {
    std::vector<Real> axis(n + 1);
    const long double total = std::pow(1.1L, static_cast<long double>(n)) - 1;
    for (std::size_t i = 0; i <= n; ++i)
        axis[i] =
            static_cast<Real>(0.05L * (std::pow(1.1L, static_cast<long double>(i)) - 1) / total);
    return axis;
}

// References work in long-double nanometres using the actual supplied cell boundaries.
std::vector<long double> nanometres(const std::vector<Real>& axis) {
    std::vector<long double> result;
    for (auto x : axis)
        result.push_back(static_cast<long double>(x) * 1e9L);
    return result;
}

std::pair<std::size_t, std::size_t>
cell_range(const std::vector<long double>& axis, long double lo, long double hi) {
    if (hi <= axis.front() || lo >= axis.back())
        return {0, 0};
    const auto begin = std::upper_bound(axis.begin(), axis.end(), std::max(lo, axis.front()));
    const auto end = std::lower_bound(axis.begin(), axis.end(), std::min(hi, axis.back()));
    return {static_cast<std::size_t>(begin - axis.begin() - 1),
            static_cast<std::size_t>(end - axis.begin())};
}

// Sutherland-Hodgman against four half-planes, independent of Clipper and production helpers.
Polygon clip(Polygon polygon, std::size_t dimension, long double boundary, bool keep_greater) {
    if (polygon.empty())
        return {};
    Polygon result;
    auto previous = polygon.back();
    const auto inside = [&](const Point& p) {
        return keep_greater ? p[dimension] >= boundary : p[dimension] <= boundary;
    };
    for (const auto& current : polygon) {
        if (inside(previous) != inside(current)) {
            const long double t =
                (boundary - previous[dimension]) / (current[dimension] - previous[dimension]);
            Point intersection{previous[0] + t * (current[0] - previous[0]),
                               previous[1] + t * (current[1] - previous[1])};
            intersection[dimension] = boundary;
            result.push_back(intersection);
        }
        if (inside(current))
            result.push_back(current);
        previous = current;
    }
    return result;
}

long double convex_cell_area(
    Polygon polygon, long double left, long double right, long double bottom, long double top) {
    polygon = clip(std::move(polygon), 0, left, true);
    polygon = clip(std::move(polygon), 0, right, false);
    polygon = clip(std::move(polygon), 1, bottom, true);
    polygon = clip(std::move(polygon), 1, top, false);
    if (polygon.size() < 3)
        return 0;
    // Translate before shoelace to avoid subtracting large, nearly equal products.
    const Point origin = polygon.front();
    long double twice_area = 0;
    auto previous = polygon.back();
    for (const auto& current : polygon) {
        twice_area += (previous[0] - origin[0]) * (current[1] - origin[1]) -
                      (current[0] - origin[0]) * (previous[1] - origin[1]);
        previous = current;
    }
    return std::abs(twice_area) / 2;
}

std::vector<long double>
reference(const Fixture& f, const std::vector<Real>& xs, const std::vector<Real>& ys) {
    const auto x = nanometres(xs), y = nanometres(ys);
    const auto nx = x.size() - 1, ny = y.size() - 1;
    std::vector<long double> areas(nx * ny);
    for (const auto& r : f.rectangles) {
        const auto [ib, ie] = cell_range(x, r.left, r.right);
        const auto [jb, je] = cell_range(y, r.bottom, r.top);
        for (auto j = jb; j < je; ++j)
            for (auto i = ib; i < ie; ++i)
                areas[j * nx + i] +=
                    std::max(0.0L,
                             std::min(x[i + 1], static_cast<long double>(r.right)) -
                                 std::max(x[i], static_cast<long double>(r.left))) *
                    std::max(0.0L,
                             std::min(y[j + 1], static_cast<long double>(r.top)) -
                                 std::max(y[j], static_cast<long double>(r.bottom)));
    }
    for (const auto& part : f.convex_parts) {
        Polygon polygon;
        Point lo{static_cast<long double>(part.front()[0]),
                 static_cast<long double>(part.front()[1])};
        Point hi = lo;
        for (const auto& p : part) {
            polygon.push_back({static_cast<long double>(p[0]), static_cast<long double>(p[1])});
            for (std::size_t d = 0; d < 2; ++d) {
                lo[d] = std::min(lo[d], static_cast<long double>(p[d]));
                hi[d] = std::max(hi[d], static_cast<long double>(p[d]));
            }
        }
        const auto [ib, ie] = cell_range(x, lo[0], hi[0]);
        const auto [jb, je] = cell_range(y, lo[1], hi[1]);
        for (auto j = jb; j < je; ++j)
            for (auto i = ib; i < ie; ++i)
                areas[j * nx + i] += convex_cell_area(polygon, x[i], x[i + 1], y[j], y[j + 1]);
    }
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i)
            areas[j * nx + i] /= (x[i + 1] - x[i]) * (y[j + 1] - y[j]);
    return areas;
}

[[noreturn]] void cell_failure(const std::string& name,
                               std::size_t i,
                               std::size_t j,
                               Real actual,
                               long double expected,
                               const std::string& reason) {
    std::ostringstream message;
    message << std::setprecision(20) << name << " cell=(" << i << ',' << j << ") actual=" << actual
            << " expected=" << expected << " error=" << std::abs(actual - expected) << ' '
            << reason;
    throw std::runtime_error(message.str());
}

long double check_field(const std::string& name,
                        const std::vector<Real>& actual,
                        const std::vector<long double>& expected,
                        std::size_t nx,
                        std::size_t ny,
                        long double tolerance) {
    if (actual.size() != nx * ny || expected.size() != nx * ny)
        throw std::runtime_error(name + " length actual=" + std::to_string(actual.size()) +
                                 " expected=" + std::to_string(nx * ny));
    long double max_error = 0;
    std::size_t worst_i = 0, worst_j = 0;
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto d = level0_dense(static_cast<std::uint32_t>(nx),
                                        static_cast<std::uint32_t>(ny),
                                        static_cast<std::uint32_t>(i),
                                        static_cast<std::uint32_t>(j));
            const auto value = actual[d];
            const auto want = expected[j * nx + i];
            if (!std::isfinite(value) || !std::isfinite(want) || value < 0 || value > 1)
                cell_failure(name, i, j, value, want, "nonfinite or outside [0,1]");
            const auto error = std::abs(static_cast<long double>(value) - want);
            if (error > max_error) {
                max_error = error;
                worst_i = i;
                worst_j = j;
            }
        }
    if (max_error > tolerance) {
        const auto d = level0_dense(static_cast<std::uint32_t>(nx),
                                    static_cast<std::uint32_t>(ny),
                                    static_cast<std::uint32_t>(worst_i),
                                    static_cast<std::uint32_t>(worst_j));
        std::ostringstream reason;
        reason << "maximum error exceeds tolerance=" << tolerance;
        cell_failure(
            name, worst_i, worst_j, actual[d], expected[worst_j * nx + worst_i], reason.str());
    }
    return max_error;
}

void fixture_accuracy(const std::string& name,
                      const Fixture& fixture,
                      const std::vector<std::size_t>& sizes,
                      long double tolerance) {
    for (auto n : sizes) {
        const auto axis = uniform_axis(n);
        const auto expected = reference(fixture, axis, axis);
        const auto phi = rasterize_copper(axis, axis, fixture.paths);
        const auto error = check_field(
            name + " n=" + std::to_string(n), phi, expected, n, n, n == 256 ? 2e-12L : tolerance);
        std::cout << "REFERENCE copper-raster-strip: " << name << " n=" << n
                  << " max_error=" << error << '\n';
    }
}

void repeat_and_reverse(const std::string& name, const Fixture& fixture, long double tolerance) {
    const auto axis = uniform_axis(64);
    const auto expected = reference(fixture, axis, axis);
    const auto first = rasterize_copper(axis, axis, fixture.paths);
    const auto second = rasterize_copper(axis, axis, fixture.paths);
    auto reversed_paths = fixture.paths;
    std::reverse(reversed_paths.begin(), reversed_paths.end());
    const auto reversed = rasterize_copper(axis, axis, reversed_paths);
    for (const auto* values : {&first, &second, &reversed})
        check_field(name, *values, expected, 64, 64, tolerance);
    for (std::uint32_t j = 0; j < 64; ++j)
        for (std::uint32_t i = 0; i < 64; ++i) {
            const auto d = level0_dense(64, 64, i, j);
            using Bits = std::array<std::byte, sizeof(Real)>;
            if (std::bit_cast<Bits>(first[d]) != std::bit_cast<Bits>(second[d]))
                cell_failure(name + " repeat", i, j, second[d], first[d], "different bit patterns");
            if (std::abs(static_cast<long double>(first[d]) - reversed[d]) > tolerance)
                cell_failure(name + " reverse", i, j, reversed[d], first[d], "order dependence");
        }
}

void edge_cases(const Fixture& rectangles) {
    const std::vector<Real> axis{0, 1, 2, 3, 4};
    const std::vector<long double> zero(16, 0);
    const std::vector<std::vector<CopperPath>> empty_cases{
        {}, {{{{1, 1}}}}, {{{{1, 1}, {2, 2}}}}, {{{{1, 1}, {2, 2}, {3, 3}}}}};
    const std::array<std::string, 4> names{"empty", "one-point", "two-point", "collinear"};
    for (std::size_t k = 0; k < empty_cases.size(); ++k)
        check_field(names[k], rasterize_copper(axis, axis, empty_cases[k]), zero, 4, 4, 1e-12L);

    Fixture one;
    one.rectangles.push_back({500'000'000, 1'250'000'000, 2'250'000'000, 3'500'000'000});
    one.paths.push_back(metres(rectangle_path(one.rectangles.front())));
    const auto expected = reference(one, axis, axis);
    const auto single = rasterize_copper(axis, axis, one.paths);
    const auto duplicate = rasterize_copper(axis, axis, {one.paths.front(), one.paths.front()});
    check_field("single rectangle", single, expected, 4, 4, 1e-12L);
    check_field("duplicate analytic", duplicate, expected, 4, 4, 1e-12L);
    std::vector<long double> single_row_major(16);
    for (std::uint32_t j = 0; j < 4; ++j)
        for (std::uint32_t i = 0; i < 4; ++i)
            single_row_major[j * 4 + i] = single[level0_dense(4, 4, i, j)];
    check_field("duplicate single", duplicate, single_row_major, 4, 4, 1e-12L);

    const CopperPath outer{{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    const CopperPath hole{{1, 1}, {1, 3}, {3, 3}, {3, 1}};
    std::vector<long double> hole_expected(16, 1), outside_expected(16, 0);
    for (std::size_t j = 1; j < 3; ++j)
        for (std::size_t i = 0; i < 4; ++i) {
            if (i == 1 || i == 2)
                hole_expected[j * 4 + i] = 0;
            if (i < 2)
                outside_expected[j * 4 + i] = 1;
        }
    check_field("hole", rasterize_copper(axis, axis, {outer, hole}), hole_expected, 4, 4, 1e-12L);
    const CopperPath outside{{-1, 1}, {2, 1}, {2, 3}, {-1, 3}};
    check_field(
        "out-of-board", rasterize_copper(axis, axis, {outside}), outside_expected, 4, 4, 1e-12L);
    const auto graded = graded_axis(64);
    const auto error = check_field("nonuniform R",
                                   rasterize_copper(graded, graded, rectangles.paths),
                                   reference(rectangles, graded, graded),
                                   64,
                                   64,
                                   1e-12L);
    std::cout << "REFERENCE copper-raster-strip: nonuniform R n=64 max_error=" << error << '\n';
}

void speed_guard() {
    const auto fixture = rect_fixture();
    const auto axis = uniform_axis(256);
    const auto start = std::chrono::steady_clock::now();
    const auto phi = rasterize_copper(axis, axis, fixture.paths);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::size_t points = 0;
    for (const auto& ring : fixture.paths)
        points += ring.size();
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("speed_guard getrusage failed");
#if defined(__APPLE__)
    const auto maxrss_kib = usage.ru_maxrss / 1024;
#else
    const auto maxrss_kib = usage.ru_maxrss;
#endif
    std::cout << "MEASURE copper-raster-strip: {\"n\":256,\"rings\":" << fixture.paths.size()
              << ",\"points\":" << points << ",\"seconds\":" << seconds
              << ",\"maxrss_kib\":" << maxrss_kib << "}" << std::endl;
    check_field("speed_guard R", phi, reference(fixture, axis, axis), 256, 256, 2e-12L);
    if (!std::isfinite(seconds) || seconds > 1.5) {
        std::ostringstream message;
        message << "speed_guard n=256 seconds=" << seconds << " exceeds 1.5 s";
        throw std::runtime_error(message.str());
    }
}
} // namespace

int main() {
    std::cout << std::setprecision(17);
    try {
        try {
            (void)rasterize_copper({0, 1}, {0, 1}, {});
        } catch (const std::runtime_error& error) {
            if (std::string(error.what()) ==
                "polygon copper rasterization requires CHIPCAE_ENABLE_ECAD") {
                std::cout << "copper-raster-strip: SKIP (no ECAD)\n";
                return 0;
            }
            throw;
        }
        const auto rectangles = rect_fixture();
        const auto slanted = slanted_fixture();
        bool passed = true;
        // Keep running independent cases so a precision failure cannot hide the speed measurement.
        const auto run = [&](const char* name, auto&& test) {
            try {
                test();
                std::cout << "copper-raster-strip: " << name << " ok\n";
            } catch (const std::exception& error) {
                passed = false;
                std::cerr << "copper-raster-strip: " << name << " FAIL " << error.what() << '\n';
            }
        };
        run("rect_fixture_accuracy",
            [&] { fixture_accuracy("R", rectangles, {32, 64, 128, 256}, 1e-12L); });
        run("slanted_fixture_accuracy",
            [&] { fixture_accuracy("S", slanted, {32, 64, 128}, 5e-11L); });
        run("repeat_and_reverse", [&] {
            repeat_and_reverse("R", rectangles, 1e-12L);
            repeat_and_reverse("S", slanted, 5e-11L);
        });
        run("edge_cases", [&] { edge_cases(rectangles); });
        run("speed_guard", speed_guard);
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "copper-raster-strip: FAIL " << error.what() << '\n';
        return 1;
    }
}
