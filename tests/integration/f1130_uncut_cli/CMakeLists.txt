chipcae_auto_tests(PREFIX f1130_ LABELS "integration;numerical;vv" TIMEOUT 1200)
if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-import AND TARGET chipcae-preview AND TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    set_property(TEST f1130_uncut_cli_e2e APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_IMPORT_EXE=$<TARGET_FILE:chipcae-import>"
        "CHIPCAE_PREVIEW_EXE=$<TARGET_FILE:chipcae-preview>"
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
endif()

# Keep fast verdict regressions independently runnable, including without backends.
if(NOT Python3_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
add_test(NAME f1130_uncut_cli_records
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
        -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_uncut_cli_e2e.py -v)
set_tests_properties(f1130_uncut_cli_records PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "integration;vv"
    TIMEOUT 1200)
