if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import)
    chipcae_auto_tests(LIBS chipcae::geometry_ecad LABELS tierA TIMEOUT 60 PREFIX import_)
    if(TARGET chipcae_stack)
        target_link_libraries(chipcae_autotest_tests_unit_test_import_soldermask_import_soldermask_layer
            PRIVATE chipcae_stack)
        target_compile_definitions(chipcae_autotest_tests_unit_test_import_soldermask_import_soldermask_layer
            PRIVATE CHIPCAE_SOLDERMASK_STACK_VALIDATION=1)
    endif()
    set(CHIPCAE_BASELINE_IMPORT_EXE "$ENV{CHIPCAE_BASELINE_IMPORT_EXE}" CACHE FILEPATH
        "Pre-change chipcae-import built from 9f2f1277ef191642719802f7e011177e63af72f6")
    set_property(TEST import_soldermask_cli APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>"
        "CHIPCAE_BASELINE_IMPORT_EXE=${CHIPCAE_BASELINE_IMPORT_EXE}")
endif()
