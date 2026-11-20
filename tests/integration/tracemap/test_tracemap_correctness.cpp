#include "chipcae/material/copper_laminate.hpp"
#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/copper_map.hpp"
#include "xfail.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
namespace mesh = chipcae::mesh;
constexpr long double length = 1e-3L, km = 0.3L, kc = 390.L;
constexpr std::array<int, 9> resolutions{1, 4, 10, 20, 40, 80, 160, 320, 640};
constexpr std::array<int, 4> coarse{1, 4, 20, 40}, fine{80, 160, 320, 640};

double parallel() {
    return static_cast<double>((kc + km) / 2);
}
double series() {
    return static_cast<double>(1 / (0.5L / kc + 0.5L / km));
}
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
void near(double got, double want, double abs, double rel, const std::string& message) {
    require(std::isfinite(got) && std::isfinite(want) &&
                std::abs(got - want) <= abs + rel * std::abs(want),
            message + " observed=" + number(got) + " target=" + number(want));
}
std::vector<double> axis(int n) {
    std::vector<double> out(static_cast<std::size_t>(n) + 1);
    for (int j = 0; j <= n; ++j)
        out[j] = static_cast<double>(length * j / n);
    return out;
}
std::vector<mesh::CopperPath> traces(bool transpose) {
    std::vector<mesh::CopperPath> out;
    for (int k = 0; k < 10; ++k) {
        const auto a = static_cast<double>((20 + 100 * k) * 1e-6L);
        const auto b = static_cast<double>((70 + 100 * k) * 1e-6L);
        mesh::CopperPath path{
            {0, a}, {static_cast<double>(length), a}, {static_cast<double>(length), b}, {0, b}};
        // Swapping coordinates reverses winding; all rings still have the same sign.
        if (transpose)
            for (auto& p : path)
                std::swap(p[0], p[1]);
        out.push_back(std::move(path));
    }
    return out;
}

// Independent interval intersection. Neither observed phi nor the mixture helper
// participates in the current-behaviour oracle.
long double reference_phi(long double lo, long double hi) {
    long double copper = 0;
    for (int k = 0; k < 10; ++k) {
        const long double a = (20 + 100 * k) * 1e-6L, b = (70 + 100 * k) * 1e-6L;
        copper += std::max(0.L, std::min(hi, b) - std::max(lo, a));
    }
    return copper / (hi - lo);
}
double prediction(int n) {
    const auto lines = axis(n);
    long double resistance = 0;
    for (int j = 0; j < n; ++j) {
        const long double h = static_cast<long double>(lines[j + 1]) - lines[j];
        const auto f = reference_phi(lines[j], lines[j + 1]);
        resistance += h / (f * kc + (1 - f) * km);
    }
    return static_cast<double>(length / resistance);
}
struct Effective {
    double along, perpendicular, area;
    std::vector<double> phi;
};
Effective measure(int n,
                  bool transpose,
                  const std::vector<mesh::CopperPath>& paths,
                  bool check_reference = true) {
    const auto lines = axis(n), whole = axis(1);
    const auto nx = static_cast<std::uint32_t>(transpose ? n : 1);
    const auto ny = static_cast<std::uint32_t>(transpose ? 1 : n);
    const auto phi =
        mesh::rasterize_copper(transpose ? lines : whole, transpose ? whole : lines, paths);
    require(phi.size() == static_cast<std::size_t>(n), "raster size");
    long double arithmetic = 0, resistance = 0, copper_area = 0;
    for (int j = 0; j < n; ++j) {
        const auto index = static_cast<std::uint32_t>(j);
        const auto dense = mesh::level0_dense(nx, ny, transpose ? index : 0, transpose ? 0 : index);
        const auto f = phi.at(dense);
        require(std::isfinite(f) && f >= 0 && f <= 1, "bounded finite phi");
        if (check_reference)
            near(f,
                 static_cast<double>(reference_phi(lines[j], lines[j + 1])),
                 1e-12,
                 0,
                 "analytic column overlap");
        std::array<double, 3> k{};
        chipcae::material::copper_laminate_conductivity(0.3, 390, f, k.data());
        for (double component : k)
            require(std::isfinite(component) && component > 0, "finite positive conductivity");
        const long double h = static_cast<long double>(lines[j + 1]) - lines[j];
        arithmetic += h * k[transpose ? 1 : 0];
        resistance += h / k[transpose ? 0 : 1];
        copper_area += f * h * length;
    }
    return {static_cast<double>(arithmetic / length),
            static_cast<double>(length / resistance),
            static_cast<double>(copper_area),
            phi};
}
Effective measure(int n, bool transpose = false) {
    return measure(n, transpose, traces(transpose));
}
std::vector<std::string> names() {
    std::vector<std::string> out{"tracemap_along_x_parallel",
                                 "tracemap_along_y_parallel",
                                 "tracemap_aligned_exact",
                                 "tracemap_refine_order",
                                 "tracemap_empty_and_duplicate"};
    for (const auto* prefix : {"tracemap_perp_x_xfail_ny", "tracemap_perp_y_xfail_nx"})
        for (int n : coarse)
            out.push_back(prefix + std::to_string(n));
    for (int n : fine)
        out.push_back("tracemap_refine_xfail_ny" + std::to_string(n));
    return out;
}
int run(const std::string& name) {
    if (name == "tracemap_along_x_parallel" || name == "tracemap_along_y_parallel") {
        const bool transpose = name == "tracemap_along_y_parallel";
        for (int n : resolutions) {
            const auto e = measure(n, transpose);
            near(e.along, parallel(), 0, 1e-12, "parallel conductivity");
            near(e.area, 5e-7, 0, 1e-12, "copper area conservation");
        }
    } else if (name == "tracemap_aligned_exact") {
        const auto e = measure(100);
        for (double f : e.phi)
            near(std::min(std::abs(f), std::abs(1 - f)), 0, 1e-12, 0, "aligned pure column");
        near(e.along, parallel(), 0, 1e-12, "aligned parallel");
        near(e.perpendicular, series(), 0, 1e-12, "aligned series");
    } else if (name == "tracemap_refine_order") {
        double previous = 0;
        for (int n : fine) {
            const double error = std::abs(measure(n).perpendicular / series() - 1);
            std::cout << "ny=" << n << " relative_error=" << number(error) << '\n';
            require(error > 0 && std::isfinite(error), "finite positive refinement error");
            if (previous > 0) {
                require(error < previous, "strictly decreasing refinement error");
                const double ratio = previous / error;
                require(ratio >= 1.8 && ratio <= 2.6, "first-order error ratio=" + number(ratio));
            }
            previous = error;
        }
    } else if (name == "tracemap_empty_and_duplicate") {
        const auto empty = measure(40, false, {}, false);
        near(empty.along, 0.3, 0, 1e-15, "empty kxx");
        near(empty.perpendicular, 0.3, 0, 1e-15, "empty kyy");
        const auto single = measure(40);
        auto paths = traces(false);
        const auto copy = paths;
        paths.insert(paths.end(), copy.begin(), copy.end());
        const auto duplicate = measure(40, false, paths);
        for (std::size_t j = 0; j < single.phi.size(); ++j)
            near(duplicate.phi[j], single.phi[j], 1e-12, 0, "NonZero duplicate union");
        near(duplicate.along, single.along, 0, 1e-12, "duplicate kxx");
        near(duplicate.perpendicular, single.perpendicular, 0, 1e-12, "duplicate kyy");
    } else {
        for (bool transpose : {false, true})
            for (int n : coarse) {
                const std::string prefix =
                    transpose ? "tracemap_perp_y_xfail_nx" : "tracemap_perp_x_xfail_ny";
                if (name == prefix + std::to_string(n))
                    return xfail_check(
                        name, measure(n, transpose).perpendicular, series(), prediction(n), 1e-6);
            }
        for (int n : fine)
            if (name == "tracemap_refine_xfail_ny" + std::to_string(n))
                return xfail_check(name, measure(n).perpendicular, series(), prediction(n), 1e-3);
        throw std::runtime_error("unhandled registered case");
    }
    std::cout << "PASS " << name << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    const auto available = names();
    if (argc != 3 || std::string(argv[1]) != "--case" ||
        std::find(available.begin(), available.end(), argv[2]) == available.end()) {
        std::cerr << "Available cases:\n";
        for (const auto& name : available)
            std::cerr << "  " << name << '\n';
        return 4;
    }
    try {
        return run(argv[2]);
    } catch (const std::exception& e) {
        std::cerr << "ERROR " << argv[2] << ' ' << e.what() << '\n';
        return 3;
    }
}
