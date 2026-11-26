if(CHIPCAE_ENABLE_HDF5)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(iose_tool io_stats_export_tests.cpp)
    target_link_libraries(iose_tool PRIVATE chipcae::io)
    target_include_directories(iose_tool PRIVATE "${PROJECT_SOURCE_DIR}/schema/src")
    target_compile_definitions(iose_tool PRIVATE
        IOSE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixture"
        IOSE_DRIVER="${CMAKE_CURRENT_SOURCE_DIR}/test_io_stats_export.py"
        IOSE_PYTHON="${Python3_EXECUTABLE}")
    chipcae_set_warnings(iose_tool)
    set(_iose_stackup "${PROJECT_SOURCE_DIR}/schema/examples/fcbga_demo.stack.json")

    add_test(NAME iose_unit COMMAND iose_tool unit "${_iose_stackup}")
    foreach(mode IN ITEMS fixture writer_bytes)
        add_test(NAME iose_${mode} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_io_stats_export.py" ${mode}
            --tool "$<TARGET_FILE:iose_tool>" --stackup "${_iose_stackup}")
        set_tests_properties(iose_${mode} PROPERTIES
            ENVIRONMENT "CHIPCAE_BINARY_DIR=${PROJECT_BINARY_DIR}")
    endforeach()
    set_tests_properties(iose_unit iose_fixture iose_writer_bytes PROPERTIES
        LABELS tierA TIMEOUT 60)
    if(CHIPCAE_ENABLE_MPI)
        add_test(NAME iose_mpi_2 COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
            ${MPIEXEC_PREFLAGS} $<TARGET_FILE:iose_tool> ${MPIEXEC_POSTFLAGS}
            mpi "${_iose_stackup}")
        set_tests_properties(iose_mpi_2 PROPERTIES LABELS "tierA;mpi;io" TIMEOUT 180)
    endif()
endif()
