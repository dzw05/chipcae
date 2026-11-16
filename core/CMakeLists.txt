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

# Serial METIS is searched only when explicitly requested.
if(CHIPCAE_ENABLE_METIS)
    find_path(CHIPCAE_METIS_INCLUDE_DIR metis.h HINTS ${METIS_ROOT} ENV METIS_ROOT PATH_SUFFIXES include)
    find_library(CHIPCAE_METIS_LIBRARY metis HINTS ${METIS_ROOT} ENV METIS_ROOT PATH_SUFFIXES lib lib64)
    if(NOT CHIPCAE_METIS_INCLUDE_DIR OR NOT CHIPCAE_METIS_LIBRARY)
        message(FATAL_ERROR "CHIPCAE_ENABLE_METIS requires METIS; set METIS_ROOT to its installation prefix")
    endif()
    target_include_directories(chipcae_core PRIVATE ${CHIPCAE_METIS_INCLUDE_DIR})
    target_link_libraries(chipcae_core PRIVATE ${CHIPCAE_METIS_LIBRARY})
    target_compile_definitions(chipcae_core PRIVATE CHIPCAE_HAVE_METIS=1)
endif()
target_sources(chipcae_core PRIVATE
    src/par/halo_plan.cpp
    src/par/column_extrude_partitioner.cpp
    src/par/gpu_aware.cpp)
