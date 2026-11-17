# Input validation also runs in builds without the optional numerical backend.
if(TARGET chipcae_solve_inputs)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_solve_backend 0)
    if(CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
        set(_solve_backend 1)
    endif()
    add_test(NAME solve_inputs COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_inputs.py $<TARGET_FILE:chipcae-solve> ${PROJECT_SOURCE_DIR} ${_solve_backend})
    add_executable(chipcae_test_solve_mesh test_mesh.cpp)
    target_link_libraries(chipcae_test_solve_mesh PRIVATE chipcae_solve_inputs)
    chipcae_set_warnings(chipcae_test_solve_mesh)
    add_test(NAME solve_mesh COMMAND chipcae_test_solve_mesh)
endif()
if(_solve_backend)
    add_test(NAME solve_pipeline COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_pipeline.py $<TARGET_FILE:chipcae-solve> ${MPIEXEC_EXECUTABLE})
    set_tests_properties(solve_pipeline PROPERTIES TIMEOUT 600)
endif()

# Preview PR #43 has not landed: keep the cross-tool contract visible in CTest.
if(_solve_backend)
    add_test(NAME solve_preview_mesh_digest COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_preview_digest.py)
    set_tests_properties(solve_preview_mesh_digest PROPERTIES SKIP_RETURN_CODE 77)
endif()
