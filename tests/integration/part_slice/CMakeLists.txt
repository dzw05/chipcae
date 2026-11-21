chipcae_auto_tests(LIBS chipcae::geometry chipcae_mesh_cut LABELS tierA TIMEOUT 60)

# Missing-dependency skips (fix-slice-tests-skip77; pr114.md B.5):
# Real cases need local demo STEP files via CHIPCAE_DEMO_STEP_DIR.
# slice_no_occt is only meaningful without OCCT.
# Keep the global C++ return-77-fails rule unchanged.
if(CHIPCAE_ENABLE_OCCT)
    # chipcae_auto_tests above creates these executables in this directory scope.
    # Include slice_no_occt: the definition makes it return the registered skip code 77.
    get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        if(_target MATCHES "^chipcae_autotest_tests_integration_part_slice_")
            get_target_property(_type ${_target} TYPE)
            if(_type STREQUAL "EXECUTABLE")
                target_include_directories(${_target} SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
                target_compile_definitions(${_target} PRIVATE CHIPCAE_ENABLE_OCCT=1)
            endif()
        endif()
    endforeach()
    # Brief scope amendment (i-part-slice-occt-test-build review): allow only these
    # two fixture dependencies in addition to the OCCT includes and definition.
    # draft uses BRepOffsetAPI_ThruSections; rounded uses BRepFilletAPI_MakeFillet.
    # Require OCCT imported targets so a misspelled name cannot become a bare -l flag.
    foreach(_occt_fixture_library IN ITEMS TKOffset TKFillet)
        if(NOT TARGET ${_occt_fixture_library})
            message(FATAL_ERROR "Missing OCCT fixture target: ${_occt_fixture_library}")
        endif()
        get_target_property(_occt_fixture_imported ${_occt_fixture_library} IMPORTED)
        if(NOT _occt_fixture_imported)
            message(FATAL_ERROR "OCCT fixture target must be imported: ${_occt_fixture_library}")
        endif()
    endforeach()
    target_link_libraries(chipcae_autotest_tests_integration_part_slice_occt_slice_draft PRIVATE TKOffset)
    target_link_libraries(chipcae_autotest_tests_integration_part_slice_occt_slice_rounded PRIVATE TKFillet)
    set_tests_properties(
        occt_slice_real_bulk_parts
        occt_slice_real_ihp_example
        slice_no_occt
        PROPERTIES SKIP_RETURN_CODE 77)
endif()
