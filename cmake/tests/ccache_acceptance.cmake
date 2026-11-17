# Run: cmake -P cmake/tests/ccache_acceptance.cmake
find_program(E7_PYTHON NAMES python3 REQUIRED)
execute_process(
    COMMAND "${E7_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/ccache_acceptance.py" -v
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "E7 compiler-cache acceptance assertions failed (${result})")
endif()
