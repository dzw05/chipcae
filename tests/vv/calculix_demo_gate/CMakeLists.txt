# SPDX-License-Identifier: Apache-2.0
option(CHIPCAE_DEMO_GATE_REFERENCE_ACCEPTANCE
       "Require the qualified DEMO-GATE reference and archived L1 decks" OFF)
chipcae_auto_tests(PREFIX vvccx_ LABELS "vv" TIMEOUT 600)
if(CHIPCAE_DEMO_GATE_REFERENCE_ACCEPTANCE)
    set(_demo_gate_test_mode reference)
else()
    # PR #179 delivers the generator and validator; reference qualification is deferred.
    set(_demo_gate_test_mode generator)
    message(STATUS "DEMO-GATE: generator/validator tests only; reference acceptance deferred")
endif()
set_property(TEST vvccx_reference_json vvccx_regen_l1 APPEND PROPERTY ENVIRONMENT
             "CHIPCAE_DEMO_GATE_TEST_MODE=${_demo_gate_test_mode}")
