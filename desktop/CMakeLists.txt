cmake_minimum_required(VERSION 3.20)
project(ChipcaeDesktop VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
option(CHIPCAE_BUILD_TESTS "Build desktop smoke tests" ON)
include(cmake/Dependencies.cmake)
include(cmake/ProtocolGeneration.cmake)
chipcae_generate_protocol()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/document-schema")
foreach(schema case-1.0 parameters run-profile-1.0 stack-0.2)
    configure_file("../schema/${schema}.schema.json"
        "document-schema/${schema}.schema.json" COPYONLY)
endforeach()
add_library(chipcae_document document/CaseDocument.cpp document/CaseDocument.hpp
    forms/SchemaValidator.cpp forms/SchemaValidator.hpp)
target_include_directories(chipcae_document PUBLIC document forms)
target_link_libraries(chipcae_document PUBLIC Qt6::Core jsoncons::jsoncons)
target_compile_definitions(chipcae_document PRIVATE
    CHIPCAE_SCHEMA_DIR="${CMAKE_CURRENT_BINARY_DIR}/document-schema")
add_library(chipcae_desktop_ui src/MainWindow.cpp src/MainWindow.hpp
    import/ImportController.cpp import/ImportController.hpp
    import/DefectsDialog.cpp import/DefectsDialog.hpp
    workers/PreviewWorker.cpp workers/PreviewWorker.hpp
    tree/CaseTreeModel.cpp tree/CaseTreeModel.hpp
    tree/CaseTreeView.cpp tree/CaseTreeView.hpp
    forms/SchemaForm.cpp forms/SchemaForm.hpp forms/CaseOutline.cpp forms/CaseOutline.hpp)
target_include_directories(chipcae_desktop_ui PUBLIC src workers forms import tree)
target_link_libraries(chipcae_desktop_ui PUBLIC chipcae_document Qt6::Core Qt6::Gui Qt6::Widgets
    Qt6::OpenGLWidgets ${VTK_LIBRARIES} jsoncons::jsoncons)
target_compile_definitions(chipcae_desktop_ui PRIVATE CHIPCAE_REPOSITORY="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_executable(chipcae-desktop src/main.cpp)
target_link_libraries(chipcae-desktop PRIVATE chipcae_desktop_ui chipcae_desktop_protocol)
vtk_module_autoinit(TARGETS chipcae_desktop_ui chipcae-desktop MODULES ${VTK_LIBRARIES})
if(CHIPCAE_BUILD_TESTS)
    enable_testing()
    foreach(test window render worker schema protocol)
        add_executable(desktop_${test}_smoke tests/${test}_smoke.cpp)
        if(test STREQUAL "schema")
            target_link_libraries(desktop_${test}_smoke PRIVATE jsoncons::jsoncons)
        elseif(test STREQUAL "protocol")
            target_link_libraries(desktop_${test}_smoke PRIVATE chipcae_desktop_protocol)
        elseif(test STREQUAL "render")
            target_link_libraries(desktop_${test}_smoke PRIVATE VTK::CommonCore VTK::CommonDataModel
                VTK::FiltersSources VTK::RenderingCore VTK::RenderingOpenGL2 VTK::IOImage)
            vtk_module_autoinit(TARGETS desktop_${test}_smoke MODULES VTK::RenderingOpenGL2)
        else()
            target_link_libraries(desktop_${test}_smoke PRIVATE chipcae_desktop_ui)
            vtk_module_autoinit(TARGETS desktop_${test}_smoke MODULES ${VTK_LIBRARIES})
        endif()
        add_test(NAME desktop_${test}_smoke COMMAND desktop_${test}_smoke)
        set_tests_properties(desktop_${test}_smoke PROPERTIES TIMEOUT 30)
    endforeach()
    set_tests_properties(desktop_window_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # DISPLAY and VTK_DEFAULT_OPENGL_WINDOW are left to the caller: local Mesa EGL
    # (no DISPLAY) or CI Xvfb + vtkXOpenGLRenderWindow. Forcing DISPLAY= here would
    # hide the Xvfb display and regress the GitHub runner.
endif()

add_library(chipcae_desktop_results io/ResultSession.cpp io/ResultSession.hpp
    ../io/src/event_log.cpp)
target_include_directories(chipcae_desktop_results PUBLIC io ../io/include)
target_link_libraries(chipcae_desktop_results PUBLIC Qt6::Core VTK::CommonDataModel
    VTK::IOHDF PRIVATE VTK::hdf5)
if(CHIPCAE_BUILD_TESTS)
    add_executable(desktop_document tests/document_test.cpp)
    target_link_libraries(desktop_document PRIVATE chipcae_document)
    target_compile_definitions(desktop_document PRIVATE
        DOCUMENT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../schema/examples")
    add_test(NAME desktop_document COMMAND desktop_document)
    add_executable(desktop_result_io tests/result_io_test.cpp)
    target_link_libraries(desktop_result_io PRIVATE chipcae_desktop_results
        VTK::RenderingOpenGL2 VTK::RenderingCore)
    vtk_module_autoinit(TARGETS desktop_result_io MODULES VTK::RenderingOpenGL2)
    find_program(CHIPCAE_FIXTURE_PYTHON NAMES python3 python HINTS "${CMAKE_PREFIX_PATH}/bin" REQUIRED)
    add_test(NAME desktop_result_fixtures COMMAND ${CHIPCAE_FIXTURE_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/result_fixtures.py ${CMAKE_CURRENT_BINARY_DIR}/result-samples)
    set_tests_properties(desktop_result_fixtures PROPERTIES FIXTURES_SETUP result_samples)
    add_test(NAME desktop_result_io COMMAND desktop_result_io ${CMAKE_CURRENT_BINARY_DIR}/result-samples)
    # Do not force DISPLAY= (same CI EGL pitfall as desktop_render_smoke).
    set_tests_properties(desktop_result_io PROPERTIES FIXTURES_REQUIRED result_samples
        TIMEOUT 90)
    add_executable(desktop_forms_test tests/forms_test.cpp)
    target_link_libraries(desktop_forms_test PRIVATE chipcae_desktop_ui)
    target_compile_definitions(desktop_forms_test PRIVATE CHIPCAE_REPOSITORY="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME desktop_forms COMMAND desktop_forms_test)
    set_tests_properties(desktop_forms PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
endif()

if(CHIPCAE_BUILD_TESTS)
    find_program(CHIPCAE_IMPORT_PYTHON NAMES python python3 REQUIRED)
    add_executable(desktop_import_test tests/import_test.cpp)
    target_link_libraries(desktop_import_test PRIVATE chipcae_desktop_ui)
    target_compile_definitions(desktop_import_test PRIVATE
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../schema/examples"
        MOCK_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tools/mock_import.py"
        PYTHON_EXECUTABLE="${CHIPCAE_IMPORT_PYTHON}")
    add_test(NAME desktop_import COMMAND desktop_import_test)
    add_test(NAME desktop_import_dialog COMMAND desktop_import_test --dialog)
    set_tests_properties(desktop_import desktop_import_dialog PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
endif()

add_library(chipcae_local_jobs jobs/JobTypes.cpp jobs/JobTypes.hpp
    jobs/ProfileMapper.cpp jobs/ProfileMapper.hpp jobs/LocalJobService.cpp jobs/LocalJobService.hpp)
target_include_directories(chipcae_local_jobs PUBLIC jobs)
target_link_libraries(chipcae_local_jobs PUBLIC chipcae_desktop_ui chipcae_desktop_protocol)
if(CHIPCAE_BUILD_TESTS)
    add_executable(desktop_launcher tests/launcher_test.cpp)
    target_link_libraries(desktop_launcher PRIVATE chipcae_local_jobs)
    target_compile_definitions(desktop_launcher PRIVATE CHIPCAE_REPOSITORY="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME desktop_launcher COMMAND desktop_launcher)
    set_tests_properties(desktop_launcher PROPERTIES TIMEOUT 45)
endif()
if(CHIPCAE_BUILD_TESTS)
    add_test(NAME desktop_launcher_control_reader
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_control_test.py")
endif()

if(CHIPCAE_BUILD_TESTS)
    add_executable(desktop_tree tests/tree_test.cpp)
    target_link_libraries(desktop_tree PRIVATE chipcae_desktop_ui)
    target_compile_definitions(desktop_tree PRIVATE CHIPCAE_REPOSITORY="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME desktop_tree COMMAND desktop_tree)
    set_tests_properties(desktop_tree PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
endif()

add_library(chipcae_desktop_viewer viewer/ResultView.cpp viewer/ResultView.hpp
    viewer/ResultControls.cpp viewer/ResultControls.hpp)
target_include_directories(chipcae_desktop_viewer PUBLIC viewer)
target_link_libraries(chipcae_desktop_viewer PUBLIC chipcae_desktop_results Qt6::Widgets
    Qt6::OpenGLWidgets ${VTK_LIBRARIES} VTK::FiltersExtraction VTK::FiltersGeneral
    VTK::RenderingAnnotation)
vtk_module_autoinit(TARGETS chipcae_desktop_viewer MODULES ${VTK_LIBRARIES})
if(CHIPCAE_BUILD_TESTS)
    foreach(test viewer viewer_capture)
        add_executable(desktop_${test} tests/${test}_test.cpp)
        target_link_libraries(desktop_${test} PRIVATE chipcae_desktop_viewer)
        vtk_module_autoinit(TARGETS desktop_${test} MODULES ${VTK_LIBRARIES})
        if(test STREQUAL "viewer_capture")
            set(test_name desktop_capture)
        else()
            set(test_name desktop_viewer)
        endif()
        add_test(NAME ${test_name} COMMAND desktop_${test} ${CMAKE_CURRENT_BINARY_DIR}/result-samples
            ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(${test_name} PROPERTIES FIXTURES_REQUIRED result_samples
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    endforeach()
endif()
