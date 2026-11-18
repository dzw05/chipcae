chipcae_auto_tests(LABELS tierA PREFIX ecaddiff_ TIMEOUT 120)
if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import)
    set_property(TEST ecaddiff_import_pair APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>")
endif()
