# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
#
# Auto-collected tests, so a new test is "add a file", not "edit a shared CMakeLists".
#
# chipcae_auto_tests([LIBS <targets>...] [LABELS <labels>...] [TIMEOUT <s>] [PREFIX <p>])
#   Call from a test subdirectory's CMakeLists.txt. Collects, non-recursively,
#   from CMAKE_CURRENT_SOURCE_DIR:
#   - test_*.cpp : one executable per file, linked to LIBS (default chipcae::core),
#                  add_test NAME = <PREFIX><file stem without "test_">.
#                  Compiled with CHIPCAE_SOURCE_DIR / CHIPCAE_TEST_SOURCE_DIR defined.
#   - test_*.py  : run with Python3 (arg 1 = project source dir), SKIP_RETURN_CODE 77.
#   - test_occt_*: collected only when CHIPCAE_ENABLE_OCCT=ON, extra label "occt".
#   Files are globbed with CONFIGURE_DEPENDS, so adding one re-runs CMake.
#
# chipcae_auto_test_subdirs()
#   Called as the last line of tests/CMakeLists.txt. Adds every tests/<a>/CMakeLists.txt
#   and tests/<a>/<b>/CMakeLists.txt that calls chipcae_auto_tests( and was not
#   already added explicitly. New cards therefore create their own subdirectory and
#   never touch tests/CMakeLists.txt.
include_guard(GLOBAL)

function(chipcae_auto_tests)
    cmake_parse_arguments(AT "" "TIMEOUT;PREFIX" "LIBS;LABELS" ${ARGN})
    if(NOT AT_LIBS)
        set(AT_LIBS chipcae::core)
    endif()
    if(NOT AT_TIMEOUT)
        set(AT_TIMEOUT 120)
    endif()
    file(RELATIVE_PATH _rel "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    string(MAKE_C_IDENTIFIER "${_rel}" _slug)

    file(GLOB _cpp CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
    file(GLOB _py CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.py")
    list(SORT _cpp)
    list(SORT _py)

    foreach(_src IN LISTS _cpp _py)
        get_filename_component(_stem "${_src}" NAME_WE)
        get_filename_component(_ext "${_src}" LAST_EXT)
        set(_labels ${AT_LABELS})
        if(_stem MATCHES "^test_occt_")
            if(NOT CHIPCAE_ENABLE_OCCT)
                continue()
            endif()
            list(APPEND _labels occt)
        endif()
        string(REGEX REPLACE "^test_" "" _name "${_stem}")
        set(_name "${AT_PREFIX}${_name}")
        if(_ext STREQUAL ".cpp")
            set(_exe "chipcae_autotest_${_slug}_${_name}")
            add_executable(${_exe} "${_src}")
            target_link_libraries(${_exe} PRIVATE ${AT_LIBS})
            target_compile_definitions(${_exe} PRIVATE CHIPCAE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                       CHIPCAE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
            if(COMMAND chipcae_set_warnings)
                chipcae_set_warnings(${_exe})
            endif()
            add_test(NAME ${_name} COMMAND ${_exe})
        else()
            if(NOT Python3_EXECUTABLE)
                find_package(Python3 COMPONENTS Interpreter REQUIRED)
            endif()
            add_test(NAME ${_name} COMMAND ${Python3_EXECUTABLE} "${_src}" "${PROJECT_SOURCE_DIR}")
            set_tests_properties(${_name} PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
        endif()
        set_tests_properties(${_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT ${AT_TIMEOUT}
            ENVIRONMENT "CHIPCAE_SOURCE_DIR=${PROJECT_SOURCE_DIR};CHIPCAE_BINARY_DIR=${PROJECT_BINARY_DIR}")
        if(_labels)
            set_tests_properties(${_name} PROPERTIES LABELS "${_labels}")
        endif()
    endforeach()
endfunction()

function(chipcae_auto_test_subdirs)
    get_property(_added DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
    file(GLOB _lists CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/*/*/CMakeLists.txt")
    list(SORT _lists)
    foreach(_list IN LISTS _lists)
        get_filename_component(_dir "${_list}" DIRECTORY)
        get_filename_component(_parent "${_dir}" DIRECTORY)
        # Skip explicit adds, and nested dirs whose parent has its own CMakeLists (it owns them).
        if(_dir IN_LIST _added OR (NOT _parent STREQUAL CMAKE_CURRENT_SOURCE_DIR AND EXISTS "${_parent}/CMakeLists.txt"))
            continue()
        endif()
        file(READ "${_list}" _body)
        string(FIND "${_body}" "chipcae_auto_tests(" _pos)
        if(_pos EQUAL -1)
            continue()
        endif()
        add_subdirectory("${_dir}")
    endforeach()
endfunction()
