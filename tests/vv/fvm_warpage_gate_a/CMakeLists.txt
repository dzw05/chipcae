# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
add_test(NAME vv_fvm_warpage_gate_a_strip
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_strip_scheme_a.py)
set_tests_properties(vv_fvm_warpage_gate_a_strip PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "integration;vv"
    TIMEOUT 1800
    SKIP_RETURN_CODE 77)
if(TARGET chipcae-solve)
    set_property(TEST vv_fvm_warpage_gate_a_strip APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
endif()
