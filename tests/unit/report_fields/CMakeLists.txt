if(NOT TARGET chipcae::geometry)
    message(FATAL_ERROR "reportfields tests require chipcae::geometry")
endif()

chipcae_auto_tests(LIBS chipcae::geometry LABELS tierA PREFIX reportfields_)
add_executable(reportfields_fixture_writer fixture_writer.cpp)
target_link_libraries(reportfields_fixture_writer PRIVATE chipcae::geometry)
set_tests_properties(reportfields_roundtrip reportfields_io_failure PROPERTIES TIMEOUT 30)
add_test(NAME reportfields_step_box_a1
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_roundtrip.py --step-box-a1)
set_tests_properties(reportfields_step_box_a1 PROPERTIES
    SKIP_RETURN_CODE 77 LABELS tierA TIMEOUT 30)
set_tests_properties(reportfields_roundtrip reportfields_step_box_a1 PROPERTIES
    ENVIRONMENT "CHIPCAE_REPORTFIELDS_WRITER=$<TARGET_FILE:reportfields_fixture_writer>")
