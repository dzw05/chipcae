if(TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    # cmake/ChipcaeAutoTests.cmake documents and passes PROJECT_SOURCE_DIR as
    # Python argument 1 for every collected test, matching the fallback below.
    chipcae_auto_tests(PREFIX demogate_ LABELS "cut;numerical" TIMEOUT 1800)
    set_property(TEST demogate_solve APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>")
else()
    # The collector has no per-file filter; keep input validation without registering the solve.
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
    endif()
    add_test(NAME demogate_inputs
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_inputs.py" "${PROJECT_SOURCE_DIR}")
    set_tests_properties(demogate_inputs PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        TIMEOUT 1800
        LABELS "cut;numerical"
        ENVIRONMENT "CHIPCAE_SOURCE_DIR=${PROJECT_SOURCE_DIR};CHIPCAE_BINARY_DIR=${PROJECT_BINARY_DIR}")
endif()
