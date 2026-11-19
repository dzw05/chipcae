set(assembly_test_libs chipcae::geometry chipcae_mesh_cut)
chipcae_auto_tests(LIBS ${assembly_test_libs} LABELS tierA TIMEOUT 60)

# Match the production writer's configured JSON support in its test translation units.
if(CHIPCAE_ENABLE_JSONCONS)
    foreach(test assembly_parts_summary stack_writer_empty_body stack_writer_outline_polyset stack_writer_roundtrip step_defect_mapping import_assembly_error_mapping assembly_placement_policy)
        target_compile_definitions(chipcae_autotest_tests_integration_import_assembly_${test}
            PRIVATE CHIPCAE_ENABLE_JSONCONS=1)
        target_link_libraries(chipcae_autotest_tests_integration_import_assembly_${test}
            PRIVATE jsoncons)
    endforeach()
endif()

if(CHIPCAE_ENABLE_JSONCONS AND CHIPCAE_ENABLE_ECAD)
    set(confirmation_test chipcae_autotest_tests_integration_import_assembly_assembly_confirmation_cli)
    target_compile_definitions(${confirmation_test} PRIVATE
        CHIPCAE_ASSEMBLY_CLI=1 CHIPCAE_ENABLE_JSONCONS=1 CHIPCAE_ENABLE_ECAD=1)
    target_link_libraries(${confirmation_test} PRIVATE chipcae::geometry_ecad chipcae_stack jsoncons)
else()
    # Temporary skip: needs CHIPCAE_ENABLE_ECAD+CHIPCAE_ENABLE_JSONCONS.
    # Follow-up infra-ci-ecad-jsoncons must land before SP7b.
    set_tests_properties(assembly_confirmation_cli PROPERTIES SKIP_RETURN_CODE 77)
endif()
