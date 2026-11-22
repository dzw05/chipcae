chipcae_auto_tests(LIBS chipcae::geometry chipcae_mesh_cut LABELS tierA TIMEOUT 60)

# SP0: each public header must compile independently of the others.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/part_headers)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/part_headers/part_slice.cpp
    "#include <chipcae/geometry/part_slice.hpp>\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/part_headers/assembly.cpp
    "#include <chipcae/geometry/assembly.hpp>\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/part_headers/region_level_set.cpp
    "#include <chipcae/mesh/cut/region_level_set.hpp>\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/part_headers/main.cpp "int main(){return 0;}\n")
add_executable(chipcae_test_part_headers_compile
    ${CMAKE_CURRENT_BINARY_DIR}/part_headers/part_slice.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/part_headers/assembly.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/part_headers/region_level_set.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/part_headers/main.cpp)
target_link_libraries(chipcae_test_part_headers_compile PRIVATE chipcae::geometry chipcae_mesh_cut)
add_test(NAME part_headers_compile COMMAND chipcae_test_part_headers_compile)
add_test(NAME part_headers_no_mpi_geometry COMMAND cmake
    -DHEADER_DIR=${PROJECT_SOURCE_DIR}/geometry/include/chipcae/geometry
    -P ${PROJECT_SOURCE_DIR}/tests/core/check_public_headers.cmake)
add_test(NAME part_headers_no_mpi_cut COMMAND cmake
    -DHEADER_DIR=${PROJECT_SOURCE_DIR}/mesh/cut/include/chipcae/mesh/cut
    -P ${PROJECT_SOURCE_DIR}/tests/core/check_public_headers.cmake)

# The CLI currently gates --stack-override; exercise its numeric reader directly.
target_sources(chipcae_autotest_tests_unit_assembly_stack_override_numbers
    PRIVATE ${PROJECT_SOURCE_DIR}/kernel/src/stack_override.cpp)
target_include_directories(chipcae_autotest_tests_unit_assembly_stack_override_numbers
    PRIVATE ${PROJECT_SOURCE_DIR}/kernel/src)
