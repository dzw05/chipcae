if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import)
    chipcae_auto_tests(LIBS chipcae::geometry_ecad LABELS tierA TIMEOUT 60 PREFIX import_)
    set_property(TEST import_metadata_tier_residual_cli APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>")
endif()
