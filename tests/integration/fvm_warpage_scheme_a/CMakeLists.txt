# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME vv_fvm_warpage_scheme_a_unit
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_fvm_warpage_scheme_a.py)
set_tests_properties(vv_fvm_warpage_scheme_a_unit PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS vv
    TIMEOUT 120)
