if(TARGET chipcae_geometry_ecad)
    chipcae_auto_tests(LIBS chipcae::geometry_ecad LABELS tierA TIMEOUT 60 PREFIX import_)
    target_compile_definitions(chipcae_autotest_tests_unit_test_import_board_defects_import_board_defects
        PRIVATE CHIPCAE_ENABLE_ECAD=1)
endif()
