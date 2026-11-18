option(CHIPCAE_F1130_RUN_BLOCKED_ACCEPTANCE
    "Run strict F1130 diagnostics despite unresolved fixture contracts (see local README)" OFF)
chipcae_auto_tests(PREFIX f1130_ LABELS "integration;numerical;vv" TIMEOUT 1200)
set_property(TEST f1130_uncut_cli_e2e APPEND PROPERTY ENVIRONMENT
    "CHIPCAE_F1130_RUN_BLOCKED_ACCEPTANCE=$<BOOL:${CHIPCAE_F1130_RUN_BLOCKED_ACCEPTANCE}>")
if(NOT CHIPCAE_F1130_RUN_BLOCKED_ACCEPTANCE)
    message(STATUS "F1130 E2E acceptance GATED: fixture contracts unresolved; see tests/integration/f1130_uncut_cli/README.md")
endif()
if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import AND TARGET chipcae-preview AND TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    set_property(TEST f1130_uncut_cli_e2e APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>"
        "CHIPCAE_PREVIEW_EXE=$<TARGET_FILE:chipcae-preview>"
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
endif()

# Keep fast verdict regressions independently runnable, including without backends.
add_test(NAME f1130_uncut_cli_records
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
        -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_uncut_cli_e2e.py -v)
set_tests_properties(f1130_uncut_cli_records PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "integration;vv"
    TIMEOUT 1200)
