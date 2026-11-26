if(CHIPCAE_ENABLE_JSONCONS AND TARGET chipcae_stack)
    chipcae_auto_tests(LIBS chipcae_stack jsoncons LABELS tierA TIMEOUT 120 PREFIX import_)
    set(_i1_test chipcae_autotest_tests_unit_test_import_i1_copper_orientation_import_copper_orientation)
    target_sources(${_i1_test} PRIVATE ${PROJECT_SOURCE_DIR}/kernel/src/stack_writer.cpp)
    target_compile_definitions(${_i1_test} PRIVATE CHIPCAE_ENABLE_JSONCONS=1)
    target_include_directories(${_i1_test} PRIVATE
        ${PROJECT_SOURCE_DIR}/kernel/src
        ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/src
        ${PROJECT_SOURCE_DIR}/apps/chipcae-solve/include
        ${PROJECT_SOURCE_DIR}/mesh/include
        ${PROJECT_SOURCE_DIR}/mesh/background/include
        ${PROJECT_SOURCE_DIR}/mesh/cut/include)
endif()

if(CHIPCAE_ENABLE_ECAD AND TARGET chipcae-solve AND CHIPCAE_ENABLE_MFEM AND CHIPCAE_ENABLE_MPI AND CHIPCAE_ENABLE_HDF5)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE "$ENV{CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE}"
        CACHE FILEPATH "Independently built branch-point chipcae-solve for I1 comparisons")
    if(NOT EXISTS "${CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE}")
        message(FATAL_ERROR "Set CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE to an independently built branch-point chipcae-solve")
    endif()
    add_test(NAME import_i1_copper_orientation
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/solve_copper_orientation.py)
    set_tests_properties(import_i1_copper_orientation PROPERTIES LABELS tierA TIMEOUT 1200)
    set_property(TEST import_i1_copper_orientation APPEND PROPERTY ENVIRONMENT
        "CHIPCAE_SOLVE_EXE=$<TARGET_FILE:chipcae-solve>"
        "CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE=${CHIPCAE_I1_BRANCHPOINT_SOLVE_EXE}"
        "CHIPCAE_MPIEXEC=${MPIEXEC_EXECUTABLE}"
        "CHIPCAE_MPIEXEC_NUMPROC_FLAG=${MPIEXEC_NUMPROC_FLAG}")
endif()
