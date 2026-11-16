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

# Optional serial METIS; no distributed partitioning dependency.
find_path(CHIPCAE_METIS_INCLUDE_DIR metis.h HINTS /workspace/deps/metis/include /workspace/deps/include)
find_library(CHIPCAE_METIS_LIBRARY metis HINTS /workspace/deps/metis/lib /workspace/deps/lib)
if(CHIPCAE_METIS_INCLUDE_DIR AND CHIPCAE_METIS_LIBRARY)
    target_include_directories(chipcae_core PRIVATE ${CHIPCAE_METIS_INCLUDE_DIR})
    target_link_libraries(chipcae_core PRIVATE ${CHIPCAE_METIS_LIBRARY})
    target_compile_definitions(chipcae_core PRIVATE CHIPCAE_HAVE_METIS=1)
    message(STATUS "Column partitioner: METIS")
else()
    message(STATUS "Column partitioner: deterministic serpentine fallback (METIS unavailable)")
endif()
target_sources(chipcae_core PRIVATE
    src/par/halo_plan.cpp
    src/par/column_extrude_partitioner.cpp
    src/par/gpu_aware.cpp)
