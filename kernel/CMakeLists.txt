add_executable(chipcae-kernel src/main.cpp)
target_link_libraries(chipcae-kernel
    PRIVATE
        chipcae::core
        chipcae::geometry
        chipcae::mesh
        chipcae::fvm
        chipcae::fem
        chipcae::coupling
        chipcae::io
        chipcae::protocol
)
chipcae_set_warnings(chipcae-kernel)

if(CHIPCAE_ENABLE_PARTICLES)
    target_link_libraries(chipcae-kernel PRIVATE chipcae::particles)
endif()

# Inspect both direct and transitive links, with a visited set for cycles.
function(chipcae_assert_no_vtk target)
    get_property(_visited GLOBAL PROPERTY CHIPCAE_VTK_VISITED)
    if("${target}" IN_LIST _visited)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY CHIPCAE_VTK_VISITED "${target}")
    foreach(_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_links ${target} ${_property})
        foreach(_link IN LISTS _links)
            # Also recognize names inside common generator expressions.
            if(_link MATCHES "(^|[:;,>])VTK::" OR _link STREQUAL "VTK")
                message(FATAL_ERROR "Kernel must not link VTK: ${target} -> ${_link}")
            endif()
            string(REPLACE "::" "__CHIPCAE_NAMESPACE__" _expression "${_link}")
            string(REGEX MATCHALL "[A-Za-z0-9_][A-Za-z0-9_.+-]*" _dependencies "${_expression}")
            foreach(_dependency IN LISTS _dependencies)
                string(REPLACE "__CHIPCAE_NAMESPACE__" "::" _dependency "${_dependency}")
                if(_dependency STREQUAL "VTK" OR _dependency MATCHES "^VTK::")
                    message(FATAL_ERROR "Kernel must not link VTK: ${target} -> ${_dependency}")
                endif()
                if(TARGET "${_dependency}")
                    chipcae_assert_no_vtk("${_dependency}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()
set_property(GLOBAL PROPERTY CHIPCAE_VTK_VISITED "")
chipcae_assert_no_vtk(chipcae-kernel)

add_executable(chipcae-import
    src/import_main.cpp
    src/import_common.hpp
    src/import_common.cpp
    src/import_ecad.cpp
    src/import_assembly.cpp
    src/stack_writer.cpp
    src/import_step.cpp
    src/stack_override.hpp
    src/stack_override.cpp
)
target_link_libraries(chipcae-import PRIVATE chipcae::geometry)
if(CHIPCAE_ENABLE_ECAD)
    target_link_libraries(chipcae-import PRIVATE chipcae::geometry_ecad)
    target_compile_definitions(chipcae-import PRIVATE CHIPCAE_ENABLE_ECAD=1)
endif()
chipcae_set_warnings(chipcae-import)

target_compile_definitions(chipcae-import PRIVATE CHIPCAE_ASSEMBLY_CLI=1)
if(CHIPCAE_ENABLE_JSONCONS)
    target_compile_definitions(chipcae-import PRIVATE CHIPCAE_ENABLE_JSONCONS=1)
    target_link_libraries(chipcae-import PRIVATE chipcae_stack jsoncons)
endif()

# Runtime contracts travel with the build/install, independently of the checkout.
foreach(asset schema/assembly-0.1.schema.json schema/stack-0.2.schema.json protocol/errors.yaml)
    configure_file("${PROJECT_SOURCE_DIR}/${asset}"
        "${PROJECT_BINARY_DIR}/import-assets/${asset}" COPYONLY)
endforeach()
# Installed schemas/protocol are already packaged by ChipcaeInstall.cmake.

# E-ODB-3 initially supplied a stub. Enable the assembly reader automatically
# once its implementation replaces that placeholder.
if(NOT DEFINED CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE)
    set(_odbpp_default OFF)
    set(_odbpp_reader "${PROJECT_SOURCE_DIR}/geometry/ecad/src/odbpp/read_odbpp.cpp")
    if(CHIPCAE_ENABLE_ECAD AND EXISTS "${_odbpp_reader}")
        file(READ "${_odbpp_reader}" _odbpp_source)
        if(NOT _odbpp_source MATCHES "IMPORT_REJECTED: not implemented")
            set(_odbpp_default ON)
        endif()
    endif()
    set(CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE ${_odbpp_default} CACHE BOOL "Implemented assembly ODB++ reader available")
endif()
if(CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE)
    target_compile_definitions(chipcae-import PRIVATE CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE=1)
endif()

find_package(yaml-cpp QUIET)
if(NOT TARGET yaml-cpp::yaml-cpp)
    include(FetchContent)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(yaml-cpp URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
        URL_HASH SHA256=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16)
    # yaml-cpp 0.8.0 declares cmake_minimum_required < 3.5, rejected by CMake 4 on CI.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(yaml-cpp)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

target_link_libraries(chipcae-import PRIVATE yaml-cpp::yaml-cpp)
