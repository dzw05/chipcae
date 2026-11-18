set(assembly_test_libs chipcae::geometry chipcae_mesh_cut)
if(CHIPCAE_ENABLE_JSONCONS)
    list(APPEND assembly_test_libs yaml-cpp::yaml-cpp)
endif()
chipcae_auto_tests(LIBS ${assembly_test_libs} LABELS tierA TIMEOUT 60)

# Match the production writer's configured JSON support in its test translation units.
if(CHIPCAE_ENABLE_JSONCONS)
    foreach(test stack_writer_outline_polyset stack_writer_roundtrip step_defect_mapping import_assembly_error_mapping assembly_placement_policy)
        target_compile_definitions(chipcae_autotest_tests_integration_import_assembly_${test}
            PRIVATE CHIPCAE_ENABLE_JSONCONS=1)
        target_link_libraries(chipcae_autotest_tests_integration_import_assembly_${test}
            PRIVATE jsoncons)
    endforeach()
endif()
