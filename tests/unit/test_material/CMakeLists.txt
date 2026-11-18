if(NOT TARGET chipcae::material)
    if(CHIPCAE_REQUIRE_KOKKOS)
        message(FATAL_ERROR "test_material: chipcae::material target missing")
    endif()
    message(STATUS "Skipping test_material: chipcae::material is unavailable (requires Kokkos)")
    return()
endif()

chipcae_auto_tests(LABELS tierA PREFIX matcli_ TIMEOUT 60)
set_property(TEST matcli_card_check_cli APPEND PROPERTY ENVIRONMENT
    "CHIPCAE_MATERIAL_CARD_CHECK=$<TARGET_FILE:material_card_check>" "OMP_NUM_THREADS=1" "OMP_PROC_BIND=false")
