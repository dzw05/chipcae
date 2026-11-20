#include "cutfem/assembly.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
using namespace chipcae::proto_cutfem;
int main(int argc, char** argv) {
    Kokkos::initialize(argc, argv);
    try {
        if (argc != 4)
            throw std::runtime_error("usage: kokkos_bench nx ny host|serial|openmp");
        int nx = std::stoi(argv[1]), ny = std::stoi(argv[2]);
        std::string mode = argv[3];
        auto m = tensor_mesh(linspace(0, 25, nx), linspace(-.5, .5, ny));
        auto bottom = rectangle_body(.037, 24.94, -.47, 0, 0),
             top = rectangle_body(.037, 24.94, 0, .31, 1);
        bottom.halfplanes[2].tag = -1;
        top.halfplanes[0].tag = -1;
        auto c = build_cut_layer(m, {bottom, top});
        Problem p;
        p.ncomp = 2;
        p.materials = {{20, 130}, {.3, .28}, {1e-5, 2.6e-6}, {.9, 150}, 125};
        p.delta_temperature = [](Point) { return -100.; };
        p.kokkos_volume = mode != "host";
        p.kokkos_serial = mode == "serial";
        double vol = 0;
        p.volume_seconds = &vol;
        std::vector<double> volumes, totals;
        int dofs = 0;
        for (int i = 0; i < 5; ++i) {
            auto start = std::chrono::steady_clock::now();
            auto s = assemble(m, c, p);
            totals.push_back(
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            volumes.push_back(vol);
            dofs = s.A.n;
        }
        std::sort(volumes.begin(), volumes.end());
        std::sort(totals.begin(), totals.end());
        double load = 0;
        std::ifstream("/proc/loadavg") >> load;
        std::cout << nx << ',' << ny << ',' << dofs << ',' << mode << ','
                  << Kokkos::OpenMP().concurrency() << ',' << volumes[2] << ',' << totals[2] << ','
                  << load << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        Kokkos::finalize();
        return 1;
    }
    Kokkos::finalize();
}
