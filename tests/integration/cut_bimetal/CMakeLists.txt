chipcae_auto_tests(PREFIX cutbim_ LABELS "integration;numerical;vv" TIMEOUT 2400)

if(TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    set_property(TEST cutbim_cut_bimetal APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
    set_tests_properties(cutbim_cut_bimetal PROPERTIES
        RESOURCE_LOCK chipcae_highmem LABELS "integration;numerical;vv;highmem")
endif()
