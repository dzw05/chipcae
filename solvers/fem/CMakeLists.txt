add_library(chipcae_fem
    src/thermal_stress.cpp
)
add_library(chipcae::fem ALIAS chipcae_fem)

target_include_directories(chipcae_fem
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_fem PUBLIC chipcae::core chipcae::mesh)
chipcae_set_warnings(chipcae_fem)

if(CHIPCAE_ENABLE_MFEM)
    target_sources(chipcae_fem PRIVATE src/cut_fem_assembly.cpp src/ghost_penalty.cpp)
    target_link_libraries(chipcae_fem PUBLIC mfem chipcae_mesh_cut)
    target_compile_definitions(chipcae_fem PUBLIC CHIPCAE_ENABLE_MFEM=1)
endif()

if(CHIPCAE_ENABLE_MFEM AND MFEM_USE_PETSC)
    target_sources(chipcae_fem PRIVATE src/column_gmg.cpp)
    target_compile_definitions(chipcae_fem PUBLIC CHIPCAE_FEM_COLUMN_GMG=1)
    target_sources(chipcae_fem PRIVATE src/batched_dense_lu.cpp)
    find_package(OpenMP COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(chipcae_fem PRIVATE OpenMP::OpenMP_CXX)
    endif()

    if(CHIPCAE_BUILD_TESTS)
        add_executable(chipcae_test_gmg_batched_lu tests/batched_dense_lu.cpp)
        target_include_directories(chipcae_test_gmg_batched_lu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(chipcae_test_gmg_batched_lu PRIVATE chipcae::fem)
        if(OpenMP_CXX_FOUND)
            target_link_libraries(chipcae_test_gmg_batched_lu PRIVATE OpenMP::OpenMP_CXX)
            target_compile_definitions(chipcae_test_gmg_batched_lu PRIVATE CHIPCAE_TEST_EXPECT_OPENMP=1)
        endif()
        chipcae_set_warnings(chipcae_test_gmg_batched_lu)
        add_test(NAME solve_gmg_batched_lu COMMAND chipcae_test_gmg_batched_lu)
        set_tests_properties(solve_gmg_batched_lu PROPERTIES
            TIMEOUT 120 LABELS "gmg;numerical" ENVIRONMENT "OPENBLAS_NUM_THREADS=1")
    endif()
endif()
