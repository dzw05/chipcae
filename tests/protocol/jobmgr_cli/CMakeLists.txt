if(TARGET chipcae::jobmgr_local)
    set(jobmgr_cli_local 1)
    set(jobmgr_cli_libs chipcae::jobmgr_local)
    add_executable(jobmgr_cli_mock_solver mock_solver.cpp)
    target_link_libraries(jobmgr_cli_mock_solver PRIVATE chipcae::io)
    chipcae_set_warnings(jobmgr_cli_mock_solver)
else()
    set(jobmgr_cli_local 0)
    set(jobmgr_cli_libs chipcae::jobmgr)
endif()

foreach(name help_version submit_get_list wait_terminal errors)
    add_executable(jobmgr_cli_${name} test_${name}.cpp)
    target_link_libraries(jobmgr_cli_${name} PRIVATE ${jobmgr_cli_libs})
    target_compile_definitions(jobmgr_cli_${name} PRIVATE
        CHIPCAE_JOBMGR_LOCAL=${jobmgr_cli_local}
        CHIPCAE_JOBMGR_EXE="$<TARGET_FILE:chipcae-jobmgr>")
    add_dependencies(jobmgr_cli_${name} chipcae-jobmgr)
    if(jobmgr_cli_local)
        target_compile_definitions(jobmgr_cli_${name} PRIVATE
            CHIPCAE_JOBMGR_MOCK="$<TARGET_FILE:jobmgr_cli_mock_solver>")
        add_dependencies(jobmgr_cli_${name} jobmgr_cli_mock_solver)
    endif()
    chipcae_set_warnings(jobmgr_cli_${name})
endforeach()

add_test(NAME jobmgr_cli_help_version COMMAND jobmgr_cli_help_version)
add_test(NAME jobmgr_cli_submit_get_list COMMAND jobmgr_cli_submit_get_list)
add_test(NAME jobmgr_cli_wait_terminal COMMAND jobmgr_cli_wait_terminal)
add_test(NAME jobmgr_cli_errors COMMAND jobmgr_cli_errors)
set_tests_properties(jobmgr_cli_help_version jobmgr_cli_submit_get_list
    jobmgr_cli_wait_terminal jobmgr_cli_errors
    PROPERTIES LABELS tierA TIMEOUT 60 SKIP_RETURN_CODE 77)
