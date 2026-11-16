add_library(chipcae_core
    src/log.cpp
    src/field.cpp
    src/parameter.cpp
    src/parallel.cpp
    src/solver.cpp
)
add_library(chipcae::core ALIAS chipcae_core)

target_include_directories(chipcae_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

chipcae_set_warnings(chipcae_core)

if(CHIPCAE_ENABLE_MPI)
    target_link_libraries(chipcae_core PUBLIC MPI::MPI_CXX)
    target_compile_definitions(chipcae_core PUBLIC CHIPCAE_ENABLE_MPI=1)
endif()

if(CHIPCAE_ENABLE_KOKKOS)
    target_link_libraries(chipcae_core PUBLIC Kokkos::kokkos)
    target_compile_definitions(chipcae_core PUBLIC CHIPCAE_ENABLE_KOKKOS=1)
endif()
