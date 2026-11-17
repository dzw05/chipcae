#include "chipcae/mesh/cut/plane_cut.hpp"
#include "support.hpp"

#include <algorithm>
#include <bit>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
using k1::expect;
// Independent analytic CDF of a positive weighted sum of three uniform variables.
long double exact(std::array<double, 3> n, double d) {
    long double sum = 0;
    for (int mask = 0; mask < 8; ++mask) {
        long double t = d;
        int sign = 1;
        for (int k = 0; k < 3; ++k)
            if (mask & (1 << k)) {
                t -= n[k];
                sign = -sign;
            }
        if (t > 0)
            sum += sign * t * t * t;
    }
    return sum / (6 * static_cast<long double>(n[0]) * n[1] * n[2]);
}
// Independent topology and signed-divergence checks on exported cells.
void check_poly(const CutPolyhedra& p, double snap) {
    using V = std::array<double, 3>;
    auto point = [&](chipcae::Index id) {
        return V{p.points[3 * id], p.points[3 * id + 1], p.points[3 * id + 2]};
    };
    auto sub = [](V a, V b) { return V{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
    auto cross = [](V a, V b) {
        return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto dot = [](V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    for (std::size_t i = 0; i < p.volume.size(); ++i) {
        std::map<std::pair<chipcae::Index, chipcae::Index>, int> edges;
        std::set<chipcae::Index> ids;
        for (auto f = p.poly_off[i]; f < p.poly_off[i + 1]; ++f) {
            auto face = p.poly_faces[f];
            for (auto j = p.face_off[face]; j < p.face_off[face + 1]; ++j)
                ids.insert(p.face_conn[j]);
        }
        V center{};
        for (auto id : ids) {
            auto x = point(id);
            for (int d = 0; d < 3; ++d)
                center[d] += x[d] / ids.size();
        }
        double volume = 0;
        for (auto a : ids)
            for (auto b : ids)
                if (a < b) {
                    auto delta = sub(point(a), point(b));
                    expect(dot(delta, delta) > snap * snap, "canonical points separated");
                }
        for (auto f = p.poly_off[i]; f < p.poly_off[i + 1]; ++f) {
            auto face = p.poly_faces[f], begin = p.face_off[face], end = p.face_off[face + 1];
            std::set<chipcae::Index> ring;
            V area{};
            auto a = point(p.face_conn[begin]);
            for (auto j = begin; j < end; ++j) {
                auto u = p.face_conn[j], v = p.face_conn[j + 1 == end ? begin : j + 1];
                expect(ring.insert(u).second, "simple face ring");
                ++edges[{u, v}];
            }
            for (auto j = begin + 1; j + 1 < end; ++j) {
                auto b = point(p.face_conn[j]), c = point(p.face_conn[j + 1]);
                auto n = cross(sub(b, a), sub(c, a));
                for (int d = 0; d < 3; ++d)
                    area[d] += n[d] / 2;
                volume += dot(sub(a, center), cross(sub(b, center), sub(c, center))) / 6;
            }
            expect(std::sqrt(dot(area, area)) >= snap * snap, "nondegenerate face");
            expect(dot(area, sub(a, center)) > 0, "outward face normal");
        }
        for (const auto& [edge, count] : edges)
            expect(count == 1 && edges[{edge.second, edge.first}] == 1, "opposite edge pairing");
        expect(volume > 0 && std::abs(volume - p.volume[i]) < 1e-12,
               "positive signed divergence volume");
        expect(std::abs(p.volume[i] - p.volfrac_ref[i]) < 1e-12,
               "export matches quadrature fraction");
    }
}
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    auto s = k1::stack();
    s.activation_groups.clear();
    s.layers.resize(1);
    s.layers[0].thickness = 1;
    s.layers[0].n_elem_min = 1;
    s.layers[0].regions.clear();
    auto o = k1::options();
    auto grid = build_tensor_grid(s, o);
    expect(bool(grid), "unit grid");
    const auto original = grid.value();
    CutOptions options;
    options.export_polyhedra = true;
    options.snap_phi = k1::threshold("snap_phi");
    options.small_sub_vf = k1::threshold("small_sub_vf");
    {
        auto r = build_cut_layer(original, {{{0, {1, 1, 0}, 1}}}, options);
        expect(bool(r), "45 degree cut");
        auto& p = *r.value().polyhedra;
        expect(p.points.size() == 18 && p.face_off.size() == 6,
               "triangular prism: six vertices five faces");
        expect(std::abs(p.volume[0] - .5) / .5 <= 1e-12, "analytic prism volume");
        check_poly(p, options.snap_phi);
        for (const auto& plane :
             std::vector<Plane>{{0, {1, 1, 1}, 1e-13}, {0, {1, 0, 0}, 1}, {0, {1, 1, 0}, 0}}) {
            auto edge = build_cut_layer(original, {{plane}}, options);
            expect(bool(edge), "grazing cut");
            check_poly(*edge.value().polyhedra, options.snap_phi);
        }
    }
    std::mt19937_64 rng(20261116);
    std::uniform_real_distribution<double> uniform(.2, 1);
    std::size_t cut_count = 0;
    double max_conservation_error = 0, max_fraction_error = 0;
    for (int trial = 0; trial < 1006; ++trial) {
        std::array<double, 3> n{uniform(rng), uniform(rng), uniform(rng)};
        double d = (n[0] + n[1] + n[2]) * uniform(rng);
        if (trial >= 1000) {
            n = {1, 1, 1};
            d = trial == 1000   ? 0
                : trial == 1001 ? 1
                : trial == 1002 ? 2
                : trial == 1003 ? 3
                : trial == 1004 ? std::sqrt(options.small_sub_vf)
                                : 1;
        }
        if (trial == 1005)
            n = {1, 0, 0};
        auto plane_n = n;
        auto plane_d = d;
        if (trial < 200)
            for (int axis = 0; axis < 3; ++axis)
                if (rng() & 1) {
                    plane_n[axis] = -n[axis];
                    plane_d -= n[axis];
                }
        // Reflect selected cube axes to exercise all normal octants while
        // retaining the independent positive-weight analytic CDF.
        CutGeometry g{{{0, plane_n, plane_d}}};
        auto r = build_cut_layer(original, g, options);
        expect(bool(r), "plane cut");
        const auto& c = r.value();
        check_poly(*c.polyhedra, options.snap_phi);
        double expected = trial == 1005 ? 1 : static_cast<double>(exact(n, d));
        double v = c.void_elem.empty() ? 1 : 0;
        if (!c.cut_elem.empty()) {
            ++cut_count;
            v = std::accumulate(c.vq_w.begin(), c.vq_w.end(), 0.0);
            double scale = expected;
            expect(std::abs(v - expected) <= k1::threshold("cut_volume_relative") * scale,
                   "G1 analytic volume");
            auto closure = c.exterior_area_vector[0];
            for (std::size_t q = 0; q < c.bq_w.size(); ++q)
                for (int k = 0; k < 3; ++k)
                    closure[k] += c.bq_n[k][q] * c.bq_w[q];
            expect(std::sqrt(
                       std::inner_product(closure.begin(), closure.end(), closure.begin(), 0.0)) <=
                       6 * k1::threshold("closure_relative"),
                   "G1 closure norm");
            if (trial == 1004)
                expect(!c.small_subs.empty() && v > 0, "small sliver retained and marked");
            for (const auto& x : c.vq_xi)
                for (auto p : x)
                    expect(p >= 0 && p <= 1, "reference coordinates");
            double moment = 0;
            for (std::size_t q = 0; q < c.vq_w.size(); ++q)
                moment += c.vq_w[q] * c.vq_xi[0][q] * c.vq_xi[0][q];
            expect(moment >= 0 && moment <= v, "positive degree-two rule");
            expect(std::is_sorted(c.vq_off.begin(), c.vq_off.end()) &&
                       std::is_sorted(c.bq_off.begin(), c.bq_off.end()),
                   "CSR monotone");
        } else
            expect(std::abs(v - expected) <= k1::threshold("cut_volume_relative"),
                   "degenerate volume");
        if (trial < 200) {
            max_conservation_error = std::max(max_conservation_error, std::abs(v - expected));
            expect(std::abs(v - expected) <= 1e-12,
                   "200 random conservation against analytic void");
            const auto& poly = *c.polyhedra;
            expect(poly.volume.size() == c.sub_volfrac.size(), "every random cut exported");
            for (std::size_t i = 0; i < poly.volume.size(); ++i) {
                max_fraction_error =
                    std::max(max_fraction_error, std::abs(poly.volume[i] - c.sub_volfrac[i]));
                expect(std::abs(poly.volume[i] - c.sub_volfrac[i]) <= 1e-12,
                       "random fraction agreement");
            }
            auto disabled = options;
            disabled.export_polyhedra = false;
            auto legacy = build_cut_layer(original, g, disabled);
            expect(bool(legacy) && !legacy.value().polyhedra &&
                       legacy.value().sub_volfrac == c.sub_volfrac &&
                       legacy.value().vq_w == c.vq_w && legacy.value().bq_w == c.bq_w &&
                       legacy.value().vq_xi == c.vq_xi && legacy.value().bq_xi == c.bq_xi,
                   "export does not change existing numerics");
        }
        auto again = build_cut_layer(original, g, options);
        expect(again.value().vq_w == c.vq_w && again.value().bq_w == c.bq_w, "cut replay");
    }
    {
        auto stack = k1::stack();
        auto opts = k1::options();
        auto source = build_tensor_grid(stack, opts).value();
        auto layered = source;
        auto cut = build_cut_layer(layered, {{{0, {1, 1, 0}, .8}}}, options);
        expect(bool(cut), "layered interface cut");
        for (auto attr : cut.value().bq_bdr_attr) {
            expect(attr > 6, "dedicated EB attribute");
            bool registered = false;
            for (const auto& [name, list] : layered.face_sets)
                if (name.starts_with("interface:") && name.ends_with("|void") &&
                    std::find(list.begin(), list.end(), attr) != list.end())
                    registered = true;
            expect(registered, "registered EB face group");
        }
        expect(!layered.face_sets.contains("exposed:void"), "no exposed:void group");
        chipcae::Index aligned = 0;
        bool listed = false;
        for (const auto& r : layered.preparation_report)
            if (r.name.ends_with("|void")) {
                aligned += r.aligned_faces;
                listed = listed || !r.side_a_bodies.empty();
            }
        expect(aligned > 0, "aligned exposed faces counted");
        expect(listed, "void interface lists the solid side");
        for (std::size_t n = 0; n < layered.node_gid.size(); ++n) {
            auto it =
                std::find(source.node_gid.begin(), source.node_gid.end(), layered.node_gid[n]);
            expect(it != source.node_gid.end(), "original node GID retained");
            auto old = static_cast<std::size_t>(it - source.node_gid.begin());
            for (int d = 0; d < 3; ++d)
                expect(layered.vertices[3 * n + d] == source.vertices[3 * old + d],
                       "node GID geometry retained");
        }
        auto eb = resolve_cut_activation(stack, layered, cut.value(), {"interface:body:base|void"});
        auto reversed =
            resolve_cut_activation(stack, layered, cut.value(), {"interface:void|body:base"});
        expect(bool(eb) && bool(reversed) && !eb.value().faces.eb_subs.empty() &&
                   eb.value().faces.bdr_attributes == reversed.value().faces.bdr_attributes,
               "canonical embedded interface");
    }
    auto split = o;
    split.nx = 4;
    split.ny = 4;
    auto mesh = build_tensor_grid(s, split).value();
    auto r = build_cut_layer(mesh, {{{0, {1, 1, 0}, 1}}}, options);
    expect(bool(r), "pruned cut");
    auto& c = r.value();
    expect(mesh.attribute.size() < 16, "void cells removed");
    const auto& poly = *c.polyhedra;
    for (std::size_t v = 0; v < poly.point_parent_elem.size(); ++v) {
        auto e = poly.point_parent_elem[v];
        expect(e >= 0 && static_cast<std::size_t>(e) < mesh.attribute.size(),
               "remapped parent local ID");
        for (int d = 0; d < 3; ++d) {
            auto lo = mesh.vertices[3 * mesh.hex[8 * e] + d];
            auto hi = mesh.vertices[3 * mesh.hex[8 * e + 6] + d];
            expect(std::abs(lo + poly.point_xi[3 * v + d] * (hi - lo) - poly.points[3 * v + d]) <=
                       1e-14,
                   "reference interpolation location survives compaction");
        }
    }
    for (std::size_t i = 0; i < poly.volume.size(); ++i) {
        expect(std::abs(poly.volume[i] / (.25 * .25) - c.sub_volfrac[i]) <= 1e-12,
               "nonunit parent volume fraction");
        expect(poly.parent_elem_gid[i] == c.cut_elem_gid[i], "parent GID survives compaction");
    }
    std::set<Local> attrs(mesh.attribute.begin(), mesh.attribute.end());
    expect(*attrs.begin() == 1, "compact after void removal");
    auto cache = GhostPenaltyCache::prepare(mesh, c, {{1}, {}});
    expect(bool(cache), "cache prepare");
    auto all = cache.value().lookup({1});
    auto expected_hash = mask_hash(activated_gids(mesh, {1}));
    expect(std::find(cache.value().hashes.begin(), cache.value().hashes.end(), expected_hash) !=
               cache.value().hashes.end(),
           "cache hashes activated element GIDs");
    expect(bool(all) && bool(cache.value().lookup({})), "two mask hits");
    expect(!cache.value().lookup({999}), "unbudgeted mask");
    std::size_t brute = 0;
    std::set<Local> cuts(c.cut_elem.begin(), c.cut_elem.end());
    for (std::size_t a = 0; a < mesh.attribute.size(); ++a)
        for (std::size_t b = a + 1; b < mesh.attribute.size(); ++b) {
            int common = 0;
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j)
                    common += mesh.hex[8 * a + i] == mesh.hex[8 * b + j];
            if (common == 4 &&
                (cuts.contains(static_cast<Local>(a)) || cuts.contains(static_cast<Local>(b))))
                ++brute;
        }
    expect(all.value().face.size() == brute, "GP brute enumeration");
    std::vector<bool> used(mesh.node_gid.size());
    for (auto v : mesh.hex)
        used[v] = true;
    expect(std::all_of(used.begin(), used.end(), [](bool v) { return v; }),
           "no unreferenced vertices");
    for (auto gid : mesh.node_gid)
        expect(std::find(original.node_gid.begin(), original.node_gid.end(), gid) !=
                       original.node_gid.end() ||
                   gid >= 0,
               "retained node GID");
    for (std::size_t i = 0; i < all.value().face_gid.size(); ++i) {
        expect(all.value().elem_gid[0][i] < all.value().elem_gid[1][i], "GP ordered GIDs");
        expect(all.value().face_minus[i] >= 0 && all.value().face_minus[i] < 6 &&
                   all.value().face_plus[i] >= 0 && all.value().face_plus[i] < 6,
               "MFEM local face numbers");
    }
    expect(mask_hash({1, 2, 1}) == mask_hash({2, 1}) && mask_hash({1, 2}) != mask_hash({1, 3}),
           "hash unique GID set");
    auto face_device = to_device<chipcae::HostSpace>(all.value());
    expect(face_device.face_gid.extent(0) == all.value().face.size(), "GP flat views");
    auto device = to_device<chipcae::HostSpace>(c);
    expect(device.ls_phi.extent(0) == c.level_sets.size() * mesh.node_gid.size(),
           "flat level sets");
    expect(device.vq_w.size() == c.vq_w.size(), "A1 device mirror");
    auto activation = resolve_cut_activation(s, mesh, c, {"layer:base"});
    expect(bool(activation) && activation.value().cells.cut_subs.size() == c.cut_elem.size(),
           "cut activation");
    auto fillet = build_cut_layer(original, {{{0, {1, 0, 0}, .8}, {0, {0, 1, 0}, .7}}}, options);
    expect(bool(fillet), "piecewise planar fillet");
    check_poly(*fillet.value().polyhedra, options.snap_phi);
    expect(std::abs(fillet.value().sub_volfrac[0] - .56) <= k1::threshold("cut_volume_relative"),
           "multi-plane intersection");
    auto tetra = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, options);
    double quadratic = 0;
    for (std::size_t q = 0; q < tetra.value().vq_w.size(); ++q)
        quadratic += tetra.value().vq_w[q] * std::pow(tetra.value().vq_xi[0][q], 2);
    expect(std::abs(quadratic - 1.0 / 60) <= k1::threshold("cut_volume_relative") / 60,
           "degree two tetra exactness");
    auto factorial = [](int n) {
        double x = 1;
        for (int i = 2; i <= n; ++i)
            x *= i;
        return x;
    };
    for (int degree : {2, 4, 6, 8}) {
        auto high = options;
        high.quad_degree = degree;
        auto cut = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, high);
        expect(bool(cut), "higher degree cut");
        for (int a = 0; a <= degree; ++a)
            for (int b = 0; b + a <= degree; ++b)
                for (int d = 0; d + a + b <= degree; ++d) {
                    double vol = 0, surf = 0;
                    for (std::size_t q = 0; q < cut.value().vq_w.size(); ++q)
                        vol += cut.value().vq_w[q] * std::pow(cut.value().vq_xi[0][q], a) *
                               std::pow(cut.value().vq_xi[1][q], b) *
                               std::pow(cut.value().vq_xi[2][q], d);
                    for (std::size_t q = 0; q < cut.value().bq_w.size(); ++q)
                        surf += cut.value().bq_w[q] * std::pow(cut.value().bq_xi[0][q], a) *
                                std::pow(cut.value().bq_xi[1][q], b) *
                                std::pow(cut.value().bq_xi[2][q], d);
                    double ev =
                        factorial(a) * factorial(b) * factorial(d) / factorial(a + b + d + 3);
                    double es = std::sqrt(3.) * factorial(a) * factorial(b) * factorial(d) /
                                factorial(a + b + d + 2);
                    expect(std::abs(vol - ev) <= 1e-12 * ev, "volume polynomial exactness");
                    expect(std::abs(surf - es) <= 1e-12 * es, "interface polynomial exactness");
                }
    }
    auto default_cut = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, CutOptions{});
    // (1,2,3) yields pentagons away from d=3; its largest component equals
    // the sum of the others, so use (1,2,2.5) as well for a true hexagon.
    for (auto normal : {std::array<double, 3>{1, 2, 3}, std::array<double, 3>{1, 2, 2.5}}) {
        double offset = normal[2] == 3 ? 2.5 : 2.75;
        int triangles = normal[2] == 3 ? 3 : 4;
        for (int degree : {2, 4, 6, 8}) {
            auto opts = options;
            opts.quad_degree = degree;
            std::array<CutLayerArrays, 2> sides;
            std::array<double, 2> areas{};
            for (int side = 0; side < 2; ++side) {
                double sign = side == 0 ? 1 : -1;
                auto sided_normal = normal;
                for (auto& x : sided_normal)
                    x *= sign;
                auto mesh_copy = original;
                auto result =
                    build_cut_layer(mesh_copy, {{{0, sided_normal, sign * offset}}}, opts);
                expect(bool(result), "oblique cut");
                sides[side] = result.value();
                const auto& c = sides[side];
                int n = degree / 2 + 1;
                int nv = degree == 2 ? 4 : n * n * n;
                int nb = degree == 2 ? 3 : n * n;
                int tets = normal[2] == 3 && side == 0 ? 12 : 16;
                expect(c.vq_w.size() == std::size_t(tets * nv), "point count per fan tetrahedron");
                expect(c.bq_w.size() == std::size_t(triangles * nb),
                       "point count per interface triangle");
                for (auto w : c.vq_w)
                    expect(w > 0, "positive volume weights");
                for (auto w : c.bq_w)
                    expect(w > 0, "positive boundary weights");
                for (const auto& x : c.vq_xi)
                    for (auto v : x)
                        expect(v > 0 && v < 1, "strictly interior volume points");
                for (const auto& x : c.bq_xi)
                    for (auto v : x)
                        expect(v > 0 && v < 1, "interface points inside cube");
                for (std::size_t sub = 0; sub < c.sub_volfrac.size(); ++sub) {
                    double volume = std::accumulate(
                        c.vq_w.begin() + c.vq_off[sub], c.vq_w.begin() + c.vq_off[sub + 1], 0.0);
                    expect(std::abs(volume - c.sub_volfrac[sub]) <= 1e-12 * c.sub_volfrac[sub],
                           "subcell weight/volume agreement");
                    double area = std::accumulate(
                        c.bq_w.begin() + c.bq_off[sub], c.bq_w.begin() + c.bq_off[sub + 1], 0.0);
                    const auto& av = c.exterior_area_vector[sub];
                    double geometric_area =
                        std::sqrt(std::inner_product(av.begin(), av.end(), av.begin(), 0.0));
                    expect(std::abs(area - geometric_area) <= 1e-12 * geometric_area,
                           "subcell weight/geometric interface area agreement");
                    areas[side] += area;
                }
                double reported_area = 0;
                for (const auto& report : mesh_copy.preparation_report)
                    reported_area += report.cut_area;
                expect(std::abs(reported_area - areas[side]) <= 1e-12 * areas[side],
                       "reported interface area agreement");
            }
            for (int a = 0; a <= degree; ++a)
                for (int b = 0; a + b <= degree; ++b)
                    for (int z = 0; a + b + z <= degree; ++z) {
                        double volume = 0;
                        std::array<double, 2> surface{};
                        for (int side = 0; side < 2; ++side) {
                            const auto& c = sides[side];
                            for (std::size_t q = 0; q < c.vq_w.size(); ++q)
                                volume += c.vq_w[q] * std::pow(c.vq_xi[0][q], a) *
                                          std::pow(c.vq_xi[1][q], b) * std::pow(c.vq_xi[2][q], z);
                            for (std::size_t q = 0; q < c.bq_w.size(); ++q)
                                surface[side] += c.bq_w[q] * std::pow(c.bq_xi[0][q], a) *
                                                 std::pow(c.bq_xi[1][q], b) *
                                                 std::pow(c.bq_xi[2][q], z);
                        }
                        double analytic = 1.0 / ((a + 1) * (b + 1) * (z + 1));
                        expect(std::abs(volume - analytic) <= 1e-12 * analytic,
                               "complementary oblique monomial volume");
                        expect(std::abs(surface[0] - surface[1]) <= 1e-12 * surface[0],
                               "complementary oblique monomial surface");
                    }
        }
    }
    auto explicit_2 = CutOptions{};
    explicit_2.quad_degree = 2;
    auto legacy_path = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, explicit_2);
    for (int k = 0; k < 3; ++k)
        for (std::size_t q = 0; q < default_cut.value().vq_xi[k].size(); ++q)
            expect(std::bit_cast<std::uint64_t>(default_cut.value().vq_xi[k][q]) ==
                       std::bit_cast<std::uint64_t>(legacy_path.value().vq_xi[k][q]),
                   "default point bits");
    for (std::size_t q = 0; q < default_cut.value().vq_w.size(); ++q)
        expect(std::bit_cast<std::uint64_t>(default_cut.value().vq_w[q]) ==
                   std::bit_cast<std::uint64_t>(legacy_path.value().vq_w[q]),
               "default weight bits");
    for (int k = 0; k < 3; ++k)
        for (std::size_t q = 0; q < default_cut.value().bq_xi[k].size(); ++q)
            expect(std::bit_cast<std::uint64_t>(default_cut.value().bq_xi[k][q]) ==
                       std::bit_cast<std::uint64_t>(legacy_path.value().bq_xi[k][q]),
                   "default boundary point bits");
    for (std::size_t q = 0; q < default_cut.value().bq_w.size(); ++q)
        expect(std::bit_cast<std::uint64_t>(default_cut.value().bq_w[q]) ==
                   std::bit_cast<std::uint64_t>(legacy_path.value().bq_w[q]),
               "default boundary weight bits");
    auto retained = build_tensor_grid(s, split).value();
    options.keep_void = true;
    auto kept = build_cut_layer(retained, {{{0, {1, 1, 0}, 1}}}, options);
    expect(bool(kept) && retained.attribute.size() == 16 && kept.value().void_attribute > 0,
           "keep void");
    expect(kept.value().polyhedra->volume.size() == kept.value().cut_elem.size(),
           "retained voids do not emit empty solids");
    for (auto kind : kept.value().polyhedra->cell_kind)
        expect(kind == 1, "only solid cuts have exported geometry");
    expect(std::is_sorted(cache.value().hashes.begin(), cache.value().hashes.end()),
           "cache hashes ordered");
    expect(c.gp_face.size() == brute, "default GP arrays");
    std::cout << "P1 random200 max_conservation_error=" << max_conservation_error
              << " max_fraction_error=" << max_fraction_error << '\n';
    std::cout << "G1 random=1000 degeneracies=6 cut-element ratio=" << double(cut_count) / 1006
              << " GP=" << brute << '\n';
}
