#include "cutfem/assembly.hpp"
#ifdef CUTFEM_WITH_KOKKOS
#include "cutfem/kokkos_assembly.hpp"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
namespace chipcae::proto_cutfem {
double
delta_temperature(const Problem& p, const HexMeshArrays& m, int e, const double N[4], Point x) {
    if (p.delta_temperature)
        return p.delta_temperature(x);
    if (!p.temperature)
        return 0;
    double t = 0;
    auto nodes = m.nodes(e);
    for (int a = 0; a < 4; ++a)
        t += N[a] * p.temperature->values[nodes[a]];
    return t - p.materials.T_stress_free;
}
namespace {
std::vector<int> face_nodes(const HexMeshArrays& m, const GhostFace& f) {
    std::vector<int> n;
    for (int e : {f.cell1, f.cell2})
        for (int a : m.nodes(e))
            n.push_back(a);
    std::sort(n.begin(), n.end());
    n.erase(std::unique(n.begin(), n.end()), n.end());
    return n;
}
} // namespace
DofMap active_dofs(const HexMeshArrays& m, const CutLayer& c, int nc) {
    DofMap d;
    d.ncomp = nc;
    d.node_to_active.assign(m.n_vert, -1);
    for (int e = 0; e < m.n_elem; ++e)
        if (c.cell_state[e])
            for (int n : m.nodes(e))
                d.node_to_active[n] = 0;
    for (int n = 0; n < m.n_vert; ++n)
        if (d.node_to_active[n] >= 0) {
            d.node_to_active[n] = d.nnode++;
            d.active_to_node.push_back(n);
        }
    return d;
}
System assemble(const HexMeshArrays& m, const CutLayer& c, const Problem& p) {
    if (p.ncomp != 1 && p.ncomp != 2)
        throw std::runtime_error("Only scalar thermal and 2D elasticity are supported");
    System s;
    int nc = p.ncomp;
    s.dofs = active_dofs(m, c, nc);
    auto& d = s.dofs;
    std::vector<std::vector<int>> rows(nc * d.nnode);
    auto pattern = [&](const auto& nodes) {
        for (int a : nodes)
            for (int i = 0; i < nc; ++i)
                for (int b : nodes)
                    for (int j = 0; j < nc; ++j)
                        rows[d.dof(a, i)].push_back(d.dof(b, j));
    };
    for (int e = 0; e < m.n_elem; ++e)
        if (c.cell_state[e])
            pattern(m.nodes(e));
    if (p.gamma_g > 0)
        for (auto f : c.gp_faces)
            pattern(face_nodes(m, f));
    s.A = CSRMatrix::pattern(std::move(rows));
    s.rhs.assign(s.A.n, 0);
#ifdef CUTFEM_WITH_KOKKOS
    ElementVolumes volumes;
    if (p.kokkos_volume)
        volumes = kokkos_volumes(m, c, p, s);
#else
    if (p.kokkos_volume)
        throw std::runtime_error("Kokkos support is disabled");
#endif
    double host_volume_seconds = 0;
    for (int e = 0; e < m.n_elem; ++e)
        if (c.cell_state[e]) {
            CellMap map(m, e);
            auto nodes = m.nodes(e);
            int mat = c.cell_material[e], nd = 4 * nc;
            double K[8][8] = {}, rhs[8] = {};
            double eps[3] = {}, sig[3], D[3][3];
            if (linear_thermoelastic_plane_strain(p.materials, mat, eps, 0, sig, D))
                throw std::runtime_error("Invalid material");
            double k = p.materials.k[mat], beta = thermal_beta(p.materials, mat),
                   h = std::min(map.hx, map.hy);
            auto basis = [&](const double dx[4], const double dy[4], double B[8][3]) {
                for (int a = 0; a < 4; ++a) {
                    B[2 * a][0] = dx[a];
                    B[2 * a][1] = 0;
                    B[2 * a][2] = dy[a];
                    B[2 * a + 1][0] = 0;
                    B[2 * a + 1][1] = dy[a];
                    B[2 * a + 1][2] = dx[a];
                }
            };
            auto volume_start = p.volume_seconds ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
#ifdef CUTFEM_WITH_KOKKOS
            if (p.kokkos_volume) {
                if (!p.kokkos_atomic_scatter)
                    for (int a = 0; a < nd; ++a) {
                        rhs[a] = volumes.rhs[e * nd + a];
                        for (int b = 0; b < nd; ++b)
                            K[a][b] = volumes.matrix[(e * nd + a) * nd + b];
                    }
            } else
#endif
                for (int64_t q = c.vq_offsets[e]; q < c.vq_offsets[e + 1]; ++q) {
                    Point r{c.vq_xi[2 * q], c.vq_xi[2 * q + 1]}, x = map.physical(r);
                    double N[4], dx[4], dy[4];
                    map.shape(r, N, dx, dy);
                    double w = c.vq_w[q] * map.hx * map.hy;
                    auto f = p.source(x, mat);
                    if (nc == 1) {
                        for (int a = 0; a < 4; ++a) {
                            rhs[a] += w * f.x * N[a];
                            for (int b = 0; b < 4; ++b)
                                K[a][b] += w * k * (dx[a] * dx[b] + dy[a] * dy[b]);
                        }
                    } else {
                        double B[8][3], CB[8][3] = {};
                        basis(dx, dy, B);
                        for (int a = 0; a < 8; ++a)
                            for (int i = 0; i < 3; ++i)
                                for (int j = 0; j < 3; ++j)
                                    CB[a][i] += D[i][j] * B[a][j];
                        double dt = delta_temperature(p, m, e, N, x);
                        for (int a = 0; a < 8; ++a) {
                            rhs[a] += w * (N[a / 2] * (a % 2 ? f.y : f.x) +
                                           beta * dt * (B[a][0] + B[a][1]));
                            for (int b = 0; b < 8; ++b)
                                for (int i = 0; i < 3; ++i)
                                    K[a][b] += w * B[a][i] * CB[b][i];
                        }
                    }
                }
            if (!p.kokkos_volume && p.volume_seconds)
                host_volume_seconds +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - volume_start)
                        .count();
            for (int64_t q = c.bq_offsets[e]; q < c.bq_offsets[e + 1]; ++q) {
                Point r{c.bq_xi[2 * q], c.bq_xi[2 * q + 1]},
                    x = map.physical(r), n{c.bq_normal[2 * q], c.bq_normal[2 * q + 1]};
                auto bc = p.boundary(c.bq_tag[q], x, n);
                if (bc.type == BCType::None)
                    continue;
                double N[4], dx[4], dy[4];
                map.shape(r, N, dx, dy);
                double w = c.bq_w_phys[q];
                if (nc == 1) {
                    double dn[4];
                    for (int a = 0; a < 4; ++a)
                        dn[a] = dx[a] * n.x + dy[a] * n.y;
                    for (int a = 0; a < 4; ++a) {
                        if (bc.type == BCType::Dirichlet) {
                            rhs[a] +=
                                w * k * (-dn[a] * bc.value.x + p.gamma_N / h * N[a] * bc.value.x);
                            for (int b = 0; b < 4; ++b)
                                K[a][b] +=
                                    w * k *
                                    (-dn[b] * N[a] - dn[a] * N[b] + p.gamma_N / h * N[a] * N[b]);
                        } else if (bc.type == BCType::Robin) {
                            rhs[a] += w * N[a] * (bc.hc * bc.Tinf + bc.q);
                            for (int b = 0; b < 4; ++b)
                                K[a][b] += w * bc.hc * N[a] * N[b];
                        } else
                            rhs[a] += w * N[a] * bc.value.x;
                    }
                } else if (bc.type == BCType::Dirichlet) {
                    double B[8][3], tr[8][2] = {};
                    basis(dx, dy, B);
                    for (int a = 0; a < 8; ++a) {
                        double st[3] = {};
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j)
                                st[i] += D[i][j] * B[a][j];
                        tr[a][0] = st[0] * n.x + st[2] * n.y;
                        tr[a][1] = st[2] * n.x + st[1] * n.y;
                    }
                    double penalty = p.gamma_N * D[0][0] / h, dt = delta_temperature(p, m, e, N, x);
                    for (int a = 0; a < 8; ++a) {
                        int ac = a % 2;
                        double g = ac ? bc.value.y : bc.value.x;
                        rhs[a] +=
                            w * (-tr[a][0] * bc.value.x - tr[a][1] * bc.value.y +
                                 penalty * N[a / 2] * g - beta * dt * N[a / 2] * (ac ? n.y : n.x));
                        for (int b = 0; b < 8; ++b)
                            K[a][b] += w * (-tr[b][ac] * N[a / 2] - tr[a][b % 2] * N[b / 2] +
                                            (ac == b % 2 ? penalty * N[a / 2] * N[b / 2] : 0));
                    }
                } else if (bc.type == BCType::Traction)
                    for (int a = 0; a < 4; ++a) {
                        rhs[2 * a] += w * N[a] * bc.value.x;
                        rhs[2 * a + 1] += w * N[a] * bc.value.y;
                    }
            }
            for (int a = 0; a < nd; ++a) {
                int ia = d.dof(nodes[a / nc], a % nc);
                s.rhs[ia] += rhs[a];
                for (int b = 0; b < nd; ++b)
                    s.A.add(ia, d.dof(nodes[b / nc], b % nc), K[a][b]);
            }
        }
    if (p.gamma_g > 0)
        for (auto f : c.gp_faces) {
            auto ns = face_nodes(m, f);
            int nf = ns.size();
            CellMap a(m, f.cell1), b(m, f.cell2);
            double length = f.orientation == 0 ? a.hy : a.hx;
            double h = std::min({a.hx, a.hy, b.hx, b.hy});
            int ma = c.cell_material[f.cell1], mb = c.cell_material[f.cell2];
            double coef = nc == 1 ? std::max(p.materials.k[ma], p.materials.k[mb])
                                  : std::max(p.materials.E[ma], p.materials.E[mb]);
            double KF[6][6] = {};
            for (int q = 0; q < 2; ++q) {
                double t = .5 + (q == 0 ? -1 : 1) * .28867513459481288225;
                Point x = f.orientation == 0 ? Point{a.x + a.hx, a.y + t * a.hy}
                                             : Point{a.x + t * a.hx, a.y + a.hy};
                double jump[6] = {};
                for (int side = 0; side < 2; ++side) {
                    int e = side ? f.cell2 : f.cell1;
                    CellMap map(m, e);
                    double N[4], dx[4], dy[4];
                    map.shape(map.reference(x), N, dx, dy);
                    auto en = m.nodes(e);
                    for (int i = 0; i < 4; ++i) {
                        int pos = std::lower_bound(ns.begin(), ns.end(), en[i]) - ns.begin();
                        jump[pos] += (side ? -1 : 1) * (f.orientation == 0 ? dx[i] : dy[i]);
                    }
                }
                double w = p.gamma_g * h * coef * length / 2;
                for (int i = 0; i < nf; ++i)
                    for (int j = 0; j < nf; ++j)
                        KF[i][j] += w * jump[i] * jump[j];
            }
            for (int i = 0; i < nf; ++i)
                for (int j = 0; j < nf; ++j)
                    for (int comp = 0; comp < nc; ++comp)
                        s.A.add(d.dof(ns[i], comp), d.dof(ns[j], comp), KF[i][j]);
        }
    if (!p.kokkos_volume && p.volume_seconds)
        *p.volume_seconds = host_volume_seconds;
    return s;
}
chipcae::k5_stub::CvField nodal_field(const HexMeshArrays& m,
                                      const System& s,
                                      const std::vector<double>& u,
                                      const std::string& name) {
    chipcae::k5_stub::CvField field;
    field.name = name;
    field.ncomp = s.dofs.ncomp;
    field.values.assign(m.n_vert * field.ncomp, 0);
    for (int i = 0; i < s.dofs.nnode; ++i)
        for (int a = 0; a < field.ncomp; ++a)
            field.values[s.dofs.active_to_node[i] * field.ncomp + a] = u[i * field.ncomp + a];
    return field;
}
chipcae::k5_stub::CvField stress_field(const HexMeshArrays& m,
                                       const CutLayer& c,
                                       const Problem& p,
                                       const chipcae::k5_stub::CvField& u) {
    chipcae::k5_stub::CvField s;
    s.name = "stress_xx_yy_xy";
    s.location = chipcae::k5_stub::CvField::Location::QuadPoint;
    s.ncomp = 3;
    s.offsets = c.vq_offsets;
    s.values.resize(3 * c.vq_offsets.back());
    for (int e = 0; e < m.n_elem; ++e)
        if (c.cell_state[e]) {
            CellMap map(m, e);
            auto nodes = m.nodes(e);
            for (auto q = c.vq_offsets[e]; q < c.vq_offsets[e + 1]; ++q) {
                Point r{c.vq_xi[2 * q], c.vq_xi[2 * q + 1]};
                double N[4], dx[4], dy[4], eps[3] = {}, D[3][3];
                map.shape(r, N, dx, dy);
                for (int a = 0; a < 4; ++a) {
                    double ux = u.values[2 * nodes[a]], uy = u.values[2 * nodes[a] + 1];
                    eps[0] += dx[a] * ux;
                    eps[1] += dy[a] * uy;
                    eps[2] += dy[a] * ux + dx[a] * uy;
                }
                linear_thermoelastic_plane_strain(p.materials,
                                                  c.cell_material[e],
                                                  eps,
                                                  delta_temperature(p, m, e, N, map.physical(r)),
                                                  &s.values[3 * q],
                                                  D);
            }
        }
    return s;
}
chipcae::k5_stub::StepResult PrototypeSolver::solve_step(const StepRequest& r) {
    auto s = assemble(r.mesh, r.cut, r.problem);
    s.A.constrain(s.rhs, r.constraints);
    LinearResult u;
    if (r.method == "pcg")
        u = conjugate_gradient(s.A, s.rhs);
    else if (r.method == "hypre")
        u = hypre_pcg(s.A, s.rhs, r.problem.ncomp);
    else
        u = banded_cholesky(s.A, s.rhs);
    chipcae::k5_stub::StepResult out;
    out.solver_stats = u.stats;
    out.status = u.stats.status == "ok" ? 0 : 1;
    if (r.problem.ncomp == 1)
        out.temperature = nodal_field(r.mesh, s, u.x, "temperature");
    else {
        out.displacement = nodal_field(r.mesh, s, u.x, "displacement");
        if (r.problem.temperature)
            out.temperature = *r.problem.temperature;
        out.stress = stress_field(r.mesh, r.cut, r.problem, out.displacement);
    }
    return out;
}
} // namespace chipcae::proto_cutfem
