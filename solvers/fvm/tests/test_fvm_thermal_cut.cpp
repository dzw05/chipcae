#include "fvm_test_util.hpp"

using namespace fvm_test;

namespace {
void cut_geometry(const CutFixture& fixture) {
    const auto& m = fixture.mesh;
    const auto& cut = fixture.cut;
    auto g = take(fv::build_grid(m, &cut), "build_grid cut quadrature");
    geometry_gate(g);
    require(g.cv_volume.size() + cut.void_elem.size() == m.attribute.size(),
            "retained positive subcells and regular cells");
    double total_volume = 0, independent_volume = 0;
    for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
        const auto e = static_cast<std::size_t>(g.cv_elem[c]);
        auto v = volume(element_poly(m, e, fixture.geometry));
        near(g.cv_volume[c], v.volume, 0, 1e-12, "physical volume from signed boundary tetrahedra");
        for (int d = 0; d < 3; ++d)
            near(g.cv_centroid[c][d], v.centroid()[d], 1e-12, 0, "physical volume centroid");
        total_volume += g.cv_volume[c];
        independent_volume += v.volume;
    }
    near(total_volume, independent_volume, 0, 1e-12, "global cut volume");
    for (std::size_t i = 0; i < cut.cut_elem.size(); ++i) {
        double fractions = 0, quadrature_volume = 0;
        for (auto s = cut.sub_off[i]; s < cut.sub_off[i + 1]; ++s)
            fractions += cut.sub_volfrac[s];
        require(fractions > 0 && fractions <= 1 + 1e-14, "sum sub_volfrac <= 1+1e-14");
        const auto e = static_cast<std::size_t>(cut.cut_elem[i]);
        auto h = sub(vertex(m, e, 6), vertex(m, e, 0));
        const double jacobian = h[0] * h[1] * h[2];
        for (auto q = cut.vq_off[i]; q < cut.vq_off[i + 1]; ++q)
            quadrature_volume += jacobian * cut.vq_w[q];
        near(quadrature_volume,
             fractions * jacobian,
             0,
             1e-12,
             "reference vq weights require physical parent Jacobian");
        Vec vector{};
        for (auto q = cut.sq_off[i]; q < cut.sq_off[i + 1]; ++q)
            if (cut.sq_side[q] >= 0)
                vector[cut.sq_side[q] / 2] += (cut.sq_side[q] % 2 ? 1 : -1) * cut.sq_w[q];
        const double parent_area = std::max({h[0] * h[1], h[0] * h[2], h[1] * h[2]});
        for (int d = 0; d < 3; ++d)
            near(vector[d],
                 cut.exterior_area_vector[i][d],
                 1e-12 * parent_area,
                 0,
                 "axial area-vector sum agrees with producer exterior vector");
        for (int side = 0; side < 6; ++side) {
            double want_area = 0, actual_area = 0;
            Vec first{};
            for (auto q = cut.sq_off[i]; q < cut.sq_off[i + 1]; ++q)
                if (cut.sq_side[q] == side) {
                    want_area += cut.sq_w[q];
                    for (int d = 0; d < 3; ++d)
                        first[d] += cut.sq_w[q] * (vertex(m, e, 0)[d] + h[d] * cut.sq_xi[d][q]);
                }
            for (std::size_t c = 0; c < g.cv_volume.size(); ++c)
                if (g.cv_elem[c] == static_cast<std::int64_t>(e))
                    for (auto j = g.cv_face_off[c]; j < g.cv_face_off[c + 1]; ++j) {
                        const auto& face = g.faces[g.cv_faces[j]];
                        if (face.axis != side / 2)
                            continue;
                        auto vector_out =
                            mul(face.area_vector, face.lo == static_cast<std::int64_t>(c) ? 1 : -1);
                        if ((side % 2 ? 1 : -1) * vector_out[side / 2] <= 0)
                            continue;
                        actual_area += face.area;
                        if (want_area > 0)
                            for (int d = 0; d < 3; ++d)
                                near(face.centroid[d],
                                     first[d] / want_area,
                                     1e-12 * h[d],
                                     0,
                                     "axial opening weighted physical centroid");
                    }
            near(actual_area, want_area, 1e-12 * parent_area, 0, "axial opening physical area");
        }
    }
}

void merge_l(const std::string& pc) {
    auto m = boxes({{0, 1, 2}, {0, 1, 2}, {0, 1}});
    m.copper_layer_of_layer = {0};
    m.copper_phi = {.1, .3, .8, .9}; // dense Morton: (0,0),(1,0),(0,1),(1,1).
    mesh::cut::CutLayerArrays cut;
    for (std::size_t e = 0; e < m.attribute.size(); ++e) {
        auto lo = vertex(m, e, 0), hi = vertex(m, e, 6);
        if (lo[0] > .5 && lo[1] > .5) {
            cut.void_elem.push_back(static_cast<mesh::Local>(e));
        } else if (lo[0] > .5 || lo[1] > .5) {
            append_sub_box(cut, m, e, lo, hi);
            cut.small_subs.push_back(static_cast<mesh::Local>(cut.sub_attribute.size() - 1));
        }
    }
    // Explicitly listed leaf CVs each have just one neighbor. Both must merge
    // into the remaining box, yielding an actual concave L, never its convex hull.
    auto g = take(fv::build_grid(m, &cut), "build_grid explicitly listed merge leaves");
    require(g.cv_volume.size() == 1, "both listed subcells merge into neighbor");
    require(g.cv_kind[0] == 2, "merged kind");
    near(g.cv_volume[0], 3, 1e-12, 0, "merge keeps union volume, not convex hull");
    near(g.cv_centroid[0][0], 5. / 6, 1e-12, 0, "merged x centroid");
    near(g.cv_centroid[0][1], 5. / 6, 1e-12, 0, "merged y centroid");
    near(g.cv_centroid[0][2], .5, 1e-12, 0, "merged z centroid");
    near(g.cv_copper_fraction[0], .4, 1e-14, 0, "volume-weight pre-merge copper fractions");
    auto fractions = fv::copper_fraction(g, m);
    near(fractions.at(0), .4, 1e-14, 0, "public merged copper fraction");
    geometry_gate(g);
    require(std::none_of(g.faces.begin(), g.faces.end(), [](auto f) { return f.hi >= 0; }),
            "merged internal faces removed");
    // Merging may retain several coplanar pieces. An affine Robin datum needs
    // just each face centroid; no quadratic reference on the merged topology.
    Polynomial u{0, {.3, -.2, .1}, false};
    auto p = problem(g, {1, 2, 3}, pc);
    for (std::size_t i = 0; i < g.faces.size(); ++i) {
        const auto& face = g.faces[i];
        fv::ThermalBc b{};
        b.kind = fv::ThermalBc::Kind::Robin;
        b.target = target(face);
        b.face = i;
        b.h = 5;
        b.T_inf = dot(u.linear, face.centroid) - exact_flux(face, u, p.k[0]) / (b.h * face.area);
        p.bcs.push_back(b);
    }
    auto r = solve(g, m, p);
    near(r.T_cell[0], u.cell(l_prism()), 1e-12, 0, "merged concave affine patch");
    // Rebuilding exactly the same merge must preserve public ordering/identity.
    auto again = take(fv::build_grid(m, &cut), "repeat deterministic merge");
    require(g.cv_elem_gid == again.cv_elem_gid && g.cv_rank == again.cv_rank &&
                g.cv_faces == again.cv_faces && g.cv_face_off == again.cv_face_off,
            "deterministic merge representative and CSR");
    require(bitwise(g.cv_volume, again.cv_volume), "deterministic merged volume");
}
} // namespace

int main(int argc, char** argv) {
    PetscSession petsc(argc, argv);
    Suite suite;
    suite.run("independent oblique clipping polynomial oracle", [] {
        auto p = clip(box_poly({0, 0, 0}, {1, 1, 1}), {1, 1, 1}, 1);
        auto v = volume(p);
        near(v.volume, 1. / 6, 1e-14, 0, "simplex volume");
        near(v.quadratic, 1. / 20, 1e-14, 0, "simplex quadratic integral");
        for (auto x : v.centroid())
            near(x, .25, 1e-14, 0, "simplex centroid");
        for (bool chamfer : {false, true}) {
            auto f = cut_fixture(4, oblique(chamfer));
            for (std::size_t i = 0; i < f.cut.cut_elem.size(); ++i) {
                const auto e = static_cast<std::size_t>(f.cut.cut_elem[i]);
                const auto exact = volume(element_poly(f.mesh, e, f.geometry));
                const auto jacobian = volume(element_poly(f.mesh, e)).volume;
                double quadrature = 0;
                for (auto q = f.cut.vq_off[i]; q < f.cut.vq_off[i + 1]; ++q)
                    quadrature += jacobian * f.cut.vq_w[q];
                near(quadrature,
                     exact.volume,
                     0,
                     1e-12,
                     "fixture quadrature agrees with independent signed volume");
            }
        }
    });
    for (bool chamfer : {false, true}) {
        const std::string shape = chamfer ? "chamfer" : "oblique";
        suite.run(shape + " volume, geometry, openings and physical quadrature units",
                  [&] { cut_geometry(cut_fixture(4, oblique(chamfer))); });
        for (const std::string pc : {"jacobi", "gamg"}) {
            suite.run(shape + " affine cut Robin patch " + pc, [&] {
                auto f = cut_fixture(4, oblique(chamfer));
                auto g = take(fv::build_grid(f.mesh, &f.cut), "build_grid cut affine");
                affine_check(g,
                             f.mesh,
                             polynomials(g, f.mesh, f.geometry),
                             {0, {.3, -.2, .1}, false},
                             pc,
                             1e-10);
            });
            suite.run(shape + " quadratic true-average cut convergence " + pc, [&] {
                std::vector<Errors> errors;
                for (int n : {4, 8, 16}) {
                    auto f = cut_fixture(n, oblique(chamfer));
                    auto g = take(fv::build_grid(f.mesh, &f.cut),
                                  "build_grid cut MMS n=" + std::to_string(n));
                    errors.push_back(quadratic_errors(
                        g, f.mesh, polynomials(g, f.mesh, f.geometry), pc, "diag"));
                }
                convergence(errors, true, shape + " " + pc);
            });
        }
    }
    for (const std::string pc : {"jacobi", "gamg"}) {
        suite.run("empty cut and regular path agree " + pc, [&] {
            auto m = cube(3);
            mesh::cut::CutLayerArrays empty;
            auto regular = take(fv::build_grid(m), "build_grid regular comparison");
            auto cut = take(fv::build_grid(m, &empty), "build_grid empty-cut comparison");
            auto p = manufactured(regular, polynomials(regular, m), {1, {}, true}, pc);
            auto q = manufactured(cut, polynomials(cut, m), {1, {}, true}, pc);
            auto a = solve(regular, m, p), b = solve(cut, m, q);
            std::map<std::int64_t, double> by_gid;
            for (std::size_t c = 0; c < a.T_cell.size(); ++c)
                by_gid[regular.cv_elem_gid[c]] = a.T_cell[c];
            require(a.T_cell.size() == b.T_cell.size(), "empty-cut CV count");
            for (std::size_t c = 0; c < b.T_cell.size(); ++c)
                near(b.T_cell[c],
                     by_gid.at(cut.cv_elem_gid[c]),
                     1e-12,
                     0,
                     "empty-cut/regular temperature");
        });
        suite.run("listed merges form concave union " + pc, [&] { merge_l(pc); });
    }
    suite.run("cut jacobi/gamg results agree", [] {
        auto f = cut_fixture(4, oblique(true));
        auto g = take(fv::build_grid(f.mesh, &f.cut), "build_grid cut PC comparison");
        auto p = manufactured(g, polynomials(g, f.mesh, f.geometry), {1, {}, true});
        auto a = solve(g, f.mesh, p);
        p.preconditioner = "gamg";
        auto b = solve(g, f.mesh, p);
        require(relative_difference(a.T_cell, b.T_cell) <= 1e-8, "cut jacobi/gamg agreement");
        std::cout << "measured cut iterations jacobi=" << a.iterations << " gamg=" << b.iterations
                  << '\n';
    });
    suite.run("unlisted positive slivers are retained", [] {
        mesh::cut::CutGeometry geometry{{{0, {1, 0, 0}, 2e-10}}};
        auto f = cut_fixture(4, geometry);
        require(f.cut.small_subs.empty(), "fixture has no listed small subcells");
        auto g = take(fv::build_grid(f.mesh, &f.cut), "build_grid unlisted sliver");
        require(g.cv_volume.size() == 16, "retain every unlisted positive sliver");
        for (double v : g.cv_volume)
            near(v, 2e-10 / 16, 0, 1e-12, "physical sliver volume");
    });
    suite.run("cut copper uses min(1,phi/s) before merging", [] {
        auto f = cut_fixture(4);
        f.mesh.copper_layer_of_layer = {0};
        f.mesh.copper_phi.resize(16);
        for (std::size_t col = 0; col < 16; ++col)
            f.mesh.copper_phi[col] = .1 + .05 * (col % 7);
        auto g = take(fv::build_grid(f.mesh, &f.cut), "build_grid cut copper");
        auto fractions = fv::copper_fraction(g, f.mesh);
        require(fractions.size() == g.cv_volume.size(), "cut fraction size");
        for (std::size_t c = 0; c < fractions.size(); ++c) {
            auto e = static_cast<std::size_t>(g.cv_elem[c]);
            double s = volume(element_poly(f.mesh, e, f.geometry)).volume /
                       volume(element_poly(f.mesh, e)).volume;
            double want = std::min(1., f.mesh.copper_phi[f.mesh.elem_gid[e] / 4] / s);
            near(fractions[c], want, 0, 1e-12, "pre-merge min(1,phi/s)");
        }
    });
    suite.run("listed small subcell chooses largest shared opening", [] {
        auto m = boxes({{0, 1, 2, 3}, {0, 1}, {0, 1}});
        m.copper_layer_of_layer = {0};
        m.copper_phi = {.15, .1, .28};
        mesh::cut::CutGeometry geometry{{{0, {-.2, 1, 0}, .2}}};
        mesh::cut::CutOptions options;
        options.quad_degree = 2;
        options.small_sub_vf = 0;
        auto cut = take(mesh::cut::build_cut_layer(std::as_const(m), geometry, options),
                        "largest-opening synthetic geometry");
        require(cut.cut_elem.size() == 3, "three trapezoidal subcells");
        for (std::size_t i = 0; i < cut.cut_elem.size(); ++i)
            if (m.elem_gid[cut.cut_elem[i]] == 1)
                cut.small_subs = {cut.sub_off[i]};
        require(cut.small_subs.size() == 1, "only middle subcell is marked");
        auto g = take(fv::build_grid(m, &cut), "largest-opening merge");
        require(g.cv_volume.size() == 2, "one listed subcell merges");
        std::size_t merged = 0;
        while (merged < g.cv_kind.size() && g.cv_kind[merged] != 2)
            ++merged;
        require(merged < g.cv_kind.size(), "merged CV present");
        // Middle opens with areas .4 (left) and .6 (right), hence .5+.7=1.2.
        near(g.cv_volume[merged], 1.2, 1e-12, 0, "select maximum shared area");
        near(g.cv_copper_fraction[merged],
             (.5 * .2 + .7 * .4) / 1.2,
             1e-12,
             0,
             "mix pre-merge min(1,phi/s), then volume-weight");
        geometry_gate(g);
    });
    suite.run("equal-opening merge tie is stable under cut-record permutation", [] {
        auto m = boxes({{0, 1, 2, 3}, {0, 1}, {0, 1}});
        std::vector<Vec> signatures;
        for (bool reverse : {false, true}) {
            mesh::cut::CutLayerArrays cut;
            for (std::size_t j = 0; j < 3; ++j) {
                const auto e = reverse ? 2 - j : j;
                auto lo = vertex(m, e, 0), hi = vertex(m, e, 6);
                hi[1] = .5;
                append_sub_box(cut, m, e, lo, hi);
                if (m.elem_gid[e] == 1)
                    cut.small_subs = {static_cast<mesh::Local>(j)};
            }
            auto g = take(fv::build_grid(m, &cut), "stable equal-opening merge");
            require(g.cv_volume.size() == 2, "tie merges exactly one marked cell");
            geometry_gate(g);
            for (std::size_t c = 0; c < g.cv_kind.size(); ++c)
                if (g.cv_kind[c] == 2)
                    signatures.push_back(g.cv_centroid[c]);
        }
        require(signatures.size() == 2, "one merged CV per permutation");
        for (int d = 0; d < 3; ++d)
            near(signatures[0][d], signatures[1][d], 1e-14, 0, "identity-based merge tie");
    });
    for (bool shifted : {false, true})
        suite.run(shifted ? "reject mismatched shared opening centroid"
                          : "reject mismatched shared opening area",
                  [&] {
                      auto m = boxes({{0, 1, 2}, {0, 1}, {0, 1}});
                      mesh::cut::CutLayerArrays cut;
                      append_sub_box(cut, m, 0, {0, 0, 0}, {1, .5, 1});
                      append_sub_box(
                          cut, m, 1, {1, shifted ? .1 : 0, 0}, {2, shifted ? .6 : .4, 1});
                      auto result = fv::build_grid(m, &cut);
                      rejected(result, "incompatible shared opening", "0");
                      require(result.error().reason.find("1") != std::string::npos,
                              "incompatible opening diagnostic includes both parent gids");
                  });
    suite.run("chamfer groups distinct embedded planes", [] {
        auto f = cut_fixture(2, oblique(true));
        auto g = take(fv::build_grid(f.mesh, &f.cut), "build_grid noncoplanar embedded quadrature");
        bool multiple = false;
        for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
            std::vector<Vec> normals;
            for (auto j = g.cv_face_off[c]; j < g.cv_face_off[c + 1]; ++j) {
                const auto& face = g.faces[g.cv_faces[j]];
                if (face.side == 6)
                    normals.push_back(mul(face.area_vector, 1 / face.area));
            }
            for (auto a : normals)
                for (auto b : normals)
                    multiple |= norm(cross(a, b)) > .1;
        }
        require(multiple, "at least one parent retains separate noncoplanar embedded faces");
        geometry_gate(g);
    });
    for (const auto kind : {fv::ThermalBc::Kind::Dirichlet, fv::ThermalBc::Kind::Flux})
        suite.run("reject unsupported embedded BC " + std::to_string(static_cast<int>(kind)), [&] {
            auto f = cut_fixture(2);
            auto g = take(fv::build_grid(f.mesh, &f.cut), "build_grid unsupported EB BC");
            auto p = problem(g);
            fv::ThermalBc eb{};
            eb.kind = kind;
            eb.target = "boundary:embedded";
            p.bcs = {dirichlet("face:zmin", 1), eb};
            rejected(fv::solve_thermal(g, f.mesh, p, MPI_COMM_WORLD), "embedded Dirichlet/Flux");
        });
    suite.run("embedded boundary defaults to insulation", [] {
        auto f = cut_fixture(3);
        auto g = take(fv::build_grid(f.mesh, &f.cut), "build_grid insulated EB");
        auto p = problem(g);
        p.bcs = {dirichlet("face:zmin", 1)};
        auto r = solve(g, f.mesh, p);
        for (std::size_t i = 0; i < g.faces.size(); ++i)
            if (g.faces[i].side == 6)
                near(r.face_flux[i], 0, 1e-11, 0, "default embedded insulation");
        for (auto u : r.T_cell)
            near(u, 1, 1e-11, 0, "constant insulated cut domain");
    });
    const std::vector<std::pair<std::string, std::function<void(mesh::cut::CutLayerArrays&)>>>
        invalid{{"broken volume", [](auto& c) { c.vq_w[0] *= 1.1; }},
                {"negative volume fraction", [](auto& c) { c.sub_volfrac[0] = -1; }},
                {"overfull volume fraction", [](auto& c) { c.sub_volfrac[0] = 1.1; }},
                {"short surface offsets", [](auto& c) { c.sq_off.pop_back(); }},
                {"negative area", [](auto& c) { c.sq_w[0] = -1; }},
                {"NaN area", [](auto& c) { c.sq_w[0] = std::numeric_limits<double>::quiet_NaN(); }},
                {"invalid parent", [](auto& c) { c.cut_elem[0] = -1; }},
                {"invalid subcell", [](auto& c) { c.small_subs = {-1}; }},
                {"broken geometry gate", [](auto& c) {
                     for (auto q = c.sq_off[0]; q < c.sq_off[1]; ++q)
                         c.sq_xi[0][q] += .01 * c.sq_xi[1][q];
                 }}};
    for (const auto& [name, mutate] : invalid)
        suite.run("reject cut " + name, [&] {
            auto f = cut_fixture(2);
            mutate(f.cut);
            rejected(fv::build_grid(f.mesh, &f.cut), name);
        });
    return suite.finish();
}
