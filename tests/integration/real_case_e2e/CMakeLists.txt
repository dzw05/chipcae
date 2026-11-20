chipcae_auto_tests(PREFIX realcase_ LABELS "integration;numerical;vv" TIMEOUT 1800)
set_tests_properties(realcase_real_case_unit PROPERTIES LABELS "integration;vv")

if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import AND TARGET chipcae-preview
   AND TARGET chipcae-solve AND CHIPCAE_ENABLE_HDF5)
    foreach(kind thermal warpage assembly)
        set_property(TEST realcase_real_case_${kind} APPEND PROPERTY ENVIRONMENT
            "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>"
            "CHIPCAE_PREVIEW_EXE=$<TARGET_FILE:chipcae-preview>"
            "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
    endforeach()
endif()
if(TARGET sp4_step_fixtures)
    set_property(TEST realcase_real_case_assembly APPEND PROPERTY ENVIRONMENT
        "SP4_FIXTURE_GENERATOR=$<TARGET_FILE:sp4_step_fixtures>")
endif()
