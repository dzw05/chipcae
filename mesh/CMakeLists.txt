add_library(chipcae_mesh
    src/cut_cell_mesh.cpp
    src/layered_hex_mesh.cpp
)
add_library(chipcae::mesh ALIAS chipcae_mesh)

target_include_directories(chipcae_mesh
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_mesh PUBLIC chipcae::core chipcae::geometry)
chipcae_set_warnings(chipcae_mesh)
add_subdirectory(background)
