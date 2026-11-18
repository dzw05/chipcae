chipcae_auto_tests(PREFIX f1130_ LABELS "integration;numerical;vv" TIMEOUT 1200)
if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import AND TARGET chipcae-preview AND TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    set_property(TEST f1130_uncut_cli_e2e APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>"
        "CHIPCAE_PREVIEW_EXE=$<TARGET_FILE:chipcae-preview>"
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
endif()
