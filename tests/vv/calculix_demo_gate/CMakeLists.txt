# SPDX-License-Identifier: Apache-2.0
# PR #179's approved brief revision (2026-11-19) defers reference qualification.
option(CHIPCAE_DEMO_GATE_REFERENCE_ACCEPTANCE
       "Require qualified DEMO-GATE reference acceptance" OFF)
chipcae_auto_tests(PREFIX vvccx_ LABELS "vv" TIMEOUT 600)

if(CHIPCAE_DEMO_GATE_REFERENCE_ACCEPTANCE)
    set(_demo_gate_mode reference)
else()
    set(_demo_gate_mode generator)
    message(STATUS "DEMO-GATE: generator/validator regressions per PR #179 scope revision "
                   "(2026-11-19); qualified-reference delivery incomplete")
endif()
foreach(_test vvccx_reference_json vvccx_regen_l1)
    set_property(TEST ${_test} APPEND PROPERTY ENVIRONMENT
                 "CHIPCAE_DEMO_GATE_TEST_MODE=${_demo_gate_mode}")
endforeach()
