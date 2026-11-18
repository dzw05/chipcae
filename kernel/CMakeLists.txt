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

# Derive capabilities from the linked implementation sources, not a manual
# cache switch. CONFIGURE_DEPENDS reruns this check when a stub is replaced.
function(chipcae_assembly_capability variable)
    set(available ON)
    foreach(source IN LISTS ARGN)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/${source}")
        file(READ "${PROJECT_SOURCE_DIR}/${source}" implementation)
        if(implementation MATCHES "not implemented")
            set(available OFF)
        endif()
    endforeach()
    set(${variable} ${available} CACHE BOOL "Detected assembly implementation capability" FORCE)
endfunction()
chipcae_assembly_capability(CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE
    geometry/ecad/src/odbpp/read_odbpp.cpp
    geometry/ecad/src/odbpp/archive.cpp
    geometry/ecad/src/odbpp/parse_matrix.cpp
    geometry/ecad/src/odbpp/parse_misc.cpp
    geometry/ecad/src/odbpp/parse_features.cpp
    geometry/ecad/src/odbpp/map_geometry.cpp
    geometry/ecad/src/odbpp/map_stackup.cpp)
if(CHIPCAE_ENABLE_ECAD AND CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE)
    target_compile_definitions(chipcae-import PRIVATE CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE=1)
endif()

# Prefer an existing dependency, including packages exporting the legacy target.
if(NOT TARGET yaml-cpp::yaml-cpp AND NOT TARGET yaml-cpp)
    find_package(yaml-cpp QUIET)
endif()
if(TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()
# With JSON enabled, apps/chipcae-solve supplies the shared YAML dependency.
if(NOT CHIPCAE_ENABLE_JSONCONS AND NOT TARGET yaml-cpp::yaml-cpp)
    find_package(yaml-cpp REQUIRED)
endif()
target_link_libraries(chipcae-import PRIVATE yaml-cpp::yaml-cpp)

chipcae_assembly_capability(CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE
    geometry/src/part_slice.cpp geometry/src/assembly.cpp)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/import-assets/assembly-capabilities.json"
    CONTENT "{\"geometry\": \"${CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE}\"}\n")
