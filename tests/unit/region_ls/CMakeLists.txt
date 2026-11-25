chipcae_auto_tests(LIBS chipcae::geometry chipcae_mesh_cut LABELS tierA TIMEOUT 60)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(cut_exact_oracle_dump cut_exact_oracle_dump.cpp)
target_link_libraries(cut_exact_oracle_dump PRIVATE chipcae::geometry chipcae_mesh_cut)
chipcae_set_warnings(cut_exact_oracle_dump)
if(CHIPCAE_ENABLE_MFEM)
  target_link_libraries(cut_exact_oracle_dump PRIVATE chipcae_mesh_mfem)
  target_compile_definitions(cut_exact_oracle_dump PRIVATE CHIPCAE_ENABLE_MFEM=1)
endif()
add_executable(cut_union_dump_actual
  ${PROJECT_SOURCE_DIR}/tests/contract/fixtures/cut-union-1.0/dump_actual.cpp)
target_include_directories(cut_union_dump_actual PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cut_union_dump_actual PRIVATE chipcae::geometry chipcae_mesh_cut)
chipcae_set_warnings(cut_union_dump_actual)
set(_oracle_groups levels variants aniso)
if(CHIPCAE_ENABLE_MFEM)
  list(APPEND _oracle_groups fem)
endif()
foreach(_group IN LISTS _oracle_groups)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cut_exact_oracle/${_group})
  add_test(NAME mesh_cut_exact_oracle_${_group} COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/cut_exact_oracle_compare.py
    --dump $<TARGET_FILE:cut_exact_oracle_dump> --group ${_group}
    --workdir ${CMAKE_CURRENT_BINARY_DIR}/cut_exact_oracle/${_group})
  set_tests_properties(mesh_cut_exact_oracle_${_group} PROPERTIES
    LABELS "mesh;tierA;oracle" TIMEOUT 300 SKIP_RETURN_CODE 77
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endforeach()
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cut_exact_oracle/union)
add_test(NAME mesh_cut_union_actual COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/cut_union_actual.py --dump $<TARGET_FILE:cut_union_dump_actual>
  --checker ${PROJECT_SOURCE_DIR}/tests/contract/fixtures/cut-union-1.0/check.py
  --out ${CMAKE_CURRENT_BINARY_DIR}/cut_exact_oracle/union/actual.json)
set_tests_properties(mesh_cut_union_actual cut_exact_oracle_selftest PROPERTIES
  LABELS "mesh;tierA;oracle" TIMEOUT 300 SKIP_RETURN_CODE 77)
set_property(TEST mesh_cut_union_actual cut_exact_oracle_selftest APPEND PROPERTY
  ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
