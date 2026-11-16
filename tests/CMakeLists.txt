add_executable(chipcae_test_core unit/test_core.cpp)
target_link_libraries(chipcae_test_core PRIVATE chipcae::core chipcae::coupling chipcae::io chipcae::jobmgr)
chipcae_set_warnings(chipcae_test_core)
add_test(NAME core COMMAND chipcae_test_core)

add_executable(chipcae_test_mesh unit/test_mesh.cpp)
target_link_libraries(chipcae_test_mesh PRIVATE chipcae::mesh chipcae::geometry chipcae::fvm chipcae::fem
                                                chipcae::particles)
chipcae_set_warnings(chipcae_test_mesh)
add_test(NAME mesh COMMAND chipcae_test_mesh)
