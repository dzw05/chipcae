# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
# tools.vv is the intended Python import surface for these V&V ci-ok tests
# (from tools.vv.warpage_qoi and tools.vv.derive_cards).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME warpage_qoi_analytic
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_warpage_qoi -v)
add_test(NAME trilayer_clt_magnitude
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_trilayer_clt -v)
set_tests_properties(warpage_qoi_analytic trilayer_clt_magnitude
    PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} TIMEOUT 30)

add_test(NAME vv_result_schema
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_result_schema -v)
set_tests_properties(vv_result_schema
    PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} TIMEOUT 60)
add_subdirectory(calculix_demo_gate)

add_test(NAME vv_f1130_rehearsal_unit
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_f1130_rehearsal -v)
set_tests_properties(vv_f1130_rehearsal_unit
    PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} TIMEOUT 60 LABELS "vv")

add_test(NAME vv_ctest_skip_audit_unit
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_ctest_skip_audit -v)
set_tests_properties(vv_ctest_skip_audit_unit
    PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} TIMEOUT 60 LABELS vv)

add_test(NAME vv_n21_ci_ledger_unit
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_n21_ci_ledger -v)
set_tests_properties(vv_n21_ci_ledger_unit
    PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} TIMEOUT 60 LABELS vv)

add_test(NAME vv_fvm_warpage_gate_unit
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_fvm_warpage_gate -v)
set_tests_properties(vv_fvm_warpage_gate_unit PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} TIMEOUT 120 LABELS vv SKIP_RETURN_CODE 77)
add_subdirectory(fvm_warpage_gate_a)
