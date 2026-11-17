if(NOT TARGET chipcae::geometry)
    message(FATAL_ERROR "reportfields tests require chipcae::geometry")
endif()

chipcae_auto_tests(LIBS chipcae::geometry LABELS tierA PREFIX reportfields_)
add_executable(reportfields_fixture_writer fixture_writer.cpp)
target_link_libraries(reportfields_fixture_writer PRIVATE chipcae::geometry)
set_tests_properties(reportfields_roundtrip reportfields_io_failure PROPERTIES TIMEOUT 30)
set_tests_properties(reportfields_roundtrip PROPERTIES
    ENVIRONMENT "CHIPCAE_REPORTFIELDS_WRITER=$<TARGET_FILE:reportfields_fixture_writer>")
