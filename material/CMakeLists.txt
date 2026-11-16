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
            GIT_TAG 4.7.1
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(kokkos)
    else()
        find_package(Kokkos QUIET)
    endif()
endif()

if(TARGET Kokkos::kokkos)
    set(CHIPCAE_MATERIAL_KOKKOS_TARGET Kokkos::kokkos)
elseif(TARGET Kokkos::kokkoscore)
    set(CHIPCAE_MATERIAL_KOKKOS_TARGET Kokkos::kokkoscore)
else()
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
        message(STATUS "chipcae material: Python/nanobind not found; Python bindings disabled")
    endif()
endif()

# ---- Tests ----
if(CHIPCAE_MATERIAL_ENABLE_TESTS)
    if(CHIPCAE_MATERIAL_STANDALONE)
        enable_testing()
    endif()

    add_executable(t_smoke tests/t_smoke.cpp)
    target_link_libraries(t_smoke PRIVATE chipcae_material)
    add_test(NAME t_smoke COMMAND t_smoke)
    set_tests_properties(t_smoke PROPERTIES LABELS "T0")

    add_executable(t03_mandel tests/t03_mandel.cpp)
    target_link_libraries(t03_mandel PRIVATE chipcae_material)
    add_test(NAME t03_mandel COMMAND t03_mandel)
    set_tests_properties(t03_mandel PROPERTIES LABELS "T3")
endif()
