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
#   - test_*.py  : run with Python3 (arg 1 = project source dir), SKIP_RETURN_CODE 77
#                  (C++ tests get no skip code, so a stray "return 77" fails).
#   Test names are global to CTest: pass PREFIX <area>_ if a stem could collide, and
#   LABELS tierA (or mesh, ...) if the test should run in label-filtered jobs.
#   - test_occt_*: collected only when CHIPCAE_ENABLE_OCCT=ON, extra label "occt".
#   Files are globbed with CONFIGURE_DEPENDS, so adding one re-runs CMake.
#
# chipcae_auto_test_subdirs()
#   Called as the last line of tests/CMakeLists.txt. Adds every tests/<a>/ and
#   tests/<a>/<b>/ directory that has a CMakeLists.txt plus an (empty) marker file
#   .chipcae-autotests, and was not already added explicitly. New cards therefore create their own subdirectory and
#   never touch tests/CMakeLists.txt.
# The helper functions are intentionally defined once for the whole project.
include_guard(GLOBAL)

function(chipcae_auto_tests)
    cmake_parse_arguments(AT "" "TIMEOUT;PREFIX" "LIBS;LABELS" ${ARGN})
    if(AT_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "chipcae_auto_tests: unknown arguments: ${AT_UNPARSED_ARGUMENTS}")
    endif()
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
        # CTest names are global: fail fast instead of a silent clash.
        get_property(_owner GLOBAL PROPERTY CHIPCAE_AUTOTEST_OWNER_${_name})
        if(TEST ${_name} OR _owner)
            message(FATAL_ERROR "chipcae_auto_tests: test name '${_name}' from ${_src} is already "
                                "registered${_owner}; pass PREFIX <area>_ in ${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
        set_property(GLOBAL PROPERTY CHIPCAE_AUTOTEST_OWNER_${_name} " by ${_src}")
        set_property(GLOBAL PROPERTY CHIPCAE_TEST_SOURCE_${_name} "${_src}")
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
            # C++ tests: a stray "return 77" must fail, not skip.
        else()
            if(NOT Python3_EXECUTABLE)
                find_package(Python3 COMPONENTS Interpreter REQUIRED)
            endif()
            add_test(NAME ${_name} COMMAND ${Python3_EXECUTABLE} "${_src}" "${PROJECT_SOURCE_DIR}")
            set_tests_properties(${_name} PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                                                     SKIP_RETURN_CODE 77)
        endif()
        set_tests_properties(${_name} PROPERTIES TIMEOUT ${AT_TIMEOUT}
            ENVIRONMENT "CHIPCAE_SOURCE_DIR=${PROJECT_SOURCE_DIR};CHIPCAE_BINARY_DIR=${PROJECT_BINARY_DIR}")
        if(_labels)
            set_tests_properties(${_name} PROPERTIES LABELS "${_labels}")
        endif()
    endforeach()
endfunction()

function(chipcae_auto_test_subdirs)
    # SUBDIRECTORIES lists the absolute *source* dirs already added (CMake docs).
    get_property(_added DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
    # Defensive: also accept binary-dir spellings by mapping them back to source paths.
    string(REPLACE "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" _added_src "${_added}")
    list(APPEND _added ${_added_src})
    file(GLOB _lists CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/*/*/CMakeLists.txt")
    list(SORT _lists)
    # Re-run CMake when a marker appears or disappears.
    file(GLOB _markers CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*/.chipcae-autotests"
        "${CMAKE_CURRENT_SOURCE_DIR}/*/*/.chipcae-autotests")
    foreach(_list IN LISTS _lists)
        get_filename_component(_dir "${_list}" DIRECTORY)
        get_filename_component(_parent "${_dir}" DIRECTORY)
        # Skip explicit adds, and nested dirs whose parent has its own CMakeLists (it owns them).
        if(_dir IN_LIST _added OR (NOT _parent STREQUAL CMAKE_CURRENT_SOURCE_DIR AND EXISTS "${_parent}/CMakeLists.txt"))
            continue()
        endif()
        # Opt-in is structural: a marker file next to the CMakeLists.txt, not text scanning.
        if(NOT EXISTS "${_dir}/.chipcae-autotests")
            continue()
        endif()
        add_subdirectory("${_dir}")
    endforeach()
endfunction()
