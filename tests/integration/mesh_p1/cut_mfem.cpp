#include "chipcae/mesh/background/mfem_adapter.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "runtime.hpp"
#include "support.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mfem.hpp>
#include <numeric>
#include <random>
using namespace chipcae::mesh;
using k1::expect;
long double analytic(std::array<double, 3> n, double d) {
    long double volume = 0;
    for (int mask = 0; mask < 8; ++mask) {
        long double t = d;
        int sign = 1;
        for (int k = 0; k < 3; ++k)
            if (mask & (1 << k)) {
                t -= n[k];
                sign = -sign;
            }
        if (t > 0)
            volume += sign * t * t * t;
    }
    return volume / (6 * static_cast<long double>(n[0]) * n[1] * n[2]);
}
int main(int argc, char** argv) {
    const std::string device_name = argc > 1 ? argv[1] : "cpu";
    if (device_name == "cuda") {
#ifndef MFEM_USE_CUDA
        return 77;
#else
        if (!std::getenv("CHIPCAE_RUN_GPU"))
            return 77;
#endif
    }
#ifdef CHIPCAE_ENABLE_MPI
    k1::init_runtime(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    {
        mfem::Device device(device_name.c_str());
        auto comm = chipcae::Communicator::world();
        auto s = k1::stack();
        s.activation_groups.clear();
        s.layers.resize(1);
        s.layers[0].thickness = 1;
        s.layers[0].n_elem_min = 1;
        s.layers[0].regions.clear();
        auto options = k1::options();
        options.nx = 2;
        options.ny = 2;
        options.ranks = comm.size();
        auto grid = build_tensor_grid(s, options).value();
        auto adapter = MfemAdapter::create(grid, comm);
        expect(bool(adapter), "cut adapter");
        cut::CutOptions co;
        co.snap_phi = k1::threshold("snap_phi");
        co.vf_min = k1::threshold("vf_min");
        std::mt19937_64 rng(17);
        std::uniform_real_distribution<double> random(.2, 1);
        double total = 0;
        for (int trial = 0; trial < 1000; ++trial) {
            std::array<double, 3> n{random(rng), random(rng), random(rng)};
            double d = (n[0] + n[1] + n[2]) * .5;
            auto c =
                cut::build_cut_layer(static_cast<const HexMeshArrays&>(grid), {{{0, n, d}}}, co);
            expect(bool(c), "cut layer");
            for (std::size_t sub = 0; sub < c.value().cut_elem.size(); ++sub) {
                auto e = c.value().cut_elem[sub];
                if (grid.partitioning[e] != comm.rank())
                    continue;
                auto gid = grid.elem_gid[e];
                const auto& gids = adapter.value().elem_gid();
                auto local = std::find(gids.begin(), gids.end(), gid);
                expect(local != gids.end(), "GID handoff");
                auto* t = adapter.value().local().GetElementTransformation(
                    static_cast<int>(local - gids.begin()));
                double volume = 0;
                for (Local q = c.value().vq_off[sub]; q < c.value().vq_off[sub + 1]; ++q) {
                    mfem::IntegrationPoint ip;
                    ip.Set3(c.value().vq_xi[0][q], c.value().vq_xi[1][q], c.value().vq_xi[2][q]);
                    t->SetIntPoint(&ip);
                    volume += c.value().vq_w[q] * t->Weight();
                }
                std::array<double, 3> scaled{};
                double shifted = d, jac = 1, surface = 0;
                std::array<double, 3> size{};
                for (int k = 0; k < 3; ++k) {
                    auto lo = grid.vertices[3 * grid.hex[8 * e] + k];
                    size[k] = grid.vertices[3 * grid.hex[8 * e + 6] + k] - lo;
                    scaled[k] = n[k] * size[k];
                    shifted -= n[k] * lo;
                    jac *= size[k];
                }
                surface = 2 * (size[0] * size[1] + size[0] * size[2] + size[1] * size[2]);
                double expected = static_cast<double>(analytic(scaled, shifted)) * jac;
                expect(std::abs(volume - expected) <=
                           k1::threshold("cut_volume_relative") * expected,
                       "MFEM reference volume convention");
                auto closure = c.value().exterior_area_vector[sub];
                for (Local q = c.value().bq_off[sub]; q < c.value().bq_off[sub + 1]; ++q)
                    for (int k = 0; k < 3; ++k)
                        closure[k] += c.value().bq_n[k][q] * c.value().bq_w[q];
                expect(std::sqrt(std::inner_product(
                           closure.begin(), closure.end(), closure.begin(), 0.0)) <=
                           k1::threshold("closure_relative") * surface,
                       "MFEM EB closure norm");
                ++total;
            }
        }
        std::vector<double> sum{total};
        comm.allreduce_sum(sum);
        expect(sum[0] >= 1000, "at least 1000 cut cells");
        if (comm.rank() == 0)
            std::cout << "MFEM cut G1 cells=" << sum[0] << '\n';
    }
#ifdef CHIPCAE_ENABLE_MPI
    k1::finalize_runtime();
#endif
}
