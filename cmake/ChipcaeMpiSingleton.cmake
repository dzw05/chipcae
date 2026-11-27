# SPDX-License-Identifier: Apache-2.0
include_guard(GLOBAL)

if(NOT CHIPCAE_BUILD_TESTS)
    return()
endif()

include(CMakePushCheckState)
include(CheckCSourceCompiles)
set(_chipcae_singleton_open_mpi FALSE)
if(CHIPCAE_ENABLE_MPI)
    foreach(_component C CXX)
        if(NOT TARGET MPI::MPI_${_component})
            continue()
        endif()
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_LIBRARIES MPI::MPI_${_component})
        set(_source "
            #include <mpi.h>
            #if !defined(OMPI_MAJOR_VERSION) || !defined(OMPI_MINOR_VERSION)
            #error Not Open MPI
            #endif
            int main(void) { return 0; }
        ")
        if(_component STREQUAL "C")
            check_c_source_compiles("${_source}" CHIPCAE_SINGLETON_OPEN_MPI_C_HEADERS)
        else()
            include(CheckCXXSourceCompiles)
            check_cxx_source_compiles("${_source}" CHIPCAE_SINGLETON_OPEN_MPI_CXX_HEADERS)
        endif()
        cmake_pop_check_state()
        if(CHIPCAE_SINGLETON_OPEN_MPI_${_component}_HEADERS)
            set(_chipcae_singleton_open_mpi TRUE)
        endif()
    endforeach()
endif()

set(_chipcae_singleton_driver "${CMAKE_CURRENT_LIST_DIR}/check_ompi_singleton_startup.cmake")
if(NOT _chipcae_singleton_open_mpi)
    add_test(NAME infra_ompi_singleton_startup
        COMMAND cmake -E echo "本构建不是 Open MPI")
    set_tests_properties(infra_ompi_singleton_startup PROPERTIES
        TIMEOUT 2400 RUN_SERIAL TRUE LABELS "infra;acceptance;owner:INFRA"
        SKIP_REGULAR_EXPRESSION "本构建不是 Open MPI")
    return()
endif()

# Cross-directory TEST DIRECTORY access requires CMake 3.28. Retain this guard
# for standalone module consumers so protection cannot silently be omitted
# in subdirectories.
if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR
        "Open MPI singleton test protection requires CMake >= 3.28; upgrade CMake or disable MPI/tests")
endif()

# Use the C wrapper next to the configured CXX wrapper, never a PATH vendor guess.
if(NOT TARGET MPI::MPI_C AND NOT MPI_C_COMPILER)
    get_filename_component(_chipcae_mpi_bin "${MPI_CXX_COMPILER}" DIRECTORY)
    find_program(_chipcae_mpicc NAMES mpicc HINTS "${_chipcae_mpi_bin}" NO_DEFAULT_PATH)
    if(NOT _chipcae_mpicc)
        message(FATAL_ERROR "Open MPI C wrapper missing next to ${MPI_CXX_COMPILER}")
    endif()
    set(MPI_C_COMPILER "${_chipcae_mpicc}")
endif()
if(NOT TARGET MPI::MPI_C)
    find_package(MPI REQUIRED COMPONENTS C)
endif()
if(TARGET MPI::MPI_CXX AND (NOT MPI_C_INCLUDE_DIRS STREQUAL MPI_CXX_INCLUDE_DIRS
        OR NOT MPI_C_LIBRARIES STREQUAL MPI_CXX_LIBRARIES
        OR NOT MPI_C_VERSION STREQUAL MPI_CXX_VERSION))
    message(FATAL_ERROR "Singleton probe requires matching MPI C/CXX installations")
endif()

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES MPI::MPI_C)
check_c_source_compiles("
    #include <mpi.h>
    #ifndef OMPI_MAJOR_VERSION
    #error Not Open MPI C
    #endif
    int main(void) { return 0; }
" CHIPCAE_SINGLETON_OPEN_MPI_C_HEADERS)
if(NOT CHIPCAE_SINGLETON_OPEN_MPI_C_HEADERS)
    message(FATAL_ERROR "Singleton probe C component is not Open MPI")
endif()

# Header macros alone cannot identify the library that the loader will use.
# MPI_Get_library_version is valid before MPI_Init, so this query does not
# enter the singleton path being diagnosed. Check each configured component.
set(_chipcae_runtime_source [=[
    #include <mpi.h>
    #include <stdio.h>
    #include <string.h>
    int main(void) {
        char actual[MPI_MAX_LIBRARY_VERSION_STRING];
        char expected[80];
        int length = 0;
        if (MPI_Get_library_version(actual, &length) != MPI_SUCCESS) return 1;
        printf("%s\n", actual);
        snprintf(expected, sizeof(expected), "Open MPI v%d.%d.%d",
                 OMPI_MAJOR_VERSION, OMPI_MINOR_VERSION, OMPI_RELEASE_VERSION);
        size_t n = strlen(expected);
        return strncmp(actual, expected, n) != 0 ||
               (actual[n] != '\0' && actual[n] != ',' && actual[n] != ' ');
    }
]=])
option(CHIPCAE_SINGLETON_RUNTIME_RUNNABLE
    "Allow executing the configured MPI runtime on this host" ON)
set(_chipcae_singleton_runnable TRUE)
if(CMAKE_CROSSCOMPILING OR NOT CHIPCAE_SINGLETON_RUNTIME_RUNNABLE)
    set(_chipcae_singleton_runnable FALSE)
endif()
foreach(_component C CXX)
    if(NOT TARGET MPI::MPI_${_component})
        continue()
    endif()
    if(_component STREQUAL "C")
        set(_extension c)
    else()
        set(_extension cpp)
    endif()
    if(_chipcae_singleton_runnable)
        try_run(_runtime_result _runtime_compiled
            SOURCE_FROM_VAR "mpi_runtime.${_extension}" _chipcae_runtime_source
            NO_CACHE LINK_LIBRARIES MPI::MPI_${_component}
            RUN_OUTPUT_VARIABLE _runtime_version
            COMPILE_OUTPUT_VARIABLE _runtime_compile_output)
        if(_runtime_compiled AND _runtime_result STREQUAL "FAILED_TO_RUN")
            # An unavailable loader/target host cannot verify the runtime.
            # Keep compile/link checks and protection, but report a real skip.
            set(_chipcae_singleton_runnable FALSE)
        elseif(_runtime_compiled AND NOT _runtime_result STREQUAL "0")
            message(FATAL_ERROR "Open MPI ${_component} linked runtime disagrees with headers: ${_runtime_version}")
        endif()
    else()
        try_compile(_runtime_compiled
            SOURCE_FROM_VAR "mpi_runtime.${_extension}" _chipcae_runtime_source
            NO_CACHE LINK_LIBRARIES MPI::MPI_${_component}
            OUTPUT_VARIABLE _runtime_compile_output)
    endif()
    if(NOT _runtime_compiled)
        message(FATAL_ERROR "Open MPI ${_component} runtime compile/link check failed: ${_runtime_compile_output}")
    endif()
    if(_chipcae_singleton_runnable)
        string(STRIP "${_runtime_version}" _runtime_version)
        message(STATUS "Open MPI singleton linked runtime (${_component}): ${_runtime_version}")
    else()
        message(STATUS "Open MPI singleton ${_component}: target runtime is not runnable; compile/link checked, runtime identity unverified")
    endif()
endforeach()
function(_chipcae_singleton_check_clock)
    set(CMAKE_REQUIRED_LIBRARIES "")
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
    check_c_source_compiles("
        #include <time.h>
        int main(void) { struct timespec t; return timespec_get(&t, TIME_UTC) == 0; }
    " CHIPCAE_SINGLETON_TIMESPEC_GET)
    if(NOT CHIPCAE_SINGLETON_TIMESPEC_GET)
        check_c_source_compiles("
            #define _POSIX_C_SOURCE 200809L
            #include <time.h>
            int main(void) { struct timespec t; return clock_gettime(CLOCK_MONOTONIC, &t); }
        " CHIPCAE_SINGLETON_POSIX_CLOCK)
        if(NOT CHIPCAE_SINGLETON_POSIX_CLOCK)
            message(FATAL_ERROR "No portable clock available for singleton probe")
        endif()
    endif()
endfunction()
_chipcae_singleton_check_clock()
cmake_pop_check_state()

add_executable(chipcae_ompi_singleton_probe "${CMAKE_CURRENT_LIST_DIR}/ompi_singleton_probe.c")
set_target_properties(chipcae_ompi_singleton_probe PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(chipcae_ompi_singleton_probe PRIVATE MPI::MPI_C)
if(CHIPCAE_SINGLETON_TIMESPEC_GET)
    target_compile_definitions(chipcae_ompi_singleton_probe PRIVATE CHIPCAE_PROBE_TIMESPEC_GET=1)
else()
    target_compile_definitions(chipcae_ompi_singleton_probe PRIVATE CHIPCAE_PROBE_POSIX_CLOCK=1)
endif()
chipcae_set_warnings(chipcae_ompi_singleton_probe)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(_chipcae_singleton_runnable)
    add_test(NAME infra_ompi_singleton_startup COMMAND cmake
        "-DPROBE=$<TARGET_FILE:chipcae_ompi_singleton_probe>"
        "-DPYTHON=${Python3_EXECUTABLE}" -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_startup PROPERTIES
        TIMEOUT 2400 RUN_SERIAL TRUE LABELS "infra;acceptance;owner:INFRA")

    # Controlled executables exercise the same real driver, with its fixed 10 s gate
    # and N >= 200 validation. No WILL_FAIL: check the specific failure diagnostic.
    foreach(_case sleep timing timeout_timing exit7 missing iterations success environment descendant_exit missing_report invalid_report)
        add_test(NAME infra_ompi_singleton_driver_${_case} COMMAND cmake
            "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
            -P "${_chipcae_singleton_driver}")
        set_tests_properties(infra_ompi_singleton_driver_${_case} PROPERTIES
            TIMEOUT 40 LABELS "infra;unit;owner:INFRA")
    endforeach()
    foreach(_case dynamic_execution cross_compile non_runnable c_only selection propagation matching inherited_baseline runtime_mismatch compatibility conflict_inherited conflict_environment
            conflict_modification conflict_unset)
        add_test(NAME infra_ompi_singleton_${_case} COMMAND cmake
            "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
            "-DBUILD_DIR=${PROJECT_BINARY_DIR}" -P "${_chipcae_singleton_driver}")
        set_tests_properties(infra_ompi_singleton_${_case} PROPERTIES
            TIMEOUT 120 LABELS "infra;unit;owner:INFRA")
    endforeach()
    foreach(_case disabled non_open_mpi tests_off component_paths)
        add_test(NAME infra_ompi_singleton_${_case} COMMAND cmake
            "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
            -P "${_chipcae_singleton_driver}")
        set_tests_properties(infra_ompi_singleton_${_case} PROPERTIES
            TIMEOUT 60 LABELS "infra;unit;owner:INFRA")
    endforeach()
    add_test(NAME infra_ompi_singleton_coverage_dynamic COMMAND cmake
        "-DPYTHON=${Python3_EXECUTABLE}" -DCHECK=coverage_dynamic
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}" -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_coverage_dynamic PROPERTIES
        TIMEOUT 60 LABELS "infra;unit;owner:INFRA")
    add_test(NAME infra_ompi_singleton_configuration COMMAND cmake
        "-DPYTHON=${Python3_EXECUTABLE}" -DCHECK=configuration
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}" -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_configuration PROPERTIES
        TIMEOUT 60 LABELS "infra;unit;owner:INFRA")

else()
    add_test(NAME infra_ompi_singleton_startup
        COMMAND cmake -E echo "MPI target runtime is not runnable on this host; startup validation not run")
    set_tests_properties(infra_ompi_singleton_startup PROPERTIES
        TIMEOUT 2400 RUN_SERIAL TRUE LABELS "infra;acceptance;owner:INFRA"
        SKIP_REGULAR_EXPRESSION "MPI target runtime is not runnable")
endif()

# Per-test session isolation for both singleton clients and launcher children.
# Prefixes: https://docs.open-mpi.org/en/v5.0.x/mca.html
# PRRTE exposes prte_tmpdir_base (prte_info --all). OpenPMIx 5.0.8 uses
# PMIX_SERVER_TMPDIR / PMIX_SYSTEM_TMPDIR directly, not MCA directory aliases:
# https://github.com/openpmix/openpmix/blob/v5.0.8/src/mca/ptl/base/ptl_base_frame.c
# https://github.com/openpmix/openpmix/blob/v5.0.8/src/mca/ptl/client/ptl_client.c
set(_chipcae_singleton_keys
    PRTE_MCA_prte_tmpdir_base PMIX_SERVER_TMPDIR PMIX_SYSTEM_TMPDIR)

function(_chipcae_singleton_protect_test directory test)
    # Hash the defining scope too: distinct scopes/build trees must not share
    # session roots. Keep the suffix short for runtime socket/file names.
    string(SHA256 _id "${directory}/${test}")
    string(SUBSTRING "${_id}" 0 16 _id)
    set(_base "${PROJECT_BINARY_DIR}/.ompi")
    set(_value "${_base}/${_id}")
    file(MAKE_DIRECTORY "${_value}")
    file(CHMOD "${_base}" "${_value}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    # Fail configuration on an unusable directory, before registering a green
    # looking test environment. Runtime-owned contents are cleaned by MPI;
    # these empty roots live with the build tree so tests can run repeatedly.
    file(WRITE "${_value}/.write-check" "singleton session directory check\n")
    file(REMOVE "${_value}/.write-check")

    get_property(_environment TEST "${test}" DIRECTORY "${directory}" PROPERTY ENVIRONMENT)
    get_property(_modifications TEST "${test}" DIRECTORY "${directory}"
        PROPERTY ENVIRONMENT_MODIFICATION)
    foreach(_key IN LISTS _chipcae_singleton_keys)
        if(DEFINED ENV{${_key}} AND NOT "$ENV{${_key}}" STREQUAL "${_value}")
            # A host's shared scratch root is a baseline, not a per-test
            # isolation request. Override it locally and retain the diagnostic.
            # Explicit per-test settings below remain strict conflicts.
            if(IS_ABSOLUTE "$ENV{${_key}}" AND IS_DIRECTORY "$ENV{${_key}}")
                message(STATUS "Open MPI singleton protection: ${test}: overriding inherited baseline ${_key}=$ENV{${_key}} with ${_value} (test-local only)")
            else()
                message(FATAL_ERROR "Open MPI singleton protection conflict: test ${test} inherited ${_key}=$ENV{${_key}} is not an absolute directory; requires ${_value}")
            endif()
        endif()
        set(_kept_environment "")
        foreach(_entry IN LISTS _environment)
            if(_entry MATCHES "^${_key}=(.*)$")
                if(NOT "${CMAKE_MATCH_1}" STREQUAL "${_value}")
                    message(FATAL_ERROR "Open MPI singleton protection conflict: test ${test} ENVIRONMENT ${_entry}; requires ${_key}=${_value}")
                endif()
            else()
                # Preserve escaped semicolons in unrelated environment values.
                string(REPLACE ";" "\\;" _escaped "${_entry}")
                list(APPEND _kept_environment "${_escaped}")
            endif()
        endforeach()
        set(_kept_modifications "")
        foreach(_entry IN LISTS _modifications)
            if(_entry MATCHES "^${_key}=(.*)$")
                # Modifications run after ENVIRONMENT. Reject unset/reset and
                # concatenation, as well as conflicting set operations.
                if(NOT "${CMAKE_MATCH_1}" STREQUAL "set:${_value}")
                    message(FATAL_ERROR "Open MPI singleton protection conflict: test ${test} ENVIRONMENT_MODIFICATION ${_entry}; requires ${_key}=set:${_value}")
                endif()
            else()
                string(REPLACE ";" "\\;" _escaped "${_entry}")
                list(APPEND _kept_modifications "${_escaped}")
            endif()
        endforeach()
        # Coalesce matching inheritance/properties to one local assignment.
        list(APPEND _kept_environment "${_key}=${_value}")
        set(_environment "${_kept_environment}")
        set(_modifications "${_kept_modifications}")
        message(STATUS "Open MPI singleton protection: ${test}: ${_key}=${_value}")
    endforeach()
    set_property(TEST "${test}" DIRECTORY "${directory}"
        PROPERTY ENVIRONMENT "${_environment}")
    set_property(TEST "${test}" DIRECTORY "${directory}"
        PROPERTY ENVIRONMENT_MODIFICATION "${_modifications}")
endfunction()

function(_chipcae_singleton_protect_directory directory)
    get_property(_tests DIRECTORY "${directory}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        # Any test can reach MPI via a dynamically resolved executable, shell
        # wrapper, generated script or library. Protect the complete inventory
        # rather than attempting to infer the runtime process tree from text.
        _chipcae_singleton_protect_test("${directory}" "${_test}")
    endforeach()
    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
        _chipcae_singleton_protect_directory("${_child}")
    endforeach()
endfunction()
_chipcae_singleton_protect_directory("${PROJECT_SOURCE_DIR}")
