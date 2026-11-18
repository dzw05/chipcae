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
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace chipcae;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using chipcae::preview::detail::digest;

constexpr const char* uncut_sha =
    "7458a8c5304bb192bd758dfc2d0989a92c4fb666381fc8a9018f205ac991005a";
constexpr const char* cut_sha = "efc79a629779602538239635706a9f4293e897f8e46b3c2aeb43c22c6b6b9f4e";

template <class T>
T checked(Expected<T> result, const char* message) {
    if (!result)
        std::cerr << result.error().path << ": " << result.error().reason << '\n';
    sp5::require(static_cast<bool>(result), message);
    return std::move(result.value());
}

StackModel fixture() {
    StackModel s;
    s.id = "fcbga_demo";
    s.unit = "m";
    s.symmetry = "none";
    s.package_type = "FCBGA";
    s.outline.box = {0, 0, .02, .02};
    s.outline.polygon = {{.002, 0},
                         {.018, 0},
                         {.02, .002},
                         {.02, .018},
                         {.018, .02},
                         {.002, .02},
                         {0, .018},
                         {0, .002}};
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

HexMeshArrays background(const StackModel& stack) {
    auto rectangular = stack;
    rectangular.outline.polygon.clear();
    GridOptions options;
    options.nx = options.ny = 4;
    options.levels = 1;
    options.max_ratio = 10;
    options.ranks = 1;
    return checked(build_tensor_grid(rectangular, options),
                   "demo-gate void digest: background construction failed");
}

CutGeometry outline_planes(const StackModel& stack, const HexMeshArrays& mesh) {
    // Reproduce apps/chipcae-solve/src/mesh_setup.cpp:32-63, without apps dependencies.
    auto ring = stack.outline.polygon;
    CutGeometry geometry;
    if (ring.empty())
        return geometry;
    if (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    double area = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        area += a[0] * b[1] - a[1] * b[0];
    }
    sp5::require(ring.size() >= 3 && area != 0,
                 "demo-gate void digest: outline must be a nondegenerate polygon");
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        double nx = (b[1] - a[1]) * (area > 0 ? 1 : -1), ny = (a[0] - b[0]) * (area > 0 ? 1 : -1);
        double length = std::hypot(nx, ny);
        sp5::require(length != 0, "demo-gate void digest: duplicate outline vertex");
        nx /= length;
        ny /= length;
        double d = nx * a[0] + ny * a[1];
        for (auto v : ring)
            sp5::require(nx * v[0] + ny * v[1] - d <= 1e-12 * length,
                         "demo-gate void digest: non-convex outline");
        for (const auto& [name, body] : mesh.attr_table.bodies)
            geometry.planes.push_back({body, {nx, ny, 0}, d});
    }
    return geometry;
}

void check_planes(const HexMeshArrays& mesh, const CutGeometry& geometry) {
    const auto& bodies = mesh.attr_table.bodies;
    sp5::require(bodies.size() == 4, "demo-gate void digest: expected four body keys");
    std::set<BodyId> ids;
    for (const auto* name : {"substrate", "bump_eq", "die_layer", "die"}) {
        sp5::require(bodies.contains(name), "demo-gate void digest: missing exact body key");
        ids.insert(bodies.at(name));
    }
    sp5::require(ids.size() == 4, "demo-gate void digest: body IDs must be distinct");
    sp5::require(geometry.planes.size() == 32, "demo-gate void digest: expected 32 planes");
    std::map<BodyId, std::size_t> counts;
    for (const auto& plane : geometry.planes) {
        sp5::require(plane.n[2] == 0, "demo-gate void digest: plane must be vertical");
        sp5::require(ids.contains(plane.body), "demo-gate void digest: unexpected plane body");
        ++counts[plane.body];
    }
    for (auto id : ids)
        sp5::require(counts[id] == 8, "demo-gate void digest: expected eight planes per body");
}

void check_digest(const HexMeshArrays& mesh, bool clipped) {
    const auto d = digest(mesh);
    std::cout << "mesh: cells=" << d.cells << " nodes=" << d.nodes << " dimensions=" << d.nx0 << ','
              << d.ny0 << ',' << d.levels << ',' << d.nz << " sha256=" << d.sha256 << '\n';
    sp5::require(d.nx0 == 17 && d.ny0 == 17 && d.levels == 0 && d.nz == 6,
                 "demo-gate void digest: digest dimensions changed");
    sp5::require(d.cells == (clipped ? 1728 : 1734) && d.nodes == (clipped ? 2261 : 2268),
                 "demo-gate void digest: mesh counts changed");
    sp5::require(d.sha256 == (clipped ? cut_sha : uncut_sha),
                 "demo-gate void digest: mesh SHA changed");
}

std::size_t node_index(const HexMeshArrays& mesh, std::size_t cell, std::size_t corner) {
    sp5::require(8 * cell + corner < mesh.hex.size(),
                 "demo-gate void digest: missing cell connectivity");
    const auto local = mesh.hex[8 * cell + corner];
    sp5::require(local >= 0 && static_cast<std::size_t>(local) < mesh.node_gid.size(),
                 "demo-gate void digest: invalid local node index");
    const auto node = static_cast<std::size_t>(local);
    sp5::require(3 * node + 2 < mesh.vertices.size(),
                 "demo-gate void digest: missing vertex coordinates");
    return node;
}

std::vector<Local> locate_voids(const HexMeshArrays& original,
                                const CutGeometry& geometry,
                                const CutOptions& options) {
    // Explicit const binding selects the overload whose indices refer to the uncut mesh.
    const HexMeshArrays uncut_copy = original;
    const auto cut = checked(build_cut_layer(uncut_copy, geometry, options),
                             "demo-gate void digest: const cut failed");
    auto removed = cut.void_elem;
    std::sort(removed.begin(), removed.end());
    sp5::require(removed.size() == 6, "demo-gate void digest: expected six void cells");
    sp5::require(std::adjacent_find(removed.begin(), removed.end()) == removed.end(),
                 "demo-gate void digest: void indices must be unique");
    std::vector<Index> gids;
    std::array<int, 6> intervals{};
    sp5::require(original.axes[2].size() == 7, "demo-gate void digest: expected seven z planes");
    for (auto local : removed) {
        sp5::require(local >= 0 && static_cast<std::size_t>(local) < original.elem_gid.size(),
                     "demo-gate void digest: invalid void cell index");
        const auto cell = static_cast<std::size_t>(local);
        gids.push_back(original.elem_gid[cell]);
        Real lo = std::numeric_limits<Real>::infinity(), hi = -lo;
        for (std::size_t corner = 0; corner < 8; ++corner) {
            const auto node = node_index(original, cell, corner);
            for (std::size_t axis = 0; axis < 2; ++axis) {
                const auto xy = original.vertices[3 * node + axis];
                sp5::require(xy >= .019 - 1e-15 && xy <= .02 + 1e-15,
                             "demo-gate void digest: void cell outside upper-right column");
            }
            lo = std::min(lo, original.vertices[3 * node + 2]);
            hi = std::max(hi, original.vertices[3 * node + 2]);
        }
        bool matched = false;
        for (std::size_t k = 0; k < intervals.size(); ++k)
            if (lo == original.axes[2][k] && hi == original.axes[2][k + 1]) {
                ++intervals[k];
                matched = true;
            }
        sp5::require(matched, "demo-gate void digest: void cell does not span one z interval");
    }
    std::sort(gids.begin(), gids.end());
    sp5::require(gids == std::vector<Index>{1728, 1729, 1730, 1731, 1732, 1733},
                 "demo-gate void digest: removed element GIDs changed");
    for (auto count : intervals)
        sp5::require(count == 1, "demo-gate void digest: void column must cover each z interval");
    return removed;
}

void check_identity(const HexMeshArrays& original,
                    const HexMeshArrays& mesh,
                    const std::vector<Local>& removed) {
    sp5::require(mesh.axes == original.axes, "demo-gate void digest: cutting changed grid axes");
    std::vector<Index> retained;
    for (std::size_t cell = 0; cell < original.elem_gid.size(); ++cell)
        if (!std::binary_search(removed.begin(), removed.end(), static_cast<Local>(cell)))
            retained.push_back(original.elem_gid[cell]);
    sp5::require(mesh.elem_gid == retained,
                 "demo-gate void digest: element GIDs must retain values and sequence");
    auto cells = mesh.elem_gid, nodes = mesh.node_gid;
    std::sort(cells.begin(), cells.end());
    std::sort(nodes.begin(), nodes.end());
    std::vector<Index> expected_cells(1728), expected_nodes(2261);
    std::iota(expected_cells.begin(), expected_cells.end(), Index{0});
    std::iota(expected_nodes.begin(), expected_nodes.end(), Index{0});
    sp5::require(cells == expected_cells, "demo-gate void digest: element GIDs must be compact");
    sp5::require(nodes == expected_nodes, "demo-gate void digest: node GIDs must be compact");
    std::map<Index, std::size_t> old_nodes, old_cells;
    for (std::size_t i = 0; i < original.node_gid.size(); ++i)
        old_nodes.emplace(original.node_gid[i], i);
    for (std::size_t i = 0; i < original.elem_gid.size(); ++i)
        old_cells.emplace(original.elem_gid[i], i);
    sp5::require(mesh.vertices.size() == 3 * mesh.node_gid.size(),
                 "demo-gate void digest: surviving vertex array size changed");
    for (std::size_t node = 0; node < mesh.node_gid.size(); ++node) {
        sp5::require(old_nodes.contains(mesh.node_gid[node]),
                     "demo-gate void digest: surviving node GID missing from original");
        const auto old = old_nodes.at(mesh.node_gid[node]);
        for (std::size_t axis = 0; axis < 3; ++axis)
            sp5::require(std::bit_cast<std::uint64_t>(mesh.vertices[3 * node + axis]) ==
                             std::bit_cast<std::uint64_t>(original.vertices[3 * old + axis]),
                         "demo-gate void digest: surviving node coordinates changed bits");
    }
    for (std::size_t cell = 0; cell < mesh.elem_gid.size(); ++cell) {
        sp5::require(old_cells.contains(mesh.elem_gid[cell]),
                     "demo-gate void digest: surviving element GID missing from original");
        const auto old = old_cells.at(mesh.elem_gid[cell]);
        for (std::size_t corner = 0; corner < 8; ++corner)
            sp5::require(mesh.node_gid[node_index(mesh, cell, corner)] ==
                             original.node_gid[node_index(original, old, corner)],
                         "demo-gate void digest: surviving cell connectivity changed");
    }
    auto original_nodes = original.node_gid;
    std::sort(original_nodes.begin(), original_nodes.end());
    std::vector<Index> deleted_nodes;
    std::set_difference(original_nodes.begin(),
                        original_nodes.end(),
                        nodes.begin(),
                        nodes.end(),
                        std::back_inserter(deleted_nodes));
    sp5::require(deleted_nodes == std::vector<Index>{2261, 2262, 2263, 2264, 2265, 2266, 2267},
                 "demo-gate void digest: deleted node GIDs changed");
    for (std::size_t k = 0; k < deleted_nodes.size(); ++k) {
        const auto old = old_nodes.at(deleted_nodes[k]);
        sp5::require(original.vertices[3 * old] == .02 && original.vertices[3 * old + 1] == .02,
                     "demo-gate void digest: deleted node must lie at upper-right corner");
        sp5::require(original.vertices[3 * old + 2] == original.axes[2][k],
                     "demo-gate void digest: deleted node z must match original axis");
    }
}

struct Pass {
    std::string sha;
    CutLayerArrays cut;
};

Pass run_pass(const StackModel& stack, bool reverse = false) {
    auto mesh = background(stack);
    check_digest(mesh, false);
    auto geometry = outline_planes(stack, mesh);
    if (stack.outline.polygon.empty()) {
        sp5::require(geometry.planes.empty(), "demo-gate void digest: empty outline has planes");
        // Match solve's empty-ring return: do not invoke either cutting overload.
        return {digest(mesh).sha256, {}};
    }
    check_planes(mesh, geometry);
    if (reverse)
        std::reverse(geometry.planes.begin(), geometry.planes.end());
    const auto original = mesh;
    CutOptions options;
    options.export_polyhedra = true;
    options.keep_void = false;
    const auto removed = locate_voids(original, geometry, options);
    // Each pass cuts its fresh background mesh exactly once through the mutable overload.
    auto cut = checked(build_cut_layer(mesh, geometry, options),
                       "demo-gate void digest: mutable cut failed");
    check_digest(mesh, true);
    sp5::require(cut.cut_elem.size() == 66, "demo-gate void digest: expected 66 cut cells");
    sp5::require(mesh.elem_gid.size() - cut.cut_elem.size() == 1662,
                 "demo-gate void digest: expected 1662 full cells");
    check_identity(original, mesh, removed);
    const auto stats = checked(cut_stats(mesh, cut), "demo-gate void digest: cut_stats failed");
    sp5::require(stats.cut_cells == 66 && stats.void_cells == 0,
                 "demo-gate void digest: cut_stats cell counts changed");
    const Real relative_error = std::abs(stats.solid_volume - 6.272e-7) / 6.272e-7;
    std::cout << std::setprecision(17) << "cut: cells=" << cut.cut_elem.size()
              << " solid_volume=" << stats.solid_volume << " relative_error=" << relative_error
              << '\n';
    sp5::require(relative_error <= 1e-10, "demo-gate void digest: solid volume changed");
    return {digest(mesh).sha256, std::move(cut)};
}

void check_repeat(const Pass& first, const Pass& second) {
    sp5::require(first.sha == second.sha, "demo-gate void digest: repeated SHA changed");
    sp5::require(first.cut.cut_elem == second.cut.cut_elem,
                 "demo-gate void digest: repeated cut element sequence changed");
    sp5::require(first.cut.sub_volfrac.size() == second.cut.sub_volfrac.size(),
                 "demo-gate void digest: repeated fraction count changed");
    for (std::size_t i = 0; i < first.cut.sub_volfrac.size(); ++i)
        sp5::require(std::bit_cast<std::uint64_t>(first.cut.sub_volfrac[i]) ==
                         std::bit_cast<std::uint64_t>(second.cut.sub_volfrac[i]),
                     "demo-gate void digest: repeated fraction bits changed");
}
} // namespace

int main() {
    return sp5::run([] {
        const auto stack = fixture();
        const auto first = run_pass(stack);
        check_repeat(first, run_pass(fixture()));
        const auto reversed = run_pass(fixture(), true);
        sp5::require(first.sha == reversed.sha,
                     "demo-gate void digest: reversed planes changed SHA");
        auto closed = fixture();
        closed.outline.polygon.push_back(closed.outline.polygon.front());
        check_repeat(first, run_pass(closed));
        auto empty = fixture();
        empty.outline.polygon.clear();
        const auto uncut = run_pass(empty);
        sp5::require(uncut.sha == uncut_sha, "demo-gate void digest: empty outline changed SHA");
    });
}
