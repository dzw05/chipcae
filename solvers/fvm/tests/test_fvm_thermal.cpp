#include "fvm_test_util.hpp"

using namespace fvm_test;

namespace {
// For boxes with inactive gamma floor, recover each side independently using
// analytic half-cell resistance. This catches incorrect ledger signs and
// diagnostics that merely copy a KSP residual or a shared face's other side.
void independent_balances(const fv::FvGrid& g,
                          const mesh::HexMeshArrays& m,
                          const fv::ThermalProblem& p,
                          const fv::ThermalResult& r) {
    std::vector<std::array<double, 2>> flux(g.faces.size());
    std::vector<double> sums(g.cv_volume.size()), abs_sums(g.cv_volume.size());
    double largest_flux = 0, in = 0, out = 0;
    auto ledger = [&](double e) {
        in += std::max(e, 0.);
        out += std::max(-e, 0.);
    };
    for (std::size_t f = 0; f < g.faces.size(); ++f) {
        const auto& face = g.faces[f];
        for (int side = 0; side < 2; ++side) {
            auto c = side ? face.hi : face.lo;
            if (c < 0)
                continue;
            const auto d = static_cast<std::size_t>(face.axis);
            const auto e = static_cast<std::size_t>(g.cv_elem[c]);
            const auto lo = vertex(m, e, 0), hi = vertex(m, e, 6);
            const double q =
                2 * p.k[c][d] * face.area / (hi[d] - lo[d]) * (r.T_cell[c] - r.T_face[f]);
            flux[f][side] = q;
            sums[c] += q;
            abs_sums[c] += std::abs(q);
            largest_flux = std::max(largest_flux, std::abs(q));
            if (!side)
                near(r.face_flux[f],
                     q,
                     1e-12 * std::max(1., std::abs(q)),
                     1e-12,
                     "returned flux equals independently recovered lo flux");
        }
        if (face.hi < 0)
            ledger(-flux[f][0]);
    }
    double balance = 0, jump = 0;
    for (std::size_t c = 0; c < sums.size(); ++c) {
        const double s = p.q_vol[c] * g.cv_volume[c];
        ledger(s);
        if (std::find(p.fixed_cv.begin(), p.fixed_cv.end(), static_cast<int>(c)) !=
            p.fixed_cv.end()) {
            ledger(sums[c] - s);
        } else {
            balance = std::max(
                balance, std::abs(sums[c] - s) / std::max({abs_sums[c], std::abs(s), 1e-300}));
        }
    }
    for (std::size_t f = 0; f < g.faces.size(); ++f)
        if (g.faces[f].hi >= 0)
            jump = std::max(
                jump,
                std::abs(flux[f][0] + flux[f][1]) /
                    std::max({std::abs(flux[f][0]), std::abs(flux[f][1]), 1e-300 * largest_flux}));
    require(balance <= 1e-12, "independent cell_balance_max exact denominator");
    require(jump <= 1e-9, "independent face_jump_max exact denominator");
    const double imbalance = std::abs(in - out) / std::max(in + out, 1e-300);
    require(imbalance <= 1e-9, "independent signed energy ledger closes");
    near(r.heat_in, in, 0, 1e-12, "signed ledger heat_in");
    near(r.heat_out, out, 0, 1e-12, "signed ledger heat_out");
    near(r.cell_balance_max, balance, 1e-12, 0, "reported cell balance");
    near(r.face_jump_max, jump, 1e-9, 0, "reported face jump");
    near(r.imbalance, imbalance, 1e-12, 0, "reported energy imbalance");
    conservation(r);
}

void layered(const std::string& pc,
             double fraction,
             bool robin,
             const std::vector<double>& base = {.3, 1.2, .7, 2.1},
             bool transverse = true) {
    const auto z = base.size() == 2 ? std::vector<double>{0, .17, 1}
                                    : std::vector<double>{0, .07, .23, .56, 1};
    auto m =
        boxes(transverse ? TestAxes{{0, .4, 1}, {0, .3, 1}, z} : TestAxes{{0, 1}, {0, 1}, z}, true);
    m.copper_layer_of_layer.resize(base.size());
    std::iota(m.copper_layer_of_layer.begin(), m.copper_layer_of_layer.end(), 0);
    m.copper_phi.assign(base.size() * (transverse ? 4 : 1), fraction);
    auto g = take(fv::build_grid(m), "build_grid layered nonuniform mesh");
    geometry_gate(g);
    auto f = fv::copper_fraction(g, m);
    require(f.size() == g.cv_volume.size(), "copper fraction size");
    auto p = problem(g, {}, pc);
    std::vector<Vec> ks(base.size());
    double resistance = 0;
    for (std::size_t l = 0; l < base.size(); ++l) {
        chipcae::material::copper_laminate_conductivity(base[l], 390, fraction, ks[l].data());
        resistance += (z[l + 1] - z[l]) / ks[l][2];
    }
    for (std::size_t c = 0; c < p.k.size(); ++c) {
        auto e = static_cast<std::size_t>(g.cv_elem[c]);
        auto l = static_cast<std::size_t>(m.attribute[e] - 1);
        p.k[c] = ks[l];
        near(f[c], fraction, 0, 1e-14, "column laminate fraction");
    }
    p.bcs = {dirichlet("face:zmin", 310)};
    fv::ThermalBc top{};
    top.target = "face:zmax";
    top.kind = robin ? fv::ThermalBc::Kind::Robin : fv::ThermalBc::Kind::Flux;
    top.h = 7;
    top.T_inf = 290;
    top.q = -2; // Inward-positive: outward heat is +2 W/m^2.
    p.bcs.push_back(top);
    const double q = robin ? 20 / (resistance + 1. / 7) : 2;
    // On multiple columns add a known transverse affine field, so every
    // interior face carries measurable flux. The separate one-column case
    // retains the exact 1D solution and insulated transverse faces.
    const Vec gradient = transverse ? Vec{.3, .2, 0} : Vec{};
    p.bcs.clear();
    for (std::size_t f = 0; f < g.faces.size(); ++f) {
        const auto& face = g.faces[f];
        if (face.hi >= 0)
            continue;
        fv::ThermalBc b{};
        const double tilt = dot(gradient, face.centroid);
        if (face.side == 4) {
            b = dirichlet(target(face), 310 + tilt);
        } else if (face.side == 5) {
            b = top;
            b.T_inf += tilt;
        } else {
            b.kind = fv::ThermalBc::Kind::Flux;
            b.target = target(face);
            b.q = p.k[face.lo][face.axis] * gradient[face.axis] * face.area_vector[face.axis] /
                  face.area;
        }
        b.face = f;
        p.bcs.push_back(b);
    }
    auto r = solve(g, m, p);
    for (std::size_t c = 0; c < r.T_cell.size(); ++c) {
        const auto l = static_cast<std::size_t>(m.attribute[g.cv_elem[c]] - 1);
        double drop = 0;
        for (std::size_t j = 0; j < l; ++j)
            drop += (z[j + 1] - z[j]) / ks[j][2];
        drop += (z[l + 1] - z[l]) / (2 * ks[l][2]);
        near(r.T_cell[c],
             310 - q * drop + dot(gradient, g.cv_centroid[c]),
             1e-10,
             0,
             "layered exact cell-average temperature");
    }
    conservation(r);
    double incoming = q;
    for (std::size_t l = 0; l < base.size(); ++l)
        incoming += (z[l + 1] - z[l]) * (ks[l][0] * gradient[0] + ks[l][1] * gradient[1]);
    near(r.heat_in, incoming, 0, 1e-12, "layered analytic incoming heat");
    near(r.heat_out, incoming, 0, 1e-12, "layered analytic outgoing heat");
    if (!transverse)
        for (std::size_t f = 0; f < g.faces.size(); ++f)
            if (g.faces[f].axis < 2)
                near(r.face_flux[f], 0, 1e-12, 0, "1D insulated transverse face flux");
}

void box_equivalence(const std::string& pc) {
    auto m = boxes({{0, .12, .47, 1}, {0, .27, 1}, {0, .09, .4, 1}});
    auto g = take(fv::build_grid(m), "build_grid TPFA equivalence");
    auto p = problem(g, {}, pc);
    // Coefficients assigned by input element, not by solver output ordering.
    std::vector<Vec> by_element;
    for (std::size_t e = 0; e < m.attribute.size(); ++e)
        by_element.push_back({1 + .2 * (e % 3), 2 + .1 * (e % 5), .7 + .3 * (e % 4)});
    fv::ThermalProblem reference = p;
    reference.k = by_element;
    reference.q_vol.resize(by_element.size());
    for (std::size_t e = 0; e < by_element.size(); ++e)
        reference.q_vol[e] = .2 + .03 * e;
    reference.bcs = {dirichlet("face:xmin", 2), dirichlet("face:xmax", 1)};
    fv::ThermalBc robin{};
    robin.kind = fv::ThermalBc::Kind::Robin;
    robin.target = "face:zmax";
    robin.h = 3;
    robin.T_inf = .5;
    reference.bcs.push_back(robin);
    auto want = tpfa(m, reference);
    p.bcs = reference.bcs;
    for (std::size_t c = 0; c < p.k.size(); ++c) {
        p.k[c] = by_element[g.cv_elem[c]];
        p.q_vol[c] = reference.q_vol[g.cv_elem[c]];
        auto local = take(fv::detail::build_local_mfd(g, c, p.k[c], "diag"), "local MFD on box");
        auto nf = static_cast<std::size_t>(g.cv_face_off[c + 1] - g.cv_face_off[c]);
        require(local.M.size() == nf * nf, "local inner product dimensions");
        double scale = 0;
        for (double x : local.M)
            scale = std::max(scale, std::abs(x));
        for (std::size_t i = 0; i < nf; ++i) {
            const auto& face = g.faces[g.cv_faces[g.cv_face_off[c] + i]];
            auto e = static_cast<std::size_t>(g.cv_elem[c]);
            auto h = sub(vertex(m, e, 6), vertex(m, e, 0));
            double diagonal = h[face.axis] / (2 * p.k[c][face.axis] * face.area);
            near(local.M[i * nf + i], diagonal, 0, 1e-14, "box half resistance");
            for (std::size_t j = 0; j < nf; ++j)
                if (i != j)
                    near(local.M[i * nf + j], 0, 1e-14 * scale, 0, "box local off-diagonal");
        }
        require(local.gamma_floor_count == 0, "box equivalence gamma floor inactive");
    }
    auto r = solve(g, m, p);
    for (std::size_t c = 0; c < r.T_cell.size(); ++c)
        near(r.T_cell[c], want[g.cv_elem[c]], 1e-11, 0, "MFD equals independent harmonic TPFA");
    require(r.gamma_floor_count == 0, "global gamma floor inactive");
}

void wiener(const std::string& pc) {
    auto m = boxes({{0, .2, .6, 1}, {0, .35, .7, 1}, {0, .1, .3, .65, 1}}, true);
    constexpr std::size_t columns = 9, layers = 4;
    m.copper_layer_of_layer = {0, 1, 2, 3};
    m.copper_phi.resize(columns * layers);
    for (std::size_t l = 0; l < layers; ++l)
        for (std::size_t col = 0; col < columns; ++col)
            m.copper_phi[l * columns + col] = .03 + .8 * ((3 * col + 5 * l) % 11) / 10;
    auto g = take(fv::build_grid(m), "build_grid varying Morton-column copper");
    auto f = fv::copper_fraction(g, m);
    require(f.size() == g.cv_volume.size(), "Wiener fraction size");
    auto p = problem(g, {}, pc);
    p.bcs = {dirichlet("face:zmin", 1), dirichlet("face:zmax", 0)};
    std::vector<double> column_r(columns), column_area(columns), layer_k(layers);
    for (std::size_t c = 0; c < p.k.size(); ++c) {
        const auto e = static_cast<std::size_t>(g.cv_elem[c]);
        const auto col = static_cast<std::size_t>(m.elem_gid[e]) / layers;
        const auto l = static_cast<std::size_t>(m.attribute[e] - 1);
        const auto expected_f = m.copper_phi[l * columns + col];
        near(f[c], expected_f, 0, 1e-14, "copper lookup uses elem_gid / nz");
        near(g.cv_copper_fraction[c], expected_f, 0, 1e-14, "stored copper fraction");
        chipcae::material::copper_laminate_conductivity(.3, 390, f[c], p.k[c].data());
        const auto h = sub(vertex(m, e, 6), vertex(m, e, 0));
        column_area[col] = h[0] * h[1];
        column_r[col] += h[2] / p.k[c][2];
        layer_k[l] += h[0] * h[1] * p.k[c][2];
    }
    double lower = 0, upper_r = 0;
    for (std::size_t col = 0; col < columns; ++col)
        lower += column_area[col] / column_r[col];
    for (std::size_t l = 0; l < layers; ++l)
        upper_r += (m.axes[2][l + 1] - m.axes[2][l]) / layer_k[l];
    auto r = solve(g, m, p);
    double q = 0;
    for (std::size_t i = 0; i < g.faces.size(); ++i)
        if (g.faces[i].hi < 0 && g.faces[i].side == 5)
            q += r.face_flux[i];
    require(q >= lower * (1 - 1e-12), "Wiener column-series lower bound");
    require(q <= (1 / upper_r) * (1 + 1e-12), "Wiener layer-average upper bound");
}

void fixed_reaction(const std::string& pc) {
    auto m = boxes({{0, 1}, {0, 1}, {0, .2, .5, 1}}, true);
    auto g = take(fv::build_grid(m), "build_grid fixed CV");
    auto p = problem(g, {2, 2, 2}, pc);
    p.bcs = {dirichlet("face:zmin", 300), dirichlet("face:zmax", 290)};
    std::size_t fixed = 0;
    while (g.cv_elem_gid[fixed] != 1)
        ++fixed;
    p.fixed_cv = {static_cast<int>(fixed)};
    p.fixed_T = {310};
    p.q_vol[fixed] = 3;
    auto r = solve(g, m, p);
    near(r.T_cell[fixed], 310, 1e-12, 0, "fixed cell is prescribed exactly");
    // Two branches from z=.35: 2*(310-300)/.35 and 2*(310-290)/.65.
    const double want_reaction = 2 * 10 / .35 + 2 * 20 / .65 - 3 * .3;
    double reaction = -3 * .3;
    for (auto j = g.cv_face_off[fixed]; j < g.cv_face_off[fixed + 1]; ++j) {
        const auto f = static_cast<std::size_t>(g.cv_faces[j]);
        reaction += (g.faces[f].lo == static_cast<std::int64_t>(fixed) ? 1 : -1) * r.face_flux[f];
    }
    near(reaction, want_reaction, 0, 1e-12, "independent fixed-CV reaction");
    independent_balances(g, m, p, r);
}

void mpi_reference(const std::string& pc) {
    int ranks = 1;
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
    auto m = boxes({{0, .13, .4, .7, 1}, {0, .2, .65, 1}, {0, .11, .4, 1}}, false, ranks);
    auto g = take(fv::build_grid(m), "build_grid MPI owned columns");
    for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
        require(g.cv_rank[c] == m.partitioning[g.cv_elem[c]], "MPI CV ownership preserves input");
        require(g.cv_elem_gid[c] == m.elem_gid[g.cv_elem[c]], "MPI CV identity preserves input");
    }
    auto polys = polynomials(g, m);
    auto p = manufactured(g, polys, {1, {.4, -.7, .2}, true}, pc);
    p.rtol = 1e-12;
    auto distributed = solve(g, m, p);
    rank_identical(distributed);
    auto serial_mesh = m;
    std::fill(serial_mesh.partitioning.begin(), serial_mesh.partitioning.end(), 0);
    auto serial_grid = take(fv::build_grid(serial_mesh), "build_grid serial reference");
    auto serial_problem = manufactured(
        serial_grid, polynomials(serial_grid, serial_mesh), {1, {.4, -.7, .2}, true}, pc);
    serial_problem.rtol = 1e-12;
    auto serial = solve(serial_grid, serial_mesh, serial_problem, MPI_COMM_SELF);
    // Stable occurrence ordinal is the subcell identity when a gid repeats.
    using Key = std::pair<std::int64_t, int>;
    std::map<Key, double> reference;
    std::map<std::int64_t, int> ordinal;
    for (std::size_t c = 0; c < serial.T_cell.size(); ++c)
        reference[{serial_grid.cv_elem_gid[c], ordinal[serial_grid.cv_elem_gid[c]]++}] =
            serial.T_cell[c];
    ordinal.clear();
    for (std::size_t c = 0; c < distributed.T_cell.size(); ++c) {
        auto gid = g.cv_elem_gid[c];
        const double exact = reference.at({gid, ordinal[gid]++});
        near(distributed.T_cell[c], exact, 0, 1e-9, "np1/np2 by gid and subcell identity");
    }
    auto repeated = solve(g, m, p);
    require(bitwise(all_fields(distributed), all_fields(repeated)),
            "repeated solve is bitwise identical");
    require(distributed.gamma_floor_count == repeated.gamma_floor_count &&
                distributed.max_condition_gid == repeated.max_condition_gid,
            "repeat integer diagnostics");
}
} // namespace

int main(int argc, char** argv) {
    const bool mpi_only = argc > 1 && std::string(argv[1]) == "--mpi-only";
    if (mpi_only) {
        for (int i = 1; i + 1 < argc; ++i)
            argv[i] = argv[i + 1];
        argv[--argc] = nullptr;
    }
    PetscSession petsc(argc, argv);
    Suite suite;
    if (mpi_only) {
        for (const std::string pc : {"jacobi", "gamg"})
            suite.run("distributed/serial/all-rank/repeat " + pc, [&] { mpi_reference(pc); });
        return suite.finish();
    }
    suite.run("independent signed polynomial oracle on concave L prism", [] {
        auto p = l_prism();
        auto v = volume(p);
        near(v.volume, 3, 1e-12, 0, "L prism volume");
        near(v.centroid()[0], 5. / 6, 1e-12, 0, "L centroid x");
        near(v.centroid()[1], 5. / 6, 1e-12, 0, "L centroid y");
        near(v.centroid()[2], .5, 1e-12, 0, "L centroid z");
        near(v.quadratic, 7, 1e-12, 0, "L integral x^2+y^2+z^2");
        // True mean is 7/3; evaluating at the centroid gives 59/36.
        require(std::abs(v.quadratic / v.volume - dot(v.centroid(), v.centroid())) > .5,
                "quadratic oracle must distinguish a true average from centroid value");
        auto m = boxes({{0, 2}, {0, 2}, {0, 1}});
        geometry_gate(hand_grid(m, {p}));
    });
    suite.run("independent dense TPFA oracle on nonuniform layered rod", [] {
        auto m = boxes({{0, 1}, {0, 1}, {0, .2, 1}});
        fv::ThermalProblem p;
        p.k = {{1, 1, 1}, {4, 4, 4}};
        p.q_vol = {0, 0};
        p.bcs = {dirichlet("face:zmin", 1), dirichlet("face:zmax", 0)};
        auto u = tpfa(m, p);
        near(u[0], .75, 1e-14, 0, "TPFA analytic lower average");
        near(u[1], .25, 1e-14, 0, "TPFA analytic upper average");
    });
    suite.run("shared local MFD consistency on concave geometry", [] {
        auto m = boxes({{0, 2}, {0, 2}, {0, 1}});
        auto g = hand_grid(m, {l_prism()});
        for (const std::string stabilization : {"diag", "scalar"}) {
            const Vec k{1, 2, 3};
            auto local = take(fv::detail::build_local_mfd(g, 0, k, stabilization),
                              "shared concave MFD inner product");
            const auto n = g.faces.size();
            require(local.M.size() == n * n, "concave local matrix dimensions");
            for (std::size_t i = 0; i < n; ++i)
                for (int d = 0; d < 3; ++d) {
                    double mnk = 0;
                    for (std::size_t j = 0; j < n; ++j)
                        mnk += local.M[i * n + j] * g.faces[j].area_vector[d] * k[d];
                    near(mnk,
                         g.faces[i].centroid[d] - g.cv_centroid[0][d],
                         1e-12,
                         0,
                         "M N K = R, including concave face geometry");
                }
        }
    });
    suite.run("local symmetric condition number and gamma-floor activation", [] {
        auto m = cube(1);
        auto g = hand_grid(m, {box_poly({0, 0, 0}, {1, 1, 1})});
        auto unit = take(fv::detail::build_local_mfd(g, 0, {1, 2, 3}, "diag"),
                         "unit-box local condition number");
        near(unit.condition, 3, 1e-13, 0, "cond2 from symmetric eigenvalues");
        require(unit.gamma_floor_count == 0, "unit box has no gamma floors");
        auto thin = hand_grid(m, {box_poly({0, 0, 0}, {1, 1, 1e-6})});
        auto local =
            take(fv::detail::build_local_mfd(thin, 0, {1, 1, 1}, "diag"), "thin-box gamma floor");
        require(local.gamma_floor_count == 2, "only the two z faces activate the gamma floor");
        require(std::isfinite(local.condition) && local.condition > 1e8,
                "thin-box conditioning is recorded without eigenvalue clipping");
    });
    suite.run("tiny source retains assembled face load", [] {
        auto m = cube(1);
        auto g = take(fv::build_grid(m), "build_grid tiny source");
        auto p = problem(g, {1, 2, 3});
        p.T_ref = 0;
        p.q_vol[0] = 1e-20;
        for (const auto& face : g.faces)
            p.bcs.push_back(dirichlet(target(face), 1));
        auto r = solve(g, m, p);
        // The cell temperature increment is below extended-precision epsilon
        // at T=1. Recovering M^-1(u-lambda) loses the entire heat source.
        // Box half conductances partition it as k_i/(2*sum(k)).
        for (std::size_t f = 0; f < g.faces.size(); ++f)
            near(r.face_flux[f],
                 p.q_vol[0] * p.k[0][g.faces[f].axis] / 12,
                 1e-40,
                 1e-14,
                 "source partition from analytic box half conductances");
        conservation(r);
    });
    suite.run("regular geometry retains gids, ownership, and one-based layers", [] {
        auto m = boxes({{0, .13, .6, 1}, {0, .25, .8, 1}, {0, .12, .43, 1}}, true);
        // Input element order is deliberately shuffled; geometry is never inferred from gid.
        std::reverse(m.elem_gid.begin(), m.elem_gid.end());
        for (std::size_t i = 0, j = m.attribute.size() - 1; i < j; ++i, --j) {
            std::swap(m.attribute[i], m.attribute[j]);
            std::swap(m.partitioning[i], m.partitioning[j]);
            for (int v = 0; v < 8; ++v)
                std::swap(m.hex[8 * i + v], m.hex[8 * j + v]);
        }
        auto g = take(fv::build_grid(m), "build_grid non-row-major geometry");
        require(g.cv_volume.size() == m.attribute.size(), "one CV per retained box");
        geometry_gate(g);
        std::set<std::int64_t> gids;
        for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
            const auto e = static_cast<std::size_t>(g.cv_elem[c]);
            auto v = volume(element_poly(m, e));
            near(g.cv_volume[c], v.volume, 0, 1e-13, "analytic box volume");
            require(g.cv_elem_gid[c] == m.elem_gid[e], "original element gid");
            require(g.cv_rank[c] == m.partitioning[e], "original owner");
            require(g.cv_layer[c] ==
                        m.attr_table.layers.at(m.attr_table.entries[m.attribute[e] - 1].layer),
                    "one-based attr layer");
            require(g.cv_face_off[c + 1] - g.cv_face_off[c] == 6, "box has six faces");
            gids.insert(g.cv_elem_gid[c]);
        }
        require(gids.size() == m.attribute.size(), "unique CV identities");
        require(g.faces.size() == 108, "shared faces stored once for 3x3x3 grid");
    });
    suite.run("void gap does not create CVs or connect components", [] {
        auto m = boxes({{0, 1}, {0, 1}, {0, .3, .6, 1}}, true);
        m.hex.erase(m.hex.begin() + 8, m.hex.begin() + 16);
        m.attribute.erase(m.attribute.begin() + 1);
        m.elem_gid.erase(m.elem_gid.begin() + 1);
        m.partitioning.erase(m.partitioning.begin() + 1);
        m.grid_report.void_cells = 1;
        auto g = take(fv::build_grid(m), "build_grid omitted void layer");
        require(g.cv_volume.size() == 2, "void has no control volume");
        geometry_gate(g);
        require(std::none_of(g.faces.begin(), g.faces.end(), [](auto f) { return f.hi >= 0; }),
                "void separates solids");
        auto p = problem(g);
        p.bcs = {dirichlet("face:zmin", 1)};
        rejected(fv::solve_thermal(g, m, p, chipcae::Communicator::world()),
                 "each disconnected component needs an anchor");
    });
    for (const std::string pc : {"jacobi", "gamg"}) {
        for (double f : {0., .3})
            for (bool robin : {false, true})
                suite.run("layered nonuniform f=" + std::to_string(f) +
                              (robin ? " Robin " : " Flux ") + pc,
                          [&] { layered(pc, f, robin); });
        for (double f : {0., .3})
            for (bool robin : {false, true})
                suite.run("one-column insulated layered f=" + std::to_string(f) +
                              (robin ? " Robin " : " Flux ") + pc,
                          [&] { layered(pc, f, robin, {.3, 1.2, .7, 2.1}, false); });
        suite.run("independent harmonic TPFA equivalence " + pc, [&] { box_equivalence(pc); });
        for (const std::string stabilization : {"diag", "scalar"}) {
            for (const Polynomial u : {Polynomial{300, {}, false},
                                       Polynomial{2, {.7, -1.2, .3}, false},
                                       Polynomial{1, {1, 0, 0}, false}})
                suite.run("constant/affine/zero-flux patch " + pc + " " + stabilization +
                              " gradient=" + std::to_string(u.linear[0]) + "," +
                              std::to_string(u.linear[1]),
                          [&] {
                              auto m = boxes({{0, .1, .42, 1}, {0, .27, 1}, {0, .13, .61, 1}});
                              auto g = take(fv::build_grid(m), "build_grid patch");
                              affine_check(g, m, polynomials(g, m), u, pc, 1e-11, stabilization);
                          });
            suite.run("quadratic true-average convergence " + pc + " " + stabilization, [&] {
                std::vector<Errors> errors;
                for (int n : {4, 8, 16}) {
                    // Smooth grading avoids the exact quadratic face/flux
                    // solution on uniform cubes, where order measures roundoff.
                    auto x = uniform(n), y = x, z = x;
                    for (auto& t : x)
                        t += .2 * t * (1 - t);
                    for (auto& t : y)
                        t -= .15 * t * (1 - t);
                    for (auto& t : z)
                        t += .1 * t * (1 - t);
                    auto m = boxes({x, y, z});
                    auto g = take(fv::build_grid(m), "build_grid quadratic n=" + std::to_string(n));
                    errors.push_back(quadratic_errors(g, m, polynomials(g, m), pc, stabilization));
                }
                convergence(errors, false, pc + " " + stabilization, stabilization == "diag");
            });
        }
        suite.run("concave L-prism shared MFD gate and affine patch " + pc, [&] {
            auto m = boxes({{0, 2}, {0, 2}, {0, 1}});
            auto polys = std::vector<Poly>{l_prism()};
            auto g = hand_grid(m, polys);
            geometry_gate(g);
            take(fv::detail::build_local_mfd(g, 0, {1, 2, 3}, "diag"),
                 "concave local MFD accepts geometry");
            affine_check(g, m, polys, {0, {.7, -.4, .2}, false}, pc, 1e-12);
        });
        suite.run("centroid outside C-prism is accepted " + pc, [&] {
            auto poly =
                prism({{0, 0}, {3, 0}, {3, .2}, {.2, .2}, {.2, 2.8}, {3, 2.8}, {3, 3}, {0, 3}});
            auto c = volume(poly).centroid();
            require(c[0] > .2 && c[1] > .2 && c[1] < 2.8,
                    "C-prism centroid lies in its empty notch");
            auto m = boxes({{0, 3}, {0, 3}, {0, 1}});
            auto g = hand_grid(m, {poly});
            affine_check(g, m, {poly}, {0, {.7, -.4, .2}, false}, pc, 1e-12);
        });
        suite.run("two disconnected CVs in one parent " + pc, [&] {
            auto m = cube(1);
            std::vector<Poly> polys{box_poly({0, 0, 0}, {.2, .3, .4}),
                                    box_poly({.7, .6, .5}, {1, 1, 1})};
            auto g = hand_grid(m, polys);
            require(g.cv_elem_gid[0] == g.cv_elem_gid[1], "two subcells share a parent gid");
            geometry_gate(g);
            affine_check(g, m, polys, {0, {.7, -.4, .2}, false}, pc, 1e-12);
        });
        suite.run("three independent conservation metrics " + pc, [&] {
            auto m = cube(3);
            auto g = take(fv::build_grid(m), "build_grid conservation");
            auto p = manufactured(g, polynomials(g, m), {1, {1, .3, .7}, true}, pc);
            p.rtol = 1e-12;
            independent_balances(g, m, p, solve(g, m, p));
        });
        suite.run("fixed CV reaction closes energy ledger " + pc, [&] { fixed_reaction(pc); });
        suite.run("Wiener bounds and Morton copper fractions " + pc, [&] { wiener(pc); });
        for (double ratio : {10., 1300., .1, 1. / 1300})
            suite.run("two-layer contrast " + std::to_string(ratio) + " " + pc,
                      [&] { layered(pc, 0, true, {1, ratio}); });
        suite.run("layer target and volume/area weighted vertex reconstruction " + pc, [&] {
            auto m = boxes({{0, .3, 1}, {0, .4, 1}, {0, .2, .5, 1}}, true);
            auto g = take(fv::build_grid(m), "build_grid layer BC");
            auto p = problem(g, {1, 1, 1}, pc);
            p.bcs = {dirichlet("layer:L0", 300)};
            auto r = solve(g, m, p);
            for (auto T : r.T_cell)
                near(T, 300, 1e-10, 0, "layer target exterior faces");
            for (auto T : r.T_vertex)
                near(T, 300, 1e-10, 0, "constant vertex reconstruction");
            p.bcs = {dirichlet("face:zmin", 1), dirichlet("face:zmax", 0)};
            r = solve(g, m, p);
            for (std::size_t v = 0; v < r.T_vertex.size(); ++v) {
                Vec x{m.vertices[3 * v], m.vertices[3 * v + 1], m.vertices[3 * v + 2]};
                if (std::abs(x[2]) < 1e-14 || std::abs(x[2] - 1) < 1e-14) {
                    near(r.T_vertex[v], 1 - x[2], 1e-12, 0, "Dirichlet boundary vertex");
                    continue;
                }
                double sum = 0, weights = 0;
                bool boundary = std::abs(x[0]) < 1e-14 || std::abs(x[0] - 1) < 1e-14 ||
                                std::abs(x[1]) < 1e-14 || std::abs(x[1] - 1) < 1e-14;
                for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
                    auto e = static_cast<std::size_t>(g.cv_elem[c]);
                    bool incident = false;
                    for (int local = 0; local < 8; ++local)
                        incident |= m.hex[8 * e + local] == static_cast<mesh::Local>(v);
                    if (incident && !boundary) {
                        sum += g.cv_volume[c] * r.T_cell[c];
                        weights += g.cv_volume[c];
                    }
                }
                if (boundary)
                    for (std::size_t f = 0; f < g.faces.size(); ++f) {
                        const auto& face = g.faces[f];
                        if (face.hi >= 0 || face.side >= 4)
                            continue;
                        auto e = static_cast<std::size_t>(g.cv_elem[face.lo]);
                        auto lo = vertex(m, e, 0), hi = vertex(m, e, 6);
                        bool incident = std::abs(x[face.axis] - face.centroid[face.axis]) < 1e-14;
                        for (int d = 0; d < 3; ++d)
                            incident &= x[d] >= lo[d] - 1e-14 && x[d] <= hi[d] + 1e-14;
                        if (incident) {
                            sum += face.area * r.T_face[f];
                            weights += face.area;
                        }
                    }
                require(weights > 0, "vertex oracle has incident geometry");
                near(r.T_vertex[v],
                     sum / weights,
                     1e-12,
                     0,
                     "weighted vertex reconstruction in original ordering");
            }
        });
    }
    suite.run("uniform quadratic exact face and flux solution", [] {
        auto m = cube(4);
        auto g = take(fv::build_grid(m), "build_grid uniform quadratic");
        auto polys = polynomials(g, m);
        const Polynomial u{0, {}, true};
        auto p = manufactured(g, polys, u);
        auto r = solve(g, m, p);
        for (std::size_t c = 0; c < g.cv_volume.size(); ++c)
            near(r.T_cell[c],
                 u.cell(polys[c]) - 1. / 48,
                 1e-12,
                 0,
                 "uniform quadratic cell shift is -h^2/3");
        for (std::size_t f = 0; f < g.faces.size(); ++f) {
            const auto& face = g.faces[f];
            near(r.T_face[f],
                 u.face(matching_face(polys[face.lo], face)),
                 1e-12,
                 0,
                 "uniform quadratic true face average");
            near(r.face_flux[f],
                 exact_flux(face, u, p.k[face.lo]),
                 1e-12,
                 0,
                 "uniform quadratic exact integrated flux");
        }
        conservation(r);
    });
    suite.run("jacobi/gamg results agree", [] {
        auto m = cube(4);
        auto g = take(fv::build_grid(m), "build_grid PC comparison");
        auto p = manufactured(g, polynomials(g, m), {1, {}, true});
        auto a = solve(g, m, p);
        p.preconditioner = "gamg";
        auto b = solve(g, m, p);
        require(relative_difference(a.T_cell, b.T_cell) <= 1e-8, "jacobi/gamg cell agreement");
        std::cout << "measured regular iterations jacobi=" << a.iterations
                  << " gamg=" << b.iterations << '\n';
    });
    suite.run("conductivity endpoints f=0/1 and intermediate fraction", [] {
        for (double f : {0., .3, 1.}) {
            auto m = cube(2);
            m.copper_layer_of_layer = {0};
            m.copper_phi.assign(4, f);
            auto g = take(fv::build_grid(m), "build_grid conductivity");
            auto fractions = fv::copper_fraction(g, m);
            auto p = problem(g);
            p.bcs = {dirichlet("face:zmin", 1), dirichlet("face:zmax", 0)};
            for (std::size_t c = 0; c < p.k.size(); ++c) {
                near(fractions.at(c), f, 0, 1e-14, "endpoint fraction");
                chipcae::material::copper_laminate_conductivity(
                    .3, 390, fractions[c], p.k[c].data());
            }
            auto r = solve(g, m, p);
            Vec expected{
                f * 390 + (1 - f) * .3, f * 390 + (1 - f) * .3, 1 / (f / 390 + (1 - f) / .3)};
            for (auto k : r.k_used)
                for (int d = 0; d < 3; ++d)
                    near(
                        k[d], expected[d], 0, 1e-14, "k_used independently checked mixing formula");
        }
    });
    suite.run("serial repeatability and MPI result contract", [] { mpi_reference("jacobi"); });
    const std::vector<std::pair<std::string, std::function<void(fv::ThermalProblem&)>>>
        bad_problems{
            {"pure Neumann", [](auto& p) { p.bcs.clear(); }},
            {"preconditioner", [](auto& p) { p.preconditioner = "ilu"; }},
            {"stabilization", [](auto& p) { p.stabilization = "unknown"; }},
            {"negative conductivity", [](auto& p) { p.k[0][1] = -1; }},
            {"NaN conductivity",
             [](auto& p) { p.k[0][1] = std::numeric_limits<double>::quiet_NaN(); }},
            {"short conductivity", [](auto& p) { p.k.pop_back(); }},
            {"short source", [](auto& p) { p.q_vol.pop_back(); }},
            {"infinite source",
             [](auto& p) { p.q_vol[0] = std::numeric_limits<double>::infinity(); }},
            {"rtol", [](auto& p) { p.rtol = 0; }},
            {"max_iter", [](auto& p) { p.max_iter = 0; }},
            {"invalid fixed CV",
             [](auto& p) {
                 p.fixed_cv = {-1};
                 p.fixed_T = {1};
             }},
            {"fixed lengths", [](auto& p) { p.fixed_cv = {0}; }},
            {"conflicting fixed CV",
             [](auto& p) {
                 p.fixed_cv = {0, 0};
                 p.fixed_T = {1, 2};
             }},
            {"duplicate Dirichlet", [](auto& p) { p.bcs.push_back(dirichlet("face:zmin", 2)); }},
            {"invalid target", [](auto& p) { p.bcs[0].target = "face:bogus"; }},
            {"NaN temperature",
             [](auto& p) { p.bcs[0].T = std::numeric_limits<double>::quiet_NaN(); }},
            {"negative Robin",
             [](auto& p) {
                 p.bcs[0].kind = fv::ThermalBc::Kind::Robin;
                 p.bcs[0].h = -1;
             }},
            {"invalid face selector",
             [](auto& p) { p.bcs[0].face = std::numeric_limits<std::size_t>::max(); }},
            {"NaN reference", [](auto& p) { p.T_ref = std::numeric_limits<double>::quiet_NaN(); }}};
    for (const auto& [name, mutate] : bad_problems)
        suite.run("reject " + name, [&] {
            auto m = cube(2);
            auto g = take(fv::build_grid(m), "build_grid negative problem");
            auto p = problem(g);
            p.bcs = {dirichlet("face:zmin", 1)};
            mutate(p);
            rejected(fv::solve_thermal(g, m, p, chipcae::Communicator::world()), name);
        });
    suite.run("empty layer target preserves FEM diagnostic", [] {
        auto m = cube(1);
        auto g = take(fv::build_grid(m), "build_grid empty target");
        auto p = problem(g);
        p.bcs = {dirichlet("layer:missing", 1)};
        rejected(fv::solve_thermal(g, m, p, chipcae::Communicator::world()),
                 "empty layer target",
                 "target has no boundary faces: layer:missing");
    });
    suite.run("forced KSP nonconvergence reports iterations and residual", [] {
        auto m = cube(4);
        auto g = take(fv::build_grid(m), "build_grid forced nonconvergence");
        auto p = manufactured(g, polynomials(g, m), {1, {.2, -.7, .3}, true});
        p.max_iter = 1;
        p.rtol = 1e-16;
        auto r = fv::solve_thermal(g, m, p, chipcae::Communicator::world());
        rejected(r, "forced nonconvergence", "residual");
        require(r.error().reason.find("iter") != std::string::npos,
                "nonconvergence iteration diagnostic");
    });
    const std::vector<std::pair<std::string, std::function<void(mesh::HexMeshArrays&)>>> bad_meshes{
        {"short connectivity", [](auto& m) { m.hex.pop_back(); }},
        {"short attributes", [](auto& m) { m.attribute.pop_back(); }},
        {"short gids", [](auto& m) { m.elem_gid.pop_back(); }},
        {"short ownership", [](auto& m) { m.partitioning.pop_back(); }},
        {"invalid attribute", [](auto& m) { m.attribute[0] = 0; }},
        {"invalid vertex", [](auto& m) { m.hex[0] = -1; }},
        {"negative owner", [](auto& m) { m.partitioning[0] = -1; }},
        {"NaN coordinate",
         [](auto& m) { m.vertices[0] = std::numeric_limits<double>::quiet_NaN(); }},
        {"zero volume",
         [](auto& m) {
             for (std::size_t v = 0; v < m.vertices.size() / 3; ++v)
                 m.vertices[3 * v + 2] = 0;
             m.axes[2] = {0, 0};
         }},
        {"short copper map", [](auto& m) {
             m.copper_layer_of_layer = {0};
             m.copper_phi = {};
         }}};
    for (const auto& [name, mutate] : bad_meshes)
        suite.run("reject mesh " + name, [&] {
            auto m = cube(1);
            mutate(m);
            rejected(fv::build_grid(m), name);
        });
    suite.run("shared local geometry gate reports gid and measured defect", [] {
        auto m = cube(1);
        auto g = hand_grid(m, {box_poly({0, 0, 0}, {1, 1, 1})});
        g.cv_elem_gid[0] = 731;
        g.faces[0].centroid[0] += .1;
        auto r = fv::detail::build_local_mfd(g, 0, {1, 2, 3}, "diag");
        rejected(r, "broken geometric identity", "731");
        require(r.error().reason.find("defect") != std::string::npos,
                "geometry failure measured defect");
    });
    suite.run("singular local tensor fails without spectral clipping", [] {
        auto m = cube(1);
        auto g = hand_grid(m, {box_poly({0, 0, 0}, {1, 1, 1})});
        g.cv_elem_gid[0] = 719;
        rejected(
            fv::detail::build_local_mfd(g, 0, {0, 1, 1}, "diag"), "singular local tensor", "719");
    });
    return suite.finish();
}
