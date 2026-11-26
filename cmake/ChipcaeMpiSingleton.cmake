# SPDX-License-Identifier: Apache-2.0
include_guard(GLOBAL)

if(NOT CHIPCAE_BUILD_TESTS)
    return()
endif()

include(CMakePushCheckState)
include(CheckCXXSourceCompiles)
set(_chipcae_singleton_open_mpi FALSE)
if(CHIPCAE_ENABLE_MPI AND TARGET MPI::MPI_CXX)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES MPI::MPI_CXX)
    check_cxx_source_compiles("
        #include <mpi.h>
        #if !defined(OMPI_MAJOR_VERSION) || !defined(OMPI_MINOR_VERSION)
        #error Not Open MPI
        #endif
        int main() { return 0; }
    " CHIPCAE_SINGLETON_OPEN_MPI_HEADERS)
    cmake_pop_check_state()
    set(_chipcae_singleton_open_mpi "${CHIPCAE_SINGLETON_OPEN_MPI_HEADERS}")
endif()

set(_chipcae_singleton_driver "${CMAKE_CURRENT_LIST_DIR}/check_ompi_singleton_startup.cmake")
if(NOT _chipcae_singleton_open_mpi)
    add_test(NAME infra_ompi_singleton_startup
        COMMAND cmake -E echo "本构建不是 Open MPI")
    set_tests_properties(infra_ompi_singleton_startup PROPERTIES
        TIMEOUT 2400 RUN_SERIAL TRUE LABELS "infra;acceptance;owner:INFRA"
        SKIP_REGULAR_EXPRESSION "本构建不是 Open MPI")
    return()
endif()

# Cross-directory TEST DIRECTORY access was added in CMake 3.28. The root
# project's 3.20 minimum still applies to tests-off and non-Open-MPI builds;
# an Open MPI test build must not silently omit protection in subdirectories.
if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR
        "Open MPI singleton test protection requires CMake >= 3.28; upgrade CMake or disable MPI/tests")
endif()

# Use the C wrapper next to the configured CXX wrapper, never a PATH vendor guess.
if(NOT MPI_C_COMPILER)
    get_filename_component(_chipcae_mpi_bin "${MPI_CXX_COMPILER}" DIRECTORY)
    find_program(_chipcae_mpicc NAMES mpicc HINTS "${_chipcae_mpi_bin}" NO_DEFAULT_PATH)
    if(NOT _chipcae_mpicc)
        message(FATAL_ERROR "Open MPI C wrapper missing next to ${MPI_CXX_COMPILER}")
    endif()
    set(MPI_C_COMPILER "${_chipcae_mpicc}")
endif()
find_package(MPI REQUIRED COMPONENTS C)
if(NOT MPI_C_INCLUDE_DIRS STREQUAL MPI_CXX_INCLUDE_DIRS
        OR NOT MPI_C_LIBRARIES STREQUAL MPI_CXX_LIBRARIES
        OR NOT MPI_C_VERSION STREQUAL MPI_CXX_VERSION)
    message(FATAL_ERROR "Singleton probe requires matching MPI C/CXX installations")
endif()

include(CheckCSourceCompiles)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES MPI::MPI_C)
check_c_source_compiles("
    #include <mpi.h>
    #ifndef OMPI_MAJOR_VERSION
    #error Not Open MPI C
    #endif
    int main(void) { return 0; }
" CHIPCAE_SINGLETON_OPEN_MPI_C_HEADERS)
if(NOT CHIPCAE_SINGLETON_OPEN_MPI_C_HEADERS)
    message(FATAL_ERROR "Singleton probe C component is not Open MPI")
endif()
function(_chipcae_singleton_check_clock)
    set(CMAKE_REQUIRED_LIBRARIES "")
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
    check_c_source_compiles("
        #include <time.h>
        int main(void) { struct timespec t; return timespec_get(&t, TIME_UTC) == 0; }
    " CHIPCAE_SINGLETON_TIMESPEC_GET)
    if(NOT CHIPCAE_SINGLETON_TIMESPEC_GET)
        check_c_source_compiles("
            #define _POSIX_C_SOURCE 200809L
            #include <time.h>
            int main(void) { struct timespec t; return clock_gettime(CLOCK_MONOTONIC, &t); }
        " CHIPCAE_SINGLETON_POSIX_CLOCK)
        if(NOT CHIPCAE_SINGLETON_POSIX_CLOCK)
            message(FATAL_ERROR "No portable clock available for singleton probe")
        endif()
    endif()
endfunction()
_chipcae_singleton_check_clock()
cmake_pop_check_state()

add_executable(chipcae_ompi_singleton_probe "${CMAKE_CURRENT_LIST_DIR}/ompi_singleton_probe.c")
set_target_properties(chipcae_ompi_singleton_probe PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(chipcae_ompi_singleton_probe PRIVATE MPI::MPI_C)
if(CHIPCAE_SINGLETON_TIMESPEC_GET)
    target_compile_definitions(chipcae_ompi_singleton_probe PRIVATE CHIPCAE_PROBE_TIMESPEC_GET=1)
else()
    target_compile_definitions(chipcae_ompi_singleton_probe PRIVATE CHIPCAE_PROBE_POSIX_CLOCK=1)
endif()
chipcae_set_warnings(chipcae_ompi_singleton_probe)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME infra_ompi_singleton_startup COMMAND cmake
    "-DPROBE=$<TARGET_FILE:chipcae_ompi_singleton_probe>"
    "-DPYTHON=${Python3_EXECUTABLE}" -P "${_chipcae_singleton_driver}")
set_tests_properties(infra_ompi_singleton_startup PROPERTIES
    TIMEOUT 2400 RUN_SERIAL TRUE LABELS "infra;acceptance;owner:INFRA")

# Controlled executables exercise the same real driver, with its fixed 10 s gate
# and N >= 200 validation. No WILL_FAIL: check the specific failure diagnostic.
foreach(_case sleep exit7 missing iterations success environment descendant_exit)
    add_test(NAME infra_ompi_singleton_driver_${_case} COMMAND cmake
        "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
        -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_driver_${_case} PROPERTIES
        TIMEOUT 40 LABELS "infra;unit;owner:INFRA")
endforeach()
foreach(_case selection propagation matching compatibility conflict_inherited conflict_environment
        conflict_modification conflict_unset)
    add_test(NAME infra_ompi_singleton_${_case} COMMAND cmake
        "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}" -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_${_case} PROPERTIES
        TIMEOUT 120 LABELS "infra;unit;owner:INFRA")
endforeach()
foreach(_case disabled non_open_mpi tests_off)
    add_test(NAME infra_ompi_singleton_${_case} COMMAND cmake
        "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
        -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_${_case} PROPERTIES
        TIMEOUT 60 LABELS "infra;unit;owner:INFRA")
endforeach()
add_test(NAME infra_ompi_singleton_configuration COMMAND cmake
    "-DPYTHON=${Python3_EXECUTABLE}" -DCHECK=configuration
    "-DBUILD_DIR=${PROJECT_BINARY_DIR}" -P "${_chipcae_singleton_driver}")
set_tests_properties(infra_ompi_singleton_configuration PROPERTIES
    TIMEOUT 60 LABELS "infra;unit;owner:INFRA")

# Per-test session isolation for both singleton clients and launcher children.
# Prefixes: https://docs.open-mpi.org/en/v5.0.x/mca.html
# PRRTE exposes prte_tmpdir_base (prte_info --all). OpenPMIx 5.0.8 uses
# PMIX_SERVER_TMPDIR / PMIX_SYSTEM_TMPDIR directly, not MCA directory aliases:
# https://github.com/openpmix/openpmix/blob/v5.0.8/src/mca/ptl/base/ptl_base_frame.c
# https://github.com/openpmix/openpmix/blob/v5.0.8/src/mca/ptl/client/ptl_client.c
set(_chipcae_singleton_keys
    PRTE_MCA_prte_tmpdir_base PMIX_SERVER_TMPDIR PMIX_SYSTEM_TMPDIR)

# Audited entry points, not a blanket 'mpi' label or 'solve' name match.
set(_chipcae_singleton_direct_tests
    infra_ompi_singleton_startup
    # mesh_p1: runtime.hpp, results_validation.cpp and direct preview children,
    # including the baseline calls inside the two mixed direct/MPI wrappers.
    mesh_digest_golden mesh_digest_mpi k1_preview k1_preview_mpi_surface
    k1_vtkhdf k1_results_validation k1_patch_gpu k1_cut_mfem_gpu
    preview_zalign
    # tools/samples/write_results.cpp and examples/s2_cut_minimal/main.cpp.
    vtkhdf_results_serial cut_results_serial
    # chipcae-solve main.cpp initializes MPI before validating CLI arguments.
    # The cut harnesses below also initialize MPI in their own entry points.
    solve_inputs solve_profile_cuda_unavailable solve_profile_cuda_gpus_zero
    solve_profile_cpu_gpus_nonzero solve_cut_manufactured solve_cut_output_eb_layer
    solve_ghost_penalty_rank_guard solve_cut_bilayer solve_cut_bilayer_gamma
    solve_cut_convection solve_pipeline solve_preview_mesh_digest solve_ghost_scan
    solve_cut_pipeline solve_demo solve_copper_trace_map_paths solve_fvm_cut_vertex_recon
    solve_lu_refine_floor solve_fvm_trace_map solve_fem_thermal_bow
    preview_outline_chamfer_differs preview_outline_volume preview_solve_outline_digest
    preview_outline_nonconvex_rejected preview_outline_external_polyset_rejected
    # Automatic Python tests in tests/unit/region_ls directly exec preview.
    mesh_hash_preview_np preview_copper_area preview_copper_phi preview_lid_nonconvex
    preview_mesh_params preview_outline_nonconvex preview_region_planes
    # PETSc initialization in column_gmg_unit.cpp and solvers/fvm/**/tests.
    solve_column_gmg_prolongation solve_column_gmg_unsupported solve_column_gmg_fcbga
    solve_column_gmg_fcbga_scaling solve_fvm_thermal solve_fvm_thermal_cut
    solve_fvm_elastic solve_fvm_elastic_concave solve_fvm_elastic_benchmark solve_fvm_bimetal
    # tests/integration/tracemap/test_tracemap_fvm.cpp uses fvm_test_util.hpp.
    tracemap_fvm_slab_harmonic tracemap_fvm_slab_series_xfail
    tracemap_cut_inside_normalized tracemap_cut_outside_xfail
    tracemap_cut_conservation_xfail tracemap_merge_conservation tracemap_merge_directional_xfail
    # CLI paths resolved inside contract/install, import_assembly and step_parts
    # wrappers rather than passed in their test's ENVIRONMENT property.
    contract_output_solver install_smoke install_relocate cpack_tgz
    import_assembly_cf_chain stack_writer_outline_polyset e2e_step_box_parts
    e2e_real_bulk_parts e2e_step_ring_chamfer_lid e2e_cylinder_volume
    e2e_derived_chamfer_lid e2e_cut_unsupported_material_layer)

function(_chipcae_singleton_protect_test directory test)
    # Hash the defining scope too: distinct scopes/build trees must not share
    # session roots. Keep the suffix short for runtime socket/file names.
    string(SHA256 _id "${directory}/${test}")
    string(SUBSTRING "${_id}" 0 16 _id)
    set(_base "${PROJECT_BINARY_DIR}/.ompi")
    set(_value "${_base}/${_id}")
    file(MAKE_DIRECTORY "${_value}")
    file(CHMOD "${_base}" "${_value}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    # Fail configuration on an unusable directory, before registering a green
    # looking test environment. Runtime-owned contents are cleaned by MPI;
    # these empty roots live with the build tree so tests can run repeatedly.
    file(WRITE "${_value}/.write-check" "singleton session directory check\n")
    file(REMOVE "${_value}/.write-check")

    get_property(_environment TEST "${test}" DIRECTORY "${directory}" PROPERTY ENVIRONMENT)
    get_property(_modifications TEST "${test}" DIRECTORY "${directory}"
        PROPERTY ENVIRONMENT_MODIFICATION)
    foreach(_key IN LISTS _chipcae_singleton_keys)
        if(DEFINED ENV{${_key}} AND NOT "$ENV{${_key}}" STREQUAL "${_value}")
            message(FATAL_ERROR "Open MPI singleton protection conflict: test ${test} inherited ${_key}=$ENV{${_key}}; requires ${_value}")
        endif()
        set(_kept_environment "")
        foreach(_entry IN LISTS _environment)
            if(_entry MATCHES "^${_key}=(.*)$")
                if(NOT "${CMAKE_MATCH_1}" STREQUAL "${_value}")
                    message(FATAL_ERROR "Open MPI singleton protection conflict: test ${test} ENVIRONMENT ${_entry}; requires ${_key}=${_value}")
                endif()
            else()
                # Preserve escaped semicolons in unrelated environment values.
                string(REPLACE ";" "\\;" _escaped "${_entry}")
                list(APPEND _kept_environment "${_escaped}")
            endif()
        endforeach()
        set(_kept_modifications "")
        foreach(_entry IN LISTS _modifications)
            if(_entry MATCHES "^${_key}=(.*)$")
                # Modifications run after ENVIRONMENT. Reject unset/reset and
                # concatenation, as well as conflicting set operations.
                if(NOT "${CMAKE_MATCH_1}" STREQUAL "set:${_value}")
                    message(FATAL_ERROR "Open MPI singleton protection conflict: test ${test} ENVIRONMENT_MODIFICATION ${_entry}; requires ${_key}=set:${_value}")
                endif()
            else()
                string(REPLACE ";" "\\;" _escaped "${_entry}")
                list(APPEND _kept_modifications "${_escaped}")
            endif()
        endforeach()
        # Coalesce matching inheritance/properties to one local assignment.
        list(APPEND _kept_environment "${_key}=${_value}")
        set(_environment "${_kept_environment}")
        set(_modifications "${_kept_modifications}")
        message(STATUS "Open MPI singleton protection: ${test}: ${_key}=${_value}")
    endforeach()
    set_property(TEST "${test}" DIRECTORY "${directory}"
        PROPERTY ENVIRONMENT "${_environment}")
    set_property(TEST "${test}" DIRECTORY "${directory}"
        PROPERTY ENVIRONMENT_MODIFICATION "${_modifications}")
endfunction()

function(_chipcae_singleton_protect_directory directory)
    get_property(_tests DIRECTORY "${directory}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        set(_selected FALSE)
        # test_parts.py's generated driver initializes MPI, including np=1.
        if(_test IN_LIST _chipcae_singleton_direct_tests OR _test MATCHES "^solve_parts_")
            set(_selected TRUE)
        endif()
        # Automatic integration wrappers explicitly export the real CLI.
        get_property(_environment TEST "${_test}" DIRECTORY "${directory}" PROPERTY ENVIRONMENT)
        foreach(_entry IN LISTS _environment)
            if(_entry MATCHES "^CHIPCAE_(SOLVE|PREVIEW)_EXE=")
                set(_selected TRUE)
            endif()
        endforeach()
        if(_selected)
            _chipcae_singleton_protect_test("${directory}" "${_test}")
        endif()
    endforeach()
    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
        _chipcae_singleton_protect_directory("${_child}")
    endforeach()
endfunction()
_chipcae_singleton_protect_directory("${PROJECT_SOURCE_DIR}")
