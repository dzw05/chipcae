#include "chipcae/io/vtkhdf_results.hpp"

#include <filesystem>
#include <fstream>
#include <hdf5.h>
#include <iostream>
#include <stdexcept>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#else
    (void)argc;
    (void)argv;
#endif
    int rank = 0, ranks = 1, status = 0;
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
#endif
    try {
        auto run = std::filesystem::temp_directory_path() /
                   ("p1-results-validation-" + std::to_string(ranks));
        if (rank == 0)
            std::filesystem::remove_all(run);
#ifdef CHIPCAE_ENABLE_MPI
        MPI_Barrier(MPI_COMM_WORLD);
#endif
        using namespace chipcae::io;
        ResultsWriter writer(run, std::string(K1_ROOT) + "/tools/samples/s1.stack.json");
        ResultsStep step;
        auto& p = step.cut;
        p.points = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
        p.connectivity = {0, 1, 2, 3, 4, 5, 6, 7};
        p.offsets = {0, 8};
        p.face_conn = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5};
        p.face_off = {0, 4, 8, 12, 16, 20, 24};
        p.poly_faces = {0, 1, 2, 3, 4, 5};
        p.poly_off = {0, 6};
        p.elem_gid = {rank};
        p.layer_id = {1};
        p.material_id = {2};
        p.cell_kind = {1};
        p.volume = {1};
        writer.write_step(0, 0, step); // Geometry-only cut fields are optional.
        auto require_missing_summary_rejection = [](ResultsWriter& result_writer) {
            bool rejected = false;
            try {
                result_writer.finalize();
            } catch (const std::exception&) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("missing solver summary accepted");
        };
        writer.add_summary("T_max", 300, "K", 0);
        require_missing_summary_rejection(writer);
        // Selection applies to physical fields; cut topology metadata stays required.
        auto selected_run = run / "selected";
        ResultsWriter selected_writer(
            selected_run, std::string(K1_ROOT) + "/tools/samples/s1.stack.json", {}, {}, {"U"});
        auto selected_step = step;
        selected_step.cut.T.assign(8, 300);
        selected_step.cut.U.assign(24, 0);
        selected_writer.write_step(0, 0, selected_step);
        selected_writer.add_summary("warpage_pv", 0, "m", 0);
        selected_writer.add_summary("T_max", 300, "K", 0);
        selected_writer.add_summary("U_mag_max", 0, "m", 0);
        selected_writer.finalize();
        ResultsWriter thermal_writer(run / "thermal",
                                     std::string(K1_ROOT) + "/tools/samples/s1.stack.json",
                                     {},
                                     {},
                                     {"T"},
                                     false);
        thermal_writer.write_step(0, 0, selected_step);
        require_missing_summary_rejection(thermal_writer);
        thermal_writer.add_summary("T_max", 300, "K", 0);
        thermal_writer.finalize();
        if (rank == 0) {
            auto file =
                H5Fopen((selected_run / "out/step-0.vtkhdf").c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
            if (file < 0)
                throw std::runtime_error("selected cut output missing");
            bool valid = H5Lexists(file, "/VTKHDF/cut/PointData/T", H5P_DEFAULT) == 0;
            for (const auto* name : {"PointData/U",
                                     "CellData/elem_gid",
                                     "CellData/volume",
                                     "CellData/layer_id",
                                     "CellData/material_id",
                                     "CellData/cell_kind"})
                valid &=
                    H5Lexists(file, (std::string("/VTKHDF/cut/") + name).c_str(), H5P_DEFAULT) > 0;
            H5Fclose(file);
            std::ifstream manifest(selected_run / "manifest.json");
            std::string json((std::istreambuf_iterator<char>(manifest)), {});
            valid &= json.find("\"name\":\"T\"") == std::string::npos;
            for (const auto* name :
                 {"U", "elem_gid", "volume", "layer_id", "material_id", "cell_kind"})
                valid &= json.find(std::string("\"name\":\"") + name + "\"") != std::string::npos;
            if (!valid)
                throw std::runtime_error("cut field selection lost required metadata");
        }
        for (int trial = 0; trial < 13; ++trial) {
            auto bad = step;
            if (rank == 0) {
                switch (trial) {
                case 0:
                    bad.cut.T = {1};
                    break;
                case 1:
                    bad.cut.U = {1};
                    break;
                case 2:
                    bad.cut.face_conn[0] = -1;
                    break;
                case 3:
                    bad.cut.poly_faces[0] = -1;
                    break;
                case 4:
                    bad.cut.face_off.back() = 25;
                    break;
                case 5:
                    bad.cut.volume[0] = 0;
                    break;
                case 6:
                    bad.embedded_boundary.face_conn = {1};
                    break;
                case 8:
                    bad.cut.connectivity[0] = 1;
                    break;
                case 9:
                    bad.cut.poly_faces[0] = 1;
                    break;
                case 10:
                    bad.cut.points.insert(bad.cut.points.end(), {2, 2, 2});
                    bad.cut.connectivity[0] = 8;
                    break;
                case 11:
                    bad.cut.face_conn[0] = 3;
                    break;
                case 12:
                    bad.hex.points = bad.cut.points;
                    bad.hex.hex = bad.cut.connectivity;
                    bad.hex.T.assign(8, 300);
                    bad.hex.U.assign(24, 0);
                    bad.hex.elem_gid = {999};
                    bad.hex.layer_id = {1};
                    bad.hex.material_id = {2};
                    bad.hex.cell_kind = {1};
                    break;
                case 7:
                    bad.cut.material_id.clear();
                    break;
                }
            }
            bool rejected = false;
            try {
                writer.write_step(trial + 1, 0, bad);
            } catch (const std::exception&) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("invalid cut arrays accepted");
        }
        if (ranks > 1) {
            auto bad = step;
            if (rank == 0)
                bad.cut.T.assign(8, 300);
            bool rejected = false;
            try {
                writer.write_step(20, 0, bad);
            } catch (const std::exception&) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("mismatched field schema accepted");
            bad = step;
            if (rank == 0)
                bad.cut_as_hex = true;
            rejected = false;
            try {
                writer.write_step(21, 0, bad);
            } catch (const std::exception&) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("mismatched fallback accepted");
        }
#ifdef CHIPCAE_ENABLE_MPI
        MPI_Barrier(MPI_COMM_WORLD);
#endif
        if (rank == 0) {
            for (const auto& entry : std::filesystem::directory_iterator(run / "out"))
                if (entry.path().filename() != "step-0.vtkhdf")
                    throw std::runtime_error("invalid write published data");
            std::filesystem::remove_all(run);
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
