add_library(chipcae_fvm
    src/conjugate_heat_transfer.cpp
)
add_library(chipcae::fvm ALIAS chipcae_fvm)

target_include_directories(chipcae_fvm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_fvm PUBLIC chipcae::core chipcae::mesh)
chipcae_set_warnings(chipcae_fvm)

if(CHIPCAE_ENABLE_MFEM AND MFEM_USE_PETSC)
    # PETSc headers and link dependencies are supplied by the MFEM target.
    target_sources(chipcae_fvm PRIVATE src/grid.cpp src/thermal.cpp src/mfd_local.cpp)
    target_link_libraries(chipcae_fvm PUBLIC mfem chipcae_mesh_cut)
    target_include_directories(chipcae_fvm PUBLIC ${PROJECT_SOURCE_DIR}/material/include)
    target_compile_definitions(chipcae_fvm PUBLIC CHIPCAE_FVM_THERMAL=1)

    if(CHIPCAE_BUILD_TESTS)
        foreach(_case thermal thermal_cut)
            set(_target chipcae_test_fvm_${_case})
            add_executable(${_target} tests/test_fvm_${_case}.cpp)
            target_include_directories(${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(${_target} PRIVATE chipcae::fvm)
            chipcae_set_warnings(${_target})
        endforeach()
        # Manual pressure scan: buildable, but deliberately outside CTest/CI.
        add_executable(chipcae_fvm_thermal_stress tests/test_fvm_thermal_stress.cpp)
        target_include_directories(chipcae_fvm_thermal_stress
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(chipcae_fvm_thermal_stress PRIVATE chipcae::fvm)
        chipcae_set_warnings(chipcae_fvm_thermal_stress)
        add_test(NAME solve_fvm_thermal COMMAND chipcae_test_fvm_thermal)
        add_test(NAME solve_fvm_thermal_cut COMMAND chipcae_test_fvm_thermal_cut)
        set_tests_properties(solve_fvm_thermal solve_fvm_thermal_cut PROPERTIES
            LABELS "fvm;numerical" TIMEOUT 600)
        if(CHIPCAE_ENABLE_MPI)
            add_test(NAME solve_fvm_thermal_np2
                COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS}
                    $<TARGET_FILE:chipcae_test_fvm_thermal> --mpi-only ${MPIEXEC_POSTFLAGS})
            set_tests_properties(solve_fvm_thermal_np2 PROPERTIES
                LABELS "fvm;numerical" TIMEOUT 600 PROCESSORS 2)
        endif()
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/elastic/CMakeLists.txt)
    add_subdirectory(elastic)
endif()
