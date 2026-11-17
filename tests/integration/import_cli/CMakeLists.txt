find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(CHIPCAE_ENABLE_ECAD)
    set(import_cli_regression import_cli_refactor_noop)
    set(import_cli_case noop)
else()
    set(import_cli_regression import_cli_refactor_noop_ecad_off)
    set(import_cli_case ecad_off)
endif()
add_test(NAME ${import_cli_regression}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py
            $<TARGET_FILE:chipcae-import> ${import_cli_case})
foreach(case step reserved help)
    if("${case}" STREQUAL "step")
        set(name import_cli_step_unchanged)
    elseif("${case}" STREQUAL "reserved")
        set(name import_cli_reserved_args)
    else()
        set(name import_cli_help_unchanged)
    endif()
    add_test(NAME ${name}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py
                $<TARGET_FILE:chipcae-import> ${case})
endforeach()
set_tests_properties(${import_cli_regression} import_cli_step_unchanged
    import_cli_reserved_args import_cli_help_unchanged
    PROPERTIES LABELS "tierA;import_cli" TIMEOUT 60)
