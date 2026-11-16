add_executable(chipcae-kernel src/main.cpp)
target_link_libraries(chipcae-kernel
    PRIVATE
        chipcae::core
        chipcae::geometry
        chipcae::mesh
        chipcae::fvm
        chipcae::fem
        chipcae::particles
        chipcae::coupling
        chipcae::io
        chipcae::protocol
)
chipcae_set_warnings(chipcae-kernel)
