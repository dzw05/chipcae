if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import)
    chipcae_auto_tests(LIBS chipcae::geometry_ecad LABELS tierA TIMEOUT 60 PREFIX import_)
    if(TARGET chipcae_stack)
        target_link_libraries(chipcae_autotest_tests_unit_test_import_soldermask_import_soldermask_layer
            PRIVATE chipcae_stack)
        target_compile_definitions(chipcae_autotest_tests_unit_test_import_soldermask_import_soldermask_layer
            PRIVATE CHIPCAE_SOLDERMASK_STACK_VALIDATION=1)
    endif()
    set_property(TEST import_soldermask_cli APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>")
endif()
