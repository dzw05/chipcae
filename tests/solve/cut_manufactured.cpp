#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "chipcae/solvers/fem/cut_fem.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
using namespace chipcae;
using namespace mfem;
namespace {
void require(bool b, const char* message) {
    if (!b)
        throw std::runtime_error(message);
}
mesh::cut::CutGeometry geometry(bool sphere, int n) {
    mesh::cut::CutGeometry g;
    if (sphere) {
        // Circumscribed planar sphere approximation. Geometry is explicitly
        // faceted; refining the normal sampling accompanies background h.
        for (int axis = 0; axis < 3; ++axis)
            for (int sign : {-1, 1})
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j) {
                        std::array<double, 3> v{};
                        v[axis] = sign;
                        v[(axis + 1) % 3] = -1. + 2. * (i + .5) / n;
                        v[(axis + 2) % 3] = -1. + 2. * (j + .5) / n;
                        double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                        for (auto& x : v)
                            x /= length;
                        g.planes.push_back({0, v, .35 + .5 * (v[0] + v[1] + v[2])});
                    }
    } else {
        double a = 17 * std::acos(-1.) / 180, b = 23 * std::acos(-1.) / 180;
        std::array<std::array<double, 3>, 3> axes = {
            {{std::cos(a) * std::cos(b), std::sin(a) * std::cos(b), -std::sin(b)},
             {-std::sin(a), std::cos(a), 0},
             {std::cos(a) * std::sin(b), std::sin(a) * std::sin(b), std::cos(b)}}};
        for (auto v : axes)
            for (int sign : {-1, 1}) {
                for (auto& x : v)
                    x *= sign;
                g.planes.push_back({0, v, .28 + .5 * (v[0] + v[1] + v[2])});
                for (auto& x : v)
                    x *= sign;
            }
    }
    return g;
}
struct Domain {
    mesh::HexMeshArrays arrays;
    mesh::cut::CutLayerArrays cut;
    std::unique_ptr<Mesh> grid;
    Domain(int n, bool sphere, int degree) {
        mesh::StackModel s;
        s.outline.box = {0, 0, 1, 1};
        mesh::Layer l;
        l.name = "solid";
        l.material = "solid";
        l.kind = "Explicit";
        l.thickness = 1;
        l.n_elem_min = n;
        s.layers = {l};
        mesh::GridOptions o;
        o.nx = o.ny = n;
        o.levels = 1;
        o.max_ratio = 10;
        o.cell_budget = 100000;
        auto built = mesh::build_tensor_grid(s, o);
        require(bool(built), "background grid");
        arrays = std::move(built.value());
        mesh::cut::CutOptions co;
        co.quad_degree = degree;
        co.export_polyhedra = true;
        auto clipped = mesh::cut::build_cut_layer(arrays, geometry(sphere, n), co);
        require(bool(clipped), "cut grid");
        cut = std::move(clipped.value());
        grid = std::make_unique<Mesh>(arrays.vertices.data(),
                                      arrays.node_gid.size(),
                                      arrays.hex.data(),
                                      Geometry::CUBE,
                                      arrays.attribute.data(),
                                      arrays.attribute.size(),
                                      arrays.bdr_quad.data(),
                                      Geometry::SQUARE,
                                      arrays.bdr_attribute.data(),
                                      arrays.bdr_attribute.size(),
                                      3,
                                      3);
        grid->Finalize(false, false);
        const auto& p = *cut.polyhedra;
        for (std::size_t cell = 0; cell < p.volume.size(); ++cell) {
            std::map<std::pair<Index, Index>, int> edges;
            long double volume = 0;
            std::array<long double, 3> origin{};
            auto v0 = p.face_conn[p.face_off[p.poly_faces[p.poly_off[cell]]]];
            for (int d = 0; d < 3; ++d)
                origin[d] = p.points[3 * v0 + d];
            for (Index j = p.poly_off[cell]; j < p.poly_off[cell + 1]; ++j) {
                auto f = p.poly_faces[j];
                for (Index k = p.face_off[f]; k < p.face_off[f + 1]; ++k) {
                    auto a = p.face_conn[k],
                         b = p.face_conn[k + 1 == p.face_off[f + 1] ? p.face_off[f] : k + 1];
                    edges[{std::min(a, b), std::max(a, b)}] += a < b ? 1 : -1;
                }
                auto a = p.face_conn[p.face_off[f]];
                for (Index k = p.face_off[f] + 1; k + 1 < p.face_off[f + 1]; ++k) {
                    auto b = p.face_conn[k], c = p.face_conn[k + 1];
                    long double x[3], y[3], z[3];
                    for (int d = 0; d < 3; ++d) {
                        x[d] = p.points[3 * a + d] - origin[d];
                        y[d] = p.points[3 * b + d] - origin[d];
                        z[d] = p.points[3 * c + d] - origin[d];
                    }
                    volume +=
                        (x[0] * (y[1] * z[2] - y[2] * z[1]) + x[1] * (y[2] * z[0] - y[0] * z[2]) +
                         x[2] * (y[0] * z[1] - y[1] * z[0])) /
                        6;
                }
            }
            for (auto [edge, balance] : edges)
                require(balance == 0, "closed outward face rings");
            require(volume > 0 && std::abs(volume - p.volume[cell]) <= 1e-12L * volume,
                    "divergence volume mismatch");
        }
    }
};
void exact(const Vector& x, Vector& v) {
    v.SetSize(3);
    v[0] = std::sin(x[0]);
    v[1] = std::cos(x[1]);
    v[2] = std::exp(x[2]);
}
class Force : public LinearFormIntegrator {
    const fem::CutRules& rules_;
    const mesh::cut::CutLayerArrays& c_;
    bool elastic_;

public:
    Force(const fem::CutRules& r, const mesh::cut::CutLayerArrays& c, bool elastic)
        : rules_(r), c_(c), elastic_(elastic) {}
    using LinearFormIntegrator::AssembleRHSElementVect;
    void AssembleRHSElementVect(const FiniteElement& el,
                                ElementTransformation& t,
                                Vector& out) override {
        int nd = el.GetDof();
        out.SetSize(nd * (elastic_ ? 3 : 1));
        out = 0.;
        Vector shape(nd), x(3);
        const auto* cutrule = rules_.get(t.ElementNo);
        const auto& rule = cutrule ? *cutrule : IntRules.Get(Geometry::CUBE, 10);
        for (int q = 0; q < rule.GetNPoints(); ++q) {
            const auto& ip = rule.IntPoint(q);
            t.SetIntPoint(&ip);
            t.Transform(ip, x);
            el.CalcShape(ip, shape);
            double f[3] = {std::sin(x[0]), std::cos(x[1]), -std::exp(x[2])};
            if (elastic_)
                for (int c = 0; c < 3; ++c)
                    for (int i = 0; i < nd; ++i)
                        out[c * nd + i] += 3 * f[c] * ip.weight * t.Weight() * shape[i];
            else
                for (int i = 0; i < nd; ++i)
                    out[i] += (f[0] + f[1] + f[2]) * ip.weight * t.Weight() * shape[i];
        }
        auto it = std::find(c_.cut_elem.begin(), c_.cut_elem.end(), t.ElementNo);
        if (it == c_.cut_elem.end())
            return;
        int s = it - c_.cut_elem.begin();
        for (int q = c_.bq_off[s]; q < c_.bq_off[s + 1]; ++q) {
            IntegrationPoint ip;
            ip.Set3(c_.bq_xi[0][q], c_.bq_xi[1][q], c_.bq_xi[2][q]);
            t.Transform(ip, x);
            el.CalcShape(ip, shape);
            double d[3] = {std::cos(x[0]), -std::sin(x[1]), std::exp(x[2])};
            double div = d[0] + d[1] + d[2], flux = 0.;
            for (int c = 0; c < 3; ++c) {
                if (elastic_)
                    for (int i = 0; i < nd; ++i)
                        out[c * nd + i] += (div + 2 * d[c]) * c_.bq_n[c][q] * c_.bq_w[q] * shape[i];
                else
                    flux += d[c] * c_.bq_n[c][q];
            }
            if (!elastic_)
                out.Add(flux * c_.bq_w[q], shape);
        }
    }
};
std::array<double, 2> solve(Domain& d, bool elastic) {
    auto& mesh = *d.grid;
    fem::CutRules rules(d.cut, mesh);
    H1_FECollection collection(2, 3);
    FiniteElementSpace space(&mesh, &collection, elastic ? 3 : 1, Ordering::byVDIM);
    GridFunction u(&space);
    u = 0.;
    BilinearForm form(&space);
    ConstantCoefficient one(1);
    if (elastic)
        form.AddDomainIntegrator(new fem::CutBilinear(rules, new ElasticityIntegrator(one, one)));
    else
        form.AddDomainIntegrator(new fem::CutBilinear(rules, new DiffusionIntegrator(one)));
    std::vector<double> scale{elastic ? 3. : 1.};
    form.AddInteriorFaceIntegrator(
        new fem::GhostPenalty(d.arrays, d.cut, scale, .1, elastic ? 3 : 1));
    LinearForm rhs(&space);
    rhs.AddDomainIntegrator(new Force(rules, d.cut, elastic));
    Array<int> essential;
    if (!elastic) {
        essential.Append(0);
        Vector x(3);
        const auto* p = mesh.GetVertex(0);
        for (int c = 0; c < 3; ++c)
            x[c] = p[c];
        u[0] = std::sin(x[0]) + std::cos(x[1]) + std::exp(x[2]);
    } else {
        std::set<int> cut(d.cut.cut_elem.begin(), d.cut.cut_elem.end());
        int e = 0;
        while (e < mesh.GetNE() && cut.contains(e))
            ++e;
        require(e < mesh.GetNE(), "regular constraint cell");
        Array<int> vertices;
        mesh.GetElementVertices(e, vertices);
        for (auto [j, count] : {std::pair{0, 3}, std::pair{1, 2}, std::pair{3, 1}}) {
            Array<int> dofs;
            space.GetVertexVDofs(vertices[j], dofs);
            Vector x(3), value(3);
            for (int c = 0; c < 3; ++c)
                x[c] = mesh.GetVertex(vertices[j])[c];
            exact(x, value);
            for (int c = 3 - count; c < 3; ++c) {
                essential.Append(dofs[c]);
                u[dofs[c]] = value[c];
            }
        }
    }
    form.Assemble();
    rhs.Assemble();
    form.Finalize();
    OperatorPtr op;
    Vector X, B;
    form.FormLinearSystem(essential, u, rhs, op, X, B);
    auto& a = *op.As<SparseMatrix>();
    GSSmoother pre(a);
    CGSolver cg;
    cg.SetOperator(a);
    cg.SetPreconditioner(pre);
    cg.SetRelTol(1e-12);
    cg.SetAbsTol(1e-14);
    cg.SetMaxIter(15000);
    cg.SetPrintLevel(-1);
    cg.Mult(B, X);
    require(cg.GetConverged(), "manufactured CG failed");
    form.RecoverFEMSolution(X, rhs, u);
    double l2 = 0, h1 = 0, volume = 0, mean = 0;
    DenseMatrix fit(6);
    fit = 0.;
    Vector moments(6);
    moments = 0.;
    Vector skew(3);
    skew = 0.;
    for (int e = 0; e < mesh.GetNE(); ++e) {
        const auto* cr = rules.get(e);
        const auto& r = cr ? *cr : IntRules.Get(Geometry::CUBE, 10);
        auto* t = mesh.GetElementTransformation(e);
        for (int q = 0; q < r.GetNPoints(); ++q) {
            const auto& ip = r.IntPoint(q);
            t->SetIntPoint(&ip);
            Vector x(3), v(3);
            t->Transform(ip, x);
            exact(x, v);
            double w = ip.weight * t->Weight();
            volume += w;
            double grad[3] = {std::cos(x[0]), -std::sin(x[1]), std::exp(x[2])};
            if (elastic) {
                Vector value(3);
                DenseMatrix gradient(3);
                u.GetVectorValue(e, ip, value);
                u.GetVectorGradient(*t, gradient);
                const double rigid[3][6] = {{1, 0, 0, 0, x[2], -x[1]},
                                            {0, 1, 0, -x[2], 0, x[0]},
                                            {0, 0, 1, x[1], -x[0], 0}};
                for (int c = 0; c < 3; ++c) {
                    l2 += w * std::pow(value[c] - v[c], 2);
                    for (int j = 0; j < 3; ++j)
                        h1 += w * std::pow(gradient(c, j) - (c == j ? grad[c] : 0), 2);
                    for (int i = 0; i < 6; ++i) {
                        moments[i] += w * rigid[c][i] * (value[c] - v[c]);
                        for (int j = 0; j < 6; ++j)
                            fit(i, j) += w * rigid[c][i] * rigid[c][j];
                    }
                }
                skew[0] += w * (gradient(2, 1) - gradient(1, 2));
                skew[1] += w * (gradient(0, 2) - gradient(2, 0));
                skew[2] += w * (gradient(1, 0) - gradient(0, 1));
            } else {
                double value = u.GetValue(e, ip);
                Vector gradient(3);
                u.GetGradient(*t, gradient);
                mean += w * (value - v[0] - v[1] - v[2]);
                l2 += w * std::pow(value - v[0] - v[1] - v[2], 2);
                for (int c = 0; c < 3; ++c)
                    h1 += w * std::pow(gradient[c] - grad[c], 2);
            }
        }
    }
    // Natural-boundary problems determine fields modulo constants/rigid motion.
    // Report quotient-space errors using the L2 best rigid fit.
    if (elastic) {
        Vector rigid(6);
        DenseMatrixInverse inv(fit);
        inv.Mult(moments, rigid);
        l2 -= rigid * moments;
        for (int c = 0; c < 3; ++c)
            h1 += 2 * volume * rigid[c + 3] * rigid[c + 3] - 2 * rigid[c + 3] * skew[c];
    } else
        l2 -= mean * mean / volume;
    return {std::sqrt(std::max(0., l2)), std::sqrt(std::max(0., h1))};
}
} // namespace
int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    try {
        std::cout << std::setprecision(17);
        for (bool sphere : {false, true})
            for (int degree : {4, 6, 8}) {
                std::array<std::array<double, 2>, 2> sy{}, sxy{};
                double sx = 0, sxx = 0;
                std::array<std::array<double, 2>, 2> previous{};
                int previous_n = 0;
                double previous_volume_error = 1, previous_area_error = 1;
                for (int n : {5, 7, 9, 13}) {
                    double x = std::log(static_cast<double>(n));
                    sx += x;
                    sxx += x * x;
                    Domain d(n, sphere, degree);
                    fem::CutRules rules(d.cut, *d.grid);
                    double volume = std::accumulate(rules.volumes.begin(), rules.volumes.end(), 0.);
                    double area = std::accumulate(d.cut.bq_w.begin(), d.cut.bq_w.end(), 0.);
                    double exact_volume =
                        sphere ? 4 * std::acos(-1.) * std::pow(.35, 3) / 3 : std::pow(.56, 3);
                    double exact_area = sphere ? 4 * std::acos(-1.) * .35 * .35 : 6 * .56 * .56;
                    double ve = std::abs(volume / exact_volume - 1),
                           ae = std::abs(area / exact_area - 1);
                    if (sphere) {
                        require(ve < previous_volume_error && ae < previous_area_error,
                                "sphere geometry must converge");
                        previous_volume_error = ve;
                        previous_area_error = ae;
                    } else
                        require(ve < 1e-12 && ae < 1e-12, "exact rotated-plane geometry");
                    std::cout << "geometry " << (sphere ? "sphere" : "rotated-box") << " n=" << n
                              << " volume_error=" << ve << " area_error=" << ae << std::endl;
                    for (int physics = 0; physics < 2; ++physics) {
                        auto error = solve(d, physics == 1);
                        if (previous_n)
                            std::cout << "LEVEL_ORDER " << (sphere ? "sphere" : "rotated-box")
                                      << " degree=" << degree << " n=" << n
                                      << " physics=" << (physics ? "elastic" : "heat") << " L2="
                                      << std::log(previous[physics][0] / error[0]) /
                                             std::log(double(n) / previous_n)
                                      << " H1="
                                      << std::log(previous[physics][1] / error[1]) /
                                             std::log(double(n) / previous_n)
                                      << std::endl;
                        previous[physics] = error;
                        for (int norm = 0; norm < 2; ++norm) {
                            double y = std::log(error[norm]);
                            sy[physics][norm] += y;
                            sxy[physics][norm] += x * y;
                        }
                        std::cout << (sphere ? "sphere" : "rotated-box") << " degree=" << degree
                                  << " n=" << n << " physics=" << (physics ? "elastic" : "heat")
                                  << " L2=" << error[0] << " H1=" << error[1] << std::endl;
                    }
                    previous_n = n;
                }
                for (int physics = 0; physics < 2; ++physics) {
                    double l2 = -(4 * sxy[physics][0] - sx * sy[physics][0]) / (4 * sxx - sx * sx);
                    double h1 = -(4 * sxy[physics][1] - sx * sy[physics][1]) / (4 * sxx - sx * sx);
                    std::cout << "ORDER " << (sphere ? "sphere" : "rotated-box")
                              << " degree=" << degree
                              << " physics=" << (physics ? "elastic" : "heat") << " L2=" << l2
                              << " H1=" << h1 << std::endl;
                    require(l2 >= 2.8 && h1 >= 1.8, "Q2 manufactured convergence order");
                }
            }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    MPI_Finalize();
}
