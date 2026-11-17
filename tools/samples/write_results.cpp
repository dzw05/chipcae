#include "chipcae/io/vtkhdf_results.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#endif
    int rank = 0;
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif
    int status = 0;
    try {
        if (argc < 3)
            throw std::runtime_error("usage: write_results RUN STACKUP [EXPORT_PREFIX]");
        chipcae::io::ResultsWriter writer(argv[1], argv[2]);
        chipcae::io::ResultsStep step;
        auto& p = step.hex;
        double pv = 0.001, umax = 0.002;
        if (argc == 4) {
            std::ifstream in(std::string(argv[3]) + "." + std::to_string(rank));
            in.exceptions(std::ios::failbit | std::ios::badbit);
            std::size_t nv, nc;
            in >> nv >> nc;
            p.points.resize(3 * nv);
            p.U.resize(3 * nv);
            p.T.assign(nv, 193.15);
            for (std::size_t v = 0; v < nv; ++v) {
                for (int c = 0; c < 3; ++c)
                    in >> p.points[3 * v + c];
                for (int c = 0; c < 3; ++c)
                    in >> p.U[3 * v + c];
            }
            p.hex.resize(8 * nc);
            p.layer_id.resize(nc);
            p.material_id.resize(nc);
            p.cell_kind.resize(nc);
            p.elem_gid.resize(nc);
            for (std::size_t e = 0; e < nc; ++e) {
                for (int c = 0; c < 8; ++c)
                    in >> p.hex[8 * e + c];
                in >> p.layer_id[e] >> p.material_id[e] >> p.cell_kind[e] >> p.elem_gid[e];
            }
            in >> pv >> umax;
        } else if (rank == 0) {
            p.points = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
            p.hex = {0, 1, 2, 3, 4, 5, 6, 7};
            p.T.assign(8, 193.15);
            p.U.assign(24, 0);
            p.layer_id = {1};
            p.material_id = {2};
            p.cell_kind = {0};
            p.elem_gid = {0};
        }
        writer.write_step(0, 0, step);
        writer.add_summary("warpage_pv", pv, "m", 0);
        writer.add_summary("T_max", 193.15, "K", 0);
        writer.add_summary("U_abs_max", umax, "m", 0);
        writer.finalize();
        if (argc == 3) {
            // Validation errors must be collective even when only rank zero is bad.
            auto bad = step;
            if (rank == 0)
                bad.hex.T.clear();
            bool rejected = false;
            try {
                writer.write_step(99, 1, bad);
            } catch (const std::exception&) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("invalid input accepted");
            writer.write_step(1, 1, step);
            rejected = false;
            try {
                writer.finalize();
            } catch (const std::exception&) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("missing summary accepted");
            if (rank == 0) {
                std::ifstream manifest(std::filesystem::path(argv[1]) / "manifest.json");
                std::string text((std::istreambuf_iterator<char>(manifest)), {});
                if (text.find("step-1.vtkhdf") != std::string::npos)
                    throw std::runtime_error("failed finalize replaced manifest");
            }
            writer.add_summary("warpage_pv", pv, "m", 1);
            writer.add_summary("T_max", 193.15, "K", 1);
            writer.add_summary("U_abs_max", umax, "m", 1);
            writer.finalize();
        }

    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Finalize();
#endif
    return status;
}
