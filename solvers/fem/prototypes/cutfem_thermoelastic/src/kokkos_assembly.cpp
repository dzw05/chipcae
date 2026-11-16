#include "cutfem/kokkos_assembly.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace chipcae::proto_cutfem {
namespace {
using Space = Kokkos::DefaultExecutionSpace::memory_space;
template <class T>
using View = Kokkos::View<T*, Space>;
template <class T>
View<T> copy_view(const char* name, const std::vector<T>& data) {
    View<T> v(std::string(name), data.size());
    auto h = Kokkos::create_mirror_view(v);
    for (size_t i = 0; i < data.size(); ++i)
        h(i) = data[i];
    Kokkos::deep_copy(v, h);
    return v;
}
struct Materials {
    View<double> E, nu, alpha, k;
};

template <class Exec>
ElementVolumes
calculate(const HexMeshArrays& mesh, const CutLayer& cut, const Problem& p, System& s) {
    const int nc = p.ncomp, nd = 4 * nc, ne = mesh.n_elem;
    std::vector<int> active, csr_indices(ne * nd * nd, 0), dofs(ne * nd, 0);
    std::vector<double> geom(ne * 4), forces(cut.vq_w.size() * 2), temp(cut.vq_w.size());
    for (int e = 0; e < ne; ++e)
        if (cut.cell_state[e]) {
            double eps[3] = {}, sig[3], D[3][3];
            if (linear_thermoelastic_plane_strain(
                    p.materials, cut.cell_material[e], eps, 0, sig, D))
                throw std::runtime_error("Invalid material");
            active.push_back(e);
            CellMap map(mesh, e);
            geom[4 * e] = map.x;
            geom[4 * e + 1] = map.y;
            geom[4 * e + 2] = map.hx;
            geom[4 * e + 3] = map.hy;
            auto nodes = mesh.nodes(e);
            for (int a = 0; a < nd; ++a) {
                int row = s.dofs.dof(nodes[a / nc], a % nc);
                dofs[e * nd + a] = row;
                if (p.kokkos_atomic_scatter)
                    for (int b = 0; b < nd; ++b) {
                        int col = s.dofs.dof(nodes[b / nc], b % nc);
                        auto it = std::lower_bound(s.A.col.begin() + s.A.row[row],
                                                   s.A.col.begin() + s.A.row[row + 1],
                                                   col);
                        if (it == s.A.col.begin() + s.A.row[row + 1] || *it != col)
                            throw std::runtime_error("CSR pattern missing entry");
                        csr_indices[(e * nd + a) * nd + b] = int(it - s.A.col.begin());
                    }
            }
            for (auto q = cut.vq_offsets[e]; q < cut.vq_offsets[e + 1]; ++q) {
                Point r{cut.vq_xi[2 * q], cut.vq_xi[2 * q + 1]}, x = map.physical(r);
                double N[4], dx[4], dy[4];
                map.shape(r, N, dx, dy);
                Point f = p.source(x, cut.cell_material[e]);
                forces[2 * q] = f.x;
                forces[2 * q + 1] = f.y;
                if (nc == 2)
                    temp[q] = delta_temperature(p, mesh, e, N, x);
            }
        }
    auto cells = copy_view("active", active), mat = copy_view("material", cut.cell_material);
    auto offsets = copy_view("offsets", cut.vq_offsets);
    auto xi = copy_view("xi", cut.vq_xi), weights = copy_view("weights", cut.vq_w);
    auto geometry = copy_view("geometry", geom), force = copy_view("force", forces),
         dt = copy_view("temperature", temp);
    Materials m{copy_view("E", p.materials.E),
                copy_view("nu", p.materials.nu),
                copy_view("alpha", p.materials.alpha),
                copy_view("k", p.materials.k)};
    View<double> km("element matrix", ne * nd * nd), fr("element rhs", ne * nd);
    auto start = std::chrono::steady_clock::now();
    Kokkos::parallel_for(
        "cutfem volume", Kokkos::RangePolicy<Exec>(0, active.size()), KOKKOS_LAMBDA(int i) {
            int e = cells(i), material = mat(e);
            double hx = geometry(4 * e + 2), hy = geometry(4 * e + 3);
            double K[8][8] = {}, R[8] = {};
            for (auto q = offsets(e); q < offsets(e + 1); ++q) {
                double N[4], dx[4], dy[4];
                q1(xi(2 * q), xi(2 * q + 1), N, dx, dy);
                for (int a = 0; a < 4; ++a) {
                    dx[a] /= hx;
                    dy[a] /= hy;
                }
                double w = weights(q) * hx * hy;
                double eps[3] = {}, sig[3], D[3][3];
                linear_thermoelastic_plane_strain(m, material, eps, dt(q), sig, D);
                if (nc == 1) {
                    for (int a = 0; a < 4; ++a) {
                        R[a] += w * force(2 * q) * N[a];
                        for (int b = 0; b < 4; ++b)
                            K[a][b] += w * m.k(material) * (dx[a] * dx[b] + dy[a] * dy[b]);
                    }
                } else {
                    double B[8][3], CB[8][3] = {};
                    for (int a = 0; a < 4; ++a) {
                        B[2 * a][0] = dx[a];
                        B[2 * a][1] = 0;
                        B[2 * a][2] = dy[a];
                        B[2 * a + 1][0] = 0;
                        B[2 * a + 1][1] = dy[a];
                        B[2 * a + 1][2] = dx[a];
                    }
                    for (int a = 0; a < 8; ++a)
                        for (int j = 0; j < 3; ++j)
                            for (int k = 0; k < 3; ++k)
                                CB[a][j] += D[j][k] * B[a][k];
                    double beta = thermal_beta(m, material);
                    for (int a = 0; a < 8; ++a) {
                        R[a] += w * (N[a / 2] * force(2 * q + a % 2) +
                                     beta * dt(q) * (B[a][0] + B[a][1]));
                        for (int b = 0; b < 8; ++b)
                            for (int j = 0; j < 3; ++j)
                                K[a][b] += w * B[a][j] * CB[b][j];
                    }
                }
            }
            for (int a = 0; a < nd; ++a) {
                fr(e * nd + a) = R[a];
                for (int b = 0; b < nd; ++b)
                    km((e * nd + a) * nd + b) = K[a][b];
            }
        });
    Exec().fence();
    if (p.volume_seconds)
        *p.volume_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ElementVolumes out;
    if (p.kokkos_atomic_scatter) {
        auto index = copy_view("csr index", csr_indices), ids = copy_view("dofs", dofs);
        auto vals = copy_view("csr values", s.A.val), rhs = copy_view("rhs", s.rhs);
        Kokkos::parallel_for(
            "cutfem atomic scatter",
            Kokkos::RangePolicy<Exec>(0, active.size()),
            KOKKOS_LAMBDA(int i) {
                int e = cells(i);
                for (int a = 0; a < nd; ++a) {
                    Kokkos::atomic_add(&rhs(ids(e * nd + a)), fr(e * nd + a));
                    for (int b = 0; b < nd; ++b)
                        Kokkos::atomic_add(&vals(index((e * nd + a) * nd + b)),
                                           km((e * nd + a) * nd + b));
                }
            });
        Exec().fence();
        auto hv = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), vals),
             hr = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), rhs);
        for (size_t i = 0; i < s.A.val.size(); ++i)
            s.A.val[i] = hv(i);
        for (size_t i = 0; i < s.rhs.size(); ++i)
            s.rhs[i] = hr(i);
    } else {
        auto hk = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), km),
             hr = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), fr);
        out.matrix.resize(ne * nd * nd);
        out.rhs.resize(ne * nd);
        for (size_t i = 0; i < out.matrix.size(); ++i)
            out.matrix[i] = hk(i);
        for (size_t i = 0; i < out.rhs.size(); ++i)
            out.rhs[i] = hr(i);
    }
    return out;
}
} // namespace
ElementVolumes
kokkos_volumes(const HexMeshArrays& m, const CutLayer& c, const Problem& p, System& s) {
    if (!Kokkos::is_initialized())
        throw std::runtime_error("Initialize Kokkos before assembly");
#ifdef KOKKOS_ENABLE_SERIAL
    if (p.kokkos_serial)
        return calculate<Kokkos::Serial>(m, c, p, s);
#endif
    return calculate<Kokkos::DefaultExecutionSpace>(m, c, p, s);
}
} // namespace chipcae::proto_cutfem
