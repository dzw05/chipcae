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
endif()
