#include "chipcae/solve/case_loader.hpp"
#include "chipcae/solvers/fem/cut_fem.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <set>
using namespace chipcae;
using namespace mfem;
namespace {
class ThermalForce : public LinearFormIntegrator {
    Coefficient& stress_;

public:
    explicit ThermalForce(Coefficient& stress) : stress_(stress) {}
    using LinearFormIntegrator::AssembleRHSElementVect;
    void AssembleRHSElementVect(const FiniteElement& el,
                                ElementTransformation& t,
                                Vector& out) override {
        int n = el.GetDof();
        out.SetSize(3 * n);
        out = 0.;
        DenseMatrix gradient(n, 3);
        const auto& r = IntRule ? *IntRule : IntRules.Get(el.GetGeomType(), 8);
        for (int q = 0; q < r.GetNPoints(); ++q) {
            const auto& ip = r.IntPoint(q);
            t.SetIntPoint(&ip);
            el.CalcPhysDShape(t, gradient);
            for (int c = 0; c < 3; ++c)
                for (int i = 0; i < n; ++i)
                    out[c * n + i] += ip.weight * t.Weight() * stress_.Eval(t, ip) * gradient(i, c);
        }
    }
};
struct Sample {
    int parent;
    IntegrationPoint ip;
    std::array<double, 3> x;
};
struct Wedge {
    int element;
    std::array<std::array<double, 2>, 3> xy;
    double lo, hi;
};
std::pair<double, double> compare(solve::RunInputs inputs, int n) {
    inputs.grid.nx = inputs.grid.ny = n;
    inputs.grid.levels = 1;
    std::optional<mesh::cut::CutLayerArrays> clipped;
    auto arrays = solve::build_mesh(inputs, &clipped);
    auto& cut = *clipped;
    Mesh background(arrays.vertices.data(),
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
    background.Finalize(false, false);
    Mesh fitted(3, 0, 0, 0, 3);
    std::map<std::array<long long, 3>, int> vertices;
    auto vertex = [&](std::array<double, 3> p) {
        std::array<long long, 3> key{};
        for (int c = 0; c < 3; ++c)
            key[c] = std::llround(p[c] * 1e14);
        auto [it, added] = vertices.emplace(key, vertices.size());
        if (added)
            fitted.AddVertex(p.data());
        return it->second;
    };
    const auto& poly = *cut.polyhedra;
    std::map<int, int> cuts;
    for (std::size_t s = 0; s < cut.cut_elem.size(); ++s)
        cuts[cut.cut_elem[s]] = s;
    std::vector<std::vector<Wedge>> mapping(background.GetNE());
    for (int e = 0; e < background.GetNE(); ++e) {
        double lo = arrays.vertices[3 * arrays.hex[8 * e] + 2],
               hi = arrays.vertices[3 * arrays.hex[8 * e + 6] + 2];
        std::vector<std::array<double, 2>> ring;
        if (cuts.contains(e)) {
            auto it = std::find(
                poly.parent_elem_gid.begin(), poly.parent_elem_gid.end(), arrays.elem_gid[e]);
            int cell = it - poly.parent_elem_gid.begin();
            for (Index j = poly.poly_off[cell]; j < poly.poly_off[cell + 1]; ++j) {
                auto f = poly.poly_faces[j];
                bool bottom = true;
                for (Index k = poly.face_off[f]; k < poly.face_off[f + 1]; ++k)
                    bottom &= std::abs(poly.points[3 * poly.face_conn[k] + 2] - lo) < 1e-14;
                if (bottom) {
                    for (Index k = poly.face_off[f]; k < poly.face_off[f + 1]; ++k) {
                        auto v = poly.face_conn[k];
                        ring.push_back({poly.points[3 * v], poly.points[3 * v + 1]});
                    }
                    break;
                }
            }
        } else
            for (int j = 0; j < 4; ++j) {
                auto v = arrays.hex[8 * e + j];
                ring.push_back({arrays.vertices[3 * v], arrays.vertices[3 * v + 1]});
            }
        if (ring.size() < 3)
            throw std::runtime_error("missing prism footprint");
        std::array<double, 2> center{};
        for (auto p : ring)
            for (int c = 0; c < 2; ++c)
                center[c] += p[c] / ring.size();
        std::sort(ring.begin(), ring.end(), [&](auto a, auto b) {
            return std::atan2(a[1] - center[1], a[0] - center[0]) <
                   std::atan2(b[1] - center[1], b[0] - center[0]);
        });
        for (std::size_t j = 0; j < ring.size(); ++j) {
            std::array<std::array<double, 2>, 3> tri = {
                center, ring[j], ring[(j + 1) % ring.size()]};
            int ids[6];
            for (int z = 0; z < 2; ++z)
                for (int v = 0; v < 3; ++v)
                    ids[3 * z + v] = vertex({tri[v][0], tri[v][1], z ? hi : lo});
            int id = fitted.GetNE();
            fitted.AddWedge(ids, arrays.attribute[e]);
            mapping[e].push_back({id, tri, lo, hi});
        }
    }
    fitted.FinalizeMesh();
    std::vector<Sample> samples;
    H1_FECollection collection(2, 3);
    for (int e = 0; e < background.GetNE(); ++e) {
        auto* t = background.GetElementTransformation(e);
        if (arrays.vertices[3 * arrays.hex[8 * e + 6] + 2] != arrays.axes[2].back())
            continue;
        const auto& r = collection.FiniteElementForGeometry(Geometry::CUBE)->GetNodes();
        for (int q = 0; q < r.GetNPoints(); ++q) {
            auto ip = r.IntPoint(q);
            if (ip.z != 1)
                continue;
            Vector x(3);
            t->Transform(ip, x);
            bool inside = true;
            const auto& ring = inputs.stack.outline.polygon;
            for (std::size_t j = 0; j < ring.size(); ++j) {
                auto a = ring[j], b = ring[(j + 1) % ring.size()];
                inside &= (b[0] - a[0]) * (x[1] - a[1]) - (b[1] - a[1]) * (x[0] - a[0]) >= -1e-14;
            }
            if (inside)
                samples.push_back({e, ip, {x[0], x[1], x[2]}});
        }
        if (cuts.contains(e)) {
            int s = cuts[e];
            for (int q = cut.sq_off[s]; q < cut.sq_off[s + 1]; ++q)
                if (cut.sq_side[q] == 5) {
                    IntegrationPoint ip;
                    ip.Set3(cut.sq_xi[0][q], cut.sq_xi[1][q], cut.sq_xi[2][q]);
                    Vector x(3);
                    t->Transform(ip, x);
                    samples.push_back({e, ip, {x[0], x[1], x[2]}});
                }
        }
    }
    int anchor_cell = 0;
    while (cuts.contains(anchor_cell))
        ++anchor_cell;
    std::array<std::array<double, 3>, 3> anchors{};
    for (int i = 0; i < 3; ++i) {
        int j = i == 0 ? 0 : i == 1 ? 1 : 3;
        auto v = arrays.hex[8 * anchor_cell + j];
        for (int c = 0; c < 3; ++c)
            anchors[i][c] = arrays.vertices[3 * v + c];
    }
    double pv[2];
    for (int model = 0; model < 2; ++model) {
        auto& grid = model ? fitted : background;
        FiniteElementSpace space(&grid, &collection, 3, Ordering::byVDIM);
        GridFunction u(&space);
        u = 0.;
        Vector lambda(arrays.attr_table.entries.size()), mu(lambda.Size()), stress(lambda.Size());
        std::vector<double> scales(lambda.Size());
        for (int a = 0; a < lambda.Size(); ++a) {
            const auto& record = arrays.attr_table.entries[a];
            std::string id;
            for (auto [name, number] : arrays.attr_table.materials)
                if (number == record.material)
                    id = name;
            auto m = inputs.materials.at(id);
            lambda[a] = m.E * m.nu / ((1 + m.nu) * (1 - 2 * m.nu));
            mu[a] = m.E / (2 * (1 + m.nu));
            stress[a] = (3 * lambda[a] + 2 * mu[a]) * m.alpha * (-100);
            scales[a] = 2 * mu[a] + lambda[a];
        }
        PWConstCoefficient l(lambda), m(mu), s(stress);
        fem::CutRules rules(cut, background);
        BilinearForm form(&space);
        LinearForm rhs(&space);
        if (model) {
            form.AddDomainIntegrator(new ElasticityIntegrator(l, m));
            rhs.AddDomainIntegrator(new ThermalForce(s));
        } else {
            form.AddDomainIntegrator(new fem::CutBilinear(rules, new ElasticityIntegrator(l, m)));
            rhs.AddDomainIntegrator(new fem::CutLinear(rules, new ThermalForce(s)));
            form.AddInteriorFaceIntegrator(
                new fem::GhostPenalty(arrays, cut, scales, inputs.ghost_penalty, 3));
        }
        Array<int> essential;
        for (int v = 0; v < grid.GetNV(); ++v)
            for (int i = 0; i < 3; ++i) {
                bool same = true;
                for (int c = 0; c < 3; ++c)
                    same &= std::abs(grid.GetVertex(v)[c] - anchors[i][c]) < 1e-13;
                if (same) {
                    Array<int> dofs;
                    space.GetVertexVDofs(v, dofs);
                    for (int c = i; c < 3; ++c)
                        essential.Append(dofs[c]);
                }
            }
        essential.Sort();
        essential.Unique();
        if (essential.Size() != 6)
            throw std::runtime_error("reference requires six constraints");
        form.Assemble();
        rhs.Assemble();
        form.Finalize(0);
        OperatorPtr op;
        Vector X, B;
        form.FormLinearSystem(essential, u, rhs, op, X, B);
        auto& a = *op.As<SparseMatrix>();
        GSSmoother pre(a);
        CGSolver cg;
        cg.SetOperator(a);
        cg.SetPreconditioner(pre);
        cg.SetRelTol(1e-11);
        cg.SetMaxIter(30000);
        cg.SetPrintLevel(-1);
        cg.Mult(B, X);
        if (!cg.GetConverged())
            throw std::runtime_error("bilayer CG failed");
        form.RecoverFEMSolution(X, rhs, u);
        std::vector<double> values;
        DenseMatrix normal(3);
        normal = 0.;
        Vector fit(3);
        fit = 0.;
        for (const auto& sample : samples) {
            Vector value(3);
            if (!model)
                u.GetVectorValue(sample.parent, sample.ip, value);
            else {
                bool found = false;
                for (const auto& wedge : mapping[sample.parent]) {
                    auto a = wedge.xy[0], b = wedge.xy[1], c = wedge.xy[2];
                    double x = sample.x[0] - a[0], y = sample.x[1] - a[1],
                           det = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                    double r = (x * (c[1] - a[1]) - y * (c[0] - a[0])) / det,
                           t = ((b[0] - a[0]) * y - (b[1] - a[1]) * x) / det;
                    if (r >= -1e-10 && t >= -1e-10 && r + t <= 1 + 1e-10) {
                        IntegrationPoint ip;
                        ip.Set3(r, t, (sample.x[2] - wedge.lo) / (wedge.hi - wedge.lo));
                        u.GetVectorValue(wedge.element, ip, value);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    throw std::runtime_error("reference sample location failed");
            }
            values.push_back(value[2]);
            double phi[3] = {1, sample.x[0], sample.x[1]};
            for (int i = 0; i < 3; ++i) {
                fit[i] += phi[i] * value[2];
                for (int j = 0; j < 3; ++j)
                    normal(i, j) += phi[i] * phi[j];
            }
        }
        Vector plane(3);
        DenseMatrixInverse inverse(normal);
        inverse.Mult(fit, plane);
        double lo = 1e30, hi = -lo;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            double w =
                values[i] - plane[0] - plane[1] * samples[i].x[0] - plane[2] * samples[i].x[1];
            lo = std::min(lo, w);
            hi = std::max(hi, w);
        }
        pv[model] = hi - lo;
    }
    std::cout << "bilayer n=" << n << " PV_cut=" << pv[0] << " PV_fitted=" << pv[1]
              << " relative=" << std::abs(pv[0] / pv[1] - 1) << std::endl;
    return {pv[0], pv[1]};
}
} // namespace
int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    try {
        auto inputs = solve::load_inputs(std::filesystem::path(argv[1]) / "case.json",
                                         std::filesystem::path(argv[1]) / "profile.json",
                                         1);
        double coarse = 0, fine = 0;
        for (int n : {6, 8, 12, 16}) {
            auto [cut, fitted] = compare(inputs, n);
            double error = std::abs(cut / fitted - 1);
            if (n == 6)
                coarse = error;
            fine = error;
            if (error > .01)
                throw std::runtime_error("bilayer PV reference error exceeds 1%");
        }
        if (fine >= coarse)
            throw std::runtime_error("bilayer finest reference error must improve on coarsest");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    MPI_Finalize();
}
