# Front-end tests intentionally run without numerical solver dependencies.
if(TARGET chipcae_solve_inputs)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME solve_inputs COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_inputs.py $<TARGET_FILE:chipcae-solve> ${PROJECT_SOURCE_DIR})
    add_executable(chipcae_test_solve_mesh test_mesh.cpp)
    target_link_libraries(chipcae_test_solve_mesh PRIVATE chipcae_solve_inputs)
    chipcae_set_warnings(chipcae_test_solve_mesh)
    add_test(NAME solve_mesh COMMAND chipcae_test_solve_mesh)
endif()
