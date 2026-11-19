if(TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    chipcae_auto_tests(PREFIX field_compare_ LABELS "integration;vv" TIMEOUT 600)
    foreach(name ranks outline_bbox fem_fvm)
        set_property(TEST field_compare_${name} APPEND PROPERTY ENVIRONMENT
            "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>"
            "PROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}")
    endforeach()
else()
    # The collector has no per-file filter; the unit test needs no solver backend.
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 COMPONENTS Interpreter QUIET)
    endif()
    if(NOT Python3_EXECUTABLE)
        return()
    endif()
    add_test(NAME field_compare_unit
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_unit.py" "${PROJECT_SOURCE_DIR}")
    set_tests_properties(field_compare_unit PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        LABELS "integration;vv"
        ENVIRONMENT "PROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}")
endif()
