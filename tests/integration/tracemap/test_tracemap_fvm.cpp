#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/copper_map.hpp"
#include "fvm_test_util.hpp"
#include "xfail.hpp"

namespace {
using namespace fvm_test;
constexpr long double km = 0.3L, kc = 390.L;
constexpr std::array<const char*, 7> cases{"tracemap_fvm_slab_harmonic",
                                           "tracemap_fvm_slab_series_xfail",
                                           "tracemap_cut_inside_normalized",
                                           "tracemap_cut_outside_xfail",
                                           "tracemap_cut_conservation_xfail",
                                           "tracemap_merge_conservation",
                                           "tracemap_merge_directional_xfail"};

double series() {
    return static_cast<double>(1 / (0.5L / kc + 0.5L / km));
}
void bounded_fractions(const std::vector<double>& fractions) {
    for (double f : fractions)
        require(std::isfinite(f) && f >= 0 && f <= 1, "bounded finite copper fraction");
}
mesh::CopperPath rectangle(double x0, double x1, double y0, double y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}
std::vector<mesh::CopperPath> traces() {
    std::vector<mesh::CopperPath> out;
    for (int k = 0; k < 10; ++k)
        out.push_back(rectangle(0,
                                1e-3,
                                static_cast<double>((20 + 100 * k) * 1e-6L),
                                static_cast<double>((70 + 100 * k) * 1e-6L)));
    return out;
}
long double reference_phi(long double lo, long double hi) {
    long double copper = 0;
    for (int k = 0; k < 10; ++k) {
        const long double a = (20 + 100 * k) * 1e-6L, b = (70 + 100 * k) * 1e-6L;
        copper += std::max(0.L, std::min(hi, b) - std::max(lo, a));
    }
    return copper / (hi - lo);
}
std::size_t axis_index(const std::vector<double>& axis, double coordinate) {
    const auto it = std::lower_bound(axis.begin(), axis.end(), coordinate);
    require(it != axis.end(), "hex vertex belongs to axis");
    near(*it, coordinate, 1e-15, 0, "hex vertex axis coordinate");
    const auto index = static_cast<std::size_t>(it - axis.begin());
    require(index + 1 < axis.size(), "hex lower vertex inside axis");
    return index;
}
struct Slab {
    double effective, harmonic, prediction;
};
Slab slab() {
    const std::vector<double> axis{0, 0.25e-3, 0.5e-3, 0.75e-3, 1e-3};
    auto m = boxes({axis, axis, {0, 35e-6}});
    const auto nz = m.axes[2].size() - 1;
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        const auto lo = vertex(m, e, 0);
        const auto i = axis_index(axis, lo[0]), j = axis_index(axis, lo[1]);
        const auto dense =
            mesh::level0_dense(4, 4, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
        require(m.elem_gid[e] / static_cast<std::int64_t>(nz) == static_cast<std::int64_t>(dense),
                "boxes column order must match level0_dense");
    }
    m.copper_layer_of_layer = {0};
    m.copper_phi = mesh::rasterize_copper(axis, axis, traces());
    require(m.copper_phi.size() == 16, "slab phi size");
    bounded_fractions(m.copper_phi);
    long double predicted_resistance = 0;
    for (std::uint32_t j = 0; j < 4; ++j) {
        const auto f = reference_phi(axis[j], axis[j + 1]);
        for (std::uint32_t i = 0; i < 4; ++i)
            near(m.copper_phi.at(mesh::level0_dense(4, 4, i, j)),
                 static_cast<double>(f),
                 1e-12,
                 0,
                 "independent slab column overlap");
        const long double h = static_cast<long double>(axis[j + 1]) - axis[j];
        predicted_resistance += h / (f * kc + (1 - f) * km);
    }
    const auto g = take(fv::build_grid(m), "slab build_grid");
    const auto fractions = fv::copper_fraction(g, m);
    require(fractions.size() == 16 && g.cv_volume.size() == 16, "slab has sixteen CVs");
    require(g.cv_elem.size() == 16, "slab CV element map size");
    bounded_fractions(fractions);
    auto p = problem(g);
    for (std::size_t c = 0; c < fractions.size(); ++c)
        chipcae::material::copper_laminate_conductivity(0.3, 390, fractions[c], p.k[c].data());
    p.bcs = {dirichlet("face:ymin", 1), dirichlet("face:ymax", 0)};
    p.rtol = 1e-14;
    const auto r =
        take(fv::solve_thermal(g, m, p, chipcae::Communicator::world()), "slab solve_thermal");
    require(r.k_used.size() == 16, "slab k_used size");
    for (const auto& k : r.k_used)
        for (double component : k)
            require(std::isfinite(component) && component > 0, "finite positive k_used");
    std::array<long double, 4> row{};
    std::array<int, 16> seen{};
    for (std::size_t c = 0; c < g.cv_elem.size(); ++c) {
        const auto lo = vertex(m, static_cast<std::size_t>(g.cv_elem[c]), 0);
        const auto i = axis_index(axis, lo[0]), j = axis_index(axis, lo[1]);
        require(++seen[4 * j + i] == 1, "one CV per slab column");
        row[j] += r.k_used[c][1] / 4.L;
    }
    long double resistance = 0;
    for (std::size_t j = 0; j < row.size(); ++j) {
        require(row[j] > 0, "positive slab row conductivity");
        resistance += (static_cast<long double>(axis[j + 1]) - axis[j]) / row[j];
    }
    const auto harmonic = static_cast<double>(1e-3L / resistance);
    const double effective = std::abs(r.heat_in) * 1e-3 / (1e-3 * 35e-6);
    near(effective, harmonic, 0, 1e-8, "slab solve matches k_used harmonic mean");
    std::cout << "slab effective=" << number(effective) << " harmonic=" << number(harmonic) << '\n';
    return {effective, harmonic, static_cast<double>(1e-3L / predicted_resistance)};
}
struct Cut {
    double fraction, volume;
};
Cut cut(const mesh::CopperPath& path, double phi_reference) {
    auto m = cube(1);
    const mesh::cut::CutGeometry geometry{{{0, {1, 1, 0}, 1}}};
    mesh::cut::CutOptions options;
    options.quad_degree = 2;
    options.small_sub_vf = 0;
    const auto layer = take(mesh::cut::build_cut_layer(std::as_const(m), geometry, options),
                            "half-cube build_cut_layer");
    require(layer.sub_volfrac.size() == 1, "one cut subcell");
    near(layer.sub_volfrac[0], 0.5, 0, 1e-12, "half-cube sub_volfrac self-check");
    m.copper_layer_of_layer = {0};
    m.copper_phi = mesh::rasterize_copper(m.axes[0], m.axes[1], {path});
    require(m.copper_phi.size() == 1, "cut single-column phi");
    bounded_fractions(m.copper_phi);
    near(m.copper_phi[0], phi_reference, 1e-12, 0, "cut rectangle full-column area");
    const auto g = take(fv::build_grid(m, &layer), "half-cube build_grid");
    const auto fractions = fv::copper_fraction(g, m);
    require(fractions.size() == 1 && g.cv_volume.size() == 1, "one cut CV");
    bounded_fractions(fractions);
    near(g.cv_volume[0], 0.5, 0, 1e-12, "half-cube physical volume");
    return {fractions[0], g.cv_volume[0]};
}
// Integral of the solid width 1-y over a horizontal band in x+y <= 1.
long double intersection_area(long double a, long double b) {
    return (b - b * b / 2) - (a - a * a / 2);
}
struct Merged {
    double fraction;
    Vec conductivity;
};
Merged merge() {
    auto m = boxes({{0, 1}, {0, 1, 2}, {0, 1}});
    m.copper_layer_of_layer = {0};
    m.copper_phi = mesh::rasterize_copper(m.axes[0], m.axes[1], {rectangle(0, 1, 0, 1)});
    require(m.copper_phi.size() == 2, "merge two-column phi");
    bounded_fractions(m.copper_phi);
    near(m.copper_phi.at(mesh::level0_dense(1, 2, 0, 0)), 1, 1e-12, 0, "column A copper");
    near(m.copper_phi.at(mesh::level0_dense(1, 2, 0, 1)), 0, 1e-12, 0, "column B matrix");
    mesh::cut::CutLayerArrays layer;
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        auto lo = vertex(m, e, 0), hi = vertex(m, e, 6);
        if (lo[1] > 0) {
            hi[1] = lo[1] + 0.1 * (hi[1] - lo[1]);
            append_sub_box(layer, m, e, lo, hi);
        }
    }
    require(layer.sub_volfrac.size() == 1, "only B is cut");
    near(layer.sub_volfrac[0], 0.1, 0, 1e-12, "B retains bottom tenth");
    const auto before = take(fv::build_grid(m, &layer), "unmerged build_grid");
    require(before.cv_volume.size() == 2, "two CVs before merge");
    const auto fractions = fv::copper_fraction(before, m);
    require(fractions.size() == 2, "unmerged copper fractions size");
    bounded_fractions(fractions);
    long double copper_before = 0;
    for (std::size_t c = 0; c < fractions.size(); ++c)
        copper_before += static_cast<long double>(fractions[c]) * before.cv_volume[c];
    near(static_cast<double>(copper_before), 1, 0, 1e-14, "analytic unmerged copper volume");
    layer.small_subs = {0};
    const auto after = take(fv::build_grid(m, &layer), "merged build_grid");
    require(after.cv_volume.size() == 1, "small B merges into A");
    const auto merged = fv::copper_fraction(after, m);
    require(merged.size() == 1, "one merged fraction");
    bounded_fractions(merged);
    near(after.cv_volume[0], 1.1, 0, 1e-12, "merged physical volume");
    near(merged[0] * after.cv_volume[0],
         static_cast<double>(copper_before),
         0,
         1e-14,
         "merge copper volume conservation");
    near(merged[0], 1 / 1.1, 1e-12, 0, "merged fraction");
    Vec k{};
    chipcae::material::copper_laminate_conductivity(0.3, 390, merged[0], k.data());
    return {merged[0], k};
}
int run(const std::string& name) {
    if (name == "tracemap_fvm_slab_harmonic" || name == "tracemap_fvm_slab_series_xfail") {
        const auto s = slab();
        if (name == "tracemap_fvm_slab_series_xfail")
            return xfail_check(name, s.effective, series(), s.prediction, 1e-6);
    } else if (name == "tracemap_cut_inside_normalized") {
        const long double area = 0.5L * 0.2L;
        const auto c = cut(rectangle(0, 0.5, 0, 0.2), static_cast<double>(area));
        near(c.fraction,
             static_cast<double>(area / 0.5L),
             1e-12,
             0,
             "inside copper normalized by solid area");
    } else if (name == "tracemap_cut_outside_xfail") {
        const long double a = 0.4L, b = 0.6L, solid = 0.5L;
        const auto c = cut(rectangle(0, 1, static_cast<double>(a), static_cast<double>(b)),
                           static_cast<double>(b - a));
        return xfail_check(name,
                           c.fraction,
                           static_cast<double>(intersection_area(a, b) / solid),
                           static_cast<double>(std::min(1.L, (b - a) / solid)),
                           1e-6);
    } else if (name == "tracemap_cut_conservation_xfail") {
        const long double a = 0, b = 0.6L, solid = 0.5L, thickness = 1;
        const auto c = cut(rectangle(0, 1, 0, static_cast<double>(b)), static_cast<double>(b));
        return xfail_check(name,
                           c.fraction * c.volume,
                           static_cast<double>(intersection_area(a, b) * thickness),
                           static_cast<double>(std::min(1.L, (b - a) / solid) * solid * thickness),
                           1e-6);
    } else {
        const auto m = merge();
        if (name == "tracemap_merge_directional_xfail") {
            const long double va = 1, vb = 0.1L, total = va + vb;
            const auto par = static_cast<double>((va * kc + vb * km) / total);
            near(m.conductivity[0], par, 0, 1e-12, "merged parallel kxx");
            const long double f = va / total;
            return xfail_check(name,
                               m.conductivity[1],
                               static_cast<double>(total / (va / kc + vb / km)),
                               static_cast<double>(f * kc + (1 - f) * km),
                               1e-6);
        }
    }
    std::cout << "PASS " << name << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--case" ||
        std::find(cases.begin(), cases.end(), std::string(argv[2])) == cases.end()) {
        std::cerr << "Available cases:\n";
        for (const auto* name : cases)
            std::cerr << "  " << name << '\n';
        return 4;
    }
    const std::string name = argv[2];
    try {
        // Case selection belongs to this dispatcher, not the PETSc option database.
        int petsc_argc = 1;
        PetscSession petsc(petsc_argc, argv);
        return run(name);
    } catch (const std::exception& e) {
        std::cerr << "ERROR " << name << ' ' << e.what() << '\n';
        return 3;
    }
}
