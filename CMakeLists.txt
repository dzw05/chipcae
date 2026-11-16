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

add_subdirectory(core)
add_subdirectory(geometry)
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

if(CHIPCAE_BUILD_GUI)
    add_subdirectory(gui)
endif()

if(CHIPCAE_ENABLE_PYTHON)
    add_subdirectory(python)
endif()

if(CHIPCAE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
