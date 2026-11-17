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
