# Strict expected failures must match both the analytic defect and its current
# numerical cause. A newly satisfied target is deliberately a CTest failure.
function(tracemap_xfail name)
    set_property(TEST ${name} APPEND PROPERTY LABELS xfail)
    set_tests_properties(${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "XFAIL ${name} "
        FAIL_REGULAR_EXPRESSION "XPASS|XFAIL-WRONG-REASON|ERROR")
endfunction()

if(CHIPCAE_ENABLE_ECAD)
    add_executable(chipcae_test_tracemap_correctness test_tracemap_correctness.cpp)
    target_link_libraries(chipcae_test_tracemap_correctness PRIVATE chipcae_mesh_background)
    target_include_directories(chipcae_test_tracemap_correctness
        PRIVATE ${PROJECT_SOURCE_DIR}/material/include)
    chipcae_set_warnings(chipcae_test_tracemap_correctness)
    set(_cases tracemap_along_x_parallel tracemap_along_y_parallel tracemap_aligned_exact
        tracemap_refine_order tracemap_empty_and_duplicate)
    foreach(_n 1 4 20 40)
        list(APPEND _cases tracemap_perp_x_xfail_ny${_n} tracemap_perp_y_xfail_nx${_n})
    endforeach()
    foreach(_n 80 160 320 640)
        list(APPEND _cases tracemap_refine_xfail_ny${_n})
    endforeach()
    foreach(_case IN LISTS _cases)
        add_test(NAME ${_case} COMMAND chipcae_test_tracemap_correctness --case ${_case})
        set_tests_properties(${_case} PROPERTIES LABELS "tracemap;tierA" TIMEOUT 60)
        if(_case MATCHES "_xfail_")
            tracemap_xfail(${_case})
        endif()
    endforeach()
endif()

if(CHIPCAE_ENABLE_ECAD AND CHIPCAE_ENABLE_MFEM AND MFEM_USE_PETSC)
    add_executable(chipcae_test_tracemap_fvm test_tracemap_fvm.cpp)
    target_link_libraries(chipcae_test_tracemap_fvm
        PRIVATE chipcae_mesh_background chipcae::fvm chipcae_mesh_cut)
    target_include_directories(chipcae_test_tracemap_fvm PRIVATE
        ${PROJECT_SOURCE_DIR}/solvers/fvm/tests ${PROJECT_SOURCE_DIR}/solvers/fvm/src)
    chipcae_set_warnings(chipcae_test_tracemap_fvm)
    foreach(_case tracemap_fvm_slab_harmonic tracemap_fvm_slab_series_xfail
            tracemap_cut_inside_normalized tracemap_cut_outside_xfail
            tracemap_cut_conservation_xfail tracemap_merge_conservation
            tracemap_merge_directional_xfail)
        add_test(NAME ${_case} COMMAND chipcae_test_tracemap_fvm --case ${_case})
        set_tests_properties(${_case} PROPERTIES LABELS "tracemap;fvm;numerical" TIMEOUT 600)
        if(_case MATCHES "_xfail$")
            tracemap_xfail(${_case})
        endif()
    endforeach()
endif()
