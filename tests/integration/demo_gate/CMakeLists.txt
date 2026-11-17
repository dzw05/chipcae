chipcae_auto_tests(PREFIX demogate_ LABELS "cut;numerical" TIMEOUT 1800)
if(TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    set_property(TEST demogate_solve APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
endif()
