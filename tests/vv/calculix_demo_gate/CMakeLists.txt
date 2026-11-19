# SPDX-License-Identifier: Apache-2.0
option(CHIPCAE_DEMO_GATE_REFERENCE_ACCEPTANCE
       "Run qualified DEMO-GATE reference acceptance (qualification pending V&V)" OFF)
chipcae_auto_tests(PREFIX vvccx_ LABELS "vv" TIMEOUT 600)

if(CHIPCAE_DEMO_GATE_REFERENCE_ACCEPTANCE)
    set(_demo_gate_mode reference)
else()
    set(_demo_gate_mode generator)
    message(STATUS "DEMO-GATE: generator/validator regressions; reference qualification deferred to V&V")
endif()
foreach(_test vvccx_reference_json vvccx_regen_l1)
    set_property(TEST ${_test} APPEND PROPERTY ENVIRONMENT
                 "CHIPCAE_DEMO_GATE_TEST_MODE=${_demo_gate_mode}")
endforeach()
