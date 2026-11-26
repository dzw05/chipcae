# SPDX-License-Identifier: Apache-2.0
include_guard(GLOBAL)

if(NOT CHIPCAE_BUILD_TESTS)
    return()
endif()

# Test-first scaffolding only: deliberately do not inject an MPI workaround.
include(CMakePushCheckState)
include(CheckCXXSourceCompiles)
set(_chipcae_singleton_open_mpi FALSE)
if(CHIPCAE_ENABLE_MPI AND TARGET MPI::MPI_CXX)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES MPI::MPI_CXX)
    check_cxx_source_compiles("
        #include <mpi.h>
        #if !defined(OMPI_MAJOR_VERSION) || !defined(OMPI_MINOR_VERSION)
        #error Not Open MPI
        #endif
        int main() { return 0; }
    " CHIPCAE_SINGLETON_OPEN_MPI_HEADERS)
    cmake_pop_check_state()
    set(_chipcae_singleton_open_mpi "${CHIPCAE_SINGLETON_OPEN_MPI_HEADERS}")
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

# Use the C wrapper next to the configured CXX wrapper, never a PATH vendor guess.
if(NOT MPI_C_COMPILER)
    get_filename_component(_chipcae_mpi_bin "${MPI_CXX_COMPILER}" DIRECTORY)
    find_program(_chipcae_mpicc NAMES mpicc HINTS "${_chipcae_mpi_bin}" NO_DEFAULT_PATH)
    if(NOT _chipcae_mpicc)
        message(FATAL_ERROR "Open MPI C wrapper missing next to ${MPI_CXX_COMPILER}")
    endif()
    set(MPI_C_COMPILER "${_chipcae_mpicc}")
endif()
find_package(MPI REQUIRED COMPONENTS C)
if(NOT MPI_C_INCLUDE_DIRS STREQUAL MPI_CXX_INCLUDE_DIRS
        OR NOT MPI_C_LIBRARIES STREQUAL MPI_CXX_LIBRARIES
        OR NOT MPI_C_VERSION STREQUAL MPI_CXX_VERSION)
    message(FATAL_ERROR "Singleton probe requires matching MPI C/CXX installations")
endif()

include(CheckCSourceCompiles)
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
add_test(NAME infra_ompi_singleton_startup COMMAND cmake
    "-DPROBE=$<TARGET_FILE:chipcae_ompi_singleton_probe>"
    "-DPYTHON=${Python3_EXECUTABLE}" -P "${_chipcae_singleton_driver}")
set_tests_properties(infra_ompi_singleton_startup PROPERTIES
    TIMEOUT 2400 RUN_SERIAL TRUE LABELS "infra;acceptance;owner:INFRA")

# Controlled executables exercise the same real driver, with its fixed 10 s gate
# and N >= 200 validation. No WILL_FAIL: check the specific failure diagnostic.
foreach(_case sleep exit7 missing iterations success environment descendant_exit)
    add_test(NAME infra_ompi_singleton_driver_${_case} COMMAND cmake
        "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
        -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_driver_${_case} PROPERTIES
        TIMEOUT 40 LABELS "infra;unit;owner:INFRA")
endforeach()
foreach(_case selection propagation matching conflict_inherited conflict_environment
        conflict_modification conflict_unset)
    add_test(NAME infra_ompi_singleton_${_case} COMMAND cmake
        "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}" -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_${_case} PROPERTIES
        TIMEOUT 120 LABELS "infra;unit;owner:INFRA")
endforeach()
foreach(_case disabled non_open_mpi tests_off)
    add_test(NAME infra_ompi_singleton_${_case} COMMAND cmake
        "-DPYTHON=${Python3_EXECUTABLE}" "-DCHECK=${_case}"
        -P "${_chipcae_singleton_driver}")
    set_tests_properties(infra_ompi_singleton_${_case} PROPERTIES
        TIMEOUT 60 LABELS "infra;unit;owner:INFRA")
endforeach()
add_test(NAME infra_ompi_singleton_configuration COMMAND cmake
    "-DPYTHON=${Python3_EXECUTABLE}" -DCHECK=configuration
    "-DBUILD_DIR=${PROJECT_BINARY_DIR}" -P "${_chipcae_singleton_driver}")
set_tests_properties(infra_ompi_singleton_configuration PROPERTIES
    TIMEOUT 60 LABELS "infra;unit;owner:INFRA")
