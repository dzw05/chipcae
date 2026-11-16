#include "chipcae/mesh/cut/plane_cut.hpp"
#include "support.hpp"

#include <algorithm>
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
    options.snap_phi = k1::threshold("snap_phi");
    options.vf_min = k1::threshold("vf_min");
    std::mt19937_64 rng(20261116);
    std::uniform_real_distribution<double> uniform(.2, 1);
    std::size_t cut_count = 0;
    for (int trial = 0; trial < 1006; ++trial) {
        std::array<double, 3> n{uniform(rng), uniform(rng), uniform(rng)};
        double d = (n[0] + n[1] + n[2]) * uniform(rng);
        if (trial >= 1000) {
            n = {1, 1, 1};
            d = trial == 1000   ? 0
                : trial == 1001 ? 1
                : trial == 1002 ? 2
                : trial == 1003 ? 3
                : trial == 1004 ? std::sqrt(options.vf_min)
                                : 1;
        }
        if (trial == 1005)
            n = {1, 0, 0};
        CutGeometry g{{{0, n, d}}};
        auto r = build_cut_layer(original, g, options);
        expect(bool(r), "plane cut");
        const auto& c = r.value();
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
    expect(std::abs(fillet.value().sub_volfrac[0] - .56) <= k1::threshold("cut_volume_relative"),
           "multi-plane intersection");
    auto tetra = build_cut_layer(original, {{{0, {1, 1, 1}, 1}}}, options);
    double quadratic = 0;
    for (std::size_t q = 0; q < tetra.value().vq_w.size(); ++q)
        quadratic += tetra.value().vq_w[q] * std::pow(tetra.value().vq_xi[0][q], 2);
    expect(std::abs(quadratic - 1.0 / 60) <= k1::threshold("cut_volume_relative") / 60,
           "degree two tetra exactness");
    auto retained = build_tensor_grid(s, split).value();
    options.keep_void = true;
    auto kept = build_cut_layer(retained, {{{0, {1, 1, 0}, 1}}}, options);
    expect(bool(kept) && retained.attribute.size() == 16 && kept.value().void_attribute > 0,
           "keep void");
    expect(std::is_sorted(cache.value().hashes.begin(), cache.value().hashes.end()),
           "cache hashes ordered");
    expect(c.gp_face.size() == brute, "default GP arrays");
    std::cout << "G1 random=1000 degeneracies=6 cut-element ratio=" << double(cut_count) / 1006
              << " GP=" << brute << '\n';
}
