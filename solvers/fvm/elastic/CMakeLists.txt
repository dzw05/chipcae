if(NOT (CHIPCAE_ENABLE_MFEM AND MFEM_USE_PETSC))
    return()
endif()

add_library(chipcae_fvm_elastic src/elastic.cpp)
add_library(chipcae::fvm_elastic ALIAS chipcae_fvm_elastic)
target_include_directories(chipcae_fvm_elastic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/schema/include)
target_link_libraries(chipcae_fvm_elastic PUBLIC mfem chipcae::core)
chipcae_set_warnings(chipcae_fvm_elastic)

if(CHIPCAE_BUILD_TESTS)
    foreach(kind elastic bimetal)
        add_executable(chipcae_test_fvm_${kind} tests/test_fvm_${kind}.cpp)
        target_include_directories(chipcae_test_fvm_${kind} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/material/include)
        target_link_libraries(chipcae_test_fvm_${kind} PRIVATE chipcae::fvm_elastic)
        chipcae_set_warnings(chipcae_test_fvm_${kind})
        add_test(NAME solve_fvm_${kind} COMMAND chipcae_test_fvm_${kind})
        set_tests_properties(solve_fvm_${kind} PROPERTIES LABELS "fvm;numerical" TIMEOUT 900)
    endforeach()
    option(CHIPCAE_FVM_ELASTIC_BENCHMARK "Register the synthetic elastic performance acceptance" OFF)
    if(CHIPCAE_FVM_ELASTIC_BENCHMARK)
        add_test(NAME solve_fvm_elastic_benchmark COMMAND chipcae_test_fvm_elastic -elastic_test_mode benchmark)
        set_tests_properties(solve_fvm_elastic_benchmark PROPERTIES
            LABELS "fvm;numerical;performance" TIMEOUT 900)
    endif()
    if(CHIPCAE_ENABLE_MPI)
        add_test(NAME solve_fvm_elastic_np2
            COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS}
                    $<TARGET_FILE:chipcae_test_fvm_elastic> ${MPIEXEC_POSTFLAGS} -elastic_test_mode mpi)
        set_tests_properties(solve_fvm_elastic_np2 PROPERTIES LABELS "fvm;numerical" TIMEOUT 900)
    endif()
endif()
