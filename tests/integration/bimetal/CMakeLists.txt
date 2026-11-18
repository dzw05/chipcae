if(TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    chipcae_auto_tests(PREFIX bimetal_ LABELS "numerical;vv" TIMEOUT 1800)
    set_property(TEST bimetal_timoshenko APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
endif()
