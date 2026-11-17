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
