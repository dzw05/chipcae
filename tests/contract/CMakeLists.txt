find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME contract_output_desktop COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_outputs.py desktop ${PROJECT_SOURCE_DIR})
set(_contract_solver disabled)
if(CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    set(_contract_solver $<TARGET_FILE:chipcae-solve>)
endif()
add_test(NAME contract_output_solver COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_outputs.py solver ${PROJECT_SOURCE_DIR} ${_contract_solver})
set_tests_properties(contract_output_desktop contract_output_solver PROPERTIES
    LABELS "contract" SKIP_RETURN_CODE 77 TIMEOUT 600)
if(CHIPCAE_ENABLE_ECAD AND CHIPCAE_ENABLE_JSONCONS)
    add_executable(chipcae_contract_ecad_stack ecad_stack.cpp)
    target_link_libraries(chipcae_contract_ecad_stack PRIVATE chipcae::geometry_ecad chipcae_stack chipcae_mesh_background)
    chipcae_set_warnings(chipcae_contract_ecad_stack)
    add_test(NAME contract_ecad_stack COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_ecad_stack.py ${PROJECT_SOURCE_DIR} $<TARGET_FILE:chipcae_contract_ecad_stack>)
    set_tests_properties(contract_ecad_stack PROPERTIES LABELS "contract" TIMEOUT 120)
endif()

add_test(NAME contract_assembly_schema COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/validate_assembly.py ${PROJECT_SOURCE_DIR})
set_tests_properties(contract_assembly_schema PROPERTIES LABELS "contract" SKIP_RETURN_CODE 77)

if(CHIPCAE_ENABLE_JSONCONS)
    add_executable(chipcae_contract_stack_relpath stack_relpath.cpp)
    target_link_libraries(chipcae_contract_stack_relpath PRIVATE chipcae_stack)
    chipcae_set_warnings(chipcae_contract_stack_relpath)
    add_test(NAME contract_stack_relpath COMMAND chipcae_contract_stack_relpath ${PROJECT_SOURCE_DIR})
    set_tests_properties(contract_stack_relpath PROPERTIES LABELS "contract" TIMEOUT 60)
endif()
