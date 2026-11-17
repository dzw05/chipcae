add_executable(chipcae_test_core unit/test_core.cpp)
target_link_libraries(chipcae_test_core PRIVATE chipcae::core chipcae::coupling chipcae::io chipcae::jobmgr)
chipcae_set_warnings(chipcae_test_core)
add_test(NAME core COMMAND chipcae_test_core)

add_executable(chipcae_test_mesh unit/test_mesh.cpp)
target_link_libraries(chipcae_test_mesh PRIVATE chipcae::mesh chipcae::geometry chipcae::fvm chipcae::fem
                                                chipcae::particles chipcae_mesh_background)
chipcae_set_warnings(chipcae_test_mesh)
add_test(NAME mesh COMMAND chipcae_test_mesh)

add_executable(chipcae_test_core_types core/test_core_types.cpp)
target_link_libraries(chipcae_test_core_types PRIVATE chipcae::core)
chipcae_set_warnings(chipcae_test_core_types)
add_test(NAME core_types COMMAND chipcae_test_core_types)
add_executable(chipcae_test_column_hierarchy unit/test_column_hierarchy.cpp)
target_link_libraries(chipcae_test_column_hierarchy PRIVATE chipcae_mesh_background)
chipcae_set_warnings(chipcae_test_column_hierarchy)
add_test(NAME column_hierarchy COMMAND chipcae_test_column_hierarchy)
set_tests_properties(core_types PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
add_test(NAME core_public_headers
    COMMAND ${CMAKE_COMMAND}
        -DHEADER_DIR=${PROJECT_SOURCE_DIR}/core/include/chipcae/core
        -P ${CMAKE_CURRENT_SOURCE_DIR}/core/check_public_headers.cmake)

add_executable(chipcae_test_event_log protocol/test_event_log.cpp)
target_link_libraries(chipcae_test_event_log PRIVATE chipcae::io)
chipcae_set_warnings(chipcae_test_event_log)
add_test(NAME event_log COMMAND chipcae_test_event_log)

add_executable(chipcae_test_par par/test_par.cpp)
target_link_libraries(chipcae_test_par PRIVATE chipcae::core chipcae::coupling)
chipcae_set_warnings(chipcae_test_par)
if(CHIPCAE_ENABLE_METIS)
    target_compile_definitions(chipcae_test_par PRIVATE CHIPCAE_ENABLE_METIS=1)
endif()
add_test(NAME par COMMAND chipcae_test_par)
set_tests_properties(par PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
add_test(NAME par_public_headers COMMAND ${CMAKE_COMMAND}
    -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/par/check_headers.cmake)
if(CHIPCAE_ENABLE_MPI)
    add_executable(chipcae_test_par_mpi par/test_par_mpi.cpp)
    target_link_libraries(chipcae_test_par_mpi PRIVATE chipcae::core)
    chipcae_set_warnings(chipcae_test_par_mpi)
    if(NOT MPIEXEC_EXECUTABLE)
        message(FATAL_ERROR "CHIPCAE_ENABLE_MPI tests require MPIEXEC_EXECUTABLE")
    endif()
    add_test(NAME par_mpi_2 COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
                                    ${MPIEXEC_PREFLAGS} $<TARGET_FILE:chipcae_test_par_mpi>
                                    ${MPIEXEC_POSTFLAGS})
    add_test(NAME par_mpi_4 COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 4
                                    ${MPIEXEC_PREFLAGS} $<TARGET_FILE:chipcae_test_par_mpi>
                                    ${MPIEXEC_POSTFLAGS})
    set_tests_properties(par_mpi_2 par_mpi_4 PROPERTIES LABELS "mpi;par")
endif()
add_subdirectory(integration/mesh_p1)

if(CHIPCAE_ENABLE_ECAD)
    add_executable(chipcae_test_copper_map unit/test_copper_map.cpp)
    target_link_libraries(chipcae_test_copper_map PRIVATE chipcae_mesh_background)
    chipcae_set_warnings(chipcae_test_copper_map)
    add_test(NAME copper_map_analytic COMMAND chipcae_test_copper_map)
    set_tests_properties(copper_map_analytic PROPERTIES LABELS "mesh;tierA" TIMEOUT 60)
endif()

add_executable(chipcae_test_import_thresholds unit/test_import_thresholds.cpp)
target_link_libraries(chipcae_test_import_thresholds PRIVATE chipcae::geometry)
chipcae_set_warnings(chipcae_test_import_thresholds)
add_test(NAME import_thresholds COMMAND chipcae_test_import_thresholds)
set_tests_properties(import_thresholds PROPERTIES LABELS "mesh;tierA" TIMEOUT 60)

add_executable(chipcae_test_control_file protocol/test_control_file.cpp)
target_link_libraries(chipcae_test_control_file PRIVATE chipcae::io)
chipcae_set_warnings(chipcae_test_control_file)
add_test(NAME control_file COMMAND chipcae_test_control_file)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME case_run_schema_fixtures
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/schema/tests/validate_fixtures.py
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(case_run_schema_fixtures PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(chipcae_test_repair_policy unit/test_repair_policy.cpp)
target_link_libraries(chipcae_test_repair_policy PRIVATE chipcae::geometry)
chipcae_set_warnings(chipcae_test_repair_policy)
add_test(NAME repair_policy_tiers COMMAND chipcae_test_repair_policy)
set_tests_properties(repair_policy_tiers PROPERTIES LABELS "mesh;tierA" TIMEOUT 60)

if(CHIPCAE_ENABLE_ECAD)
    add_executable(chipcae_test_ipc2581 unit/test_ipc2581.cpp)
    target_link_libraries(chipcae_test_ipc2581 PRIVATE chipcae::geometry_ecad)
    target_compile_definitions(chipcae_test_ipc2581 PRIVATE CHIPCAE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    chipcae_set_warnings(chipcae_test_ipc2581)
    add_test(NAME ipc2581_stackup COMMAND chipcae_test_ipc2581)
    add_test(NAME ipc2581_copper_area COMMAND chipcae_test_ipc2581)
    add_test(NAME ipc2581_unsupported COMMAND chipcae_test_ipc2581)
    set_tests_properties(ipc2581_stackup ipc2581_copper_area ipc2581_unsupported
        PROPERTIES LABELS "tierA" TIMEOUT 60)

    add_executable(chipcae_test_material_map unit/test_material_map.cpp)
    target_link_libraries(chipcae_test_material_map PRIVATE chipcae::geometry_ecad)
    chipcae_set_warnings(chipcae_test_material_map)
    add_test(NAME ecad_material_missing COMMAND chipcae_test_material_map)
    set_tests_properties(ecad_material_missing PROPERTIES LABELS "tierA" TIMEOUT 60)

    add_executable(chipcae_test_ecad_e2e integration/ecad/e2e.cpp)
    target_link_libraries(chipcae_test_ecad_e2e PRIVATE chipcae::geometry_ecad)
    chipcae_set_warnings(chipcae_test_ecad_e2e)
    add_test(NAME ecad_e2e_fcbga COMMAND chipcae_test_ecad_e2e)
    set_tests_properties(ecad_e2e_fcbga PROPERTIES LABELS "nightly" TIMEOUT 300)
endif()
