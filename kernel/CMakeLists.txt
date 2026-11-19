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

# Owners may advertise support on their linked targets. Explicit cache options
# also allow integration builds to enable newly merged implementations.
function(chipcae_assembly_capability variable target property)
    set(available OFF)
    if(TARGET "${target}")
        get_target_property(declared "${target}" "${property}")
        if(declared)
            set(available ON)
        endif()
    endif()
    if(available)
        if(DEFINED ${variable} AND NOT ${variable})
            message(WARNING "${variable}: cached OFF conflicts with ${property}; enabling support")
        endif()
        set(${variable} ON CACHE BOOL "Enable ${property} for assembly imports" FORCE)
    else()
        option(${variable} "Enable ${property} for assembly imports" OFF)
    endif()
    set(${variable} "${${variable}}" PARENT_SCOPE)
endfunction()
chipcae_assembly_capability(CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE
    chipcae::geometry_ecad CHIPCAE_ODBPP_AVAILABLE)
if(CHIPCAE_ENABLE_ECAD AND CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE)
    target_compile_definitions(chipcae-import PRIVATE CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE=1)
endif()
chipcae_assembly_capability(CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE
    chipcae::geometry CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/import-assets/assembly-capabilities.json"
    CONTENT "{\"geometry\": \"${CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE}\"}\n")

if(CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE)
    target_compile_definitions(chipcae-import PRIVATE CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE=1)
endif()
if(CHIPCAE_BUILD_TESTS AND CHIPCAE_ENABLE_OCCT)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tests/integration/import_assembly/fixtures"
        "${PROJECT_BINARY_DIR}/sp4-step-fixtures")
endif()
