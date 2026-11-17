cmake_minimum_required(VERSION 3.20)

project(chipcae
    VERSION 0.1.0
    DESCRIPTION "Multiphysics CAE for semiconductor packaging and electronics (芯仿)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ChipcaeOptions)
include(ChipcaeLicensePolicy)
include(ChipcaeDependencies)
include(ChipcaeWarnings)

if(CHIPCAE_BUILD_TESTS)
    # Enable CTest before add_subdirectory(material) so K3 tests are registered
    # in the top-level dashboard (enable_testing after a subdir would drop them).
    enable_testing()
endif()

add_subdirectory(core)
add_subdirectory(geometry)
if(CHIPCAE_ENABLE_ECAD)
    add_subdirectory(geometry/ecad)
endif()
add_subdirectory(mesh)
add_subdirectory(solvers/fvm)
add_subdirectory(solvers/fem)
add_subdirectory(solvers/particles)
add_subdirectory(coupling)
add_subdirectory(io)
add_subdirectory(protocol)
add_subdirectory(jobmgr)
add_subdirectory(preview)
add_subdirectory(kernel)

# Material point library (K3). Self-gated: no-op when Kokkos is unavailable and
# CHIPCAE_MATERIAL_FETCH_KOKKOS is OFF, so a bare `cmake -B build` still succeeds.
add_subdirectory(material)

if(CHIPCAE_BUILD_GUI)
    add_subdirectory(gui)
endif()

if(CHIPCAE_ENABLE_PYTHON)
    add_subdirectory(python)
endif()

if(CHIPCAE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
