cmake_minimum_required(VERSION 3.24)

# 独立构建：cmake -S material -B material/build-cpu -G Ninja
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(chipcae_material VERSION 0.1.0 LANGUAGES CXX)
    set(CHIPCAE_MATERIAL_STANDALONE ON)
else()
    set(CHIPCAE_MATERIAL_STANDALONE OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CHIPCAE_MATERIAL_FETCH_KOKKOS "Fetch and build Kokkos via FetchContent" OFF)
option(CHIPCAE_MATERIAL_ENABLE_PYTHON "Build the nanobind Python module" ON)
option(CHIPCAE_MATERIAL_ENABLE_TESTS "Build material unit tests" ON)

# ---- Kokkos ----
if(NOT TARGET Kokkos::kokkos AND NOT TARGET Kokkos::kokkoscore)
    if(CHIPCAE_MATERIAL_FETCH_KOKKOS)
        include(FetchContent)
        set(Kokkos_ENABLE_SERIAL ON CACHE BOOL "" FORCE)
        if(NOT DEFINED Kokkos_ENABLE_OPENMP)
            set(Kokkos_ENABLE_OPENMP ON CACHE BOOL "" FORCE)
        endif()
        FetchContent_Declare(kokkos
            GIT_REPOSITORY https://github.com/kokkos/kokkos.git
            GIT_TAG 4.7.01
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(kokkos)
    else()
        find_package(Kokkos 4.3 QUIET)
    endif()
endif()

if(TARGET Kokkos::kokkos)
    set(CHIPCAE_MATERIAL_KOKKOS_TARGET Kokkos::kokkos)
elseif(TARGET Kokkos::kokkoscore)
    set(CHIPCAE_MATERIAL_KOKKOS_TARGET Kokkos::kokkoscore)
else()
    if(CHIPCAE_REQUIRE_KOKKOS)
        message(FATAL_ERROR
            "CHIPCAE_REQUIRE_KOKKOS=ON but Kokkos was not found. "
            "Set Kokkos_ROOT or CMAKE_PREFIX_PATH to a Kokkos install.")
    endif()
    message(STATUS
        "chipcae material: Kokkos not found; skipping the material library. "
        "Set CMAKE_PREFIX_PATH to a Kokkos install or enable CHIPCAE_MATERIAL_FETCH_KOKKOS.")
    return()
endif()

# ---- Library ----
add_library(chipcae_material STATIC
    src/factory.cpp
    src/schema.cpp
    src/temperature_table.cpp
    src/cte_convert.cpp
    driver/single_point.cpp)
add_library(chipcae::material ALIAS chipcae_material)

target_include_directories(chipcae_material
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${CMAKE_CURRENT_SOURCE_DIR}/driver
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(chipcae_material PUBLIC cxx_std_20)
target_link_libraries(chipcae_material PUBLIC ${CHIPCAE_MATERIAL_KOKKOS_TARGET})
# Host and device floating-point contraction are set explicitly so T11's
# exact T_sf zero and CPU/GPU comparisons stay consistent across compilers.
target_compile_options(chipcae_material PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-ffp-contract=fast>
    $<$<COMPILE_LANGUAGE:CUDA>:--fmad=true>)

# ---- Optional Python bindings ----
if(CHIPCAE_MATERIAL_ENABLE_PYTHON)
    find_package(Python 3.9 COMPONENTS Interpreter Development QUIET)
    if(Python_Interpreter_FOUND AND Python_Development_FOUND)
        find_package(nanobind CONFIG QUIET)
    else()
        set(nanobind_FOUND FALSE)
    endif()
    if(nanobind_FOUND)
        nanobind_add_module(chipcae_material_python python/bindings.cpp)
        target_link_libraries(chipcae_material_python PRIVATE chipcae_material)
        set_target_properties(chipcae_material_python PROPERTIES LIBRARY_OUTPUT_NAME chipcae_material)
        message(STATUS "chipcae material: nanobind module enabled")
    else()
        message(STATUS "chipcae material: Python development headers (Python.h) or nanobind not found; skipping the optional bindings module")
    endif()
endif()

# ---- Tests ----
if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    if(CHIPCAE_MATERIAL_STANDALONE)
        enable_testing()
    endif()

    add_executable(material_t_smoke tests/t_smoke.cpp)
    target_link_libraries(material_t_smoke PRIVATE chipcae_material)
    add_test(NAME material_t_smoke COMMAND material_t_smoke)
    set_tests_properties(material_t_smoke PROPERTIES LABELS "T0")

    add_executable(material_exec_spaces tests/exec_spaces.cpp)
    target_link_libraries(material_exec_spaces PRIVATE chipcae_material)
    add_test(NAME material_exec_serial COMMAND material_exec_spaces Serial)
    add_test(NAME material_exec_openmp COMMAND material_exec_spaces OpenMP)
    set_tests_properties(material_exec_serial material_exec_openmp PROPERTIES
        LABELS "exec_spaces"
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false"
        SKIP_REGULAR_EXPRESSION "SKIP:")

    add_executable(material_t03_mandel tests/t03_mandel.cpp)
    target_link_libraries(material_t03_mandel PRIVATE chipcae_material)
    add_test(NAME material_t03_mandel COMMAND material_t03_mandel)
    set_tests_properties(material_t03_mandel PROPERTIES LABELS "T3")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_core_math tests/core_math.cpp)
    target_link_libraries(material_core_math PRIVATE chipcae_material)
    add_test(NAME material_core_math COMMAND material_core_math)
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_conductivity tests/conductivity.cpp)
    target_link_libraries(material_conductivity PRIVATE chipcae_material)
    add_test(NAME material_conductivity COMMAND material_conductivity)
    set_tests_properties(material_conductivity PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_laminate_conductivity tests/laminate_conductivity.cpp)
    target_link_libraries(material_laminate_conductivity PRIVATE chipcae_material)
    add_test(NAME material_laminate_conductivity COMMAND material_laminate_conductivity)
    set_tests_properties(material_laminate_conductivity PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_library_parse tests/library_parse.cpp)
    target_link_libraries(material_library_parse PRIVATE chipcae_material)
    add_test(NAME material_library_parse COMMAND material_library_parse
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../schema/examples/fcbga_demo.materials.json)
    set_tests_properties(material_library_parse PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_table_paths tests/table_paths.cpp)
    target_link_libraries(material_table_paths PRIVATE chipcae_material)
    add_test(NAME material_table_paths COMMAND material_table_paths
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    set_tests_properties(material_table_paths PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_id_pattern tests/id_pattern.cpp)
    target_link_libraries(material_id_pattern PRIVATE chipcae_material)
    add_test(NAME material_id_pattern COMMAND material_id_pattern
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    set_tests_properties(material_id_pattern PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_constant_single_point tests/constant_single_point.cpp)
    target_link_libraries(material_constant_single_point PRIVATE chipcae_material)
    add_test(NAME material_constant_single_point COMMAND material_constant_single_point
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../examples/s2_solve/materials.json
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/integration/bimetal/data/materials.json)
    set_tests_properties(material_constant_single_point PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_library(acceptance_cases OBJECT tests/acceptance.cpp)
    target_link_libraries(acceptance_cases PRIVATE chipcae_material)
    foreach(id IN ITEMS T1 T2 T4 T5 T6 T7 T8 T9 T10 T11 T12 T13 T15 T16 T17 T18 T19 T20 T21 T22 T23 T24pt)
        add_executable(material_${id} $<TARGET_OBJECTS:acceptance_cases>)
        target_link_libraries(material_${id} PRIVATE chipcae_material)
        add_test(NAME material_${id} COMMAND material_${id} ${id} ${CMAKE_CURRENT_SOURCE_DIR}/tests/negative)
        set_tests_properties(material_${id} PROPERTIES LABELS ${id} ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
    endforeach()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(K3_DEFAULT_TEST_PYTHON "${Python3_EXECUTABLE}")
    set(K3_TEST_PYTHON "${K3_DEFAULT_TEST_PYTHON}" CACHE FILEPATH "Python with NumPy/SciPy")
    add_test(NAME material_T14 COMMAND ${K3_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/dma_nnls.py)
    set_tests_properties(material_T14 PROPERTIES LABELS T14)
    add_test(NAME material_T16-d19 COMMAND ${K3_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_yaml_numbers.py)
    set_tests_properties(material_T16-d19 PROPERTIES LABELS "T16;D19")
    add_test(NAME material_T15_threads COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_threads.py $<TARGET_FILE:material_T15>)
    set_tests_properties(material_T15_threads PROPERTIES LABELS T15)
endif()

add_executable(material_card_check tools/card_check.cpp)
target_link_libraries(material_card_check PRIVATE chipcae_material)

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_custom_target(material_T14 COMMAND ${K3_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/dma_nnls.py)
    add_custom_target(material_T24 COMMAND ${CMAKE_COMMAND} -E echo "SKIP: T24 requires later K2/P1 FEM")
    add_test(NAME material_T24 COMMAND ${CMAKE_COMMAND} -E echo "SKIP: T24 requires later K2/P1 FEM")
    set_tests_properties(material_T24 PROPERTIES LABELS T24 SKIP_REGULAR_EXPRESSION "SKIP:")
    add_custom_target(material_T25 COMMAND ${CMAKE_COMMAND} -E echo "PENDING: T25 requires a self-hosted GPU runner")
    add_test(NAME material_T25 COMMAND ${CMAKE_COMMAND} -E echo "PENDING: T25 requires a self-hosted GPU runner")
    set_tests_properties(material_T25 PROPERTIES LABELS T25 SKIP_REGULAR_EXPRESSION "PENDING:")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_review_regressions tests/review_regressions.cpp)
    target_link_libraries(material_review_regressions PRIVATE chipcae_material)
    add_test(NAME material_review_regressions COMMAND material_review_regressions ${CMAKE_CURRENT_SOURCE_DIR}/tests/negative)
    set_tests_properties(material_review_regressions PROPERTIES LABELS "T22;layout_hash;memory_space" ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_copper_mixture tests/copper_mixture.cpp)
    target_link_libraries(material_copper_mixture PRIVATE chipcae_material)
    target_include_directories(material_copper_mixture PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../mesh/background/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../schema/include)
    target_compile_definitions(material_copper_mixture PRIVATE CHIPCAE_ENABLE_KOKKOS)
    foreach(case IN ITEMS limits laminate spd reject_visco exec expand)
        add_test(NAME material_cu_mix_${case} COMMAND material_copper_mixture ${case})
        set_tests_properties(material_cu_mix_${case} PROPERTIES LABELS tierA TIMEOUT 60
            ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false" SKIP_REGULAR_EXPRESSION "SKIP:")
    endforeach()
    add_test(NAME material_point_param_empty COMMAND material_copper_mixture empty)
    set_tests_properties(material_point_param_empty PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_test(NAME material_cu_mix_cuda COMMAND material_copper_mixture exec_cuda)
    set_tests_properties(material_cu_mix_cuda PROPERTIES LABELS nightly TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false" SKIP_REGULAR_EXPRESSION "SKIP:")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_test(NAME material_cu_mix_numpy
        COMMAND ${K3_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/laminate_reference.py
                $<TARGET_FILE:material_copper_mixture>)
    set_tests_properties(material_cu_mix_numpy PROPERTIES LABELS tierA
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    foreach(variant IN ITEMS material_expint material_expint_fallback)
        add_executable(${variant} tests/expint.cpp)
        target_link_libraries(${variant} PRIVATE chipcae_material)
        add_test(NAME ${variant} COMMAND ${variant})
    endforeach()
    target_compile_definitions(material_expint_fallback PRIVATE CHIPCAE_MATERIAL_FORCE_EXPINT_FALLBACK=1)
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_tableview_single_point tests/tableview_single_point.cpp)
    target_link_libraries(material_tableview_single_point PRIVATE chipcae_material)
    add_test(NAME material_tableview_single_point COMMAND material_tableview_single_point host)
    set_tests_properties(material_tableview_single_point PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
    add_test(NAME material_tableview_single_point_cuda COMMAND material_tableview_single_point cuda)
    set_tests_properties(material_tableview_single_point_cuda PROPERTIES LABELS nightly TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false" SKIP_RETURN_CODE 77)
endif()

if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    add_executable(material_library_soldermask_lpi tests/library_soldermask_lpi.cpp)
    target_link_libraries(material_library_soldermask_lpi PRIVATE chipcae_material)
    # Keep the fixture parser available in standalone builds without an optional package.
    target_include_directories(material_library_soldermask_lpi SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../desktop/third_party/jsoncons/include)
    add_test(NAME material_library_soldermask_lpi COMMAND material_library_soldermask_lpi
        ${CMAKE_CURRENT_SOURCE_DIR}/library/soldermask_lpi_eq.materials.json)
    set_tests_properties(material_library_soldermask_lpi PROPERTIES LABELS tierA TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=2;OMP_PROC_BIND=false")
endif()
