chipcae_auto_tests(PREFIX fvm_ LABELS "integration;numerical;fvm" TIMEOUT 1200)

# Keep the public-input probe in the build tree: this card owns no tests/solve files.
if(TARGET chipcae_solve_inputs)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/loader_probe.cpp" [=[
#include "chipcae/io/vtkhdf_results.hpp"
#include "chipcae/solve/case_loader.hpp"

#include <cmath>
#include <iostream>
#include <jsoncons/json.hpp>

using Json = jsoncons::json;
template <class T>
Json value(const T& x) {
    return Json(x);
}
template <class T>
Json family(const T& x) {
    if constexpr (requires { x.family; })
        return x.family;
    else
        return Json::null(); // Missing API is an assertion failure, never a
                             // fabricated default.
}
template <class T>
bool copper_fields(const T&) {
    return requires(T x) {
        x.copper_fraction;
        x.conductivity_diag;
    };
}
Json footprint(const chipcae::mesh::Footprint& x) {
    Json j;
    j["box"] = value(x.box);
    j["polygon"] = value(x.polygon);
    return j;
}
Json table(const chipcae::solve::PropertyTable& x) {
    Json j;
    j["temperatures"] = value(x.temperatures);
    j["values"] = value(x.values);
    j["interpolation"] = x.interpolation;
    j["extrapolation"] = x.extrapolation;
    return j;
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--half-polyset") {
        chipcae::mesh::Polyset polygons;
        polygons.dbu_m = 1e-6;
        polygons.layers.push_back(
            {"trace", {0, 4}, {0, 0, 500000, 0, 500000, 1000000, 0, 1000000}});
        const auto result = chipcae::mesh::write_polyset(argv[2], polygons);
        if (!result) {
            std::cerr << result.error().reason << '\n';
            return 1;
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--result-fields") {
        std::cout << (copper_fields(chipcae::io::ResultsPartition{}) ? "true" : "false") << '\n';
        return 0;
    }
    if (argc != 4)
        return 2;
    try {
        const auto x = chipcae::solve::load_inputs(argv[1], argv[2], std::stoi(argv[3]));
        Json j;
        j["family"] = family(x);
        j["backend"] = x.backend;
        j["case_path"] = x.case_path.string();
        j["profile_path"] = x.profile_path.string();
        j["stack_path"] = x.stack_path.string();
        j["material_path"] = x.material_path.string();
        j["thermal"] = x.thermal;
        j["T_ref"] = x.T_ref;
        j["cut_as_hex"] = x.cut_as_hex;
        j["ghost_penalty"] = x.ghost_penalty;
        j["fixed"] = value(x.fixed);
        j["fields"] = value(x.fields);
        j["linear"]["rtol"] = x.linear.rtol;
        j["linear"]["max_iter"] = x.linear.max_iter;
        j["linear"]["preconditioner"] = x.linear.preconditioner;
        Json g;
        g["snap_tol"] = x.grid.snap_tol;
        g["max_ratio"] = x.grid.max_ratio;
        g["ar_warn"] = std::isfinite(x.grid.ar_warn) ? value(x.grid.ar_warn) : Json("inf");
        g["ar_max"] = std::isfinite(x.grid.ar_max) ? value(x.grid.ar_max) : Json("inf");
        g["cell_budget"] = x.grid.cell_budget;
        g["ranks"] = x.grid.ranks;
        g["nx"] = x.grid.nx;
        g["ny"] = x.grid.ny;
        g["levels"] = x.grid.levels;
        g["use_metis"] = x.grid.use_metis;
        j["grid"] = g;
        Json c;
        c["snap_phi"] = x.cut_options.snap_phi;
        c["small_sub_vf"] = x.cut_options.small_sub_vf;
        c["quad_degree"] = x.cut_options.quad_degree;
        c["keep_void"] = x.cut_options.keep_void;
        c["export_polyhedra"] = x.cut_options.export_polyhedra;
        j["cut_options"] = c;
        Json s;
        s["id"] = x.stack.id;
        s["unit"] = x.stack.unit;
        s["symmetry"] = x.stack.symmetry;
        s["package_type"] = x.stack.package_type;
        s["warnings"] = value(x.stack.warnings);
        s["activation_groups"] = value(x.stack.activation_groups);
        s["outline"] = footprint(x.stack.outline);
        s["layers"] = Json::array();
        for (const auto& l : x.stack.layers) {
            Json a;
            a["name"] = l.name;
            a["kind"] = l.kind;
            a["material"] = l.material;
            a["body"] = l.body;
            a["thickness"] = l.thickness;
            a["grading"] = l.grading;
            a["n_elem_min"] = l.n_elem_min;
            a["regions"] = Json::array();
            for (const auto& r : l.regions) {
                Json b;
                b["name"] = r.name;
                b["material"] = r.material;
                b["body"] = r.body;
                b["footprint"] = footprint(r.footprint);
                a["regions"].push_back(b);
            }
            a["copper"] = Json::null();
            if (l.copper) {
                a["copper"] = Json::object();
                a["copper"]["metal"] = l.copper->metal;
                a["copper"]["fraction"] =
                    l.copper->fraction ? value(*l.copper->fraction) : Json::null();
                a["copper"]["polygons"] = Json::null();
                if (l.copper->polygons) {
                    a["copper"]["polygons"] = Json::object();
                    a["copper"]["polygons"]["file"] = l.copper->polygons->file;
                    a["copper"]["polygons"]["layer"] = l.copper->polygons->layer;
                }
            }
            s["layers"].push_back(a);
        }
        j["stack"] = s;
        j["materials"] = Json::object();
        for (const auto& [id, m] : x.materials) {
            Json a;
            a["id"] = m.id;
            a["E"] = m.E;
            a["nu"] = m.nu;
            a["alpha"] = m.alpha;
            a["k"] = m.k;
            a["E_table"] = table(m.E_table);
            a["nu_table"] = table(m.nu_table);
            a["alpha_table"] = table(m.alpha_table);
            a["E_clamped"] = m.E_clamped;
            a["nu_clamped"] = m.nu_clamped;
            a["alpha_clamped"] = m.alpha_clamped;
            j["materials"][id] = a;
        }
        j["loads"] = Json::array();
        for (const auto& l : x.loads) {
            Json a;
            a["type"] = l.type;
            a["target"] = l.target;
            a["id"] = l.id;
            a["T_ref"] = l.T_ref;
            a["T"] = l.T;
            a["h"] = l.h;
            a["T_inf"] = l.T_inf;
            a["source"] = l.source;
            a["total_power"] = l.total_power;
            j["loads"].push_back(a);
        }
        std::cout << j << '\n';
    } catch (const chipcae::solve::InputError& e) {
        std::cerr << e.what() << '\n';
        return e.code;
    }
}
]=])
    add_executable(fvm_case_loader_probe "${CMAKE_CURRENT_BINARY_DIR}/loader_probe.cpp")
    # The input library resolves schemas relative to chipcae-solve's executable directory.
    set_target_properties(fvm_case_loader_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/apps/chipcae-solve")
    target_link_libraries(fvm_case_loader_probe PRIVATE chipcae_solve_inputs chipcae::io)
    chipcae_set_warnings(fvm_case_loader_probe)
    set_property(TEST fvm_fvm_case_e2e APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_FVM_LOADER_PROBE=$<TARGET_FILE:fvm_case_loader_probe>")
endif()

if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import AND TARGET chipcae-preview AND TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    string(REPLACE ";" "|" _fvm_mpi_pre "${MPIEXEC_PREFLAGS}")
    string(REPLACE ";" "|" _fvm_mpi_post "${MPIEXEC_POSTFLAGS}")
    set_property(TEST fvm_fvm_case_e2e APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>"
        "CHIPCAE_PREVIEW_EXE=$<TARGET_FILE:chipcae-preview>"
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>"
        "CHIPCAE_MPIEXEC=${MPIEXEC_EXECUTABLE}"
        "CHIPCAE_MPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}"
        "CHIPCAE_MPIEXEC_PREFLAGS=${_fvm_mpi_pre}"
        "CHIPCAE_MPIEXEC_POSTFLAGS=${_fvm_mpi_post}")
endif()
