chipcae_auto_tests(LIBS chipcae::geometry chipcae_mesh_cut LABELS tierA TIMEOUT 60)

# Missing-dependency skips (fix-slice-tests-skip77; pr114.md B.5):
# Real cases need local demo STEP files via CHIPCAE_DEMO_STEP_DIR.
# slice_no_occt is only meaningful without OCCT.
# Keep the global C++ return-77-fails rule unchanged.
if(CHIPCAE_ENABLE_OCCT)
    set_tests_properties(
        occt_slice_real_bulk_parts
        occt_slice_real_ihp_example
        slice_no_occt
        PROPERTIES SKIP_RETURN_CODE 77)
endif()
