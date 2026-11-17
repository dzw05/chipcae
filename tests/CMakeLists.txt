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

if(CHIPCAE_ENABLE_HDF5)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME vtkhdf_results_serial COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_vtkhdf_results.py $<TARGET_FILE:chipcae_write_results>
        ${PROJECT_SOURCE_DIR}/tools/samples/s1.stack.json 1)
    set_tests_properties(vtkhdf_results_serial PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(CHIPCAE_ENABLE_MPI)
        add_test(NAME vtkhdf_results_mpi_2 COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_vtkhdf_results.py $<TARGET_FILE:chipcae_write_results>
            ${PROJECT_SOURCE_DIR}/tools/samples/s1.stack.json 2 ${MPIEXEC_EXECUTABLE})
        set_tests_properties(vtkhdf_results_mpi_2 PROPERTIES LABELS "mpi;io" TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
endif()
add_executable(chipcae_test_repair_policy unit/test_repair_policy.cpp)
target_link_libraries(chipcae_test_repair_policy PRIVATE chipcae::geometry)
chipcae_set_warnings(chipcae_test_repair_policy)
add_test(NAME repair_policy_tiers COMMAND chipcae_test_repair_policy)
set_tests_properties(repair_policy_tiers PROPERTIES LABELS "mesh;tierA" TIMEOUT 60)

add_subdirectory(solve)
