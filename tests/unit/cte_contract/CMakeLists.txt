if(NOT TARGET chipcae::material)
    if(CHIPCAE_REQUIRE_KOKKOS)
        message(FATAL_ERROR "cte_contract: chipcae::material target missing")
    endif()
    message(STATUS "Skipping cte_contract: chipcae::material is unavailable (requires Kokkos)")
    return()
endif()

chipcae_auto_tests(LIBS chipcae::material LABELS tierA PREFIX ctecontract_)
