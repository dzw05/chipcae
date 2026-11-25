if(TARGET chipcae::jobmgr_local)
    set(jobmgr_test_libs chipcae::jobmgr_local)
    set(jobmgr_local_enabled 1)
    if(NOT TARGET jsoncons::jsoncons)
        find_package(jsoncons REQUIRED)
    endif()
    add_executable(jobmgr_local_mock_solver mock_solver.cpp)
    target_link_libraries(jobmgr_local_mock_solver PRIVATE chipcae::io jsoncons::jsoncons)
    chipcae_set_warnings(jobmgr_local_mock_solver)
    configure_file(fake_mpirun.sh "${CMAKE_CURRENT_BINARY_DIR}/fake_mpirun.sh" COPYONLY)
    file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/fake_mpirun.sh"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
else()
    set(jobmgr_test_libs chipcae::jobmgr)
    set(jobmgr_local_enabled 0)
endif()

# Preserve existing target names, while keeping the custom desktop target out of discovery.
foreach(name paths layout state_machine profile calls recovery poll launch_e2e cancel stop_roundtrip)
    set(target chipcae_autotest_tests_protocol_jobmgr_local_jobmgr_local_${name})
    add_executable(${target} test_${name}.cpp)
    if(name STREQUAL "paths")
        target_link_libraries(${target} PRIVATE chipcae::jobmgr)
    else()
        target_link_libraries(${target} PRIVATE ${jobmgr_test_libs})
        target_compile_definitions(${target} PRIVATE CHIPCAE_JOBMGR_LOCAL=${jobmgr_local_enabled})
    endif()
    target_compile_definitions(${target} PRIVATE CHIPCAE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        CHIPCAE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    chipcae_set_warnings(${target})
    add_test(NAME jobmgr_local_${name} COMMAND ${target})
    set_tests_properties(jobmgr_local_${name} PROPERTIES TIMEOUT 60 LABELS tierA
        ENVIRONMENT "CHIPCAE_SOURCE_DIR=${PROJECT_SOURCE_DIR};CHIPCAE_BINARY_DIR=${PROJECT_BINARY_DIR}")
    if(NOT name STREQUAL "paths")
        set_tests_properties(jobmgr_local_${name} PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    if(jobmgr_local_enabled AND (name STREQUAL "launch_e2e" OR name STREQUAL "cancel" OR name STREQUAL "stop_roundtrip"))
        target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/jobmgr/src")
        target_compile_definitions(${target} PRIVATE
            JOBMGR_MOCK_SOLVER="$<TARGET_FILE:jobmgr_local_mock_solver>"
            JOBMGR_FAKE_MPIRUN="${CMAKE_CURRENT_BINARY_DIR}/fake_mpirun.sh")
        add_dependencies(${target} jobmgr_local_mock_solver)
    endif()
endforeach()

find_package(Qt6 COMPONENTS Core QUIET)
add_executable(jobmgr_local_desktop_parity test_desktop_parity.cpp)
if(TARGET chipcae::jobmgr_local AND Qt6Core_FOUND)
    target_sources(jobmgr_local_desktop_parity PRIVATE
        "${PROJECT_SOURCE_DIR}/desktop/jobs/ProfileMapper.cpp"
        "${PROJECT_SOURCE_DIR}/desktop/jobs/JobTypes.cpp")
    target_include_directories(jobmgr_local_desktop_parity PRIVATE
        "${PROJECT_SOURCE_DIR}/desktop/jobs" "${PROJECT_SOURCE_DIR}/jobmgr/src")
    target_link_libraries(jobmgr_local_desktop_parity PRIVATE
        Qt6::Core chipcae::jobmgr_local chipcae_jobmgr_protocol)
    target_compile_definitions(jobmgr_local_desktop_parity PRIVATE CHIPCAE_JOBMGR_DESKTOP_PARITY=1)
else()
    target_link_libraries(jobmgr_local_desktop_parity PRIVATE chipcae::jobmgr)
    target_compile_definitions(jobmgr_local_desktop_parity PRIVATE CHIPCAE_JOBMGR_DESKTOP_PARITY=0)
endif()
target_compile_definitions(jobmgr_local_desktop_parity PRIVATE
    CHIPCAE_SOURCE_DIR="${PROJECT_SOURCE_DIR}" CHIPCAE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
chipcae_set_warnings(jobmgr_local_desktop_parity)
add_test(NAME jobmgr_local_desktop_parity COMMAND jobmgr_local_desktop_parity)
set_tests_properties(jobmgr_local_desktop_parity PROPERTIES TIMEOUT 60 LABELS tierA SKIP_RETURN_CODE 77
    ENVIRONMENT "CHIPCAE_SOURCE_DIR=${PROJECT_SOURCE_DIR};CHIPCAE_BINARY_DIR=${PROJECT_BINARY_DIR}")
