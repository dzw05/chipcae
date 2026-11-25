cmake_minimum_required(VERSION 3.20)
project(solve_cuda_fault_acceptance LANGUAGES CXX)

# Compile only: no replacement solver, link stubs, or GPU execution.
add_library(solve_cuda_fault OBJECT "${CHIPCAE_SOURCE_DIR}/apps/chipcae-solve/src/main.cpp")
target_compile_features(solve_cuda_fault PRIVATE cxx_std_20)
target_include_directories(solve_cuda_fault PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CHIPCAE_SOURCE_DIR}/core/include"
    "${CHIPCAE_SOURCE_DIR}/schema/include"
    "${CHIPCAE_SOURCE_DIR}/mesh/background/include"
    "${CHIPCAE_SOURCE_DIR}/mesh/cut/include"
    "${CHIPCAE_SOURCE_DIR}/apps/chipcae-solve/include")
target_compile_options(solve_cuda_fault PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(solve_cuda_fault PRIVATE CHIPCAE_SOLVE_INPUTS=1 ${FAULT_DEFINITIONS})
