#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/copper_map.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <typeinfo>
#include <utility>
#include <vector>

using namespace chipcae::mesh;
using chipcae::Real;

namespace {
constexpr Real dbu = 1e-6;
constexpr const char* no_ecad = "polygon copper rasterization requires CHIPCAE_ENABLE_ECAD";
struct Rect {
    std::int64_t x0, y0, x1, y1;
};
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::vector<Real> axis(std::uint32_t n, Real extent) {
    std::vector<Real> result(n + 1);
    for (std::uint32_t i = 0; i <= n; ++i)
        result[i] = extent * static_cast<Real>(i) / static_cast<Real>(n);
    return result;
}
PolysetLayer layer(const std::vector<Rect>& rectangles) {
    PolysetLayer result;
    result.ring_offsets = {0};
    for (const auto& r : rectangles) {
        result.coordinates.insert(result.coordinates.end(),
                                  {r.x0, r.y0, r.x1, r.y0, r.x1, r.y1, r.x0, r.y1});
        result.ring_offsets.push_back(static_cast<std::int64_t>(result.coordinates.size() / 2));
    }
    return result;
}
std::vector<CopperPath> paths(const PolysetLayer& input, Real unit) {
    std::vector<CopperPath> result;
    for (std::size_t r = 1; r < input.ring_offsets.size(); ++r) {
        auto& path = result.emplace_back();
        for (auto p = input.ring_offsets[r - 1]; p < input.ring_offsets[r]; ++p) {
            const auto i = static_cast<std::size_t>(p) * 2;
            path.push_back({static_cast<Real>(input.coordinates[i]) * unit,
                            static_cast<Real>(input.coordinates[i + 1]) * unit});
        }
    }
    return result;
}
// All rectangles supplied here are disjoint. Their analytic overlap is an independent
// union-with-cell/grid area oracle, including half-DBU cell boundaries at n=16.
long double overlap(const std::vector<Rect>& rectangles,
                    Real unit,
                    long double x0,
                    long double y0,
                    long double x1,
                    long double y1) {
    long double area = 0;
    for (const auto& r : rectangles) {
        const long double dx = std::max(0.L,
                                        std::min(x1, r.x1 * static_cast<long double>(unit)) -
                                            std::max(x0, r.x0 * static_cast<long double>(unit)));
        const long double dy = std::max(0.L,
                                        std::min(y1, r.y1 * static_cast<long double>(unit)) -
                                            std::max(y0, r.y0 * static_cast<long double>(unit)));
        area += dx * dy;
    }
    return area;
}
void cell_gate(const PolysetLayer& input,
               Real unit,
               const std::vector<Rect>& rectangles,
               const std::vector<Real>& xs,
               const std::vector<Real>& ys,
               long double expected_area,
               bool partial_required = false) {
    const auto nx = static_cast<std::uint32_t>(xs.size() - 1);
    const auto ny = static_cast<std::uint32_t>(ys.size() - 1);
    const auto ref = copper_column_ref_phi(xs, ys, input, unit);
    const auto actual = rasterize_copper(xs, ys, paths(input, unit));
    require(ref.size() == static_cast<std::size_t>(nx) * ny, "reference length != nx*ny");
    require(actual.size() == ref.size(), "raster length != nx*ny");
    std::size_t empty = 0, occupied = 0, partial = 0, full = 0;
    long double sum = 0;
    for (std::uint32_t j = 0; j < ny; ++j) {
        for (std::uint32_t i = 0; i < nx; ++i) {
            const auto dense = level0_dense(nx, ny, i, j);
            const auto r = ref[dense], a = actual[dense];
            const auto where = " cell(" + std::to_string(i) + "," + std::to_string(j) + ")";
            require(std::isfinite(r) && r >= 0 && r <= 1, "reference range" + where);
            require(std::isfinite(a) && a >= 0 && a <= 1, "raster range" + where);
            require(r == 0 ? a == 0 : std::abs(a - r) <= 1e-12 * std::max(1., std::abs(r)),
                    "cell precision gate" + where);
            const long double cell_area = static_cast<long double>(xs[i + 1] - xs[i]) *
                                          static_cast<long double>(ys[j + 1] - ys[j]);
            const auto analytic = overlap(rectangles, unit, xs[i], ys[j], xs[i + 1], ys[j + 1]);
            require(std::abs(r - analytic / cell_area) <= 1e-12L,
                    "analytic overlap / Morton order" + where);
            empty += r == 0;
            occupied += r > 0;
            partial += r > 0 && r < 1;
            full += std::abs(r - 1) <= 1e-12;
            sum += r * cell_area;
        }
    }
    require(empty >= 1 && occupied >= 1, "layer must contain empty and occupied cells");
    if (partial_required)
        require(partial >= 1 && full >= 1, "F must contain partial and full cells");
    const auto clipped = overlap(rectangles, unit, xs.front(), ys.front(), xs.back(), ys.back());
    require(std::abs(sum - clipped) <= 1e-12L * std::max(1.L, std::abs(clipped)),
            "union-with-grid area closure");
    require(std::abs(sum - expected_area) <= 1e-12L * std::abs(expected_area),
            "hand-computed area relative error > 1e-12");
}

// Bounded test-local CCPOLY01 decoder: the existing .json-named fixture is binary.
// Avoid linking chipcae_stack or changing the automatic test registration.
std::uint64_t read_le(std::istream& in, unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        const auto byte = in.get();
        require(byte != std::char_traits<char>::eof(), "truncated CCPOLY01 fixture");
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte)) << (8 * i);
    }
    return value;
}
Polyset schema_fixture() {
    std::ifstream in(std::filesystem::path(CHIPCAE_SOURCE_DIR) /
                         "schema/examples/copper_polygons.polyset.json",
                     std::ios::binary);
    require(static_cast<bool>(in), "cannot open schema fixture");
    char magic[8]{};
    in.read(magic, 8);
    require(in && std::string(magic, 8) == "CCPOLY01", "invalid fixture magic");
    Polyset result;
    result.dbu_m = std::bit_cast<double>(read_le(in, 8));
    require(std::isfinite(result.dbu_m) && result.dbu_m > 0, "invalid fixture dbu");
    const auto count = read_le(in, 4);
    require(count == 1, "expected one schema copper layer");
    for (std::uint64_t l = 0; l < count; ++l) {
        PolysetLayer item;
        const auto size = read_le(in, 4);
        require(size > 0 && size <= 1024, "invalid fixture name size");
        item.name.resize(static_cast<std::size_t>(size));
        in.read(item.name.data(), static_cast<std::streamsize>(size));
        require(static_cast<bool>(in), "truncated fixture layer name");
        const auto rings = read_le(in, 4);
        require(rings == 2, "expected two schema squares");
        for (std::uint64_t r = 0; r <= rings; ++r) {
            const auto offset = std::bit_cast<std::int64_t>(read_le(in, 8));
            require(offset >= 0 && offset <= 64 &&
                        (r == 0 ? offset == 0 : offset > item.ring_offsets.back()),
                    "invalid fixture offsets");
            item.ring_offsets.push_back(offset);
        }
        const auto points = static_cast<std::size_t>(item.ring_offsets.back());
        for (std::size_t p = 0; p < points * 2; ++p)
            item.coordinates.push_back(std::bit_cast<std::int64_t>(read_le(in, 8)));
        result.layers.push_back(std::move(item));
    }
    require(in.peek() == std::char_traits<char>::eof(), "trailing fixture bytes");
    return result;
}

template <class Error, class Function>
void expect_error(Function&& function, const std::string& expected) {
    try {
        function();
    } catch (const Error& error) {
        require(typeid(error) == typeid(Error), "wrong exception type");
        require(error.what() == expected, "wrong exception text: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected exception: " + expected);
}

void invalid_inputs() {
    const auto good = layer({{500, 500, 2000, 2000}});
    const auto xs = axis(4, .005);
    const auto reject = [&](const PolysetLayer& bad, const char* message) {
        expect_error<std::invalid_argument>([&] { (void)copper_column_ref_phi(xs, xs, bad, dbu); },
                                            message);
        expect_error<std::invalid_argument>([&] { (void)copper_union_area_m2(bad, dbu); }, message);
    };
    // Eight points ensure {0,5,3,8} has valid endpoints yet fails full-array validation.
    for (const auto& offsets : std::vector<std::vector<std::int64_t>>{
             {0, 5, 3, 8}, {0, 4, 4, 8}, {0, -1, 8}, {0, 9, 8}, {1, 8}, {0, 7}}) {
        auto bad = layer({{500, 500, 2000, 2000}, {2500, 500, 4500, 2000}});
        bad.ring_offsets = offsets;
        reject(bad, "invalid polyset ring offsets");
    }
    for (const auto& offsets : std::vector<std::vector<std::int64_t>>{{}, {0}}) {
        auto bad = good;
        bad.ring_offsets = offsets;
        reject(bad, "invalid polyset geometry");
    }
    auto odd = good;
    odd.coordinates.pop_back();
    reject(odd, "invalid polyset geometry");
    const auto nan = std::numeric_limits<Real>::quiet_NaN();
    const auto inf = std::numeric_limits<Real>::infinity();
    for (const auto unit : {0., -1., nan, inf}) {
        expect_error<std::invalid_argument>(
            [&] { (void)copper_column_ref_phi(xs, xs, good, unit); },
            "copper database unit must be finite and positive");
    }
    const char* short_axis = "copper map axes must be strictly increasing";
    const char* finite_axis = "copper map axes must be finite and strictly increasing";
    const std::vector<std::pair<std::vector<Real>, const char*>> invalid_axes{
        {{}, short_axis},
        {{0}, short_axis},
        {{1, 0}, short_axis},
        {{0, 0}, finite_axis},
        {{0, nan}, finite_axis},
        {{0, inf}, finite_axis}};
    for (const auto& [bad, message] : invalid_axes) {
        expect_error<std::invalid_argument>(
            [&] { (void)copper_column_ref_phi(bad, xs, good, dbu); }, message);
        expect_error<std::invalid_argument>(
            [&] { (void)copper_column_ref_phi(xs, bad, good, dbu); }, message);
    }
}
} // namespace

int main() {
    const auto start = std::chrono::steady_clock::now();
    std::cout << std::setprecision(17);
    bool passed = true;
    const auto run = [&](const std::string& name, const std::function<void()>& test) {
        try {
            test();
            std::cout << name << " ok\n";
        } catch (const std::exception& error) {
            passed = false;
            std::cerr << name << " FAIL " << error.what() << '\n';
        }
    };
    bool off = false;
    run("test_ecad_off", [&] {
        try {
            (void)rasterize_copper({0, 1}, {0, 1}, {});
        } catch (const std::runtime_error& error) {
            require(error.what() == std::string(no_ecad), "unexpected ECAD probe exception");
            off = true;
            expect_error<std::runtime_error>(
                [&] { (void)copper_column_ref_phi({0, 1}, {0, 1}, layer({{0, 0, 1, 1}}), dbu); },
                no_ecad);
        }
    });
    if (off) {
        std::cout << "SKIP (no ECAD)\n";
    } else {
        const std::vector<std::string> names{"In1", "In2", "B", "F"};
        const std::vector<std::vector<Rect>> rectangles{
            {{500, 500, 2000, 2000}},
            {{2500, 500, 4500, 2000}},
            {{500, 2500, 4000, 4500}},
            {{500, 750, 4000, 1500}, {500, 2500, 1500, 4500}}};
        const std::vector<long double> areas{2.25e-6L, 3e-6L, 7e-6L, 4.625e-6L};
        for (const auto n : {4U, 10U, 16U}) {
            for (std::size_t l = 0; l < names.size(); ++l) {
                run("test_f1130_rectangles_cell_gate " + names[l] + " n=" + std::to_string(n), [&] {
                    cell_gate(layer(rectangles[l]),
                              dbu,
                              rectangles[l],
                              axis(n, .005),
                              axis(n, .005),
                              areas[l],
                              l == 3 && n == 10);
                });
            }
        }
        run("test_schema_copper_polygons_cell_gate", [&] {
            const auto fixture = schema_fixture();
            require(fixture.layers.front().name == "F.Cu", "unexpected schema layer");
            // The checked-in example contains two 1 mm squares on a 4 x 3 mm outline.
            const std::vector<Rect> squares{{500, 500, 1500, 1500}, {2500, 1500, 3500, 2500}};
            require(fixture.dbu_m == dbu, "unexpected schema database unit");
            cell_gate(fixture.layers.front(),
                      fixture.dbu_m,
                      squares,
                      axis(10, .004),
                      axis(8, .003),
                      2e-6L);
        });
        run("test_duplicate_rectangle_idempotent", [&] {
            const auto xs = axis(16, .005);
            const auto single = copper_column_ref_phi(xs, xs, layer(rectangles[0]), dbu);
            auto twice = rectangles[0];
            twice.push_back(twice.front());
            const auto duplicate = copper_column_ref_phi(xs, xs, layer(twice), dbu);
            require(single.size() == 256 && duplicate.size() == 256, "duplicate output length");
            require(single == duplicate, "duplicate rectangle changes NonZero reference");
        });
        run("test_zero_union_area", [&] {
            PolysetLayer collinear;
            collinear.ring_offsets = {0, 3};
            collinear.coordinates = {0, 0, 1, 1, 2, 2};
            const auto xs = axis(4, .005);
            const auto ref = copper_column_ref_phi(xs, xs, collinear, dbu);
            const auto empty = rasterize_copper(xs, xs, {});
            require(ref.size() == 16 && empty.size() == 16, "zero-area output length");
            require(std::all_of(ref.begin(), ref.end(), [](Real x) { return x == 0; }),
                    "zero-area union must return zeros");
            require(ref == empty, "zero-area union differs from empty raster");
        });
        // No-ring acceptance awaits a contract ruling: {}, {0}, and {0,0} all fail
        // copper_union_area_m2 validation. Do not invent a legal representation here.
        run("test_ring_offsets_rejected_and_invalid_inputs", invalid_inputs);
    }
    run("measurement", [&] {
        rusage usage{};
        require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
        const auto seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        // macOS ru_maxrss is bytes; Linux ru_maxrss is KiB. Convert both to MiB.
#if defined(__APPLE__)
        const auto rss = static_cast<double>(usage.ru_maxrss) / (1024. * 1024.);
#else
        const auto rss = static_cast<double>(usage.ru_maxrss) / 1024.;
#endif
        std::cout << "MEASURE wall_s=" << seconds << " rss_mib=" << rss << '\n';
        require(std::isfinite(seconds) && seconds <= 10, "wall time exceeds 10 s");
        require(rss >= 0 && rss <= 150, "peak RSS exceeds 150 MiB");
    });
    return passed ? 0 : 1;
}
