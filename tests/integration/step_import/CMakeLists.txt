if(CHIPCAE_ENABLE_OCCT)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        set(STEP_INJECT_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/step-inject-matrix.json")
        add_test(NAME step_inject_manifest
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_defects.py
                    ${STEP_INJECT_MANIFEST})
        set_tests_properties(step_inject_manifest PROPERTIES LABELS "nightly;occt" TIMEOUT 120)
    else()
        message(STATUS "Python3 not found; skipping STEP injection manifest test")
    endif()
endif()
