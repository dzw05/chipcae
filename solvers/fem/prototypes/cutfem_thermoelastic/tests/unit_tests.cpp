#include "cutfem/experiments.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace chipcae::proto_cutfem {
namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error("TEST FAILED: " + message);
}
void near(double a, double b, double tol, const std::string& message) {
    require(std::abs(a - b) < tol, message + " error=" + std::to_string(std::abs(a - b)));
}
void quadrature_test() {
    auto m = tensor_mesh(linspace(0, 1, 7), linspace(0, 1, 9));
    auto body = mms_body();
    auto c = build_cut_layer(m, {body});
    double area = 0, perimeter = 0;
    std::vector<double> edges(4, 0);
    int64_t cv = 0;
    for (int e = 0; e < m.n_elem; ++e) {
        CellMap map(m, e);
        double sum = 0;
        for (auto q = c.vq_offsets[e]; q < c.vq_offsets[e + 1]; ++q)
            sum += c.vq_w[q];
        near(sum, c.cell_volfrac[e], 2e-14, "reference quadrature area");
        area += sum * map.hx * map.hy;
        if (c.cell_state[e])
            require(c.cv_id[e] == cv++, "CV compact ordering");
        else
            require(c.cv_id[e] == -1, "inactive CV");
    }
    for (size_t q = 0; q < c.bq_w_phys.size(); ++q) {
        perimeter += c.bq_w_phys[q];
        edges[c.bq_tag[q]] += c.bq_w_phys[q];
        near(std::hypot(c.bq_normal[2 * q], c.bq_normal[2 * q + 1]), 1, 1e-14, "unit normal");
    }
    Polygon p = {{.1037, .1219}, {.8913, .1781}, {.8519, .8833}, {.1471, .8247}};
    double exactp = 0;
    for (int i = 0; i < 4; ++i) {
        auto d = p[(i + 1) % 4] - p[i];
        double len = std::hypot(d.x, d.y);
        exactp += len;
        near(edges[i], len, 1e-12, "boundary tag length");
    }
    near(area, polygon_area(p), 1e-13, "clipped polygon area");
    near(perimeter, exactp, 1e-12, "clipped polygon perimeter");
    // Genuine body edges on grid faces must be included once (fitted reference).
    auto full = tensor_mesh(linspace(0, 1, 4), linspace(0, 1, 4));
    auto cf = build_cut_layer(full, {rectangle_body(0, 1, 0, 1)});
    double pf = 0;
    for (double w : cf.bq_w_phys)
        pf += w;
    near(pf, 4, 1e-13, "grid-aligned body perimeter");
    for (auto f : c.gp_faces)
        require(c.cell_material[f.cell1] == c.cell_material[f.cell2], "GP material identity");
    std::cout << "PASS cut area/perimeter, tags, offsets, CV IDs and fitted boundary\n";
}
void recursive_test() {
    HalfPlane h{.7, 1, -.82, 0};
    auto exact = polygon_quadrature(clip_polygon({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, h));
    auto recursive = recursive_quadrature([&](Point p) { return h.value(p); }, 4);
    for (int i = 0; i <= 4; ++i)
        for (int j = 0; j <= 4 - i; ++j) {
            auto moment = [&](const auto& q) {
                double a = 0;
                for (auto x : q)
                    a += x.w * std::pow(x.p.x, i) * std::pow(x.p.y, j);
                return a;
            };
            near(moment(exact), moment(recursive), 3e-14, "recursive planar moment");
        }
    auto m = tensor_mesh(linspace(0, 1, 3), linspace(0, 1, 3));
    auto direct = build_cut_layer(m, {Body{{h}, 0}});
    auto layer = build_level_set_layer(m, [&](Point p) { return h.value(p); }, 3);
    double perimeter[2] = {}, area[2] = {};
    int index = 0;
    for (const auto* cut : {&direct, &layer}) {
        for (int e = 0; e < m.n_elem; ++e) {
            CellMap map(m, e);
            for (auto q = cut->vq_offsets[e]; q < cut->vq_offsets[e + 1]; ++q)
                area[index] += cut->vq_w[q] * map.hx * map.hy;
        }
        for (double w : cut->bq_w_phys)
            perimeter[index] += w;
        ++index;
    }
    near(area[0], area[1], 1e-13, "recursive CutLayer area");
    near(perimeter[0], perimeter[1], 1e-12, "recursive CutLayer boundary");
    std::cout << "PASS recursive subdivision vs half-plane (degree 4 moments and CutLayer CSR)\n";
}
void patch_test(int nc) {
    auto m = tensor_mesh(linspace(0, 1, 12), linspace(0, 1, 12));
    auto c = build_cut_layer(m, {mms_body()});
    Problem p;
    p.ncomp = nc;
    p.materials = mms_material();
    auto exact = [&](Point x) {
        return Point{.3 + .12 * x.x - .21 * x.y, -.2 + .17 * x.x + .08 * x.y};
    };
    p.delta_temperature = [](Point x) { return 1 + .5 * x.x; };
    p.source = [&](Point, int) {
        return nc == 1 ? Point{} : Point{.5 * thermal_beta(p.materials, 0), 0};
    };
    p.boundary = [&](int, Point x, Point) { return BoundaryData{BCType::Dirichlet, exact(x)}; };
    auto s = assemble(m, c, p);
    auto u = banded_cholesky(s.A, s.rhs);
    double err = 0;
    for (int n : s.dofs.active_to_node) {
        Point ex = exact(m.node(n));
        err = std::max(err, std::abs(u.x[s.dofs.dof(n, 0)] - ex.x));
        if (nc == 2)
            err = std::max(err, std::abs(u.x[s.dofs.dof(n, 1)] - ex.y));
    }
    require(err < 1e-10, "linear Q1 patch test, including thermal Nitsche term");
    require(u.stats.residual < 1e-12, "patch linear residual");
    std::cout << "PASS " << (nc == 1 ? "thermal" : "elastic")
              << " linear cut patch max nodal error=" << err << '\n';
}
void force_test() {
    auto m = mms_material();
    double maxerr = 0;
    for (Point p : std::vector<Point>{{.2, .3}, {.7, .6}, {.4, .8}}) {
        double h = 1e-5;
        auto sxp = elastic_traction({p.x + h, p.y}, {1, 0}, m),
             sxm = elastic_traction({p.x - h, p.y}, {1, 0}, m),
             syp = elastic_traction({p.x, p.y + h}, {0, 1}, m),
             sym = elastic_traction({p.x, p.y - h}, {0, 1}, m);
        Point fd = (-1 / (2 * h)) * (sxp - sxm + syp - sym), f = elastic_force(p, m);
        maxerr = std::max({maxerr, std::abs(fd.x - f.x), std::abs(fd.y - f.y)});
    }
    require(maxerr < 2e-10, "MMS force finite difference");
    std::cout << "PASS elastic -div(sigma) finite difference max error=" << maxerr << '\n';
}
void solver_test() {
    std::vector<std::vector<int>> rows(31);
    for (int i = 0; i < 31; ++i)
        for (int j = std::max(0, i - 1); j <= std::min(30, i + 1); ++j)
            rows[i].push_back(j);
    auto A = CSRMatrix::pattern(rows);
    std::vector<double> b(31, 1);
    for (int i = 0; i < 31; ++i) {
        A.add(i, i, 2.5);
        if (i)
            A.add(i, i - 1, -1);
        if (i < 30)
            A.add(i, i + 1, -1);
    }
    A.constrain(b, {{0, .3}, {30, -.7}});
    auto d = banded_cholesky(A, b), cg = conjugate_gradient(A, b, true, 1e-12);
    require(cg.stats.status == "ok", "PCG converges");
    double err = 0;
    for (int i = 0; i < 31; ++i)
        err = std::max(err, std::abs(d.x[i] - cg.x[i]));
    require(err < 1e-11, "banded vs PCG");
    near(d.x[0], .3, 1e-14, "nonzero elimination");
    near(d.x[30], -.7, 1e-14, "nonzero elimination");
    auto diag = CSRMatrix::pattern({{0}, {1}, {2}, {3}});
    std::vector<double> v{1, 1, 1, 1};
    for (int i = 0; i < 4; ++i)
        diag.add(i, i, double(1 << i));
    auto un = conjugate_gradient(diag, v, false, 1e-12);
    near(un.stats.cond_est, 8, 1e-10, "Lanczos extrema");
    auto jac = conjugate_gradient(diag, v, true, 1e-12);
    near(jac.stats.cond_est, 1, 1e-12, "Jacobi spectrum");
    auto bad = CSRMatrix::pattern({{0}, {1}});
    bad.add(0, 0, 1);
    bad.add(1, 1, -1);
    require(conjugate_gradient(bad, {0, 1}, false).stats.indefinite,
            "negative curvature detection");
    if (hypre_available()) {
        auto amg = hypre_pcg(A, b);
        require(amg.stats.status == "ok" && amg.stats.residual < 1.1e-8, "hypre PCG smoke test");
        std::cout << "PASS hypre PCG iterations=" << amg.stats.iterations << '\n';
    }
    std::cout
        << "PASS banded/PCG agreement, elimination, Lanczos eigenvalues, indefinite detection\n";
}
void material_test() {
    auto m = mms_material();
    double e[3] = {.1, -.03, .07}, s[3], D[3][3];
    require(!linear_thermoelastic_plane_strain(m, 0, e, 4, s, D), "material status");
    double mu = 1 / (2 * 1.3), lam = .3 / (1.3 * .4);
    near(
        s[0], lam * .07 + 2 * mu * .1 - (3 * lam + 2 * mu) * .01 * 4, 1e-14, "plane strain stress");
    near(s[2], mu * .07, 1e-14, "engineering shear");
    require(linear_thermoelastic_plane_strain(m, 8, e, 0, s, D) != 0,
            "invalid material returns status");
    std::cout << "PASS plane strain material and engineering shear\n";
}
} // namespace
int run_unit_tests() {
    material_test();
    quadrature_test();
    recursive_test();
    force_test();
    patch_test(1);
    patch_test(2);
    solver_test();
    std::cout << "All unit tests passed\n";
    return 0;
}
} // namespace chipcae::proto_cutfem
