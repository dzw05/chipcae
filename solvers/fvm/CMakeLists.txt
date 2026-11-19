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
