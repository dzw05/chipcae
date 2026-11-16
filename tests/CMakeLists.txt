add_executable(chipcae_test_core unit/test_core.cpp)
target_link_libraries(chipcae_test_core PRIVATE chipcae::core chipcae::coupling chipcae::io chipcae::jobmgr)
chipcae_set_warnings(chipcae_test_core)
add_test(NAME core COMMAND chipcae_test_core)

add_executable(chipcae_test_mesh unit/test_mesh.cpp)
target_link_libraries(chipcae_test_mesh PRIVATE chipcae::mesh chipcae::geometry chipcae::fvm chipcae::fem
                                                chipcae::particles)
chipcae_set_warnings(chipcae_test_mesh)
add_test(NAME mesh COMMAND chipcae_test_mesh)

add_executable(chipcae_test_core_types core/test_core_types.cpp)
target_link_libraries(chipcae_test_core_types PRIVATE chipcae::core)
chipcae_set_warnings(chipcae_test_core_types)
add_test(NAME core_types COMMAND chipcae_test_core_types)
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
add_test(NAME par COMMAND chipcae_test_par)
set_tests_properties(par PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
add_test(NAME par_public_headers COMMAND ${CMAKE_COMMAND}
    -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/par/check_headers.cmake)
