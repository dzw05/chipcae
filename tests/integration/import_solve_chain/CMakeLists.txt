chipcae_auto_tests(PREFIX chain_ LABELS "integration;numerical" TIMEOUT 900)
if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import AND TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    set_property(TEST chain_ipc_uncut_chain APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>"
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
endif()
