#include "chipcae/mesh/cut/cut_stats.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace chipcae;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;

constexpr Real L = .02, H = .0016, h = .00125, h2 = .001;

struct Position {
    char family;
    std::size_t index;
    Real c;
    std::optional<Real> t;
    std::size_t cells, cuts;
};

struct Snapshot {
    std::vector<Real> sub_volfrac;
    std::vector<Local> cut_elem, small_subs;
};

template <class T>
std::string show(const T& value) {
    std::ostringstream out;
    out << std::setprecision(17);
    if constexpr (requires { out << value; }) {
        out << value;
    } else {
        out << '[';
        for (const auto& entry : value)
            out << entry << ',';
        out << ']';
    }
    return out.str();
}

struct Context {
    std::string prefix;

    template <class A, class E>
    void check(bool ok, const std::string& name, const A& actual, const E& expected) const {
        if (!ok) {
            const auto message =
                prefix + " " + name + " actual=" + show(actual) + " expected=" + show(expected);
            sp5::require(false, message.c_str());
        }
    }

    template <class A, class E>
    void equal(const std::string& name, const A& actual, const E& expected) const {
        check(actual == expected, name, actual, expected);
    }
};

Context context(const Position& p, int pass) {
    std::ostringstream out;
    out << "demo-gate small sweep: " << p.family << '[' << p.index << "] pass=" << pass
        << " c=" << std::setprecision(17) << p.c;
    return {out.str()};
}

template <class T>
T checked(Expected<T> result, const Context& ctx, const char* name) {
    if (!result) {
        const auto error = result.error().path + ": " + result.error().reason;
        std::cerr << ctx.prefix << ' ' << name << ' ' << error << '\n';
        ctx.check(false, name, error, "success");
    }
    return std::move(result.value());
}

StackModel fixture(Real c) {
    StackModel s;
    s.id = "fcbga_demo";
    s.unit = "m";
    s.symmetry = "none";
    s.package_type = "FCBGA";
    s.outline.box = {0, 0, L, L};
    s.outline.polygon = {
        {c, 0}, {L - c, 0}, {L, c}, {L, L - c}, {L - c, L}, {c, L}, {0, L - c}, {0, c}};
    for (const auto& [name, material, thickness, kind] :
         {std::tuple{"substrate", "organic_eq", .001, "Homogenized"},
          std::tuple{"bump_eq", "sac305_ball_eq", .0001, "Homogenized"},
          std::tuple{"die_layer", "underfill", .0005, "Explicit"}}) {
        Layer layer;
        layer.name = name;
        layer.material = material;
        layer.thickness = thickness;
        layer.kind = kind;
        layer.n_elem_min = 2;
        s.layers.push_back(layer);
    }
    s.layers.back().regions.push_back({"die", "silicon", "", {{.005, .005, .015, .015}, {}}});
    s.activation_groups["assembly"] = {"layer:substrate", "body:die"};
    return s;
}

HexMeshArrays background(const StackModel& stack, const Context& ctx) {
    auto rectangular = stack;
    rectangular.outline.polygon.clear();
    GridOptions options;
    options.nx = options.ny = 4;
    options.levels = 1;
    options.max_ratio = 10;
    options.ranks = 1;
    return checked(build_tensor_grid(rectangular, options), ctx, "build_tensor_grid");
}

CutGeometry outline_planes(const StackModel& stack, const HexMeshArrays& mesh, const Context& ctx) {
    const auto& bodies = mesh.attr_table.bodies;
    ctx.equal("body key count", bodies.size(), std::size_t{4});
    std::set<BodyId> ids;
    for (const auto* name : {"substrate", "bump_eq", "die_layer", "die"}) {
        ctx.check(
            bodies.contains(name), "body key " + std::string(name), bodies.contains(name), true);
        ids.insert(bodies.at(name));
    }
    ctx.equal("distinct body IDs", ids.size(), std::size_t{4});

    // Solve's signed-area and unit-outward-normal half spaces, kept local to this test.
    auto ring = stack.outline.polygon;
    if (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    Real area = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto a = ring[i], b = ring[(i + 1) % ring.size()];
        area += a[0] * b[1] - a[1] * b[0];
    }
    ctx.check(ring.size() >= 3 && std::abs(area) > 0, "outline area", area, "nonzero");
    CutGeometry geometry;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto a = ring[i], b = ring[(i + 1) % ring.size()];
        Real nx = (b[1] - a[1]) * (area > 0 ? 1 : -1);
        Real ny = (a[0] - b[0]) * (area > 0 ? 1 : -1);
        const Real length = std::hypot(nx, ny);
        ctx.check(length > 0, "outline edge length", length, ">0");
        nx /= length;
        ny /= length;
        const Real d = nx * a[0] + ny * a[1];
        for (const auto& [name, body] : bodies)
            geometry.planes.push_back({body, {nx, ny, 0}, d});
    }
    ctx.equal("plane count", geometry.planes.size(), std::size_t{32});
    return geometry;
}

std::vector<Position> positions() {
    constexpr std::array ts{
        3e-3, 3e-4, 3e-5, 3e-6, 3e-7, 3e-8, 2e-8, 5e-9, 1e-9, 1e-10, 1e-11, 1e-12, 1e-14};
    std::vector<Position> result;
    for (char family : {'A', 'C', 'B'})
        for (std::size_t i = 0; i < ts.size(); ++i) {
            const Real t = ts[i];
            if (family == 'A')
                result.push_back({family, i, 2 * h - h * std::sqrt(2 * t), t, 1716, 96});
            else if (family == 'C')
                result.push_back({family, i, .002 - h2 * std::sqrt(2 * t), t, 1734, 72});
            else
                result.push_back({family, i, 2 * h + h * std::sqrt(2 * t), t, 1710, 120});
        }
    constexpr std::array<std::pair<std::size_t, std::size_t>, 11> counts{{{1734, 24},
                                                                          {1734, 24},
                                                                          {1734, 72},
                                                                          {1728, 66},
                                                                          {1710, 90},
                                                                          {1698, 108},
                                                                          {1674, 132},
                                                                          {1644, 150},
                                                                          {1734, 48},
                                                                          {1662, 132},
                                                                          {1620, 180}}};
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const Real c =
            i < 8 ? .0005 * static_cast<Real>(i + 1) : std::array{.00125, .00375, .0045}[i - 8];
        result.push_back({'R', i, c, std::nullopt, counts[i].first, counts[i].second});
    }
    const auto ctx = context(result.front(), 0);
    ctx.equal("position count", result.size(), std::size_t{50});
    return result;
}

Snapshot sweep_position(const Position& p, int pass, Real& max_volume_error, Real& max_vf_error) {
    const auto ctx = context(p, pass);
    const auto stack = fixture(p.c);
    auto mesh = background(stack, ctx);
    const bool baseline = p.family == 'R' && p.index == 3;
    const auto original_gids = baseline ? mesh.elem_gid : std::vector<Index>{};
    const auto geometry = outline_planes(stack, mesh, ctx);
    CutOptions options;
    options.export_polyhedra = true;
    options.keep_void = false;
    const auto cut = checked(build_cut_layer(mesh, geometry, options), ctx, "build_cut_layer");
    const auto stats = checked(cut_stats(mesh, cut), ctx, "cut_stats");

    const Real volume = (L * L - 2 * p.c * p.c) * H;
    const Real volume_error = std::abs(stats.solid_volume - volume) / volume;
    max_volume_error = std::max(max_volume_error, volume_error);
    ctx.check(volume_error <= 1e-10,
              "volume relative error (solid_volume=" + show(stats.solid_volume) +
                  ", analytical=" + show(volume) + ")",
              volume_error,
              "<=1e-10");
    ctx.equal("total cells", stats.total_cells, p.cells);
    ctx.equal("cut cells", stats.cut_cells, p.cuts);
    ctx.equal("mesh element count", stats.total_cells, mesh.elem_gid.size());
    ctx.equal("mesh attribute count", stats.total_cells, mesh.attribute.size());
    ctx.equal("mesh connectivity count", mesh.hex.size(), 8 * stats.total_cells);
    ctx.equal("cut array count", stats.cut_cells, cut.cut_elem.size());
    ctx.equal("subcell count", stats.sub_cells, cut.sub_volfrac.size());
    ctx.equal("subcell attribute count", stats.sub_cells, cut.sub_attribute.size());
    ctx.equal("void count", stats.void_cells, cut.void_elem.size());
    ctx.equal(
        "cell partition", stats.full_cells + stats.cut_cells + stats.void_cells, stats.total_cells);
    ctx.equal("recorded small count", stats.small_subs_recorded, cut.small_subs.size());

    std::array<std::size_t, 4> bins{};
    std::set<std::size_t> independent_small, recorded_small;
    for (std::size_t i = 0; i < cut.sub_volfrac.size(); ++i) {
        const Real vf = cut.sub_volfrac[i];
        ctx.check(std::isfinite(vf) && vf > 0, "positive finite vf[" + show(i) + "]", vf, ">0");
        if (vf < 1e-8)
            independent_small.insert(i);
        for (std::size_t b = 0; b < bins.size(); ++b)
            if (vf < kSmallVfBins[b])
                ++bins[b];
    }
    for (auto local : cut.small_subs) {
        ctx.check(local >= 0 && static_cast<std::size_t>(local) < cut.sub_volfrac.size(),
                  "small_subs index",
                  local,
                  "[0," + show(cut.sub_volfrac.size()) + ")");
        const auto index = static_cast<std::size_t>(local);
        ctx.check(recorded_small.insert(index).second, "unique small_subs index", index, "unique");
    }
    ctx.equal("small_subs set", recorded_small, independent_small);
    ctx.equal("independent vf bins", stats.small_vf_cumulative, bins);

    if (p.family == 'A' || p.family == 'C') {
        ctx.check(stats.min_sub_volfrac.has_value(),
                  "minimum vf present",
                  stats.min_sub_volfrac.has_value(),
                  true);
        const Real delta = p.family == 'A' ? 2 * h - p.c : .002 - p.c;
        const Real width = p.family == 'A' ? h : h2;
        const Real analytical = delta * delta / (2 * width * width);
        const Real error = std::abs(*stats.min_sub_volfrac - analytical) / analytical;
        max_vf_error = std::max(max_vf_error, error);
        ctx.check(error <= 1e-6,
                  "minimum vf relative error (actual_vf=" + show(*stats.min_sub_volfrac) +
                      ", analytical=" + show(analytical) + ")",
                  error,
                  "<=1e-6");
        const auto fragments = std::count_if(
            cut.sub_volfrac.begin(), cut.sub_volfrac.end(), [&](Real vf) { return vf < 2 * *p.t; });
        ctx.equal("vf<2t fragment count", fragments, 6);
        ctx.equal("CSR offset count", cut.sub_off.size(), cut.cut_elem.size() + 1);
        std::vector<Index> parents;
        for (std::size_t row = 0; row < cut.cut_elem.size(); ++row) {
            const auto local = cut.cut_elem[row];
            ctx.check(local >= 0 && static_cast<std::size_t>(local) < mesh.elem_gid.size(),
                      "fragment parent index",
                      local,
                      "valid post-cut mesh index");
            const auto begin = cut.sub_off[row], end = cut.sub_off[row + 1];
            ctx.check(begin >= 0 && end >= begin &&
                          static_cast<std::size_t>(end) <= cut.sub_volfrac.size(),
                      "CSR range",
                      std::array{begin, end},
                      "ordered in sub_volfrac bounds");
            for (auto sub = static_cast<std::size_t>(begin); sub < static_cast<std::size_t>(end);
                 ++sub)
                if (cut.sub_volfrac[sub] < 2 * *p.t)
                    parents.push_back(mesh.elem_gid[static_cast<std::size_t>(local)]);
        }
        std::sort(parents.begin(), parents.end());
        const std::vector<Index> expected =
            p.family == 'A' ? std::vector<Index>{0, 1, 2, 3, 4, 5}
                            : std::vector<Index>{1728, 1729, 1730, 1731, 1732, 1733};
        ctx.equal("fragment parent GIDs", parents, expected);
        for (std::size_t b = 0; b < bins.size(); ++b)
            ctx.equal("analytical vf bin[" + show(b) + "]",
                      stats.small_vf_cumulative[b],
                      std::size_t{kSmallVfBins[b] > *p.t ? 6U : 0U});
        ctx.equal("analytical small_subs count",
                  cut.small_subs.size(),
                  std::size_t{*p.t < 1e-8 ? 6U : 0U});
        if (p.family == 'C')
            ctx.equal("maximum retained GID",
                      *std::max_element(mesh.elem_gid.begin(), mesh.elem_gid.end()),
                      Index{1733});
    } else {
        ctx.equal("B/R vf bins", stats.small_vf_cumulative, std::array<std::size_t, 4>{});
    }

    if (baseline) {
        ctx.check(stats.min_sub_volfrac.has_value(),
                  "baseline minimum vf present",
                  stats.min_sub_volfrac.has_value(),
                  true);
        const Real error = std::abs(*stats.min_sub_volfrac - .025) / .025;
        ctx.check(error <= 1e-12,
                  "baseline minimum vf relative error (actual_vf=" + show(*stats.min_sub_volfrac) +
                      ")",
                  error,
                  "<=1e-12 against 0.025");
        ctx.equal("baseline small_subs count", cut.small_subs.size(), std::size_t{0});
        ctx.equal("baseline cut count", stats.cut_cells, std::size_t{66});
        const std::set<Index> retained(mesh.elem_gid.begin(), mesh.elem_gid.end());
        std::vector<Index> removed;
        for (auto gid : original_gids)
            if (!retained.contains(gid))
                removed.push_back(gid);
        std::sort(removed.begin(), removed.end());
        ctx.equal("baseline removed GIDs",
                  removed,
                  std::vector<Index>{1728, 1729, 1730, 1731, 1732, 1733});
        std::cout << ctx.prefix << " measured baseline bins=" << show(stats.small_vf_cumulative)
                  << " min_vf=" << std::setprecision(17) << *stats.min_sub_volfrac << '\n';
    }
    return {cut.sub_volfrac, cut.cut_elem, cut.small_subs};
}
} // namespace

int main() {
    return sp5::run([] {
        const auto sweep = positions();
        std::vector<Snapshot> first;
        Real max_volume_error = 0, max_vf_error = 0;
        for (int pass = 1; pass <= 2; ++pass)
            for (std::size_t i = 0; i < sweep.size(); ++i) {
                auto current = sweep_position(sweep[i], pass, max_volume_error, max_vf_error);
                if (pass == 1) {
                    first.push_back(std::move(current));
                    continue;
                }
                const auto ctx = context(sweep[i], pass);
                ctx.equal("repeat cut_elem", current.cut_elem, first[i].cut_elem);
                ctx.equal("repeat small_subs", current.small_subs, first[i].small_subs);
                ctx.equal(
                    "repeat vf size", current.sub_volfrac.size(), first[i].sub_volfrac.size());
                for (std::size_t sub = 0; sub < current.sub_volfrac.size(); ++sub)
                    ctx.equal("repeat vf bits[" + show(sub) + "]",
                              std::bit_cast<std::uint64_t>(current.sub_volfrac[sub]),
                              std::bit_cast<std::uint64_t>(first[i].sub_volfrac[sub]));
            }
        std::cout << std::setprecision(17)
                  << "demo-gate small sweep: measured 100 fresh grids/cuts; max_volume_error="
                  << max_volume_error << " max_A/C_min_vf_error=" << max_vf_error << '\n';
    });
}
