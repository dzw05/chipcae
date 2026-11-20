#include "chipcae/io/vtkhdf_results.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#endif
    int rank = 0, ranks = 1;
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
#endif
    int status = 0;
    try {
        if (argc < 3)
            throw std::runtime_error("usage: write_results RUN STACKUP [EXPORT_PREFIX [s1]]");
        const bool cut_fixture = argc == 4 && std::string(argv[3]) == "cut";
        const std::string mode = argc == 4 ? argv[3] : "";
        const bool diagnostics_fixture = mode.starts_with("diagnostics");
        chipcae::io::ResultsWriter writer(
            argv[1],
            argv[2],
            argc == 3 || cut_fixture || diagnostics_fixture ||
                    (argc == 5 && std::string(argv[4]) == "s1")
                ? std::vector<std::string>{"substrate", "die_attach", "Si"}
                : std::vector<std::string>{},
            argc == 3 || cut_fixture || diagnostics_fixture ||
                    (argc == 5 && std::string(argv[4]) == "s1")
                ? std::vector<std::string>{"substrate", "die_attach", "Si"}
                : std::vector<std::string>{});
        chipcae::io::ResultsStep step;
        auto& p = step.hex;
        double pv = 0.001, umax = 0.002;
        if (argc >= 4 && !cut_fixture && !diagnostics_fixture) {
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
        if (cut_fixture && rank == 0) {
            step.cut = p;
            auto& c = step.cut;
            c.connectivity = c.hex;
            c.hex.clear();
            c.offsets = {0, 8};
            c.face_conn = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 1, 2, 6, 5, 2, 3, 7, 6, 3, 0, 4, 7};
            c.face_off = {0, 4, 8, 12, 16, 20, 24};
            c.poly_faces = {0, 1, 2, 3, 4, 5};
            c.poly_off = {0, 6};
            c.cell_kind = {1};
            c.volume = {1.0};
        }
        if (diagnostics_fixture) {
            // Distinct, deliberately unsorted GIDs catch filtering/component permutations.
            // Empty-rank coverage uses the same data on rank zero only.
            if (rank == 0 || mode.find("empty") == std::string::npos) {
                p.points = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
                p.T.assign(8, 193.15);
                p.U.assign(24, 0);
                p.hex.clear();
                for (int e = 0; e < 3; ++e)
                    p.hex.insert(p.hex.end(), {0, 1, 2, 3, 4, 5, 6, 7});
                p.elem_gid = {100 * rank + 7, 100 * rank + 2, 100 * rank + 9};
                p.layer_id = {1, 1, 1};
                p.material_id = {2, 2, 2};
                p.cell_kind = {0, 1, 0};
                for (auto gid : p.elem_gid) {
                    p.copper_fraction.push_back(static_cast<double>(gid % 10) / 10);
                    p.conductivity_diag.insert(p.conductivity_diag.end(),
                                               {1. + gid, 100. + gid, 1000. + gid});
                }
                auto& c = step.cut;
                c.points = p.points;
                c.T = p.T;
                c.U = p.U;
                c.connectivity = {0, 1, 2, 3, 4, 5, 6, 7};
                c.offsets = {0, 8};
                c.face_conn = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4,
                               1, 2, 6, 5, 2, 3, 7, 6, 3, 0, 4, 7};
                c.face_off = {0, 4, 8, 12, 16, 20, 24};
                c.poly_faces = {0, 1, 2, 3, 4, 5};
                c.poly_off = {0, 6};
                c.elem_gid = {100 * rank + 2};
                c.layer_id = {1};
                c.material_id = {2};
                c.cell_kind = {1};
                c.volume = {1};
                c.copper_fraction = {.2};
                c.conductivity_diag = {3. + 100 * rank, 102. + 100 * rank, 1002. + 100 * rank};
            }
            step.cut_as_hex = mode == "diagnostics-fallback";
            if (mode == "diagnostics-validation") {
                std::string accepted;
                for (int fault = 0; fault < (ranks > 1 ? 13 : 7); ++fault) {
                    auto bad = step;
                    if (fault >= 11) {
                        // Filtering every hex parent must not hide a partial input schema.
                        bad.hex.hex.resize(8);
                        bad.hex.elem_gid = bad.cut.elem_gid;
                        bad.hex.layer_id = bad.cut.layer_id;
                        bad.hex.material_id = bad.cut.material_id;
                        bad.hex.cell_kind = {1};
                        bad.hex.copper_fraction = bad.cut.copper_fraction;
                        bad.hex.conductivity_diag = bad.cut.conductivity_diag;
                    }
                    if (rank == 0) {
                        if (fault == 0)
                            bad.hex.copper_fraction.pop_back();
                        if (fault == 1)
                            bad.hex.conductivity_diag.pop_back();
                        if (fault == 2)
                            bad.cut.copper_fraction.push_back(.4);
                        if (fault == 3)
                            bad.cut.conductivity_diag.pop_back();
                        if (fault == 4)
                            bad.hex.copper_fraction[0] = std::numeric_limits<double>::quiet_NaN();
                        if (fault == 5)
                            bad.hex.conductivity_diag[0] = std::numeric_limits<double>::infinity();
                        if (fault == 6)
                            bad.cut.conductivity_diag[0] = std::numeric_limits<double>::quiet_NaN();
                    }
                    if (rank == 1) {
                        if (fault == 7)
                            bad.hex.copper_fraction.clear();
                        if (fault == 8)
                            bad.hex.conductivity_diag.clear();
                        if (fault == 9)
                            bad.cut.conductivity_diag.clear();
                        if (fault == 11)
                            bad.hex.copper_fraction.clear();
                        if (fault == 12)
                            bad.hex.conductivity_diag.clear();
                        if (fault == 10) {
                            bad.hex = {};
                            bad.cut = {};
                            bad.hex.conductivity_diag = {1, 2, 3};
                        }
                    }
                    bool rejected = false;
                    try {
                        writer.write_step(99 + fault, 1, bad);
                    } catch (const std::exception&) {
                        rejected = true;
                    }
                    if (!rejected)
                        accepted += " " + std::to_string(fault);
                }
                if (!accepted.empty())
                    throw std::runtime_error(
                        "writer accepted malformed copper diagnostics; faults:" + accepted);
            }
        }
        if (cut_fixture) {
            for (int fault = 0; fault < 4; ++fault) {
                auto bad = step;
                if (rank == 0) {
                    if (fault == 0)
                        bad.cut.poly_faces[0] = -1;
                    if (fault == 1)
                        bad.cut.face_off.back()++;
                    if (fault == 2)
                        bad.cut.connectivity[0] = 8;
                    if (fault == 3)
                        bad.cut.poly_off.clear();
                }
                bool rejected = false;
                try {
                    writer.write_step(99, 1, bad);
                } catch (const std::exception&) {
                    rejected = true;
                }
                if (!rejected)
                    throw std::runtime_error("invalid cut input accepted");
            }
        }
        writer.write_step(0, 0, step);
        writer.add_summary("warpage_pv",
                           pv,
                           "m",
                           0,
                           "top surface (z=max) Q2 nodes; least-squares plane w=a+bx+cy removed; "
                           "PV=max-min residual");
        writer.add_summary("T_max", 193.15, "K", 0);
        writer.add_summary("U_mag_max", umax, "m", 0);
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
            writer.add_summary("warpage_pv",
                               pv,
                               "m",
                               1,
                               "top surface (z=max) Q2 nodes; least-squares plane w=a+bx+cy "
                               "removed; PV=max-min residual");
            writer.add_summary("T_max", 193.15, "K", 1);
            writer.add_summary("U_mag_max", umax, "m", 1);
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
