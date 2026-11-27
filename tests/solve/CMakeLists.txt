# Offline acceptance for the nightly job and the real main.cpp compile-fault switch.
if(MFEM_USE_CUDA OR CHIPCAE_CI_INJECT_CUDA_FAULT)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    foreach(_cuda_nightly_case IN ITEMS workflow dependencies cuda_default cuda_injected
            cpu_injected no_mfem_injected)
        add_test(NAME solve_cuda_nightly_${_cuda_nightly_case}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cuda_nightly_compile.py
            ${PROJECT_SOURCE_DIR} ${_cuda_nightly_case}
            --compiler ${CMAKE_CXX_COMPILER} --generator ${CMAKE_GENERATOR})
        set_tests_properties(solve_cuda_nightly_${_cuda_nightly_case} PROPERTIES
            TIMEOUT 180 LABELS "solve;cuda;ci;tierA")
    endforeach()
endif()

# Input validation also runs in builds without the optional numerical backend.
if(TARGET chipcae_solve_inputs)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_solve_backend 0)
    if(CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
        set(_solve_backend 1)
    endif()
    add_test(NAME solve_inputs COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_inputs.py $<TARGET_FILE:chipcae-solve> ${PROJECT_SOURCE_DIR} ${_solve_backend})
    set_tests_properties(solve_inputs PROPERTIES TIMEOUT 600)
    foreach(_profile_case IN ITEMS cuda_unavailable cuda_gpus_zero cpu_gpus_nonzero)
        # The unavailable diagnostic applies only without CUDA-enabled MFEM.
        if(_profile_case STREQUAL "cuda_unavailable" AND MFEM_USE_CUDA)
            continue()
        endif()
        add_test(NAME solve_profile_${_profile_case} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_run_profile.py
            $<TARGET_FILE:chipcae-solve> ${PROJECT_SOURCE_DIR} ${_profile_case})
        set_tests_properties(solve_profile_${_profile_case} PROPERTIES TIMEOUT 150)
    endforeach()
    add_executable(chipcae_test_solve_interpolation test_interpolation.cpp)
    target_link_libraries(chipcae_test_solve_interpolation PRIVATE chipcae_solve_inputs)
    chipcae_set_warnings(chipcae_test_solve_interpolation)
    add_test(NAME solve_interpolation COMMAND chipcae_test_solve_interpolation)
    add_executable(chipcae_test_solve_mesh test_mesh.cpp)
    target_link_libraries(chipcae_test_solve_mesh PRIVATE chipcae_solve_inputs)
    chipcae_set_warnings(chipcae_test_solve_mesh)
    add_test(NAME solve_mesh COMMAND chipcae_test_solve_mesh)
    set_tests_properties(solve_mesh PROPERTIES
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

# Exercise the app's real configure guard without requiring another MFEM installation.
# Dependency targets only let configuration reach the guard; no sources are built here.
set(_gmg_no_petsc_check "${CMAKE_CURRENT_BINARY_DIR}/column_gmg_no_petsc")
file(MAKE_DIRECTORY "${_gmg_no_petsc_check}/source")
file(WRITE "${_gmg_no_petsc_check}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(column_gmg_no_petsc LANGUAGES NONE)
add_library(chipcae::core INTERFACE IMPORTED)
add_library(mfem INTERFACE IMPORTED)
add_library(yaml-cpp::yaml-cpp INTERFACE IMPORTED)
set(CMAKE_DISABLE_FIND_PACKAGE_yaml-cpp ON)
function(chipcae_set_warnings)
endfunction()
set(CHIPCAE_ENABLE_JSONCONS ON)
set(CHIPCAE_ENABLE_MFEM ON)
set(CHIPCAE_ENABLE_MPI ON)
set(CHIPCAE_ENABLE_HDF5 ON)
set(MFEM_USE_PETSC OFF)
add_subdirectory("${CHIPCAE_SOURCE_DIR}/apps/chipcae-solve" solve)
]=])
file(WRITE "${_gmg_no_petsc_check}/check.cmake" [=[
execute_process(COMMAND cmake -G "${CHECK_GENERATOR}"
    -S "${CHECK_SOURCE_DIR}" -B "${CHECK_BINARY_DIR}"
    "-DCHIPCAE_SOURCE_DIR=${CHIPCAE_SOURCE_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT error MATCHES "chipcae-solve requires MFEM built with PETSc support")
    message(FATAL_ERROR "Expected the PETSc backend configure rejection; got ${result}:\n${output}\n${error}")
endif()
]=])
add_test(NAME solve_column_gmg_requires_petsc COMMAND cmake
    "-DCHECK_GENERATOR=${CMAKE_GENERATOR}"
    "-DCHECK_SOURCE_DIR=${_gmg_no_petsc_check}/source"
    "-DCHECK_BINARY_DIR=${_gmg_no_petsc_check}/build"
    "-DCHIPCAE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${_gmg_no_petsc_check}/check.cmake")
set_tests_properties(solve_column_gmg_requires_petsc PROPERTIES
    TIMEOUT 120 LABELS "gmg;configuration")
if(_solve_backend)
    add_test(NAME solve_pipeline COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_pipeline.py $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE})
    set_tests_properties(solve_pipeline PROPERTIES TIMEOUT 600)
endif()

# Compare the public solver and preview canonical mesh contracts at np=1 and np=2.
if(_solve_backend)
    add_test(NAME solve_preview_mesh_digest COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_preview_digest.py
        $<TARGET_FILE:chipcae-solve> $<TARGET_FILE:chipcae-preview>
        ${MPIEXEC_EXECUTABLE})
    set_tests_properties(solve_preview_mesh_digest PROPERTIES TIMEOUT 600)
endif()

if(_solve_backend AND TARGET chipcae_material)
    add_executable(chipcae_test_solve_mixture test_mixture.cpp)
    target_include_directories(chipcae_test_solve_mixture PRIVATE ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/include)
    target_link_libraries(chipcae_test_solve_mixture PRIVATE chipcae_solve_inputs chipcae_material mfem)
    add_test(NAME solve_material_mixture COMMAND chipcae_test_solve_mixture)

    add_executable(chipcae_test_copper_trace_map test_copper_trace_map.cpp)
    target_include_directories(chipcae_test_copper_trace_map PRIVATE ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/include)
    target_link_libraries(chipcae_test_copper_trace_map PRIVATE chipcae_solve_inputs chipcae_material mfem)
    chipcae_set_warnings(chipcae_test_copper_trace_map)
    add_test(NAME solve_copper_trace_map COMMAND chipcae_test_copper_trace_map)
    set_tests_properties(solve_copper_trace_map PROPERTIES TIMEOUT 60 LABELS "solve;material;tierA")
endif()
if(_solve_backend AND CHIPCAE_ENABLE_ECAD)
    add_test(NAME solve_copper_trace_map_paths COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_copper_trace_map.py
        $<TARGET_FILE:chipcae-solve> ${PROJECT_SOURCE_DIR})
    set_tests_properties(solve_copper_trace_map_paths PROPERTIES
        TIMEOUT 600 LABELS "solve;material;mpi;io;cut;numerical"
        ENVIRONMENT "MPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE};OMP_NUM_THREADS=2;OPENBLAS_NUM_THREADS=1")
endif()
if(_solve_backend)
    add_executable(chipcae_cut_manufactured cut_manufactured.cpp)
    target_link_libraries(chipcae_cut_manufactured PRIVATE chipcae::fem)
    chipcae_set_warnings(chipcae_cut_manufactured)
    add_test(NAME solve_cut_manufactured COMMAND chipcae_cut_manufactured)
    set_tests_properties(solve_cut_manufactured PROPERTIES TIMEOUT 1800 LABELS "cut;numerical")
endif()
if(_solve_backend)
    add_test(NAME solve_cut_pipeline COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_cut_pipeline.py $<TARGET_FILE:chipcae-solve>
        ${MPIEXEC_EXECUTABLE} ${PROJECT_SOURCE_DIR})
    set_tests_properties(solve_cut_pipeline PROPERTIES TIMEOUT 1200 LABELS "cut;mpi;io")
endif()
if(_solve_backend)
    add_executable(chipcae_test_cut_output_guard cut_output_guard.cpp)
    target_include_directories(chipcae_test_cut_output_guard PRIVATE ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/src)
    target_link_libraries(chipcae_test_cut_output_guard PRIVATE chipcae_solve_inputs chipcae::fem chipcae::io mfem)
    chipcae_set_warnings(chipcae_test_cut_output_guard)
    add_test(NAME solve_cut_output_eb_layer COMMAND chipcae_test_cut_output_guard eb_layer)
    add_test(NAME solve_ghost_penalty_rank_guard COMMAND chipcae_test_cut_output_guard ghost)

    add_executable(chipcae_ghost_scan ghost_scan.cpp)
    target_link_libraries(chipcae_ghost_scan PRIVATE chipcae::fem)
    chipcae_set_warnings(chipcae_ghost_scan)
    add_test(NAME solve_ghost_scan COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_ghost_scan.py $<TARGET_FILE:chipcae_ghost_scan>)
    set_tests_properties(solve_ghost_scan PROPERTIES TIMEOUT 300 LABELS "cut;numerical")
endif()

if(_solve_backend)
    add_executable(chipcae_cut_bilayer cut_bilayer.cpp)
    target_link_libraries(chipcae_cut_bilayer PRIVATE chipcae_solve_inputs chipcae::fem)
    chipcae_set_warnings(chipcae_cut_bilayer)
    add_test(NAME solve_cut_bilayer COMMAND chipcae_cut_bilayer ${PROJECT_SOURCE_DIR}/examples/s2_solve)
    set_tests_properties(solve_cut_bilayer PROPERTIES TIMEOUT 1200 LABELS "cut;numerical")
endif()
if(_solve_backend)
    add_executable(chipcae_test_cut_convection cut_convection.cpp)
    target_link_libraries(chipcae_test_cut_convection PRIVATE chipcae_solve_inputs chipcae::fem)
    chipcae_set_warnings(chipcae_test_cut_convection)
    add_test(NAME solve_cut_convection COMMAND chipcae_test_cut_convection)
endif()

if(_solve_backend)
    add_test(NAME solve_demo COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_demo.py $<TARGET_FILE:chipcae-solve> ${PROJECT_SOURCE_DIR})
    set_tests_properties(solve_demo PROPERTIES TIMEOUT 600)
endif()

if(TARGET chipcae_cut_bilayer)
    add_test(NAME solve_cut_bilayer_gamma COMMAND chipcae_cut_bilayer ${PROJECT_SOURCE_DIR}/examples/s2_solve --gamma-scan)
    set_tests_properties(solve_cut_bilayer_gamma PROPERTIES TIMEOUT 1200 LABELS "cut;numerical")
endif()

if(_solve_backend AND TARGET chipcae-solve AND MFEM_USE_PETSC)
    add_executable(chipcae_test_column_gmg column_gmg_unit.cpp)
    target_include_directories(chipcae_test_column_gmg PRIVATE
        ${PROJECT_SOURCE_DIR}/solvers/fem/src ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/src)
    target_link_libraries(chipcae_test_column_gmg PRIVATE chipcae::fem mfem MPI::MPI_CXX)
    chipcae_set_warnings(chipcae_test_column_gmg)
    add_test(NAME solve_column_gmg_prolongation COMMAND chipcae_test_column_gmg prolongation)
    add_test(NAME solve_column_gmg_unsupported COMMAND chipcae_test_column_gmg unsupported)
    add_test(NAME solve_column_gmg_fcbga COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_column_gmg.py
        $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE} ${PROJECT_SOURCE_DIR}
        --coverage reference)
    add_test(NAME solve_column_gmg_fcbga_scaling COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_column_gmg.py
        $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE} ${PROJECT_SOURCE_DIR}
        --coverage scaling)
    set_tests_properties(solve_column_gmg_prolongation solve_column_gmg_unsupported
        PROPERTIES TIMEOUT 120 LABELS "gmg;numerical")
    set_tests_properties(solve_column_gmg_fcbga solve_column_gmg_fcbga_scaling
        PROPERTIES TIMEOUT 900 LABELS "gmg;numerical")
    # Pinned Release RSS, including the killed scaling run, selects lock members.
    # The lock covers each complete Python wrapper, including its MPI children.
    set_tests_properties(solve_column_gmg_fcbga solve_column_gmg_fcbga_scaling
        PROPERTIES RESOURCE_LOCK chipcae_highmem)
    set_property(TEST solve_column_gmg_fcbga solve_column_gmg_fcbga_scaling
        APPEND PROPERTY LABELS highmem)
endif()

if(_solve_backend)
    foreach(_outline_test IN ITEMS preview_outline_chamfer_differs preview_outline_volume
            preview_solve_outline_digest preview_outline_nonconvex_rejected
            preview_outline_external_polyset_rejected)
        add_test(NAME ${_outline_test} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_preview_outline.py
            $<TARGET_FILE:chipcae-solve> $<TARGET_FILE:chipcae-preview>
            ${MPIEXEC_EXECUTABLE} ${_outline_test})
        set_tests_properties(${_outline_test} PROPERTIES TIMEOUT 600 LABELS "mesh;cut;preview;mpi;io")
    endforeach()
endif()

# SP6 test-first acceptance. The Python file owns the generated, build-tree-only
# C++ harness so this card does not add production hooks or installed interfaces.
if(TARGET chipcae_solve_inputs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS test_parts.py)
    set(_parts_generated "${CMAKE_CURRENT_BINARY_DIR}/parts_generated")
    execute_process(COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_parts.py --emit "${_parts_generated}"
        COMMAND_ERROR_IS_FATAL ANY)
    add_executable(chipcae_test_solve_parts "${_parts_generated}/parts_driver.cpp")
    target_include_directories(chipcae_test_solve_parts PRIVATE
        ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/src
        ${PROJECT_SOURCE_DIR}/schema/src)
    target_link_libraries(chipcae_test_solve_parts PRIVATE chipcae_solve_inputs)
    chipcae_set_warnings(chipcae_test_solve_parts)
    if(_solve_backend AND MFEM_USE_PETSC)
        target_compile_definitions(chipcae_test_solve_parts PRIVATE PARTS_BACKEND=1)
        target_include_directories(chipcae_test_solve_parts PRIVATE ${PROJECT_SOURCE_DIR}/material/include)
        target_link_libraries(chipcae_test_solve_parts PRIVATE chipcae::fem chipcae::io mfem)
    endif()

    function(_parts_test name)
        add_test(NAME solve_parts_${name} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_parts.py
            --driver $<TARGET_FILE:chipcae_test_solve_parts>
            --solve $<TARGET_FILE:chipcae-solve> --source ${PROJECT_SOURCE_DIR}
            --test ${name} ${ARGN})
        set_tests_properties(solve_parts_${name} PROPERTIES
            TIMEOUT 600 LABELS "solve;parts;tierA"
            ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
    endfunction()

    foreach(_parts_case IN ITEMS region_material region_material_missing copper_material_missing
            empty_mesh alias_case alias_gap alias_missing_zero alias_prefix alias_bad_suffix
            alias_leading_zero alias_negative alias_load_split alias_load_exact
            alias_load_exact_malformed alias_load_numeric heat_load_intact)
        _parts_test(${_parts_case})
    endforeach()
    if(_solve_backend AND MFEM_USE_PETSC)
        foreach(_parts_case IN ITEMS omitted_cells unused_attribute used_unmapped
                floating_lid floating_reverse floating_plate floating_constrained
                floating_thermal_constraint floating_cut floating_anchors floating_anchors_connected
                floating_multiple floating_multiple_reverse floating_names floating_mixed
                floating_mechanical
                shared_node coincident_distinct_nodes thermal_only
                thermal_only_no_constraints thermal_only_rigid_cut
                thermal_only_temperature_field thermal_only_yaml summary_contract
                alias_split alias_exact alias_numeric mechanical_alias convection_alias
                convection_cut_alias
                heat_split heat_q_vol heat_additive heat_zero_volume heat_cut_volume
                summary_writer summary_default summary_pipeline)
            _parts_test(${_parts_case})
        endforeach()
        foreach(_np IN ITEMS 1 2 3)
            foreach(_match IN ITEMS match mismatch)
                _parts_test(ranks_${_match}_np${_np} --mpiexec ${MPIEXEC_EXECUTABLE})
                set_tests_properties(solve_parts_ranks_${_match}_np${_np} PROPERTIES
                    LABELS "solve;parts;tierA;mpi")
            endforeach()
        endforeach()
        foreach(_np IN ITEMS 2 3)
            _parts_test(omitted_mpi${_np} --mpiexec ${MPIEXEC_EXECUTABLE})
            set_tests_properties(solve_parts_omitted_mpi${_np} PROPERTIES
                LABELS "solve;parts;tierA;mpi;io")
        endforeach()
        foreach(_parts_case IN ITEMS thermal_only_np2 summary_contract_np2)
            _parts_test(${_parts_case} --mpiexec ${MPIEXEC_EXECUTABLE})
            set_tests_properties(solve_parts_${_parts_case} PROPERTIES
                LABELS "solve;parts;tierA;mpi;io")
        endforeach()
    endif()

    # B is written today and remains disabled until region cutting is integrated.
    foreach(_parts_case IN ITEMS solve_ring_rotated_c pending_covered pending_missing
            pending_missing_reverse region_cut_receiver)
        _parts_test(${_parts_case})
        set_tests_properties(solve_parts_${_parts_case} PROPERTIES
            DISABLED TRUE LABELS "solve;parts;cut;sp6-b")
    endforeach()
    if(_solve_backend AND MFEM_USE_PETSC)
        foreach(_parts_case IN ITEMS region_ls_cut_quad6 cut_chamfer_lid_volume
                solve_lid_rounded solve_chamfer_lid_cut cut_convection_alias)
            _parts_test(${_parts_case})
            set_tests_properties(solve_parts_${_parts_case} PROPERTIES
                DISABLED TRUE TIMEOUT 1200 LABELS "solve;parts;cut;numerical;sp6-b")
        endforeach()
    endif()
endif()

if(_solve_backend AND CHIPCAE_ENABLE_ECAD)
    add_test(NAME solve_fem_thermal_bow COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_fem_thermal_bow.py
        $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE})
    set_tests_properties(solve_fem_thermal_bow PROPERTIES
        TIMEOUT 600 LABELS "solve;numerical")
endif()

# Nonconvex outline acceptance; containment needs no numerical backend.
add_executable(chipcae_test_solve_outline_contains test_outline_contains.cpp)
target_compile_features(chipcae_test_solve_outline_contains PRIVATE cxx_std_20)
target_include_directories(chipcae_test_solve_outline_contains PRIVATE
    ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/src)
chipcae_set_warnings(chipcae_test_solve_outline_contains)
add_test(NAME solve_outline_contains COMMAND chipcae_test_solve_outline_contains)
set_tests_properties(solve_outline_contains PROPERTIES TIMEOUT 30 LABELS "solve;cut")

if(_solve_backend)
    add_executable(chipcae_test_solve_cutgeom_nc_consume_outline test_cutgeom_nc_consume_outline.cpp)
    target_link_libraries(chipcae_test_solve_cutgeom_nc_consume_outline PRIVATE
        chipcae_mesh_cut jsoncons)
    chipcae_set_warnings(chipcae_test_solve_cutgeom_nc_consume_outline)
    add_test(NAME solve_cutgeom_nc_consume_outline
        COMMAND chipcae_test_solve_cutgeom_nc_consume_outline
        ${PROJECT_SOURCE_DIR}/tests/contract/fixtures/cutgeom-nc-1.0)
    set_tests_properties(solve_cutgeom_nc_consume_outline PROPERTIES
        TIMEOUT 180 LABELS "cut;contract")
endif()

if(TARGET chipcae_solve_inputs AND _solve_backend)
    add_test(NAME solve_fvm_solver_convergence COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_fvm_solver_convergence.py
        $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE})
    set_tests_properties(solve_fvm_solver_convergence PROPERTIES
        TIMEOUT 600 LABELS "solve;fvm;numerical;mpi"
        ENVIRONMENT "OMP_NUM_THREADS=2;OPENBLAS_NUM_THREADS=1")
endif()

if(TARGET chipcae_solve_inputs AND _solve_backend)
    add_test(NAME solve_fvm_elastic_convergence COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_fvm_elastic_convergence.py
        $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE})
    set_tests_properties(solve_fvm_elastic_convergence PROPERTIES
        TIMEOUT 600 SKIP_RETURN_CODE 77 LABELS "solve;fvm;numerical"
        ENVIRONMENT "OMP_NUM_THREADS=2;OPENBLAS_NUM_THREADS=1")
endif()

if(_solve_backend AND TARGET chipcae-solve AND MFEM_USE_PETSC)
    add_test(NAME solve_fvm_cut_vertex_recon COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_fvm_cut_vertex_recon.py $<TARGET_FILE:chipcae-solve>
        ${PROJECT_SOURCE_DIR} --with-fem)
    set_tests_properties(solve_fvm_cut_vertex_recon PROPERTIES
        TIMEOUT 1200 LABELS "solve;numerical;cut" SKIP_RETURN_CODE 77)
endif()

if(_solve_backend)
    add_executable(chipcae_test_direct_refine test_direct_refine.cpp)
    target_include_directories(chipcae_test_direct_refine PRIVATE ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/src)
    target_link_libraries(chipcae_test_direct_refine PRIVATE mfem MPI::MPI_CXX)
    chipcae_set_warnings(chipcae_test_direct_refine)
    add_test(NAME solve_direct_refine COMMAND chipcae_test_direct_refine)
    set_tests_properties(solve_direct_refine PROPERTIES TIMEOUT 120 LABELS "solve;numerical")
endif()
if(_solve_backend AND CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import AND TARGET chipcae-preview)
    add_test(NAME solve_lu_refine_floor COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_lu_refine_floor.py
        $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE}
        $<TARGET_FILE:chipcae-import> $<TARGET_FILE:chipcae-preview>)
    set_tests_properties(solve_lu_refine_floor PROPERTIES TIMEOUT 600 LABELS "solve;numerical;mpi;io"
        ENVIRONMENT "OMP_NUM_THREADS=2;OPENBLAS_NUM_THREADS=1;OMPI_ALLOW_RUN_AS_ROOT=1;OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1")
endif()
if(_solve_backend AND CHIPCAE_ENABLE_ECAD)
    add_test(NAME solve_fvm_trace_map COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_fvm_trace_map.py
        $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE})
    set_tests_properties(solve_fvm_trace_map PROPERTIES
        TIMEOUT 900 LABELS "fvm;numerical")
endif()

# Registration acceptance runs even when the optional solve inputs are absent.
add_test(NAME solve_ecad_test_registration
    COMMAND cmake
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCONFIG=$<CONFIG>"
        "-DECAD=$<BOOL:${CHIPCAE_ENABLE_ECAD}>"
        "-DSOLVE_BACKEND=$<BOOL:${_solve_backend}>"
        "-DHAS_IMPORT=$<TARGET_EXISTS:chipcae-import>"
        "-DHAS_PREVIEW=$<TARGET_EXISTS:chipcae-preview>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_ecad_test_registration.cmake")
set_tests_properties(solve_ecad_test_registration PROPERTIES
    TIMEOUT 60 LABELS "solve;configuration;unit;owner:M3")
