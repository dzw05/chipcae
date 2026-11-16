add_library(chipcae_particles
    src/particle_solver.cpp
)
add_library(chipcae::particles ALIAS chipcae_particles)

target_include_directories(chipcae_particles
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_particles PUBLIC chipcae::core)
chipcae_set_warnings(chipcae_particles)
