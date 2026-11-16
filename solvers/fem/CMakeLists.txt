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
    target_link_libraries(chipcae_fem PUBLIC mfem)
    target_compile_definitions(chipcae_fem PUBLIC CHIPCAE_ENABLE_MFEM=1)
endif()
