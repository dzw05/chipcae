find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(mode install_smoke install_relocate cpack_tgz)
    add_test(NAME ${mode} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/acceptance.py ${mode}
        --build ${PROJECT_BINARY_DIR} --source ${PROJECT_SOURCE_DIR}
        --version ${PROJECT_VERSION} --os ${CMAKE_SYSTEM_NAME}
        --arch ${CMAKE_SYSTEM_PROCESSOR} --mpi "${MPIEXEC_EXECUTABLE}")
    # Source schemas are hidden briefly; never overlap any other CTest test.
    set_tests_properties(${mode} PROPERTIES LABELS install RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
