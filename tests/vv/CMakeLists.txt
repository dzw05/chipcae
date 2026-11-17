find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME warpage_qoi_analytic
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_warpage_qoi -v)
add_test(NAME trilayer_clt_magnitude
    COMMAND ${Python3_EXECUTABLE} -m unittest tests.vv.test_trilayer_clt -v)
set_tests_properties(warpage_qoi_analytic trilayer_clt_magnitude
    PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} TIMEOUT 30)
