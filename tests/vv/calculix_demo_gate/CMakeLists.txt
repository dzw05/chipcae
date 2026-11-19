# SPDX-License-Identifier: Apache-2.0
# Reference qualification remains pending; acceptance must fail closed meanwhile.
option(CHIPCAE_DEMO_GATE_REFERENCE_ACCEPTANCE
       "Require qualified DEMO-GATE reference acceptance" ON)
if(NOT CHIPCAE_DEMO_GATE_REFERENCE_ACCEPTANCE)
    message(FATAL_ERROR "DEMO-GATE reference acceptance cannot be disabled. "
                        "Publish a qualified REFERENCE.json and its L1 decks to pass acceptance.")
endif()
chipcae_auto_tests(PREFIX vvccx_ LABELS "vv" TIMEOUT 600)
