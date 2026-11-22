find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME ctest_tier_labels
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_tier_labels.py"
        "${PROJECT_BINARY_DIR}")
set_tests_properties(ctest_tier_labels PROPERTIES LABELS "unit;owner:INFRA" TIMEOUT 60)
