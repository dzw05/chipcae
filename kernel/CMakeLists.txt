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

# Implementation owners can declare capabilities on their linked targets with
# CHIPCAE_ODBPP_AVAILABLE / CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE. Until those
# implementations land, recognize only the frozen SP0 stubs. Unknown sources
# require an explicit owner declaration; they must never silently skip tests.
function(chipcae_assembly_capability variable target property)
    if(NOT TARGET "${target}")
        set(available OFF)
    else()
        get_target_property(available "${target}" "${property}")
        get_target_property(linked_sources "${target}" SOURCES)
        get_target_property(source_dir "${target}" SOURCE_DIR)
        set(linked_paths "")
        foreach(linked_source IN LISTS linked_sources)
            if(NOT linked_source MATCHES "^\\$<")
                get_filename_component(linked_path "${linked_source}" ABSOLUTE BASE_DIR "${source_dir}")
                list(APPEND linked_paths "${linked_path}")
            endif()
        endforeach()
        set(known_stubs TRUE)
        set(has_stub FALSE)
        foreach(entry IN LISTS ARGN)
            string(REPLACE "=" ";" fields "${entry}")
            list(GET fields 0 source)
            list(GET fields 1 expected)
            if(NOT "${PROJECT_SOURCE_DIR}/${source}" IN_LIST linked_paths)
                message(FATAL_ERROR "${target} does not link capability source ${source}")
            endif()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${PROJECT_SOURCE_DIR}/${source}")
            file(SHA256 "${PROJECT_SOURCE_DIR}/${source}" actual)
            if(NOT actual STREQUAL expected)
                set(known_stubs FALSE)
            else()
                set(has_stub TRUE)
            endif()
        endforeach()
        if(available STREQUAL "available-NOTFOUND")
            if(NOT known_stubs)
                message(FATAL_ERROR
                    "${target} must declare target property ${property}: implementation changed")
            endif()
            set(available OFF)
        elseif(available AND has_stub)
            message(FATAL_ERROR "${property} contradicts the linked SP0 stub implementation")
        endif()
    endif()
    if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "${available}")
        message(FATAL_ERROR "${variable} contradicts capability of linked target ${target}")
    endif()
    # Normal scope value: no forced cache entry and no user override of detection.
    set(${variable} "${available}" PARENT_SCOPE)
endfunction()
chipcae_assembly_capability(CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE
    chipcae::geometry_ecad CHIPCAE_ODBPP_AVAILABLE
    geometry/ecad/src/odbpp/read_odbpp.cpp=6628d0a6da63c95d671516b8f363c81815d8ce2a3a833f2197025a644480e78c
    geometry/ecad/src/odbpp/archive.cpp=a6ff915908e71adc9c3856f120341a6a0a9a94515c54814aa5cd8a6905c6486d
    geometry/ecad/src/odbpp/parse_matrix.cpp=b2ce6c53c25112ada17ffaf53a634b92e9fcd46ca054c836ddae6685faa3d8ec
    geometry/ecad/src/odbpp/parse_misc.cpp=c1980e074585e92b72a1775fd426bb64af670b741ae81ddc1c52e19f6dc0d707
    geometry/ecad/src/odbpp/parse_features.cpp=56b70d3646b398a01033615a1b63fdf3825a108a57f02fd76f89510733d6d225
    geometry/ecad/src/odbpp/map_geometry.cpp=3c5c0333cd1313921e1bb73165a62539500724b9337aad4b56c32d6a1ebd9b33
    geometry/ecad/src/odbpp/map_stackup.cpp=e25128a8e1195ce2b2a45217a485bcfa4a18621e6aba73a82b8ff3c01cf7b8df)
if(CHIPCAE_ENABLE_ECAD AND CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE)
    target_compile_definitions(chipcae-import PRIVATE CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE=1)
endif()

if(CHIPCAE_ENABLE_JSONCONS)
    if(NOT TARGET yaml-cpp::yaml-cpp AND NOT TARGET yaml-cpp)
        find_package(yaml-cpp QUIET)
        if(NOT TARGET yaml-cpp::yaml-cpp AND NOT TARGET yaml-cpp)
            include(FetchContent)
            set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(yaml-cpp
                URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
                URL_HASH SHA256=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16)
            set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
            FetchContent_MakeAvailable(yaml-cpp)
            unset(CMAKE_POLICY_VERSION_MINIMUM)
        endif()
    endif()
    if(TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
        add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
    endif()
    target_link_libraries(chipcae-import PRIVATE yaml-cpp::yaml-cpp)
endif()

chipcae_assembly_capability(CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE
    chipcae::geometry CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE
    geometry/src/part_slice.cpp=272343039fc9cc88524823ba4186de1bab636ad8c7cadaf40830451400ef5249
    geometry/src/assembly.cpp=5be184d43be7a2a27b7ef0874a08fcec2de1c35d9e6c639e986223b621e38627)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/import-assets/assembly-capabilities.json"
    CONTENT "{\"geometry\": \"${CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE}\"}\n")
