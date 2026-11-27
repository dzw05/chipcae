#include "cutfem/experiments.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace chipcae::proto_cutfem {
MaterialParamsSoA mms_material() {
    return {{1}, {.3}, {.01}, {1.3}, 0};
}
Body mms_body() {
    return polygon_body({{.1037, .1219}, {.8913, .1781}, {.8519, .8833}, {.1471, .8247}});
}
double exact_thermal(Point p) {
    return std::sin(2 * p.x + .3) * std::cos(1.5 * p.y - .2) + p.x * p.y;
}
Point exact_thermal_gradient(Point p) {
    return {2 * std::cos(2 * p.x + .3) * std::cos(1.5 * p.y - .2) + p.y,
            -1.5 * std::sin(2 * p.x + .3) * std::sin(1.5 * p.y - .2) + p.x};
}
Point exact_elastic(Point p) {
    return {.1 * std::sin(1.3 * p.x + .4 * p.y), .1 * std::cos(.7 * p.x - 1.1 * p.y)};
}
void exact_elastic_gradient(Point p, double g[2][2]) {
    double s = 1.3 * p.x + .4 * p.y, t = .7 * p.x - 1.1 * p.y;
    g[0][0] = .13 * std::cos(s);
    g[0][1] = .04 * std::cos(s);
    g[1][0] = -.07 * std::sin(t);
    g[1][1] = .11 * std::sin(t);
}
double mms_dT(Point p) {
    return 1 + .5 * std::sin(p.x) * std::cos(p.y);
}
Point elastic_force(Point p, const MaterialParamsSoA& m, int id) {
    double mu = m.E[id] / (2 * (1 + m.nu[id])),
           lam = m.E[id] * m.nu[id] / ((1 + m.nu[id]) * (1 - 2 * m.nu[id])),
           beta = thermal_beta(m, id), s = std::sin(1.3 * p.x + .4 * p.y),
           t = std::cos(.7 * p.x - 1.1 * p.y);
    return {.1 * ((lam + 2 * mu) * 1.69 * s + mu * .16 * s + (lam + mu) * (-.77) * t) +
                beta * .5 * std::cos(p.x) * std::cos(p.y),
            .1 * (mu * .49 * t + (lam + 2 * mu) * 1.21 * t + (lam + mu) * .52 * s) -
                beta * .5 * std::sin(p.x) * std::sin(p.y)};
}
Point elastic_traction(Point p, Point n, const MaterialParamsSoA& m, int id) {
    double g[2][2];
    exact_elastic_gradient(p, g);
    double eps[3] = {g[0][0], g[1][1], g[0][1] + g[1][0]}, sig[3] = {}, D[3][3];
    if (linear_thermoelastic_plane_strain(m, id, eps, mms_dT(p), sig, D))
        throw std::runtime_error("Invalid MMS material");
    return {sig[0] * n.x + sig[2] * n.y, sig[2] * n.x + sig[1] * n.y};
}
namespace {
std::ofstream csv(const Options& o, const std::string& name) {
    std::filesystem::create_directories(o.output);
    std::ofstream f(o.output + "/" + name);
    if (!f)
        throw std::runtime_error("Cannot create output");
    f << std::setprecision(16);
    return f;
}
LinearResult solve(const System& s, const Options& o, bool indefinite = false) {
    if (o.solver == "pcg")
        return conjugate_gradient(s.A, s.rhs);
    if (o.solver == "hypre")
        return hypre_pcg(s.A, s.rhs, s.dofs.ncomp);
    return banded_cholesky(s.A, s.rhs, indefinite);
}
struct Errors {
    double l2, h1;
};
Errors errors(const HexMeshArrays& m,
              const CutLayer& c,
              const System& s,
              const std::vector<double>& u,
              const std::function<Point(Point, int)>& exact,
              const std::function<void(Point, int, double[2][2])>& gradient) {
    double l2 = 0, h1 = 0;
    int nc = s.dofs.ncomp;
    for (int e = 0; e < m.n_elem; ++e)
        if (c.cell_state[e]) {
            CellMap map(m, e);
            auto nodes = m.nodes(e);
            int mat = c.cell_material[e];
            for (auto q = c.vq_offsets[e]; q < c.vq_offsets[e + 1]; ++q) {
                Point r{c.vq_xi[2 * q], c.vq_xi[2 * q + 1]}, x = map.physical(r), v = exact(x, mat);
                double ex[2] = {v.x, v.y}, g[2][2] = {};
                gradient(x, mat, g);
                double N[4], dx[4], dy[4];
                map.shape(r, N, dx, dy);
                for (int a = 0; a < nc; ++a) {
                    double val = 0, gx = 0, gy = 0;
                    for (int i = 0; i < 4; ++i) {
                        double z = u[s.dofs.dof(nodes[i], a)];
                        val += N[i] * z;
                        gx += dx[i] * z;
                        gy += dy[i] * z;
                    }
                    double w = c.vq_w[q] * map.hx * map.hy;
                    l2 += w * (val - ex[a]) * (val - ex[a]);
                    h1 += w * ((gx - g[a][0]) * (gx - g[a][0]) + (gy - g[a][1]) * (gy - g[a][1]));
                }
            }
        }
    return {std::sqrt(l2), std::sqrt(h1)};
}
} // namespace
void run_mms(bool elastic, bool bimat, const Options& o) {
    auto out =
        csv(o, elastic ? "mms_elastic.csv" : (bimat ? "mms_thermal_bimat.csv" : "mms_thermal.csv"));
    out << "N,h,variant,gamma_N,gamma_g,ndof,L2,H1,order_L2,order_H1,iterations,residual,status\n";
    std::vector<int> levels = o.N ? std::vector<int>{o.N} : std::vector<int>{8, 16, 32, 64, 128};
    int variants = bimat ? 1 : 2;
    for (int variant = 0; variant < variants; ++variant) {
        double prevl = 0, prevh = 0, prevsize = 0;
        for (int N : levels) {
            auto m = tensor_mesh(linspace(0, 1, N), linspace(0, 1, N));
            auto body = mms_body();
            std::vector<Body> bodies{body};
            Problem p;
            p.ncomp = elastic ? 2 : 1;
            p.gamma_N = o.gamma_N;
            p.gamma_g = elastic && variant == 1 ? 0 : o.gamma_g;
            p.materials = mms_material();
            if (bimat) {
                p.materials = {{1, 1}, {.3, .3}, {.01, .01}, {1, 10}, 0};
                bodies = {body, body};
                bodies[0].halfplanes.push_back({0, 1, -.5, -1});
                bodies[1].halfplanes.push_back({0, -1, .5, -1});
                bodies[1].material_id = 1;
            }
            auto c = build_cut_layer(m, bodies, o.quadrature);
            auto tex = [bimat](Point x, int mat) {
                if (!bimat)
                    return exact_thermal(x);
                double k = mat ? 10 : 1;
                return std::sin(2 * x.x + .5) * (1 + std::sin(x.y - .5) / k);
            };
            auto tgrad = [bimat](Point x, int mat) {
                if (!bimat)
                    return exact_thermal_gradient(x);
                double k = mat ? 10 : 1;
                return Point{2 * std::cos(2 * x.x + .5) * (1 + std::sin(x.y - .5) / k),
                             std::sin(2 * x.x + .5) * std::cos(x.y - .5) / k};
            };
            if (elastic) {
                p.delta_temperature = mms_dT;
                p.source = [&](Point x, int mat) { return elastic_force(x, p.materials, mat); };
                p.boundary = [&](int tag, Point x, Point n) {
                    return BoundaryData{tag == 3 ? BCType::Traction : BCType::Dirichlet,
                                        tag == 3 ? elastic_traction(x, n, p.materials)
                                                 : exact_elastic(x)};
                };
            } else {
                p.source = [&](Point x, int mat) {
                    double k = p.materials.k[mat];
                    return Point{bimat
                                     ? 4 * k * std::sin(2 * x.x + .5) +
                                           5 * std::sin(2 * x.x + .5) * std::sin(x.y - .5)
                                     : k * 6.25 * std::sin(2 * x.x + .3) * std::cos(1.5 * x.y - .2),
                                 0};
                };
                p.boundary = [&](int tag, Point x, Point n) {
                    int mat = bimat && x.y > .5 ? 1 : 0;
                    double t = tex(x, mat);
                    if (!bimat && variant == 1 && tag == 0)
                        return BoundaryData{BCType::Robin,
                                            {},
                                            2,
                                            0,
                                            p.materials.k[mat] * dot(tgrad(x, mat), n) + 2 * t};
                    return BoundaryData{BCType::Dirichlet, {t, 0}};
                };
            }
            auto s = assemble(m, c, p);
            auto u = solve(s, o, elastic && variant == 1);
            auto er = errors(
                m,
                c,
                s,
                u.x,
                [&](Point x, int mat) {
                    return elastic ? exact_elastic(x) : Point{tex(x, mat), 0};
                },
                [&](Point x, int mat, double g[2][2]) {
                    if (elastic)
                        exact_elastic_gradient(x, g);
                    else {
                        auto a = tgrad(x, mat);
                        g[0][0] = a.x;
                        g[0][1] = a.y;
                    }
                });
            double h = 1. / N, ol = prevl ? std::log(prevl / er.l2) / std::log(prevsize / h) : 0,
                   oh = prevh ? std::log(prevh / er.h1) / std::log(prevsize / h) : 0;
            out << N << ',' << h << ','
                << (elastic ? (variant ? "gp_off" : "gp_on")
                            : (bimat ? "bimat" : (variant ? "robin" : "dirichlet")))
                << ',' << p.gamma_N << ',' << p.gamma_g << ',' << s.A.n << ',' << er.l2 << ','
                << er.h1 << ',' << ol << ',' << oh << ',' << u.stats.iterations << ','
                << u.stats.residual << ',' << u.stats.status << '\n';
            out.flush();
            std::cout << (elastic ? "elastic" : (bimat ? "thermal_bimat" : "thermal"))
                      << " variant=" << variant << " N=" << N << " L2=" << er.l2 << " H1=" << er.h1
                      << " orders=" << ol << "," << oh << " " << u.stats.status << std::endl;
            prevl = er.l2;
            prevh = er.h1;
            prevsize = h;
        }
    }
}
namespace {
constexpr double t1 = .47e-3, t2 = .31e-3, thick = t1 + t2, L = 25 * thick;
MaterialParamsSoA strip_material() {
    return {{20e9, 130e9}, {.3, .28}, {1e-5, 2.6e-6}, {.9, 150}, 125};
}
double curvature_reference() {
    auto p = strip_material();
    double a1 = p.alpha[0] * (1 + p.nu[0]), a2 = p.alpha[1] * (1 + p.nu[1]),
           E1 = p.E[0] / (1 - p.nu[0] * p.nu[0]), E2 = p.E[1] / (1 - p.nu[1] * p.nu[1]),
           m = t1 / t2, n = E1 / E2;
    // eps_xx = u0' - y*uy'': larger contraction below requires uy'' < 0.
    return -6 * (a2 - a1) * (-100) * (1 + m) * (1 + m) /
           (thick * (3 * (1 + m) * (1 + m) + (1 + m * n) * (m * m + 1 / (m * n))));
}
struct StripResult {
    double kappa = 0, kappa_narrow = 0, warpage = 0, thermal_error = 0, residual = 0;
    int ndof = 0;
};
double fit_curvature(const HexMeshArrays& m,
                     const CutLayer& c,
                     const chipcae::k5_stub::CvField& u,
                     double x0,
                     double window) {
    (void)c;
    double sum[5] = {}, rhs[3] = {};
    int count = 0;
    for (int n = 0; n < m.n_vert; ++n) {
        auto x = m.node(n);
        double s = (x.x - (x0 + L / 2)) / L;
        if (std::abs(x.y) > 1e-14 || std::abs(s) > window + 1e-12)
            continue;
        double v = u.values[2 * n + 1];
        double power = 1;
        for (int j = 0; j < 5; ++j) {
            sum[j] += power;
            if (j < 3)
                rhs[j] += power * v;
            power *= s;
        }
        ++count;
    }
    if (count < 3)
        throw std::runtime_error("Too few interface nodes for curvature fit");
    double a[3][4];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            a[i][j] = sum[i + j];
        a[i][3] = rhs[i];
    }
    for (int i = 0; i < 3; ++i) {
        double d = a[i][i];
        for (int j = i; j < 4; ++j)
            a[i][j] /= d;
        for (int k = 0; k < 3; ++k)
            if (k != i) {
                double v = a[k][i];
                for (int j = i; j < 4; ++j)
                    a[k][j] -= v * a[i][j];
            }
    }
    return 2 * a[2][3] / (L * L);
}
StripResult strip(int Nt,
                  bool fitted,
                  double gg,
                  const Options& o,
                  bool kokkos = false,
                  std::vector<double>* displacement = nullptr) {
    double hc = thick / Nt;
    if (!fitted) {
        auto aligns = [&](double thickness) {
            double q = thickness / hc;
            return std::abs(q - std::round(q)) < 1e-3;
        };
        while (aligns(t1) || aligns(t2))
            hc *= 1 + std::sqrt(2.) * 1e-4;
    }
    double x0 = fitted ? 0 : 1.37 * hc;
    std::vector<double> x, y;
    if (fitted) {
        x = linspace(0, L, 25 * Nt);
        int n1 = std::max(1, int(std::round(t1 / hc))), n2 = std::max(1, int(std::round(t2 / hc)));
        y = linspace(-t1, 0, n1);
        auto yt = linspace(0, t2, n2);
        y.insert(y.end(), yt.begin() + 1, yt.end());
    } else {
        x = linspace(0, std::ceil((x0 + L) / hc) * hc, int(std::ceil((x0 + L) / hc)));
        int below = int(std::ceil(t1 / hc)) + 1, above = int(std::ceil(t2 / hc)) + 1;
        for (int j = -below; j <= above; ++j)
            y.push_back(j * hc);
        for (double bound : {x0, x0 + L, -t1, t2}) {
            double fraction = std::abs(bound / hc - std::round(bound / hc));
            if (fraction < 1e-3)
                throw std::runtime_error("Unexpected aligned strip boundary");
        }
    }
    auto m = tensor_mesh(x, y);
    auto bottom = rectangle_body(x0, x0 + L, -t1, 0, 0, 0),
         top = rectangle_body(x0, x0 + L, 0, t2, 1, 4);
    bottom.halfplanes[2].tag = -1;
    top.halfplanes[0].tag = -1;
    auto c = build_cut_layer(m, {bottom, top}, o.quadrature);
    Problem thermal;
    thermal.kokkos_volume = kokkos;
    thermal.materials = strip_material();
    thermal.gamma_g = gg;
    thermal.gamma_N = o.gamma_N;
    thermal.boundary = [](int, Point, Point) { return BoundaryData{BCType::Robin, {}, 10, 25, 0}; };
    PrototypeSolver solver;
    auto ts = solver.solve_step({m, c, thermal, {}, o.solver});
    if (ts.status)
        throw std::runtime_error("Strip thermal solve failed");
    StripResult result;
    auto dofs = active_dofs(m, c, 2);
    for (int n : dofs.active_to_node)
        result.thermal_error =
            std::max(result.thermal_error, std::abs(ts.temperature.values[n] - 25));
    Problem elastic = thermal;
    elastic.ncomp = 2;
    elastic.temperature = &ts.temperature;
    elastic.boundary = [](int, Point, Point) { return BoundaryData{}; };
    int nodeA = -1, nodeB = -1;
    double distA = 1e300, distB = 1e300;
    // Both constraints must be strictly inside the body, on the material interface.
    for (int n : dofs.active_to_node) {
        auto p = m.node(n);
        if (std::abs(p.y) > 1e-14 || p.x <= x0 || p.x >= x0 + L)
            continue;
        double da = std::abs(p.x - (x0 + L / 2)), db = std::abs(p.x - (x0 + 3 * L / 4));
        if (da < distA) {
            distA = da;
            nodeA = n;
        }
        if (db < distB) {
            distB = db;
            nodeB = n;
        }
    }
    if (nodeA < 0 || nodeB < 0 || nodeA == nodeB)
        throw std::runtime_error("Invalid rigid mode constraints");
    auto step = solver.solve_step(
        {m,
         c,
         elastic,
         {{dofs.dof(nodeA, 0), 0}, {dofs.dof(nodeA, 1), 0}, {dofs.dof(nodeB, 1), 0}},
         o.solver});
    if (step.status)
        throw std::runtime_error("Strip elastic solve failed");
    const auto& u = step.displacement;
    if (displacement)
        *displacement = u.values;
    if (step.stress.values.empty() || step.stress.offsets != c.vq_offsets)
        throw std::runtime_error("Invalid stress CSR field");
    result.kappa = fit_curvature(m, c, u, x0, .25);
    result.kappa_narrow = fit_curvature(m, c, u, x0, .125);
    result.ndof = 2 * dofs.nnode;
    result.residual = step.solver_stats.residual;
    step.curvature = result.kappa;
    // Evaluate the Q1 displacement at the physical top surface, including endpoints.
    double ymin = 1e300, ymax = -1e300;
    // At fixed y, Q1 is piecewise linear in x: extrema are exactly at these knots.
    std::vector<double> top_knots{x0, x0 + L};
    for (double xx : x)
        if (xx > x0 && xx < x0 + L)
            top_knots.push_back(xx);
    for (double xx : top_knots) {
        Point p{xx, t2};
        int ix = std::clamp(
                int(std::upper_bound(x.begin(), x.end(), p.x) - x.begin()) - 1, 0, m.nx() - 1),
            iy = std::clamp(
                int(std::lower_bound(y.begin(), y.end(), p.y) - y.begin()) - 1, 0, m.ny() - 1);
        int e = ix * m.ny() + iy;
        CellMap map(m, e);
        double N[4], dx[4], dy[4];
        map.shape(map.reference(p), N, dx, dy);
        double v = 0;
        auto ns = m.nodes(e);
        for (int a = 0; a < 4; ++a)
            v += N[a] * u.values[2 * ns[a] + 1];
        ymin = std::min(ymin, v);
        ymax = std::max(ymax, v);
    }
    step.warpage = ymax - ymin;
    result.warpage = step.warpage;
    return result;
}
} // namespace
#ifdef CUTFEM_WITH_KOKKOS
void test_kokkos_timoshenko() {
    Options o;
    std::vector<double> host, device;
    strip(16, false, .1, o, false, &host);
    strip(16, false, .1, o, true, &device);
    double error = 0, scale = 0;
    for (size_t i = 0; i < host.size(); ++i) {
        error = std::max(error, std::abs(host[i] - device[i]));
        scale = std::max(scale, std::abs(host[i]));
    }
    if (error > 1e-12 * scale)
        throw std::runtime_error("Timoshenko Nt=16 host/Kokkos solution mismatch");
    std::cout << "PASS Kokkos Timoshenko Nt=16 displacement relative error=" << error / scale
              << '\n';
}
#endif
void run_timoshenko(const Options& o) {
    auto out = csv(o, "timoshenko.csv");
    out << "Nt,h,gamma_N,gamma_g,ndof,kappa_cut,kappa_cut_narrow,kappa_fitted,kappa_fitted_narrow,"
           "kappa_"
           "ref,rel_err_cut,rel_err_fitted,warpage_cut,warpage_fitted,thermal_error_cut,thermal_"
           "error_fitted,residual_cut,parameters\n";
    double ref = curvature_reference();
    std::vector<int> levels = o.N ? std::vector<int>{o.N} : std::vector<int>{4, 8, 16, 32, 64};
    for (int Nt : levels) {
        auto fitted = strip(Nt, true, 0, o);
        for (auto [gn, gg] : std::vector<std::pair<double, double>>{
                 {10, .01}, {10, .1}, {10, .3}, {10, 1.}, {20, .1}, {40, .1}}) {
            Options params = o;
            params.gamma_N = gn;
            auto cut = strip(Nt, false, gg, params);
            out << Nt << ',' << thick / Nt << ',' << gn << ',' << gg << ',' << cut.ndof << ','
                << cut.kappa << ',' << cut.kappa_narrow << ',' << fitted.kappa << ','
                << fitted.kappa_narrow << ',' << ref << ',' << std::abs((cut.kappa - ref) / ref)
                << ',' << std::abs((fitted.kappa - ref) / ref) << ',' << cut.warpage << ','
                << fitted.warpage << ',' << cut.thermal_error << ',' << fitted.thermal_error << ','
                << cut.residual << ",example parameters\n";
            out.flush();
            std::cout << "timoshenko example parameters Nt=" << Nt << " gamma_g=" << gg
                      << " cut=" << cut.kappa << " fitted=" << fitted.kappa << " ref=" << ref
                      << " T_error=" << cut.thermal_error << std::endl;
        }
    }
}
void run_gp_sweep(const Options& o) {
    auto out = csv(o, "gp_sweep.csv");
    out << "physics,vf,min_volfrac,gamma_N,gamma_g,ndof,jacobi_iterations,jacobi_kappa_estimate,"
           "jacobi_residual,"
           "jacobi_status,cg_iterations,cg_kappa_estimate,cg_residual,cg_status,amg_iterations,amg_"
           "residual,"
           "amg_status,indefinite,ldl_indefinite\n";
    for (int variant : {0, 1, 2})
        for (double gn : {10., 20., 40.})
            for (double vf : {.5, .1, 1e-2, 1e-3, 1e-4})
                for (double gg : {0., .03, .1, .3, 1., 3.}) {
                    int nc = variant == 0 ? 1 : 2;
                    const std::string physics =
                        variant == 0 ? "thermal"
                                     : (variant == 1 ? "elastic" : "elastic_dirichlet_cut");
                    auto m = tensor_mesh(linspace(0, 4, 64), linspace(0, 1, 16));
                    double hx = 4. / 64, hy = 1. / 16;
                    auto body =
                        rectangle_body(3.37 * hx, (59 + vf) * hx, 2.27 * hy, (13 + vf) * hy);
                    auto c = build_cut_layer(m, {body}, o.quadrature);
                    Problem p;
                    p.ncomp = nc;
                    p.gamma_N = gn;
                    p.gamma_g = gg;
                    p.materials = {{1}, {.3}, {1e-3}, {1.3}, 0};
                    if (nc == 1) {
                        p.source = [](Point, int) { return Point{1, 0}; };
                        p.boundary = [](int, Point x, Point) {
                            return BoundaryData{BCType::Dirichlet, {1 + .2 * x.x, 0}};
                        };
                    } else {
                        p.delta_temperature = [](Point) { return -100.; };
                        p.boundary = [variant](int tag, Point, Point) {
                            return tag == (variant == 2 ? 1 : 3)
                                       ? BoundaryData{BCType::Dirichlet, {0, 0}}
                                       : BoundaryData{};
                        };
                    }
                    auto s = assemble(m, c, p);
                    if (o.dump_matrices) {
                        auto dir = std::filesystem::path(o.output) / "debug";
                        std::filesystem::create_directories(dir);
                        std::ostringstream name;
                        name << physics << "_N" << gn << "_g" << gg << "_vf" << vf << ".mtx";
                        std::ofstream matrix(dir / name.str());
                        if (!matrix)
                            throw std::runtime_error("Cannot create matrix dump");
                        size_t nnz = 0;
                        for (double value : s.A.val)
                            if (value != 0)
                                ++nnz;
                        matrix << "%%MatrixMarket matrix coordinate real general\n"
                               << s.A.n << ' ' << s.A.n << ' ' << nnz << '\n'
                               << std::setprecision(17);
                        for (int i = 0; i < s.A.n; ++i)
                            for (int k = s.A.row[i]; k < s.A.row[i + 1]; ++k)
                                if (s.A.val[k] != 0)
                                    matrix << i + 1 << ' ' << s.A.col[k] + 1 << ' ' << s.A.val[k]
                                           << '\n';
                    }
                    auto j = conjugate_gradient(s.A, s.rhs, true),
                         cg = conjugate_gradient(s.A, s.rhs, false);
                    LinearResult amg;
                    bool indefinite = j.stats.indefinite || cg.stats.indefinite;
                    // Independent algebraic check: distinguish genuine loss of coercivity
                    // from a possible iterative-roundoff breakdown without tuning penalties.
                    auto diagnostic = banded_cholesky(s.A, s.rhs, true);
                    bool ldl_indefinite = diagnostic.stats.indefinite;
                    if (!hypre_available())
                        amg.stats.status = "unavailable";
                    else if (indefinite || ldl_indefinite)
                        amg.stats.status = "skipped_indefinite";
                    else
                        amg = hypre_pcg(s.A, s.rhs, nc);
                    double minvf = 1;
                    for (int e = 0; e < m.n_elem; ++e)
                        if (c.cell_state[e])
                            minvf = std::min(minvf, c.cell_volfrac[e]);
                    out << physics << ',' << vf << ',' << minvf << ',' << gn << ',' << gg << ','
                        << s.A.n << ',' << j.stats.iterations << ',' << j.stats.cond_est << ','
                        << j.stats.residual << ',' << j.stats.status << ',' << cg.stats.iterations
                        << ',' << cg.stats.cond_est << ',' << cg.stats.residual << ','
                        << cg.stats.status << ',' << amg.stats.iterations << ','
                        << amg.stats.residual << ',' << amg.stats.status << ',' << indefinite << ','
                        << ldl_indefinite << '\n';
                    out.flush();
                    std::cout << "gp_sweep " << physics << " gamma_N=" << gn << " vf=" << vf
                              << " gg=" << gg << " Jacobi=" << j.stats.iterations << " "
                              << j.stats.status << " CG=" << cg.stats.iterations << " "
                              << cg.stats.status << " AMG=" << amg.stats.iterations << " "
                              << amg.stats.status << std::endl;
                }
}
} // namespace chipcae::proto_cutfem
