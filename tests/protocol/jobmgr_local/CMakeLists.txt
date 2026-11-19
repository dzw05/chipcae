if(TARGET chipcae::jobmgr_local)
    set(jobmgr_test_libs chipcae::jobmgr_local)
    set(jobmgr_local_enabled 1)
else()
    set(jobmgr_test_libs chipcae::jobmgr)
    set(jobmgr_local_enabled 0)
endif()

chipcae_auto_tests(LIBS ${jobmgr_test_libs} LABELS tierA PREFIX jobmgr_local_ TIMEOUT 60)
# The always-running path checks must not acquire optional dependencies.
set_property(TARGET chipcae_autotest_tests_protocol_jobmgr_local_jobmgr_local_paths
    PROPERTY LINK_LIBRARIES chipcae::jobmgr)

foreach(name layout state_machine profile calls recovery poll)
    target_compile_definitions(
        chipcae_autotest_tests_protocol_jobmgr_local_jobmgr_local_${name}
        PRIVATE CHIPCAE_JOBMGR_LOCAL=${jobmgr_local_enabled})
    set_tests_properties(jobmgr_local_${name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
