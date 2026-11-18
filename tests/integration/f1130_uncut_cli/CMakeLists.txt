# The full chain is a manual diagnostic until the contracts linked in README.md
# are resolved. Do not auto-collect test_uncut_cli_e2e.py as acceptance coverage.
# Keep its assertion/verdict regression cases in CTest independently of main().
if(NOT Python3_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
add_test(NAME f1130_uncut_cli_records
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
        -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_uncut_cli_e2e.py -v)
set_tests_properties(f1130_uncut_cli_records PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "integration;vv"
    TIMEOUT 1200
    ENVIRONMENT "CHIPCAE_SOURCE_DIR=${PROJECT_SOURCE_DIR};CHIPCAE_BINARY_DIR=${PROJECT_BINARY_DIR}")
