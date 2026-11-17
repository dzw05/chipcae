if(NOT TARGET chipcae::material)
    if(CHIPCAE_REQUIRE_KOKKOS)
        message(FATAL_ERROR "temp_edges: chipcae::material target missing")
    endif()
    message(STATUS "Skipping temp_edges: chipcae::material is unavailable (requires Kokkos)")
    return()
endif()

chipcae_auto_tests(LIBS chipcae::material LABELS tierA PREFIX tempedge_)
