if(CHIPCAE_ENABLE_HDF5)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(iowt_write_timing_tests write_timing_tests.cpp)
    target_link_libraries(iowt_write_timing_tests PRIVATE chipcae::io)
    chipcae_set_warnings(iowt_write_timing_tests)

    foreach(mode IN ITEMS stats_after_write before_finalize two_steps finalize_only)
        add_test(NAME iowt_${mode} COMMAND iowt_write_timing_tests ${mode}
            "${PROJECT_SOURCE_DIR}/schema/examples/fcbga_demo.stack.json")
        set_tests_properties(iowt_${mode} PROPERTIES LABELS tierA TIMEOUT 60)
    endforeach()
    add_test(NAME iowt_files_unchanged COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_files_unchanged.py"
        "$<TARGET_FILE:iowt_write_timing_tests>"
        "${PROJECT_SOURCE_DIR}/schema/examples/fcbga_demo.stack.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/expected.json")
    set_tests_properties(iowt_files_unchanged PROPERTIES LABELS tierA TIMEOUT 60)
endif()
